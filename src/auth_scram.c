// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "auth_scram.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
#else
  #include <stdio.h>
#endif

#define SHA256_LEN 32

static void scram_err(OwlScramSha256* s, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * Helpers crypto (mbedTLS)
 * ================================================================ */

static int hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        uint8_t out[SHA256_LEN]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
}

static int sha256_buf(const uint8_t* msg, size_t msg_len, uint8_t out[SHA256_LEN]) {
    return mbedtls_sha256(msg, msg_len, out, 0);
}

static int pbkdf2_sha256(const uint8_t* pwd, size_t pwd_len,
                          const uint8_t* salt, size_t salt_len,
                          int iter, uint8_t out[SHA256_LEN]) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                          pwd, pwd_len,
                                          salt, salt_len,
                                          (unsigned)iter,
                                          SHA256_LEN, out);
}

/* Base64 encode dans out (size sortie écrite dans *out_len incluant \0). */
static int b64_encode(const uint8_t* src, size_t src_len,
                       char* out, size_t out_size, size_t* out_len) {
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char*)out, out_size, &olen, src, src_len);
    if (rc == 0) {
        if (olen < out_size) out[olen] = '\0';
        if (out_len) *out_len = olen;
    }
    return rc;
}

/* Base64 decode dans out. Retourne longueur effective. */
static int b64_decode(const char* src, size_t src_len,
                       uint8_t* out, size_t out_size, size_t* out_len) {
    size_t olen = 0;
    int rc = mbedtls_base64_decode(out, out_size, &olen,
                                    (const unsigned char*)src, src_len);
    if (rc == 0 && out_len) *out_len = olen;
    return rc;
}

/* CSPRNG OS-natif. */
static bool random_bytes(uint8_t* buf, size_t len) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)len,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len;
#endif
}

/* Génère un nonce ASCII printable de `len` caractères. */
static bool gen_nonce(char* out, size_t len) {
    /* Pool ASCII printable excluant ',', '=', ' ' (réservés SCRAM). */
    static const char pool[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "!#$%&'()*+-./:;<>?@[]^_`{|}~";
    size_t poolLen = sizeof(pool) - 1;
    uint8_t buf[64];
    if (len > sizeof(buf)) return false;
    if (!random_bytes(buf, len)) return false;
    for (size_t i = 0; i < len; i++) {
        out[i] = pool[buf[i] % poolLen];
    }
    out[len] = '\0';
    return true;
}

/* ================================================================
 * Init
 * ================================================================ */

void owl_scram_init(OwlScramSha256* s, const char* username, const char* password) {
    memset(s, 0, sizeof(*s));
    snprintf(s->username, sizeof(s->username), "%s", username ? username : "");
    snprintf(s->password, sizeof(s->password), "%s", password ? password : "");
}

/* ================================================================
 * Étape 1 : client-first-message
 *
 *   Format : "n,,n=<user>,r=<client-nonce>"
 *   Le préfixe "n,," est le GS2 header (channel binding non-supporté).
 *   La portion "n=<user>,r=<nonce>" est appelée "client-first-bare".
 * ================================================================ */

bool owl_scram_client_first(OwlScramSha256* s,
                             const char* client_nonce_override,
                             char* out_msg, size_t out_msg_size) {
    if (!s->username[0]) { scram_err(s, "username vide"); return false; }

    if (client_nonce_override) {
        snprintf(s->client_nonce, sizeof(s->client_nonce), "%s",
                 client_nonce_override);
    } else {
        /* 24 chars ≈ 144 bits entropie (selon pool ASCII printable). */
        if (!gen_nonce(s->client_nonce, 24)) {
            scram_err(s, "RNG echec");
            return false;
        }
    }

    int n = snprintf(out_msg, out_msg_size,
                     "n,,n=%s,r=%s", s->username, s->client_nonce);
    if (n < 0 || (size_t)n >= out_msg_size) {
        scram_err(s, "buffer trop petit pour client-first");
        return false;
    }

    /* Mémorise client-first-bare (sans "n,," GS2 prefix) pour
     * AuthMessage ultérieur. */
    int bn = snprintf(s->auth_message, sizeof(s->auth_message),
                      "n=%s,r=%s", s->username, s->client_nonce);
    if (bn < 0 || (size_t)bn >= sizeof(s->auth_message)) {
        scram_err(s, "AuthMessage overflow (client-first-bare)");
        return false;
    }
    s->auth_message_len = (size_t)bn;
    return true;
}

/* ================================================================
 * Étape 2 : client-final-message
 *
 *   Parse server-first : "r=<nonce>,s=<salt-b64>,i=<iter>"
 *   Construit client-final-without-proof : "c=biws,r=<nonce>"
 *     - "c=biws" = base64("n,,") = GS2 header binding (sans cb)
 *   Calcule ClientProof + ServerSignature
 *   Construit client-final-message : "c=biws,r=<nonce>,p=<proof-b64>"
 * ================================================================ */

bool owl_scram_client_final(OwlScramSha256* s,
                             const char* server_first_msg,
                             char* out_msg, size_t out_msg_size) {
    /* Parse server-first. Champs séparés par ','. Ordre : r=, s=, i=. */
    const char* combined_nonce = NULL; size_t nonce_len = 0;
    const char* salt_b64 = NULL;       size_t salt_b64_len = 0;
    int iter = 0;

    const char* p = server_first_msg;
    while (*p) {
        const char* eq = strchr(p, '=');
        if (!eq) { scram_err(s, "server-first malforme: pas de '='"); return false; }
        const char* end = strchr(eq + 1, ',');
        if (!end) end = eq + 1 + strlen(eq + 1);

        if (p[0] == 'r' && eq == p + 1) {
            combined_nonce = eq + 1;
            nonce_len = (size_t)(end - (eq + 1));
        } else if (p[0] == 's' && eq == p + 1) {
            salt_b64 = eq + 1;
            salt_b64_len = (size_t)(end - (eq + 1));
        } else if (p[0] == 'i' && eq == p + 1) {
            char buf[16] = {0};
            size_t cp = (size_t)(end - (eq + 1));
            if (cp >= sizeof(buf)) cp = sizeof(buf) - 1;
            memcpy(buf, eq + 1, cp);
            iter = atoi(buf);
        }
        /* m=, e= ignored for V1 (mandatory extensions / errors handled
         * separately). */
        if (*end == ',') p = end + 1; else break;
    }

    if (!combined_nonce || !salt_b64 || iter <= 0) {
        scram_err(s, "server-first incomplet (r/s/i)");
        return false;
    }
    /* Validation anti-replay : combined-nonce DOIT commencer par notre
     * client_nonce. Sinon le serveur tente de rejouer une autre session. */
    size_t cn_len = strlen(s->client_nonce);
    if (nonce_len < cn_len || memcmp(combined_nonce, s->client_nonce, cn_len) != 0) {
        scram_err(s, "server combined-nonce ne commence pas par notre client-nonce");
        return false;
    }

    /* Decode salt. */
    uint8_t salt[64];
    size_t salt_len = 0;
    if (b64_decode(salt_b64, salt_b64_len, salt, sizeof(salt), &salt_len) != 0) {
        scram_err(s, "salt base64 invalide");
        return false;
    }

    /* SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, iter, 32). */
    uint8_t salted_pwd[SHA256_LEN];
    if (pbkdf2_sha256((const uint8_t*)s->password, strlen(s->password),
                       salt, salt_len, iter, salted_pwd) != 0) {
        scram_err(s, "pbkdf2 echec");
        return false;
    }

    /* ClientKey = HMAC(SaltedPassword, "Client Key"). */
    uint8_t client_key[SHA256_LEN];
    if (hmac_sha256(salted_pwd, SHA256_LEN,
                     (const uint8_t*)"Client Key", 10,
                     client_key) != 0) {
        scram_err(s, "hmac client_key echec");
        return false;
    }

    /* StoredKey = SHA-256(ClientKey). */
    uint8_t stored_key[SHA256_LEN];
    if (sha256_buf(client_key, SHA256_LEN, stored_key) != 0) {
        scram_err(s, "sha256 stored_key echec");
        return false;
    }

    /* client-final-without-proof = "c=biws,r=<combined-nonce>". */
    char cf_wo_proof[256];
    int cfn = snprintf(cf_wo_proof, sizeof(cf_wo_proof),
                       "c=biws,r=%.*s", (int)nonce_len, combined_nonce);
    if (cfn < 0 || (size_t)cfn >= sizeof(cf_wo_proof)) {
        scram_err(s, "client-final-without-proof overflow");
        return false;
    }

    /* AuthMessage = client-first-bare + "," + server-first + "," + cf_wo_proof. */
    int total = snprintf(s->auth_message + s->auth_message_len,
                          sizeof(s->auth_message) - s->auth_message_len,
                          ",%s,%s", server_first_msg, cf_wo_proof);
    if (total < 0 ||
        s->auth_message_len + (size_t)total >= sizeof(s->auth_message)) {
        scram_err(s, "AuthMessage overflow");
        return false;
    }
    s->auth_message_len += (size_t)total;

    /* ClientSignature = HMAC(StoredKey, AuthMessage). */
    uint8_t client_sig[SHA256_LEN];
    if (hmac_sha256(stored_key, SHA256_LEN,
                     (const uint8_t*)s->auth_message, s->auth_message_len,
                     client_sig) != 0) {
        scram_err(s, "hmac client_sig echec");
        return false;
    }

    /* ClientProof = ClientKey XOR ClientSignature. */
    uint8_t client_proof[SHA256_LEN];
    for (int i = 0; i < SHA256_LEN; i++) {
        client_proof[i] = client_key[i] ^ client_sig[i];
    }

    char proof_b64[64];
    size_t proof_b64_len = 0;
    if (b64_encode(client_proof, SHA256_LEN,
                    proof_b64, sizeof(proof_b64), &proof_b64_len) != 0) {
        scram_err(s, "b64_encode proof echec");
        return false;
    }

    /* Construction client-final-message. */
    int fn = snprintf(out_msg, out_msg_size, "%s,p=%s",
                      cf_wo_proof, proof_b64);
    if (fn < 0 || (size_t)fn >= out_msg_size) {
        scram_err(s, "buffer trop petit pour client-final");
        return false;
    }

    /* Pré-calcule ServerSignature attendue = HMAC(ServerKey, AuthMessage).
     * ServerKey = HMAC(SaltedPassword, "Server Key"). */
    uint8_t server_key[SHA256_LEN];
    if (hmac_sha256(salted_pwd, SHA256_LEN,
                     (const uint8_t*)"Server Key", 10,
                     server_key) != 0) {
        scram_err(s, "hmac server_key echec");
        return false;
    }
    if (hmac_sha256(server_key, SHA256_LEN,
                     (const uint8_t*)s->auth_message, s->auth_message_len,
                     s->expected_server_sig) != 0) {
        scram_err(s, "hmac server_sig echec");
        return false;
    }
    s->has_expected_server_sig = true;
    return true;
}

/* ================================================================
 * Étape 3 : vérifie server-final-message
 *
 *   Format : "v=<server-signature-b64>"
 *
 * Comparaison en temps constant pour résister aux timing attacks.
 * ================================================================ */

static bool ct_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    /* Constant-time : OR cumulatif sur XOR, le compilateur ne peut
     * pas court-circuiter. */
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool owl_scram_verify_server_final(OwlScramSha256* s,
                                    const char* server_final_msg) {
    if (!s->has_expected_server_sig) {
        scram_err(s, "verify_server_final appele avant client_final");
        return false;
    }
    if (!server_final_msg || server_final_msg[0] != 'v' || server_final_msg[1] != '=') {
        scram_err(s, "server-final attendu: 'v=...'");
        return false;
    }
    const char* sig_b64 = server_final_msg + 2;
    size_t sig_b64_len = strlen(sig_b64);
    /* Le serveur peut ajouter ",extensions" — on coupe au premier ','. */
    const char* comma = strchr(sig_b64, ',');
    if (comma) sig_b64_len = (size_t)(comma - sig_b64);

    uint8_t got[SHA256_LEN];
    size_t got_len = 0;
    if (b64_decode(sig_b64, sig_b64_len, got, sizeof(got), &got_len) != 0
        || got_len != SHA256_LEN) {
        scram_err(s, "server-signature base64 invalide");
        return false;
    }

    if (!ct_equal(got, s->expected_server_sig, SHA256_LEN)) {
        scram_err(s, "server-signature ne matche pas (auth invalide ou MITM)");
        return false;
    }
    return true;
}

const char* owl_scram_last_error(const OwlScramSha256* s) {
    return s->last_error;
}
