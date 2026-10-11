// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* compress_ae2.c — WinZip AES-2 crypto primitives (AE-2 spec)
 *
 * Implements the WinZip AE-2 encryption format for ZIP archives, built
 * on mbedTLS (already vendored for stdlib_crypto.c). Keeps binary
 * footprint minimal vs. vendoring a full separate ZIP lib.
 *
 * Spec (Appnote PKZip 6.3 §7, WinZip AE-2 technote) :
 *   layout = salt[SL] | verifier[2] | ciphertext[N] | hmac[10]
 *   SL depends on strength: 8 bytes (AES-128), 12 (AES-192), 16 (AES-256)
 *   Key derivation: PBKDF2-HMAC-SHA1(password, salt, 1000 iter, 2*KL+2)
 *     where KL = 16/24/32 for 128/192/256
 *     bytes[0..KL-1]           = AES encryption key
 *     bytes[KL..2*KL-1]        = HMAC-SHA1 key
 *     bytes[2*KL..2*KL+1]      = password verifier (2 bytes)
 *   Ciphertext: AES-CTR(plaintext, enc_key) with a little-endian 128-bit
 *     counter starting at 1, incremented per 16-byte block. NOTE:
 *     WinZip's counter is little-endian wrt mode of operation — not
 *     standard NIST CTR but the documented WinZip variant.
 *   HMAC: HMAC-SHA1(hmac_key, ciphertext) truncated to first 10 bytes.
 *
 * We expose two operations :
 *   owl_ae2_encrypt — produce salt|verifier|ct|hmac from plaintext+password
 *   owl_ae2_decrypt — extract plaintext; validates verifier first, then
 *                     recomputes HMAC and compares with trailing bytes.
 *
 * Both use AES-256 exclusively in OWL (most common in modern archives,
 * single strength keeps the code simple; AES-128/192 can be added later
 * by plumbing a strength parameter through).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif
#endif

#define OWL_AE2_SALT_LEN        16   /* AES-256 uses 16-byte salt */
#define OWL_AE2_KEY_LEN         32   /* AES-256 key */
#define OWL_AE2_VERIFIER_LEN    2
#define OWL_AE2_HMAC_LEN        10
#define OWL_AE2_PBKDF2_ITER     1000
#define OWL_AE2_DERIVED_LEN     (2 * OWL_AE2_KEY_LEN + OWL_AE2_VERIFIER_LEN)
#define OWL_AE2_OVERHEAD        (OWL_AE2_SALT_LEN + OWL_AE2_VERIFIER_LEN + OWL_AE2_HMAC_LEN)

/* Fill `buf` with `n` cryptographically secure bytes. Returns 0 on
 * success, non-zero on failure. Uses BCryptGenRandom on Windows,
 * /dev/urandom elsewhere. */
static int ae2_random(uint8_t* buf, size_t n) {
#ifdef _WIN32
    NTSTATUS s = BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return s >= 0 ? 0 : -1;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
}

/* Derive AES key + HMAC key + password verifier from password and salt. */
static int ae2_derive_keys(const char* password, size_t pw_len,
                            const uint8_t* salt, size_t salt_len,
                            uint8_t out_enc_key[OWL_AE2_KEY_LEN],
                            uint8_t out_hmac_key[OWL_AE2_KEY_LEN],
                            uint8_t out_verifier[OWL_AE2_VERIFIER_LEN]) {
    uint8_t derived[OWL_AE2_DERIVED_LEN];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1 /* HMAC */) != 0) {
        mbedtls_md_free(&ctx);
        return -1;
    }
    int rc = mbedtls_pkcs5_pbkdf2_hmac(&ctx,
                                         (const unsigned char*)password, pw_len,
                                         salt, salt_len,
                                         OWL_AE2_PBKDF2_ITER,
                                         OWL_AE2_DERIVED_LEN, derived);
    mbedtls_md_free(&ctx);
    if (rc != 0) return -1;

    memcpy(out_enc_key,  derived,                            OWL_AE2_KEY_LEN);
    memcpy(out_hmac_key, derived + OWL_AE2_KEY_LEN,          OWL_AE2_KEY_LEN);
    memcpy(out_verifier, derived + 2 * OWL_AE2_KEY_LEN,      OWL_AE2_VERIFIER_LEN);

    /* Scrub the big derived blob — only the split copies remain. */
    memset(derived, 0, sizeof(derived));
    return 0;
}

/* Apply WinZip AE-2 CTR mode: XOR plaintext with AES-ECB-encrypted
 * counter blocks. Counter is little-endian 128-bit, starts at 1 for
 * the first block (note: standard NIST CTR typically starts at 0).
 * In/out can alias. */
static int ae2_ctr_xor(const uint8_t aes_key[OWL_AE2_KEY_LEN],
                        uint8_t* data, size_t len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, aes_key, 256) != 0) {
        mbedtls_aes_free(&aes);
        return -1;
    }
    uint8_t counter[16] = {0};
    uint8_t keystream[16];
    /* WinZip spec: counter starts at 1, little-endian increment. */
    counter[0] = 1;

    for (size_t i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, counter, keystream);
        size_t chunk = (i + 16 <= len) ? 16 : len - i;
        for (size_t j = 0; j < chunk; j++) data[i + j] ^= keystream[j];
        /* Increment counter little-endian. */
        for (int k = 0; k < 16; k++) {
            if (++counter[k] != 0) break;
        }
    }
    mbedtls_aes_free(&aes);
    /* Scrub keystream + counter. */
    memset(counter, 0, sizeof(counter));
    memset(keystream, 0, sizeof(keystream));
    return 0;
}

/* ------------------------------------------------------------------
 * PUBLIC API
 * ------------------------------------------------------------------ */

/* Encrypt `pt_len` bytes of `pt` with `password` using WinZip AE-2
 * (AES-256). Allocates and returns the output buffer via `*out` (caller
 * must free). Returns the byte count, or -1 on failure.
 *
 * Output layout: salt[16] | verifier[2] | ciphertext[pt_len] | hmac[10]
 * (total = pt_len + 28 bytes). */
int owl_ae2_encrypt(const uint8_t* pt, size_t pt_len,
                     const char* password, size_t pw_len,
                     uint8_t** out, size_t* out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;

    size_t total = pt_len + OWL_AE2_OVERHEAD;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return -1;

    /* Generate salt. */
    if (ae2_random(buf, OWL_AE2_SALT_LEN) != 0) { free(buf); return -1; }

    uint8_t enc_key[OWL_AE2_KEY_LEN];
    uint8_t hmac_key[OWL_AE2_KEY_LEN];
    uint8_t verifier[OWL_AE2_VERIFIER_LEN];
    if (ae2_derive_keys(password, pw_len, buf, OWL_AE2_SALT_LEN,
                        enc_key, hmac_key, verifier) != 0) {
        free(buf);
        return -1;
    }

    memcpy(buf + OWL_AE2_SALT_LEN, verifier, OWL_AE2_VERIFIER_LEN);

    /* Copy plaintext to output area, then XOR in place with CTR keystream. */
    uint8_t* ct = buf + OWL_AE2_SALT_LEN + OWL_AE2_VERIFIER_LEN;
    memcpy(ct, pt, pt_len);
    if (ae2_ctr_xor(enc_key, ct, pt_len) != 0) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(hmac_key, 0, sizeof(hmac_key));
        free(buf);
        return -1;
    }

    /* HMAC over ciphertext, truncated to 10 bytes. */
    uint8_t full_hmac[20];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(info, hmac_key, OWL_AE2_KEY_LEN, ct, pt_len, full_hmac);
    memcpy(ct + pt_len, full_hmac, OWL_AE2_HMAC_LEN);

    /* Scrub sensitive material. */
    memset(enc_key, 0, sizeof(enc_key));
    memset(hmac_key, 0, sizeof(hmac_key));
    memset(verifier, 0, sizeof(verifier));
    memset(full_hmac, 0, sizeof(full_hmac));

    *out = buf;
    *out_len = total;
    return (int)total;
}

/* Decrypt an AE-2 buffer. Returns 0 on success, -1 on bad password
 * (verifier mismatch), -2 on HMAC mismatch (tampering or corruption),
 * -3 on other errors. On success allocates and returns plaintext via
 * `*out_pt` (caller must free), of length `*out_len`.
 *
 * Input layout: salt[16] | verifier[2] | ciphertext[N] | hmac[10]
 * Minimum input size: 28 bytes (empty plaintext). */
int owl_ae2_decrypt(const uint8_t* in, size_t in_len,
                     const char* password, size_t pw_len,
                     uint8_t** out_pt, size_t* out_len) {
    if (!out_pt || !out_len) return -3;
    *out_pt = NULL;
    *out_len = 0;
    if (in_len < OWL_AE2_OVERHEAD) return -3;

    size_t ct_len = in_len - OWL_AE2_OVERHEAD;
    const uint8_t* salt       = in;
    const uint8_t* verifier   = in + OWL_AE2_SALT_LEN;
    const uint8_t* ct         = in + OWL_AE2_SALT_LEN + OWL_AE2_VERIFIER_LEN;
    const uint8_t* hmac_tag   = ct + ct_len;

    uint8_t enc_key[OWL_AE2_KEY_LEN];
    uint8_t hmac_key[OWL_AE2_KEY_LEN];
    uint8_t computed_verifier[OWL_AE2_VERIFIER_LEN];
    if (ae2_derive_keys(password, pw_len, salt, OWL_AE2_SALT_LEN,
                        enc_key, hmac_key, computed_verifier) != 0) {
        return -3;
    }

    /* Check password verifier first (cheap early-exit on wrong password). */
    if (memcmp(verifier, computed_verifier, OWL_AE2_VERIFIER_LEN) != 0) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(hmac_key, 0, sizeof(hmac_key));
        memset(computed_verifier, 0, sizeof(computed_verifier));
        return -1;
    }

    /* Verify HMAC over the ciphertext. */
    uint8_t computed_hmac[20];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(info, hmac_key, OWL_AE2_KEY_LEN, ct, ct_len, computed_hmac);
    int bad_hmac = 0;
    /* Constant-time compare on the first 10 bytes. */
    for (int i = 0; i < OWL_AE2_HMAC_LEN; i++)
        bad_hmac |= (computed_hmac[i] ^ hmac_tag[i]);
    if (bad_hmac != 0) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(hmac_key, 0, sizeof(hmac_key));
        memset(computed_verifier, 0, sizeof(computed_verifier));
        memset(computed_hmac, 0, sizeof(computed_hmac));
        return -2;
    }

    /* Decrypt (CTR mode is symmetric — same XOR operation). */
    uint8_t* pt = (uint8_t*)malloc(ct_len > 0 ? ct_len : 1);
    if (!pt) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(hmac_key, 0, sizeof(hmac_key));
        return -3;
    }
    memcpy(pt, ct, ct_len);
    if (ae2_ctr_xor(enc_key, pt, ct_len) != 0) {
        memset(enc_key, 0, sizeof(enc_key));
        memset(hmac_key, 0, sizeof(hmac_key));
        free(pt);
        return -3;
    }

    /* Scrub. */
    memset(enc_key, 0, sizeof(enc_key));
    memset(hmac_key, 0, sizeof(hmac_key));
    memset(computed_verifier, 0, sizeof(computed_verifier));
    memset(computed_hmac, 0, sizeof(computed_hmac));

    *out_pt = pt;
    *out_len = ct_len;
    return 0;
}
