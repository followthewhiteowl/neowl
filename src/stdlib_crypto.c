// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL crypto stdlib: hashes, HMAC, AES symmetric encryption.
 * Backed by mbedTLS (vendored in vendor/mbedtls/).
 *
 * Functions:
 *   HashChaine(str, [algo])                 -> hex string
 *   HashFichier(path, [algo])               -> hex string
 *   HMACChaine(msg, key, [algo])            -> hex string
 *   Crypte(plaintext, password)             -> base64 ciphertext (legacy, AES-256-CBC)
 *   Decrypte(ciphertext, password)          -> plaintext (legacy)
 *   CrypteStandard(msg, key, [algo,mode,pad]) -> buffer (raw bytes)
 *   DecrypteStandard(buf, key, [algo,mode,pad]) -> plaintext
 *
 * Default hash algo: SHA256. Supported: MD5, SHA1, SHA256, SHA384, SHA512.
 *
 * Crypte/Decrypte format (LEGACY — password-based, base64, self-describing):
 *   salt = 16 random bytes
 *   key  = PBKDF2-HMAC-SHA256(password, salt, 100000 iter, 32 bytes)
 *   IV   = 16 random bytes
 *   out  = base64("OWL1" || salt[16] || IV[16] || AES-256-CBC-PKCS7(key, IV, plaintext))
 *   Magic "OWL1" marks v1 format; format upgrades will change the tag.
 *
 * CrypteStandard/DecrypteStandard format (raw bytes, NO magic/salt/base64):
 *   key = SHA-256(keyData)  (32 bytes; truncated to algo key size for AES-128/192)
 *   CBC (default): out = IV[16] || AES-CBC-PKCS7(key, IV, plaintext)
 *   GCM          : out = IV[12] || tag[16] || AES-GCM(key, IV, plaintext)
 *   Algo/mode/padding selectable via optional crypte* enum args.
 */

#define OWL_CRYPTE_MAGIC   "OWL1"
#define OWL_CRYPTE_MAGIC_LEN 4
#define OWL_CRYPTE_PBKDF2_ITER 100000
#define OWL_CRYPTE_SALT_LEN 16
#define OWL_CRYPTE_IV_LEN   16

#include "owl_stdlib.h"
/* Guard requise : ce module include windows.h plus bas et winnt.h definit
 * un TokenType qui colliderait avec scanner.h tire par owl_errors.h via
 * son API compile-aware. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "owl_registry.h"
#include "memory.h"
#include "sentinels.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include "mbedtls/pkcs5.h"

#include "sha3.h"    /* SHA-3 maison (FIPS 202) - mbedTLS ne le supporte pas */
#include "ripemd.h"  /* RIPEMD-128/160 maison */
#include "blake2.h"  /* BLAKE2b maison (RFC 7693) - extension OWL */

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif
#endif

/* ================================================================
 * HELPERS
 * ================================================================ */

static int hex_encode(const unsigned char* in, int len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    return len * 2;
}

/* Case-insensitive algo name -> mbedtls md type.
 * Accepts "MD5", "SHA1", "SHA-1", "SHA256", "SHA-256", etc. */
static mbedtls_md_type_t parse_md_type(const char* name, int len) {
    char buf[16];
    if (len <= 0 || len >= (int)sizeof(buf)) return MBEDTLS_MD_NONE;
    for (int i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)name[i]);
    buf[len] = '\0';
    if (strcmp(buf, "MD5") == 0)                                   return MBEDTLS_MD_MD5;
    if (strcmp(buf, "SHA1") == 0   || strcmp(buf, "SHA-1") == 0)   return MBEDTLS_MD_SHA1;
    if (strcmp(buf, "SHA256") == 0 || strcmp(buf, "SHA-256") == 0) return MBEDTLS_MD_SHA256;
    if (strcmp(buf, "SHA384") == 0 || strcmp(buf, "SHA-384") == 0) return MBEDTLS_MD_SHA384;
    if (strcmp(buf, "SHA512") == 0 || strcmp(buf, "SHA-512") == 0) return MBEDTLS_MD_SHA512;
    return MBEDTLS_MD_NONE;
}

/* Famille d'algos cote impl */
typedef enum {
    HASH_FAMILY_MBEDTLS,    /* MD5/SHA1/SHA-256/384/512 + HMAC -- via mbedTLS */
    HASH_FAMILY_SHA3,       /* SHA3-224/256/384/512 -- via sha3.c maison */
    HASH_FAMILY_RIPEMD,     /* RIPEMD-128/160 -- via ripemd.c maison */
    HASH_FAMILY_BLAKE2B,    /* BLAKE2b 256/384/512 -- via blake2.c maison */
} HashFamily;

/* Resultat de la resolution d'une constante HA_* (ou string algo). */
typedef struct {
    HashFamily family;
    mbedtls_md_type_t md;        /* utilise si family==HASH_FAMILY_MBEDTLS */
    int sha3Bits;                /* utilise si family==HASH_FAMILY_SHA3 (224/256/384/512) */
    int ripemdBits;              /* utilise si family==HASH_FAMILY_RIPEMD (128 ou 160) */
    int blake2bBytes;            /* utilise si family==HASH_FAMILY_BLAKE2B (32/48/64) */
    bool isHmac;                 /* HMAC variante ? */
    bool isDouble;               /* SHA-256-DOUBLE (Bitcoin-style) ? */
} HashAlgoSpec;

/* Statut de la tentative de resolution de l'argument 0 comme algo. */
typedef enum {
    HA_OK,                 /* algo valide */
    HA_INT_OUT_OF_RANGE,   /* entier mais pas dans la zone HA_* (= invalide) */
    HA_STRING_NOT_ALGO,    /* string mais pas un nom d'algo connu (probablement data) */
    HA_NOT_INT_NOR_STRING, /* autre type (buffer/array/etc) */
} HashAlgoLookup;

/* Resout v en HashAlgoSpec et retourne un code statut. Aucun side effect
 * sur le soft-error global -- le caller decide selon le contexte. */
static HashAlgoLookup resolve_hash_algo_v2(Value v, HashAlgoSpec* out) {
    out->family = HASH_FAMILY_MBEDTLS;
    out->md = MBEDTLS_MD_NONE;
    out->sha3Bits = 0;
    out->ripemdBits = 0;
    out->blake2bBytes = 0;
    out->isHmac = false;
    out->isDouble = false;

    if (IS_INTEGER(v)) {
        int64_t code = AS_INTEGER(v);
        switch (code) {
            /* mbedTLS family */
            case OWL_HA_MD5_128:           out->md = MBEDTLS_MD_MD5;    return HA_OK;
            case OWL_HA_SHA_160:           out->md = MBEDTLS_MD_SHA1;   return HA_OK;
            case OWL_HA_SHA_256:           out->md = MBEDTLS_MD_SHA256; return HA_OK;
            case OWL_HA_SHA_256_DOUBLE:    out->md = MBEDTLS_MD_SHA256; out->isDouble = true; return HA_OK;
            case OWL_HA_SHA_384:           out->md = MBEDTLS_MD_SHA384; return HA_OK;
            case OWL_HA_SHA_512:           out->md = MBEDTLS_MD_SHA512; return HA_OK;
            case OWL_HA_HMAC_MD5_128:      out->md = MBEDTLS_MD_MD5;    out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA_160:      out->md = MBEDTLS_MD_SHA1;   out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA_256:      out->md = MBEDTLS_MD_SHA256; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA_256_DOUBLE: out->md = MBEDTLS_MD_SHA256; out->isHmac = true; out->isDouble = true; return HA_OK;
            case OWL_HA_HMAC_SHA_384:      out->md = MBEDTLS_MD_SHA384; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA_512:      out->md = MBEDTLS_MD_SHA512; out->isHmac = true; return HA_OK;

            /* SHA-3 family (FIPS 202, via sha3.c maison) */
            case OWL_HA_SHA3_224:          out->family = HASH_FAMILY_SHA3; out->sha3Bits = 224; return HA_OK;
            case OWL_HA_SHA3_256:          out->family = HASH_FAMILY_SHA3; out->sha3Bits = 256; return HA_OK;
            case OWL_HA_SHA3_384:          out->family = HASH_FAMILY_SHA3; out->sha3Bits = 384; return HA_OK;
            case OWL_HA_SHA3_512:          out->family = HASH_FAMILY_SHA3; out->sha3Bits = 512; return HA_OK;
            case OWL_HA_HMAC_SHA3_224:     out->family = HASH_FAMILY_SHA3; out->sha3Bits = 224; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA3_256:     out->family = HASH_FAMILY_SHA3; out->sha3Bits = 256; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA3_384:     out->family = HASH_FAMILY_SHA3; out->sha3Bits = 384; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_SHA3_512:     out->family = HASH_FAMILY_SHA3; out->sha3Bits = 512; out->isHmac = true; return HA_OK;

            /* RIPEMD family (via ripemd.c maison) */
            case OWL_HA_RIPEMD_128:        out->family = HASH_FAMILY_RIPEMD; out->ripemdBits = 128; return HA_OK;
            case OWL_HA_RIPEMD_160:        out->family = HASH_FAMILY_RIPEMD; out->ripemdBits = 160; return HA_OK;
            case OWL_HA_HMAC_RIPEMD_128:   out->family = HASH_FAMILY_RIPEMD; out->ripemdBits = 128; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_RIPEMD_160:   out->family = HASH_FAMILY_RIPEMD; out->ripemdBits = 160; out->isHmac = true; return HA_OK;

            /* BLAKE2b family (via blake2.c maison, extension OWL hors doc) */
            case OWL_HA_BLAKE2B_256:       out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 32; return HA_OK;
            case OWL_HA_BLAKE2B_384:       out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 48; return HA_OK;
            case OWL_HA_BLAKE2B_512:       out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 64; return HA_OK;
            case OWL_HA_HMAC_BLAKE2B_256:  out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 32; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_BLAKE2B_384:  out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 48; out->isHmac = true; return HA_OK;
            case OWL_HA_HMAC_BLAKE2B_512:  out->family = HASH_FAMILY_BLAKE2B; out->blake2bBytes = 64; out->isHmac = true; return HA_OK;

            default: return HA_INT_OUT_OF_RANGE;
        }
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        out->md = parse_md_type(s->chars, s->length);
        return (out->md != MBEDTLS_MD_NONE) ? HA_OK : HA_STRING_NOT_ALGO;
    }
    return HA_NOT_INT_NOR_STRING;
}

/* Wrapper bool legacy pour les call sites simples. */
static bool resolve_hash_algo(Value v, HashAlgoSpec* out) {
    HashAlgoLookup r = resolve_hash_algo_v2(v, out);
    return r == HA_OK;
}

/* OS-backed CSPRNG. Returns 0 on success.
 *
 * Exposé publiquement (prototype dans owl_stdlib.h) car aussi consommé par
 * stdlib_uuid.c — la source de randomness cryptographique est transverse
 * (UUID v4 RFC 4122, IV AES, salt PBKDF2, etc.). Préfixe `owl_` pour éviter
 * collision avec un symbole système éventuel. */
int owl_random_bytes(unsigned char* buf, size_t len) {
#ifdef _WIN32
    NTSTATUS s = BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (s == 0) ? 0 : -1;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
#endif
}

/* ================================================================
 * HASH
 * ================================================================ */

/* HashChaine(algo, str) -> hex string. Ordre canon.
 *   - algo = constante HA_* (sentinelle entiere) ou string ("MD5","SHA256"...).
 *   - Forme 1-arg HashChaine(str) : defaut SHA-256.
 *   - Forme HMAC (HA_HMAC_*) exige une cle 3e arg.
 *   - Forme legacy (str, algo_string) detectee et rejetee avec message clair. */
static Value hash_chaine_impl(int argCount, Value* args, const char* nativeName) {
    HashAlgoSpec spec = {0};
    int dataIdx;

    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, nativeName,
                              "arg", "algo",
                              NULL);
        return NIL_VAL;
    }

    HashAlgoLookup lookup = resolve_hash_algo_v2(args[0], &spec);
    switch (lookup) {
        case HA_OK:
            dataIdx = 1;
            if (argCount < 2) {
                owl_emit_native_error(E_NATIVE_ARG_MISSING, nativeName,
                                      "arg", "chaine",
                                      NULL);
                return NIL_VAL;
            }
            break;
        case HA_INT_OUT_OF_RANGE:
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, nativeName,
                                  "arg", "algo",
                                  NULL);
            return NIL_VAL;
        case HA_STRING_NOT_ALGO:
            if (argCount == 1) {
                /* Forme 1-arg : args[0] = data, defaut SHA-256 */
                spec.md = MBEDTLS_MD_SHA256;
                dataIdx = 0;
            } else {
                /* args[0] est une string non-algo + il y a un args[1].
                 * Probablement l'ancien ordre (str, algo). On rejette pour
                 * forcer la migration vers l'ordre canon. */
                owl_emit_native_error(E_NATIVE_ARG_INVALID, nativeName,
                                      NULL);
                return NIL_VAL;
            }
            break;
        case HA_NOT_INT_NOR_STRING:
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "algo",
                                  "expected", "constante HA_* ou chaine",
                                  "actual", "autre type",
                                  NULL);
            return NIL_VAL;
    }

    if (!IS_STRING(args[dataIdx])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "chaine",
                              "expected", "chaine",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    ObjString* s = AS_STRING(args[dataIdx]);

    /* HMAC requires une cle (arg suivant) */
    ObjString* key = NULL;
    if (spec.isHmac) {
        OWL_REQUIRE_ARG(nativeName, "cle (variante HMAC)", dataIdx + 1,
                        IS_STRING(args[dataIdx + 1]), "chaine", NIL_VAL);
        key = AS_STRING(args[dataIdx + 1]);
    }

    unsigned char hash[64];
    int hlen;

    if (spec.family == HASH_FAMILY_BLAKE2B) {
        hlen = spec.blake2bBytes;
        const int blockSize = 128;  /* BLAKE2b block = 128 bytes */
        if (spec.isHmac) {
            unsigned char kpad[128] = {0};
            if (key->length > blockSize) {
                owl_blake2b(spec.blake2bBytes, key->chars, (size_t)key->length, kpad);
            } else {
                memcpy(kpad, key->chars, (size_t)key->length);
            }
            unsigned char ikey[128], okey[128];
            for (int i = 0; i < blockSize; i++) {
                ikey[i] = kpad[i] ^ 0x36;
                okey[i] = kpad[i] ^ 0x5c;
            }
            unsigned char inner[64];
            OwlBlake2bCtx bctx;
            owl_blake2b_init(&bctx, spec.blake2bBytes);
            owl_blake2b_update(&bctx, ikey, blockSize);
            owl_blake2b_update(&bctx, s->chars, (size_t)s->length);
            owl_blake2b_final(&bctx, inner);
            owl_blake2b_init(&bctx, spec.blake2bBytes);
            owl_blake2b_update(&bctx, okey, blockSize);
            owl_blake2b_update(&bctx, inner, (size_t)hlen);
            owl_blake2b_final(&bctx, hash);
        } else {
            owl_blake2b(spec.blake2bBytes, s->chars, (size_t)s->length, hash);
        }
    } else if (spec.family == HASH_FAMILY_RIPEMD) {
        hlen = spec.ripemdBits / 8;
        const int blockSize = 64;  /* RIPEMD : 512-bit block */
        if (spec.isHmac) {
            /* HMAC-RIPEMD : pattern HMAC standard custom */
            unsigned char kpad[64] = {0};
            if (key->length > blockSize) {
                if (spec.ripemdBits == 160) {
                    owl_ripemd160(key->chars, (size_t)key->length, kpad);
                } else {
                    owl_ripemd128(key->chars, (size_t)key->length, kpad);
                }
            } else {
                memcpy(kpad, key->chars, (size_t)key->length);
            }
            unsigned char ikey[64], okey[64];
            for (int i = 0; i < blockSize; i++) {
                ikey[i] = kpad[i] ^ 0x36;
                okey[i] = kpad[i] ^ 0x5c;
            }
            unsigned char inner[20];
            if (spec.ripemdBits == 160) {
                OwlRipemd160Ctx ctx;
                owl_ripemd160_init(&ctx);
                owl_ripemd160_update(&ctx, ikey, blockSize);
                owl_ripemd160_update(&ctx, s->chars, (size_t)s->length);
                owl_ripemd160_final(&ctx, inner);
                owl_ripemd160_init(&ctx);
                owl_ripemd160_update(&ctx, okey, blockSize);
                owl_ripemd160_update(&ctx, inner, 20);
                owl_ripemd160_final(&ctx, hash);
            } else {
                OwlRipemd128Ctx ctx;
                owl_ripemd128_init(&ctx);
                owl_ripemd128_update(&ctx, ikey, blockSize);
                owl_ripemd128_update(&ctx, s->chars, (size_t)s->length);
                owl_ripemd128_final(&ctx, inner);
                owl_ripemd128_init(&ctx);
                owl_ripemd128_update(&ctx, okey, blockSize);
                owl_ripemd128_update(&ctx, inner, 16);
                owl_ripemd128_final(&ctx, hash);
            }
        } else {
            if (spec.ripemdBits == 160) {
                owl_ripemd160(s->chars, (size_t)s->length, hash);
            } else {
                owl_ripemd128(s->chars, (size_t)s->length, hash);
            }
        }
    } else if (spec.family == HASH_FAMILY_SHA3) {
        hlen = spec.sha3Bits / 8;
        if (spec.isHmac) {
            /* HMAC-SHA-3 : pattern HMAC standard custom (mbedTLS ne sait pas).
             * H((K' XOR opad) || H((K' XOR ipad) || msg))
             * K' = key paddee a la rate de SHA-3 (block size sponge). */
            int blockSize = (1600 - 2 * spec.sha3Bits) / 8;  /* rate en bytes */
            unsigned char kpad[200] = {0};
            if (key->length > blockSize) {
                /* K' = H(key) puis padding zeros */
                if (!owl_sha3(spec.sha3Bits, key->chars, (size_t)key->length, kpad)) return NIL_VAL;
            } else {
                memcpy(kpad, key->chars, (size_t)key->length);
            }
            unsigned char ikey[200], okey[200];
            for (int i = 0; i < blockSize; i++) {
                ikey[i] = kpad[i] ^ 0x36;
                okey[i] = kpad[i] ^ 0x5c;
            }
            unsigned char inner[64];
            OwlSha3Ctx ctx;
            owl_sha3_init(&ctx, spec.sha3Bits);
            owl_sha3_update(&ctx, ikey, (size_t)blockSize);
            owl_sha3_update(&ctx, s->chars, (size_t)s->length);
            owl_sha3_final(&ctx, inner);
            owl_sha3_init(&ctx, spec.sha3Bits);
            owl_sha3_update(&ctx, okey, (size_t)blockSize);
            owl_sha3_update(&ctx, inner, (size_t)hlen);
            owl_sha3_final(&ctx, hash);
        } else {
            if (!owl_sha3(spec.sha3Bits, s->chars, (size_t)s->length, hash)) return NIL_VAL;
        }
    } else {
        /* mbedTLS family (MD5, SHA-1, SHA-256, SHA-384, SHA-512) */
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(spec.md);
        if (info == NULL) return NIL_VAL;
        hlen = (int)mbedtls_md_get_size(info);

        if (spec.isHmac) {
            if (mbedtls_md_hmac(info,
                                (const unsigned char*)key->chars, (size_t)key->length,
                                (const unsigned char*)s->chars, (size_t)s->length,
                                hash) != 0) {
                return NIL_VAL;
            }
        } else {
            if (mbedtls_md(info, (const unsigned char*)s->chars, (size_t)s->length, hash) != 0) {
                return NIL_VAL;
            }
            /* SHA-256-DOUBLE : hasher une 2e fois */
            if (spec.isDouble) {
                unsigned char hash2[64];
                if (mbedtls_md(info, hash, (size_t)hlen, hash2) != 0) return NIL_VAL;
                memcpy(hash, hash2, (size_t)hlen);
            }
        }
    }

    char hex[129];
    int out = hex_encode(hash, hlen, hex);
    hex[out] = '\0';
    return OBJ_VAL(obj_copy_string(hex, out));
}

static Value fn_hash_chaine(int argCount, Value* args) {
    return hash_chaine_impl(argCount, args, "HashChaine");
}

/* HashFichier(algo, path) -> hex string (streaming, no full file load).
 * Ordre canon. SHA-256-DOUBLE non supporte ici (single-pass streaming). */
static Value hash_fichier_impl(int argCount, Value* args, const char* nativeName) {
    HashAlgoSpec spec = {0};
    int pathIdx;

    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, nativeName,
                              "arg", "algo",
                              NULL);
        return NIL_VAL;
    }

    HashAlgoLookup lookup = resolve_hash_algo_v2(args[0], &spec);
    switch (lookup) {
        case HA_OK:
            if (spec.isHmac) {
                owl_emit_native_error(E_NATIVE_ARG_INVALID, nativeName,
                                      NULL);
                return NIL_VAL;
            }
            if (spec.isDouble) {
                owl_emit_native_error(E_NATIVE_CRYPTO_UNSUPPORTED_ALGO, nativeName,
                                      "algo", "SHA-256-DOUBLE",
                                      NULL);
                return NIL_VAL;
            }
            pathIdx = 1;
            if (argCount < 2) {
                owl_emit_native_error(E_NATIVE_ARG_MISSING, nativeName,
                                      "arg", "chemin",
                                      NULL);
                return NIL_VAL;
            }
            break;
        case HA_INT_OUT_OF_RANGE:
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, nativeName,
                                  "arg", "algo",
                                  NULL);
            return NIL_VAL;
        case HA_STRING_NOT_ALGO:
            if (argCount == 1) {
                spec.md = MBEDTLS_MD_SHA256;
                pathIdx = 0;
            } else {
                owl_emit_native_error(E_NATIVE_ARG_INVALID, nativeName,
                                      NULL);
                return NIL_VAL;
            }
            break;
        case HA_NOT_INT_NOR_STRING:
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "algo",
                                  "expected", "constante HA_* ou chaine",
                                  "actual", "autre type",
                                  NULL);
            return NIL_VAL;
    }

    if (!IS_STRING(args[pathIdx])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "chemin",
                              "expected", "chaine",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    ObjString* path = AS_STRING(args[pathIdx]);

    FILE* f = fopen(path->chars, "rb");
    if (!f) {
        owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, nativeName,
                              "path", path->chars,
                              "detail", strerror(errno),
                              NULL);
        return NIL_VAL;
    }

    unsigned char hash[64];
    int hlen;

    if (spec.family == HASH_FAMILY_BLAKE2B) {
        OwlBlake2bCtx bctx;
        owl_blake2b_init(&bctx, spec.blake2bBytes);
        unsigned char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            owl_blake2b_update(&bctx, buf, n);
        }
        fclose(f);
        owl_blake2b_final(&bctx, hash);
        hlen = spec.blake2bBytes;
    } else if (spec.family == HASH_FAMILY_RIPEMD) {
        unsigned char buf[4096];
        size_t n;
        if (spec.ripemdBits == 160) {
            OwlRipemd160Ctx rctx;
            owl_ripemd160_init(&rctx);
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                owl_ripemd160_update(&rctx, buf, n);
            }
            fclose(f);
            owl_ripemd160_final(&rctx, hash);
        } else {
            OwlRipemd128Ctx rctx;
            owl_ripemd128_init(&rctx);
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                owl_ripemd128_update(&rctx, buf, n);
            }
            fclose(f);
            owl_ripemd128_final(&rctx, hash);
        }
        hlen = spec.ripemdBits / 8;
    } else if (spec.family == HASH_FAMILY_SHA3) {
        OwlSha3Ctx sctx;
        owl_sha3_init(&sctx, spec.sha3Bits);
        unsigned char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            owl_sha3_update(&sctx, buf, n);
        }
        fclose(f);
        owl_sha3_final(&sctx, hash);
        hlen = spec.sha3Bits / 8;
    } else {
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(spec.md);
        if (info == NULL) { fclose(f); return NIL_VAL; }
        hlen = (int)mbedtls_md_get_size(info);

        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        if (mbedtls_md_setup(&ctx, info, 0) != 0 || mbedtls_md_starts(&ctx) != 0) {
            mbedtls_md_free(&ctx);
            fclose(f);
            owl_emit_native_error(E_NATIVE_CRYPTO_INIT_FAILURE, nativeName,
                                  "detail", "echec mbedtls_md_setup/starts",
                                  NULL);
            return NIL_VAL;
        }
        unsigned char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (mbedtls_md_update(&ctx, buf, n) != 0) {
                mbedtls_md_free(&ctx);
                fclose(f);
                return NIL_VAL;
            }
        }
        fclose(f);
        mbedtls_md_finish(&ctx, hash);
        mbedtls_md_free(&ctx);
    }

    char hex[129];
    int out = hex_encode(hash, hlen, hex);
    hex[out] = '\0';
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(hex, out));
}

static Value fn_hash_fichier(int argCount, Value* args) {
    return hash_fichier_impl(argCount, args, "HashFichier");
}

/* ================================================================
 * HMAC
 * ================================================================ */

/* HMACChaine -- alias historique. Forwarde vers l implementation partagee du hachage de chaine, avec un
 * algo HMAC, qui dispatche correctement entre mbedTLS et SHA-3.
 *
 * Formes acceptees :
 *   HMACChaine(algo, msg, cle)        -- ordre canon (algo = HA_HMAC_*)
 *   HMACChaine(msg, cle)              -- legacy 2-args, defaut HA_HMAC_SHA_256 */
static Value fn_hmac_chaine(int argCount, Value* args) {
    HashAlgoSpec probe = {0};
    if (argCount >= 1 && resolve_hash_algo_v2(args[0], &probe) == HA_OK) {
        /* Forme canon (algo, msg, cle) : forward direct */
        return hash_chaine_impl(argCount, args, "HMACChaine");
    }
    /* Forme legacy (msg, cle) : injecter HA_HMAC_SHA_256 en tete */
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "HMACChaine",
                              "native", "HMACChaine",
                              "arg", "msg/cle (2 args minimum)",
                              NULL);
        return NIL_VAL;
    }
    Value newArgs[3] = { INTEGER_VAL(OWL_HA_HMAC_SHA_256), args[0], args[1] };
    return hash_chaine_impl(3, newArgs, "HMACChaine");
}

/* ================================================================
 * HASH VERIFY (doc 1000007112 / 1000007113)
 * ================================================================ */

/* Helper : compare deux strings hex de meme taille en temps constant
 * (resistance timing attacks pour verifications de hash). */
static bool hex_const_time_eq(const char* a, int alen, const char* b, int blen) {
    if (alen != blen) return false;
    unsigned char diff = 0;
    for (int i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* HashVerifieChaine(algo, str, hashAttendu) -> booleen.
 * Recalcule le hash de str et compare en temps constant avec hashAttendu.
 * hashAttendu est un hex string (sortie de HashChaine). */
static Value fn_hash_verifie_chaine(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "HashVerifieChaine",
                              "native", "HashVerifieChaine",
                              "arg", "algo/chaine/hashAttendu (3 args requis)",
                              NULL);
        return BOOL_VAL(false);
    }
    /* Recalcul via l implementation partagee (forme HMAC requiert 4-args : algo, msg, key, hashAttendu) */
    Value computed;
    HashAlgoSpec spec = {0};
    if (!resolve_hash_algo(args[0], &spec)) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "HashVerifieChaine",
                              "native", "HashVerifieChaine",
                              NULL);
        return BOOL_VAL(false);
    }
    if (spec.isHmac) {
        /* HMAC needs key : (algo, msg, key, hashAttendu) -> argCount == 4 */
        if (argCount < 4) {
            owl_emit_native_error(E_NATIVE_ARG_MISSING, "HashVerifieChaine",
                                  "native", "HashVerifieChaine",
                                  "arg", "cle (HMAC requiert algo, msg, cle, hashAttendu)",
                                  NULL);
            return BOOL_VAL(false);
        }
        Value hashArgs[3] = { args[0], args[1], args[2] };
        computed = hash_chaine_impl(3, hashArgs, "HashVerifieChaine");
    } else {
        Value hashArgs[2] = { args[0], args[1] };
        computed = hash_chaine_impl(2, hashArgs, "HashVerifieChaine");
    }
    if (!IS_STRING(computed)) return BOOL_VAL(false);
    ObjString* got = AS_STRING(computed);

    /* hashAttendu = dernier arg */
    Value attenduVal = args[spec.isHmac ? 3 : 2];
    if (!IS_STRING(attenduVal)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "HashVerifieChaine",
                              "native", "HashVerifieChaine",
                              "arg", "hashAttendu",
                              "expected", "chaine hex",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    ObjString* expected = AS_STRING(attenduVal);

    return BOOL_VAL(hex_const_time_eq(got->chars, got->length,
                                       expected->chars, expected->length));
}

/* HashVerifieFichier(algo, path, hashAttendu) -> booleen. Variante fichier. */
static Value fn_hash_verifie_fichier(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "HashVerifieFichier",
                              "native", "HashVerifieFichier",
                              "arg", "algo/chemin/hashAttendu (3 args requis)",
                              NULL);
        return BOOL_VAL(false);
    }
    Value hashArgs[2] = { args[0], args[1] };
    Value computed = hash_fichier_impl(2, hashArgs, "HashVerifieFichier");
    if (!IS_STRING(computed)) return BOOL_VAL(false);
    ObjString* got = AS_STRING(computed);

    if (!IS_STRING(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "HashVerifieFichier",
                              "native", "HashVerifieFichier",
                              "arg", "hashAttendu",
                              "expected", "chaine hex",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    ObjString* expected = AS_STRING(args[2]);

    return BOOL_VAL(hex_const_time_eq(got->chars, got->length,
                                       expected->chars, expected->length));
}


/* ================================================================
 * CrypteStandard / DécrypteStandard (canon)
 * Version moderne recommandée (Crypte/Décrypte deprecated depuis canon 22).
 *
 * OWL: AES-128/256 selon crypteAES128/crypteAES256, modes CBC/CFB/CTR/GCM,
 * remplissage PKCS7 ou zéro (CBC seul). Clé dérivée de la chaîne fournie via
 * SHA-256 (32 octets ; AES128 prend les 16 premiers). NON interopérable avec
 * canon/tiers (decision dev : interop non recherchée).
 *
 * Format de sortie :
 *   CBC/CFB/CTR : IV(16) || ciphertext
 *   GCM         : IV(12) || tag(16) || ciphertext
 *
 * Défaut (params omis) : AES-256-CBC + PKCS7, byte-identique à l'impl
 * historique → les données chiffrées par défaut restent déchiffrables.
 *
 * Divergences canon assumées (cf ADR) : ajout du mode GCM (hors panel canon),
 * dérivation de clé SHA-256, format de buffer propre OWL. crypteDES/crypte3DES
 * (algos) et crypteECB (mode) volontairement absents (obsolètes/non sûrs).
 * ================================================================ */

/* Selection runtime resolue depuis les args optionnels. */
typedef enum { CR_ALGO_AES128, CR_ALGO_AES256 } CrAlgo;
typedef enum { CR_MODE_CBC, CR_MODE_CFB, CR_MODE_CTR, CR_MODE_GCM } CrMode;
typedef enum { CR_PAD_PKCS, CR_PAD_ZERO } CrPad;

#define OWL_CRYPTE_GCM_IV_LEN  12
#define OWL_CRYPTE_GCM_TAG_LEN 16

/* Parse les args optionnels [algorithme, mode, remplissage] a partir de
 * l'index `start`. Retourne 0 si une valeur fournie est hors enum. */
static int crypte_parse_options(int argCount, Value* args, int start,
                                CrAlgo* algo, CrMode* mode, CrPad* pad) {
    *algo = CR_ALGO_AES256;
    *mode = CR_MODE_CBC;
    *pad  = CR_PAD_PKCS;
    if (argCount > start && !IS_NIL(args[start])) {
        if (!IS_INTEGER(args[start])) return 0;
        switch ((int32_t)AS_INTEGER(args[start])) {
            case OWL_CRYPTE_AES128: *algo = CR_ALGO_AES128; break;
            case OWL_CRYPTE_AES256: *algo = CR_ALGO_AES256; break;
            default: return 0;
        }
    }
    if (argCount > start + 1 && !IS_NIL(args[start + 1])) {
        if (!IS_INTEGER(args[start + 1])) return 0;
        switch ((int32_t)AS_INTEGER(args[start + 1])) {
            case OWL_CRYPTE_MODE_CBC: *mode = CR_MODE_CBC; break;
            case OWL_CRYPTE_MODE_CFB: *mode = CR_MODE_CFB; break;
            case OWL_CRYPTE_MODE_CTR: *mode = CR_MODE_CTR; break;
            case OWL_CRYPTE_MODE_GCM: *mode = CR_MODE_GCM; break;
            default: return 0;
        }
    }
    if (argCount > start + 2 && !IS_NIL(args[start + 2])) {
        if (!IS_INTEGER(args[start + 2])) return 0;
        switch ((int32_t)AS_INTEGER(args[start + 2])) {
            case OWL_CRYPTE_PAD_PKCS: *pad = CR_PAD_PKCS; break;
            case OWL_CRYPTE_PAD_ZERO: *pad = CR_PAD_ZERO; break;
            default: return 0;
        }
    }
    return 1;
}

static int crypte_key_bits(CrAlgo algo) { return algo == CR_ALGO_AES128 ? 128 : 256; }

#include "mbedtls/sha256.h"

/* Accept chaîne ou buffer comme source de bytes. */
static int crypte_args_bytes(Value v, const unsigned char** out, int* outLen) {
    if (IS_STRING(v)) {
        *out = (const unsigned char*)AS_STRING(v)->chars;
        *outLen = AS_STRING(v)->length;
        return 1;
    }
    if (IS_BUFFER(v)) {
        *out = AS_BUFFER(v)->bytes;
        *outLen = AS_BUFFER(v)->length;
        return 1;
    }
    return 0;
}

static void crypte_standard_derive_key(const unsigned char* keyData, int keyLen,
                                        unsigned char out_key[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, keyData, (size_t)keyLen);
    mbedtls_sha256_finish(&ctx, out_key);
    mbedtls_sha256_free(&ctx);
}

static Value fn_crypte_standard(int argCount, Value* args) {
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "CrypteStandard",
                              "native", "CrypteStandard",
                              "arg", "message/cle",
                              NULL);
        return NIL_VAL;
    }
    const unsigned char* msg_data; int msg_len;
    const unsigned char* key_data; int key_len;
    if (!crypte_args_bytes(args[0], &msg_data, &msg_len)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "CrypteStandard",
                              "native", "CrypteStandard",
                              "arg", "message",
                              "expected", "chaine ou buffer",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    if (!crypte_args_bytes(args[1], &key_data, &key_len) || key_len == 0) {
        owl_emit_native_error(E_NATIVE_CRYPTO_BAD_KEY, "CrypteStandard",
                              "native", "CrypteStandard",
                              "detail", "cle vide ou type invalide (chaine/buffer non vide attendu)",
                              NULL);
        return NIL_VAL;
    }

    CrAlgo algo; CrMode mode; CrPad pad;
    if (!crypte_parse_options(argCount, args, 2, &algo, &mode, &pad)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "CrypteStandard",
                              "native", "CrypteStandard",
                              "arg", "algorithme/mode/remplissage",
                              "expected", "constante crypte* valide",
                              "actual", "valeur hors enum",
                              NULL);
        return NIL_VAL;
    }

    unsigned char key[32];
    crypte_standard_derive_key(key_data, key_len, key);
    int key_bits = crypte_key_bits(algo);

    /* ---- GCM : IV(12) || tag(16) || ciphertext ---- */
    if (mode == CR_MODE_GCM) {
        unsigned char iv[OWL_CRYPTE_GCM_IV_LEN];
        if (owl_random_bytes(iv, OWL_CRYPTE_GCM_IV_LEN) != 0) return NIL_VAL;

        int total = OWL_CRYPTE_GCM_IV_LEN + OWL_CRYPTE_GCM_TAG_LEN + msg_len;
        unsigned char* out = malloc((size_t)total);
        if (!out) return NIL_VAL;
        memcpy(out, iv, OWL_CRYPTE_GCM_IV_LEN);
        unsigned char* tag_dst = out + OWL_CRYPTE_GCM_IV_LEN;
        unsigned char* ct_dst  = tag_dst + OWL_CRYPTE_GCM_TAG_LEN;

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, (unsigned)key_bits);
        if (rc == 0) {
            rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                           (size_t)msg_len,
                                           iv, OWL_CRYPTE_GCM_IV_LEN, NULL, 0,
                                           msg_data, ct_dst,
                                           OWL_CRYPTE_GCM_TAG_LEN, tag_dst);
        }
        mbedtls_gcm_free(&gcm);
        if (rc != 0) { free(out); return NIL_VAL; }

        ObjBuffer* buf = obj_new_buffer(out, total);
        free(out);
        owl_clear_error();
        return OBJ_VAL(buf);
    }

    /* ---- CBC / CFB / CTR : IV(16) || ciphertext ---- */
    unsigned char iv[16];
    if (owl_random_bytes(iv, 16) != 0) return NIL_VAL;

    /* Prepare le payload a chiffrer. Modes flux : longueur exacte du clair.
     * CBC : remplissage jusqu'au multiple de 16 (PKCS7 ou zeros). */
    int body_len;
    unsigned char* body;
    if (mode == CR_MODE_CBC) {
        int pad_n = 16 - (msg_len % 16);          /* PKCS7 : toujours 1..16 */
        if (pad == CR_PAD_ZERO) {
            /* Zero-padding : complete au multiple de 16, mais si deja aligne
             * et non vide, aucun octet ajoute. Cas vide → un bloc de zeros. */
            pad_n = (msg_len % 16 == 0) ? (msg_len == 0 ? 16 : 0) : (16 - (msg_len % 16));
        }
        body_len = msg_len + pad_n;
        body = malloc((size_t)body_len);
        if (!body) return NIL_VAL;
        memcpy(body, msg_data, (size_t)msg_len);
        if (pad == CR_PAD_ZERO)
            memset(body + msg_len, 0, (size_t)pad_n);
        else
            memset(body + msg_len, pad_n, (size_t)pad_n);
    } else {
        body_len = msg_len;
        body = malloc((size_t)(body_len > 0 ? body_len : 1));
        if (!body) return NIL_VAL;
        if (msg_len > 0) memcpy(body, msg_data, (size_t)msg_len);
    }

    unsigned char* ct = malloc((size_t)(body_len > 0 ? body_len : 1));
    if (!ct) { free(body); return NIL_VAL; }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, key, (unsigned)key_bits);
    if (rc == 0) {
        unsigned char iv_copy[16]; memcpy(iv_copy, iv, 16);
        if (mode == CR_MODE_CBC) {
            rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                       (size_t)body_len, iv_copy, body, ct);
        } else if (mode == CR_MODE_CFB) {
            size_t iv_off = 0;
            rc = mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_ENCRYPT,
                                          (size_t)body_len, &iv_off, iv_copy,
                                          body, ct);
        } else { /* CTR */
            size_t nc_off = 0;
            unsigned char stream_block[16];
            memset(stream_block, 0, sizeof(stream_block));
            rc = mbedtls_aes_crypt_ctr(&aes, (size_t)body_len, &nc_off, iv_copy,
                                       stream_block, body, ct);
        }
    }
    mbedtls_aes_free(&aes);
    free(body);
    if (rc != 0) { free(ct); return NIL_VAL; }

    int total = 16 + body_len;
    unsigned char* out = malloc((size_t)total);
    if (!out) { free(ct); return NIL_VAL; }
    memcpy(out, iv, 16);
    if (body_len > 0) memcpy(out + 16, ct, (size_t)body_len);
    free(ct);

    ObjBuffer* buf = obj_new_buffer(out, total);
    free(out);
    owl_clear_error();
    return OBJ_VAL(buf);
}

static Value fn_decrypte_standard(int argCount, Value* args) {
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "arg", "buffer/cle",
                              NULL);
        return NIL_VAL;
    }
    const unsigned char* in_data; int in_len;
    const unsigned char* key_data; int key_len;
    if (!crypte_args_bytes(args[0], &in_data, &in_len)) {
        owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "detail", "buffer invalide (chaine ou buffer attendu)",
                              NULL);
        return NIL_VAL;
    }
    if (!crypte_args_bytes(args[1], &key_data, &key_len) || key_len == 0) {
        owl_emit_native_error(E_NATIVE_CRYPTO_BAD_KEY, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "detail", "cle vide ou type invalide",
                              NULL);
        return NIL_VAL;
    }

    CrAlgo algo; CrMode mode; CrPad pad;
    if (!crypte_parse_options(argCount, args, 2, &algo, &mode, &pad)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "arg", "algorithme/mode/remplissage",
                              "expected", "constante crypte* valide",
                              "actual", "valeur hors enum",
                              NULL);
        return NIL_VAL;
    }

    unsigned char key[32];
    crypte_standard_derive_key(key_data, key_len, key);
    int key_bits = crypte_key_bits(algo);

    /* ---- GCM : IV(12) || tag(16) || ciphertext ---- */
    if (mode == CR_MODE_GCM) {
        int hdr = OWL_CRYPTE_GCM_IV_LEN + OWL_CRYPTE_GCM_TAG_LEN;
        if (in_len < hdr) {
            owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                                  "native", "DécrypteStandard",
                                  "detail", "buffer GCM trop court (28 octets min)",
                                  NULL);
            return NIL_VAL;
        }
        const unsigned char* iv_src  = in_data;
        const unsigned char* tag_src = in_data + OWL_CRYPTE_GCM_IV_LEN;
        const unsigned char* ct_src  = in_data + hdr;
        int ct_len = in_len - hdr;

        unsigned char* pt = malloc((size_t)(ct_len > 0 ? ct_len : 1));
        if (!pt) return NIL_VAL;

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, (unsigned)key_bits);
        if (rc == 0) {
            rc = mbedtls_gcm_auth_decrypt(&gcm, (size_t)ct_len,
                                          iv_src, OWL_CRYPTE_GCM_IV_LEN, NULL, 0,
                                          tag_src, OWL_CRYPTE_GCM_TAG_LEN,
                                          ct_src, pt);
        }
        mbedtls_gcm_free(&gcm);
        if (rc != 0) {
            free(pt);
            /* Auth tag invalide → buffer altéré ou mauvaise clé : rejet. */
            owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                                  "native", "DécrypteStandard",
                                  "detail", "authentification GCM echouee (buffer altere ou mauvaise cle)",
                                  NULL);
            return NIL_VAL;
        }
        ObjBuffer* buf = obj_new_buffer(pt, ct_len);
        free(pt);
        owl_clear_error();
        return OBJ_VAL(buf);
    }

    /* ---- CBC / CFB / CTR : IV(16) || ciphertext ---- */
    if (in_len < 16) {
        owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "detail", "buffer invalide (16 octets IV min)",
                              NULL);
        return NIL_VAL;
    }
    unsigned char iv[16]; memcpy(iv, in_data, 16);
    const unsigned char* ct_src = in_data + 16;
    int ct_len = in_len - 16;

    if (mode == CR_MODE_CBC && (ct_len <= 0 || ct_len % 16 != 0)) {
        owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                              "native", "DécrypteStandard",
                              "detail", "taille donnees invalide (multiple de 16 attendu)",
                              NULL);
        return NIL_VAL;
    }

    unsigned char* pt = malloc((size_t)(ct_len > 0 ? ct_len : 1));
    if (!pt) return NIL_VAL;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc;
    if (mode == CR_MODE_CBC) {
        rc = mbedtls_aes_setkey_dec(&aes, key, (unsigned)key_bits);
        if (rc == 0)
            rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                       (size_t)ct_len, iv, ct_src, pt);
    } else if (mode == CR_MODE_CFB) {
        /* CFB/CTR : la primitive de dechiffrement utilise la clé de chiffrement. */
        rc = mbedtls_aes_setkey_enc(&aes, key, (unsigned)key_bits);
        if (rc == 0) {
            size_t iv_off = 0;
            rc = mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_DECRYPT,
                                          (size_t)ct_len, &iv_off, iv,
                                          ct_src, pt);
        }
    } else { /* CTR */
        rc = mbedtls_aes_setkey_enc(&aes, key, (unsigned)key_bits);
        if (rc == 0) {
            size_t nc_off = 0;
            unsigned char stream_block[16];
            memset(stream_block, 0, sizeof(stream_block));
            rc = mbedtls_aes_crypt_ctr(&aes, (size_t)ct_len, &nc_off, iv,
                                       stream_block, ct_src, pt);
        }
    }
    mbedtls_aes_free(&aes);
    if (rc != 0) { free(pt); return NIL_VAL; }

    int plain_len = ct_len;
    if (mode == CR_MODE_CBC) {
        if (pad == CR_PAD_PKCS) {
            int pad_n = pt[ct_len - 1];
            if (pad_n < 1 || pad_n > 16 || pad_n > ct_len) {
                free(pt);
                owl_emit_native_error(E_NATIVE_CRYPTO_BAD_KEY, "DécrypteStandard",
                                      "native", "DécrypteStandard",
                                      "detail", "padding invalide (mauvaise cle ?)",
                                      NULL);
                return NIL_VAL;
            }
            for (int i = 0; i < pad_n; i++) {
                if (pt[ct_len - 1 - i] != pad_n) {
                    free(pt);
                    owl_emit_native_error(E_NATIVE_CRYPTO_BAD_DATA, "DécrypteStandard",
                                          "native", "DécrypteStandard",
                                          "detail", "padding corrompu",
                                          NULL);
                    return NIL_VAL;
                }
            }
            plain_len = ct_len - pad_n;
        } else {
            /* Zero-padding : retire les zéros de fin. Un clair finissant par
             * \0 verrait ces octets supprimés — limite assumée de ce mode. */
            while (plain_len > 0 && pt[plain_len - 1] == 0) plain_len--;
        }
    }

    ObjBuffer* buf = obj_new_buffer(pt, plain_len);
    free(pt);
    owl_clear_error();
    return OBJ_VAL(buf);
}

/* ================================================================
 * INIT
 * ================================================================ */

/* ================================================================
 * Registry declarations — Crypto family.
 * Canonical names: HashChaîne, HashFichier, Crypte, Décrypte.
 * OWL extensions: HMACChaîne (no canonical HMAC function exists).
 * ================================================================ */

/* Enum algoHachage : les constantes HA_* acceptees par les natives de
 * hachage. Un seul enum global (pas de split standard/HMAC) car le
 * resolveur runtime (resolve_hash_algo_v2) accepte indifferemment toute
 * la famille HA_* pour chaque native : la variante HMAC (besoin d'une cle)
 * est portee par la constante elle-meme (isHmac), pas par la native ni
 * par l'arite. Splitter en deux enums mentirait au LSP (un HA_HMAC_* est
 * valide sur HashChaine 3-args, un HA_* simple est valide sur HMACChaine).
 *
 * Sous-ensemble assume : MD4 / Tiger / Whirlpool retires (cf.
 * docs/ADR/). Les valeurs vivent en zone sentinel OWL_HA_BASE,
 * donc force_small_int reste faux (self-test rule 20). Les noms doivent
 * matcher exactement les canon de HA_CONSTS (self-test rule 5). */
static const char* const E_hashAlgo_values[] = {
    "HA_MD5_128", "HA_SHA_160", "HA_SHA_256", "HA_SHA_256_DOUBLE",
    "HA_SHA_384", "HA_SHA_512", "HA_RIPEMD_128", "HA_RIPEMD_160",
    "HA_SHA3_224", "HA_SHA3_256", "HA_SHA3_384", "HA_SHA3_512",
    "HA_HMAC_MD5_128", "HA_HMAC_SHA_160", "HA_HMAC_SHA_256",
    "HA_HMAC_SHA_256_DOUBLE", "HA_HMAC_SHA_384", "HA_HMAC_SHA_512",
    "HA_HMAC_RIPEMD_128", "HA_HMAC_RIPEMD_160", "HA_HMAC_SHA3_224",
    "HA_HMAC_SHA3_256", "HA_HMAC_SHA3_384", "HA_HMAC_SHA3_512",
    "HA_BLAKE2B_256", "HA_BLAKE2B_384", "HA_BLAKE2B_512",
    "HA_HMAC_BLAKE2B_256", "HA_HMAC_BLAKE2B_384", "HA_HMAC_BLAKE2B_512",
    NULL
};
static const OwlEnum E_hashAlgo = {
    .name="algoHachage", .nameAscii="algoHachage", .nameEN="hashAlgo",
    .category="Cryptographie",
    .descFR="Algorithme de hachage retenu par les natives Hash* : familles standard (MD5, SHA-1, SHA-2, SHA-3, RIPEMD, BLAKE2b) et leurs variantes à clé HMAC. Une variante HMAC exige une clé en argument supplémentaire.",
    .descEN="Hash algorithm selected by the Hash* natives: standard families (MD5, SHA-1, SHA-2, SHA-3, RIPEMD, BLAKE2b) and their keyed HMAC variants. An HMAC variant requires an extra key argument.",
    .values=E_hashAlgo_values, .combinable=false
};

/* Enums crypteAlgo / crypteMode / cryptePadding : selection passee aux natives
 * CrypteStandard / DecrypteStandard. Valeurs en zone sentinel OWL_CRYPTE_BASE
 * (force_small_int reste faux). Les noms de valeurs matchent les constantes
 * crypte* enregistrees plus bas (self-test rule 5). */
static const char* const E_crypteAlgo_values[] = {
    "crypteAES128", "crypteAES256", NULL
};
static const OwlEnum E_crypteAlgo = {
    .name="crypteAlgo", .nameAscii="crypteAlgo", .nameEN="encryptAlgo",
    .category="Cryptographie",
    .descFR="Algorithme de chiffrement retenu par CrypteStandard / DécrypteStandard : AES sur 128 ou 256 bits.",
    .descEN="Cipher algorithm selected by EncryptStandard / DecryptStandard: AES with a 128-bit or 256-bit key.",
    .values=E_crypteAlgo_values, .combinable=false, .force_small_int=false
};

static const char* const E_crypteMode_values[] = {
    "crypteCBC", "crypteCFB", "crypteCTR", "crypteGCM", NULL
};
static const OwlEnum E_crypteMode = {
    .name="crypteMode", .nameAscii="crypteMode", .nameEN="encryptMode",
    .category="Cryptographie",
    .descFR="Mode d'opération du chiffrement : CBC (par blocs), CFB et CTR (flux, sans remplissage) ou GCM (authentifié, détecte toute altération).",
    .descEN="Cipher operation mode: CBC (block), CFB and CTR (stream, no padding) or GCM (authenticated, detects any tampering).",
    .values=E_crypteMode_values, .combinable=false, .force_small_int=false
};

static const char* const E_cryptePadding_values[] = {
    "cryptePaddingPKCS", "cryptePaddingZ\xc3\xa9ro", NULL  /* cryptePaddingZéro */
};
static const OwlEnum E_cryptePadding = {
    .name="cryptePadding", .nameAscii="cryptePadding", .nameEN="encryptPadding",
    .category="Cryptographie",
    .descFR="Remplissage appliqué en mode CBC pour compléter le dernier bloc : PKCS7 ou remplissage par zéros. Sans effet sur les modes flux et GCM.",
    .descEN="Padding applied in CBC mode to fill the last block: PKCS7 or zero padding. No effect on stream and GCM modes.",
    .values=E_cryptePadding_values, .combinable=false, .force_small_int=false
};

static const OwlEnum* const CRYPTO_ENUMS[] = {
    &E_hashAlgo,
    &E_crypteAlgo,
    &E_crypteMode,
    &E_cryptePadding
};

static const OwlParam p_hash_str[]  = { {"algorithme", "entier", "constante HA_SHA_256, HA_MD5_128…", .allowedBase=&E_hashAlgo},
                                         {"chaîne",     "chaîne", "données à hacher"}, {0} };
static const OwlParam p_hash_file[] = { {"algorithme", "entier", "constante HA_SHA_256…", .allowedBase=&E_hashAlgo},
                                         {"chemin",     "chaîne", "fichier à hacher"}, {0} };
static const OwlParam p_hmac[]      = { {"algorithme", "entier", "constante HA_HMAC_SHA_256…", .allowedBase=&E_hashAlgo},
                                         {"chaîne",     "chaîne", "données"},
                                         {"clé",        "chaîne", "clé secrète"}, {0} };
static const OwlParam p_hverif[]    = { {"algorithme", "entier", "constante HA_SHA_256…", .allowedBase=&E_hashAlgo},
                                         {"chaîne",     "chaîne", "données à vérifier"},
                                         {"hashAttendu","chaîne", "hash hex de référence"}, {0} };
static const OwlParam p_hverif_f[]  = { {"algorithme", "entier", "constante HA_SHA_256…", .allowedBase=&E_hashAlgo},
                                         {"chemin",     "chaîne", "fichier à vérifier"},
                                         {"hashAttendu","chaîne", "hash hex de référence"}, {0} };
/* Le 4e arg canon <vecteur d'initialisation> n'est pas exposé : l'IV est tiré
 * aléatoirement à chaque appel et préfixé au buffer (cf. format de sortie). */
static const OwlParam p_crypte[]    = { {"message",      "chaîne", "texte clair (chaîne ou buffer)"},
                                         {"clé",          "chaîne", "clé (chaîne ou buffer non vide)"},
                                         {"algorithme",   "entier", "optionnel — algorithme de chiffrement", .allowedBase=&E_crypteAlgo},
                                         {"mode",         "entier", "optionnel — mode d'opération", .allowedBase=&E_crypteMode},
                                         {"remplissage",  "entier", "optionnel — type de remplissage", .allowedBase=&E_cryptePadding}, {0} };
static const OwlParam p_decrypte[]  = { {"messageChiffré","chaîne", "buffer produit par CrypteStandard"},
                                         {"clé",          "chaîne", "clé (chaîne ou buffer non vide)"},
                                         {"algorithme",   "entier", "optionnel — algorithme de chiffrement", .allowedBase=&E_crypteAlgo},
                                         {"mode",         "entier", "optionnel — mode d'opération", .allowedBase=&E_crypteMode},
                                         {"remplissage",  "entier", "optionnel — type de remplissage", .allowedBase=&E_cryptePadding}, {0} };

/*  — formes d'arité discrètes (ex-{-1} masqué). Bornes alignées sur
 * l'IMPL réelle (les formes PBKDF salt/itération/longueur et l'arg Jauge de la
 * doc ne sont pas implémentées, donc non exposées). */

/* HashChaîne : 1 arg (data, SHA-256 défaut), 2 args (algo, data),
 * 3 args (algo, data, clé) pour les variantes HMAC. */
static const OwlSignature sig_hash[]   = {
    {1, p_hash_str, "chaîne", "hash hexadécimal (SHA-256 par défaut)"},
    {2, p_hash_str, "chaîne", "hash hexadécimal"},
    {3, p_hash_str, "chaîne", "HMAC hexadécimal (variante avec clé)"},
    {0}
};
/* HashFichier : 1 arg (chemin, SHA-256 défaut), 2 args (algo, chemin).
 * Pas de variante HMAC fichier ni SHA-256-DOUBLE. */
static const OwlSignature sig_hf[]     = {
    {1, p_hash_file, "chaîne", "hash hexadécimal (SHA-256 par défaut)"},
    {2, p_hash_file, "chaîne", "hash hexadécimal"},
    {0}
};
/* HMACChaîne : 2 args (msg, clé → SHA-256 défaut), 3 args (algo, msg, clé). */
static const OwlSignature sig_hmac[]   = {
    {2, p_hmac, "chaîne", "HMAC hexadécimal (SHA-256 par défaut)"},
    {3, p_hmac, "chaîne", "HMAC hexadécimal"},
    {0}
};
/* HashVérifieChaîne : 3 args (algo, chaîne, hash), 4 args avec clé (HMAC). */
static const OwlSignature sig_hverif[] = {
    {3, p_hverif, "booléen", "Vrai si match"},
    {4, p_hverif, "booléen", "Vrai si match (variante HMAC avec clé)"},
    {0}
};
/* HashVérifieFichier : 3 args uniquement (algo, chemin, hash). */
static const OwlSignature sig_hverif_f[]= {
    {3, p_hverif_f, "booléen", "Vrai si match"},
    {0}
};
/* CrypteStandard : 2 args (message, clé) + 3 args optionnels (algo, mode,
 * remplissage), honorés au runtime. */
static const OwlSignature sig_crypte[] = {
    {2, p_crypte, "chaîne", "buffer chiffré (IV || ciphertext)"},
    {3, p_crypte, "chaîne", "buffer chiffré (IV || ciphertext)"},
    {4, p_crypte, "chaîne", "buffer chiffré (IV || ciphertext)"},
    {5, p_crypte, "chaîne", "buffer chiffré (IV || ciphertext)"},
    {0}
};
/* DécrypteStandard : 2 args (message chiffré, clé) + 3 args optionnels (algo,
 * mode, remplissage) — doivent correspondre à ceux du chiffrement. */
static const OwlSignature sig_decrypte[] = {
    {2, p_decrypte, "chaîne", "buffer déchiffré"},
    {3, p_decrypte, "chaîne", "buffer déchiffré"},
    {4, p_decrypte, "chaîne", "buffer déchiffré"},
    {5, p_decrypte, "chaîne", "buffer déchiffré"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical      = "HashCha\xc3\xae" "ne",  /* HashChaîne */
        .canonicalAscii = "HashChaine",
        .nameEN         = "HashString",
        .category       = "Cryptographie",
        .descFR         = "Calcule le hash d'une chaîne. Ordre : HashChaîne(algo, chaîne) où algo est une constante HA_* (SHA-256 par défaut). Variantes HMAC requièrent une 3e arg (clé).",
        .descEN         = "Compute the hash of a string. Order: HashString(algo, str) with HA_* constant (SHA-256 default). HMAC variants require a 3rd arg (key).",
        .signatures     = sig_hash,
        .impl           = fn_hash_chaine,
    },
    {
        .canonical  = "HashFichier",
        .nameEN     = "HashFile",
        .category   = "Cryptographie",
        .descFR     = "Calcule le hash du contenu d'un fichier (streaming). Ordre : HashFichier(algo, chemin).",
        .descEN     = "Compute the hash of a file (streaming). Canonical order: HashFile(algo, path).",
        .signatures = sig_hf,
        .impl       = fn_hash_fichier,
    },
    {
        .canonical      = "HashV\xc3\xa9" "rifieCha\xc3\xae" "ne",  /* HashVérifieChaîne */
        .canonicalAscii = "HashVerifieChaine",
        .nameEN         = "HashCheckString",
        .category       = "Cryptographie",
        .descFR         = "Vérifie qu'un hash correspond à une chaîne (comparaison en temps constant).",
        .descEN         = "Check that a hash matches a string (constant-time comparison).",
        .signatures     = sig_hverif,
        .impl           = fn_hash_verifie_chaine,
    },
    {
        .canonical      = "HashV\xc3\xa9" "rifieFichier",  /* HashVérifieFichier */
        .canonicalAscii = "HashVerifieFichier",
        .nameEN         = "HashCheckFile",
        .category       = "Cryptographie",
        .descFR         = "Vérifie qu'un hash correspond à un fichier (comparaison en temps constant).",
        .descEN         = "Check that a hash matches a file (constant-time comparison).",
        .signatures     = sig_hverif_f,
        .impl           = fn_hash_verifie_fichier,
    },
    {
        .canonical      = "HMACCha\xc3\xae" "ne",  /* HMACChaîne */
        .canonicalAscii = "HMACChaine",
        .nameEN         = "HMACString",
        .category       = "Cryptographie",
        .descFR         = "Calcule le HMAC d'une chaîne. Forme canonique : HMACChaîne(algo, msg, clé). Forme legacy : HMACChaîne(msg, clé) → SHA-256 par défaut.",
        .descEN         = "Compute the HMAC of a string. Order: HMACString(algo, msg, key). Legacy form: HMACString(msg, key) → SHA-256 default.",
        .signatures     = sig_hmac,
        .impl           = fn_hmac_chaine,
    },
    {
        .canonical  = "CrypteStandard",
        .nameEN     = "EncryptStandard",
        .category   = "Cryptographie",
        .descFR     = "Chiffre un message avec une clé dérivée par SHA-256. L'algorithme (crypteAES128/256), le mode (crypteCBC/CFB/CTR/GCM) et le remplissage sont optionnels ; par défaut AES-256-CBC avec PKCS7. Retourne un buffer (IV puis texte chiffré ; en GCM, IV puis empreinte d'authentification puis texte chiffré).",
        .descEN     = "Encrypts a message with a SHA-256 derived key. Algorithm (encryptAES128/256), mode (encryptCBC/CFB/CTR/GCM) and padding are optional; defaults to AES-256-CBC with PKCS7. Returns a buffer (IV then ciphertext; in GCM, IV then authentication tag then ciphertext).",
        .signatures = sig_crypte,
        .impl       = fn_crypte_standard,
    },
    {
        .canonical      = "D\xc3\xa9" "crypteStandard",   /* DécrypteStandard */
        .canonicalAscii = "DecrypteStandard",
        .nameEN         = "DecryptStandard",
        .category       = "Cryptographie",
        .descFR         = "Déchiffre un buffer produit par CrypteStandard, avec la même clé et les mêmes algorithme, mode et remplissage que ceux du chiffrement. En mode GCM, rejette tout buffer altéré. Retourne le texte clair.",
        .descEN         = "Decrypts a buffer produced by EncryptStandard, using the same key, algorithm, mode and padding as encryption. In GCM mode, rejects any tampered buffer. Returns the plaintext.",
        .signatures     = sig_decrypte,
        .impl           = fn_decrypte_standard,
    },
};

/* HA_* algorithm constants (doc 1000007111). Sentinelles entieres
 * centralisees dans sentinels.h (zone INT32_MIN+OWL_HA_BASE).
 *
 * Tous les algos enregistres ici sont supportes runtime :
 *   - mbedTLS : MD5, SHA-1, SHA-256, SHA-384, SHA-512 (+ HMAC, + SHA-256-DOUBLE) ;
 *   - SHA-3 (FIPS 202, sha3.c maison) ;
 *   - RIPEMD-128/160 (ripemd.c maison) ;
 *   - BLAKE2b (extension OWL hors doc, blake2.c maison).
 *
 * Les algos canon obsoletes/casses (MD4, Tiger 128/160/192, Whirlpool,
 * formes hash et HMAC) ont ete RETIRES du registry : divergence assumee
 * (cf. docs/ADR/-retrait-algos-hash-hmac-non-implementes.md). On ne
 * les expose plus pour ne pas offrir une constante qui planterait a l'appel.
 *
 * Categorie "Cryptographie" pour LSP/VSCode. Coloration constante.
 *
 * nameEN = nm : les HA_* sont des identifiants techniques universels
 * (MD5, SHA256, BLAKE2B…), EN==FR. L'alias EN canon = le nom lui-meme
 * (invariant bilingue n°3). Re-set runtime meme cle/valeur, inoffensif. */
#define C_HA(nm, val, fr, en) {nm, "Cryptographie", INTEGER_VAL(val), fr, en, nm}
static const OwlConstDef HA_CONSTS[] = {
    /* Standard hash supportes */
    C_HA("HA_MD5_128",           OWL_HA_MD5_128,
        "Hash MD5 128-bit (déprécié, ne plus utiliser pour la sécurité)",
        "MD5 128-bit hash (deprecated for security)"),
    C_HA("HA_SHA_160",           OWL_HA_SHA_160,
        "Hash SHA-1 160-bit (alias HA_SHA_1)",
        "SHA-1 160-bit hash (alias HA_SHA_1)"),
    C_HA("HA_SHA_256",           OWL_HA_SHA_256,
        "Hash SHA-256 (FIPS PUB 198)",
        "SHA-256 hash (FIPS PUB 198)"),
    C_HA("HA_SHA_256_DOUBLE",    OWL_HA_SHA_256_DOUBLE,
        "Hash SHA-256 applique deux fois (Bitcoin-style)",
        "Double SHA-256 (Bitcoin-style)"),
    C_HA("HA_SHA_384",           OWL_HA_SHA_384,
        "Hash SHA-384",
        "SHA-384 hash"),
    C_HA("HA_SHA_512",           OWL_HA_SHA_512,
        "Hash SHA-512",
        "SHA-512 hash"),
    /* RIPEMD + SHA-3 (impls maison hors mbedTLS) */
    C_HA("HA_RIPEMD_128",        OWL_HA_RIPEMD_128,
        "Hash RIPEMD-128 (4-rounds, output 16 octets)",
        "RIPEMD-128 hash (4 rounds, 16-byte output)"),
    C_HA("HA_RIPEMD_160",        OWL_HA_RIPEMD_160,
        "Hash RIPEMD-160 (5-rounds, output 20 octets, utilise par Bitcoin)",
        "RIPEMD-160 hash (5 rounds, 20-byte output, used by Bitcoin)"),
    C_HA("HA_SHA3_224",          OWL_HA_SHA3_224,
        "Hash SHA-3 224-bit (FIPS 202)",
        "SHA-3 224-bit (FIPS 202)"),
    C_HA("HA_SHA3_256",          OWL_HA_SHA3_256,
        "Hash SHA-3 256-bit (FIPS 202)",
        "SHA-3 256-bit (FIPS 202)"),
    C_HA("HA_SHA3_384",          OWL_HA_SHA3_384,
        "Hash SHA-3 384-bit (FIPS 202)",
        "SHA-3 384-bit (FIPS 202)"),
    C_HA("HA_SHA3_512",          OWL_HA_SHA3_512,
        "Hash SHA-3 512-bit (FIPS 202)",
        "SHA-3 512-bit (FIPS 202)"),
    /* HMAC variantes supportees */
    C_HA("HA_HMAC_MD5_128",      OWL_HA_HMAC_MD5_128,
        "HMAC-MD5 (déprécié pour la sécurité)", "HMAC-MD5 (deprecated)"),
    C_HA("HA_HMAC_SHA_160",      OWL_HA_HMAC_SHA_160,
        "HMAC-SHA-1", "HMAC-SHA-1"),
    C_HA("HA_HMAC_SHA_256",      OWL_HA_HMAC_SHA_256,
        "HMAC-SHA-256", "HMAC-SHA-256"),
    C_HA("HA_HMAC_SHA_256_DOUBLE", OWL_HA_HMAC_SHA_256_DOUBLE,
        "HMAC-SHA-256 applique deux fois", "Double HMAC-SHA-256"),
    C_HA("HA_HMAC_SHA_384",      OWL_HA_HMAC_SHA_384,
        "HMAC-SHA-384", "HMAC-SHA-384"),
    C_HA("HA_HMAC_SHA_512",      OWL_HA_HMAC_SHA_512,
        "HMAC-SHA-512", "HMAC-SHA-512"),
    C_HA("HA_HMAC_RIPEMD_128",   OWL_HA_HMAC_RIPEMD_128,
        "HMAC-RIPEMD-128", "HMAC-RIPEMD-128"),
    C_HA("HA_HMAC_RIPEMD_160",   OWL_HA_HMAC_RIPEMD_160,
        "HMAC-RIPEMD-160 (RFC 2286)", "HMAC-RIPEMD-160 (RFC 2286)"),
    C_HA("HA_HMAC_SHA3_224",     OWL_HA_HMAC_SHA3_224,
        "HMAC-SHA-3-224", "HMAC-SHA-3-224"),
    C_HA("HA_HMAC_SHA3_256",     OWL_HA_HMAC_SHA3_256,
        "HMAC-SHA-3-256", "HMAC-SHA-3-256"),
    C_HA("HA_HMAC_SHA3_384",     OWL_HA_HMAC_SHA3_384,
        "HMAC-SHA-3-384", "HMAC-SHA-3-384"),
    C_HA("HA_HMAC_SHA3_512",     OWL_HA_HMAC_SHA3_512,
        "HMAC-SHA-3-512", "HMAC-SHA-3-512"),
    /* BLAKE2 family (extension OWL hors doc, RFC 7693) */
    C_HA("HA_BLAKE2B_256",       OWL_HA_BLAKE2B_256,
        "Hash BLAKE2b 256-bit (RFC 7693, plus rapide que SHA-2/3 sur 64-bit)",
        "BLAKE2b 256-bit hash (RFC 7693, faster than SHA-2/3 on 64-bit)"),
    C_HA("HA_BLAKE2B_384",       OWL_HA_BLAKE2B_384,
        "Hash BLAKE2b 384-bit (RFC 7693)",
        "BLAKE2b 384-bit hash (RFC 7693)"),
    C_HA("HA_BLAKE2B_512",       OWL_HA_BLAKE2B_512,
        "Hash BLAKE2b 512-bit (RFC 7693, output canonical)",
        "BLAKE2b 512-bit hash (RFC 7693, canonical output)"),
    C_HA("HA_HMAC_BLAKE2B_256",  OWL_HA_HMAC_BLAKE2B_256,
        "HMAC-BLAKE2b-256", "HMAC-BLAKE2b-256"),
    C_HA("HA_HMAC_BLAKE2B_384",  OWL_HA_HMAC_BLAKE2B_384,
        "HMAC-BLAKE2b-384", "HMAC-BLAKE2b-384"),
    C_HA("HA_HMAC_BLAKE2B_512",  OWL_HA_HMAC_BLAKE2B_512,
        "HMAC-BLAKE2b-512", "HMAC-BLAKE2b-512"),
};
#undef C_HA

/* Constantes crypte* : selection algo/mode/remplissage de CrypteStandard /
 * DecrypteStandard. Sentinelles entieres (zone OWL_CRYPTE_BASE). nameEN
 * fourni (invariant bilingue n°3). cryptePaddingZéro : l'alias ASCII
 * cryptePaddingZero est auto-genere par register_consts. */
#define C_CR(nm, val, fr, en, nmEN) {nm, "Cryptographie", INTEGER_VAL(val), fr, en, nmEN}
static const OwlConstDef CRYPTE_CONSTS[] = {
    /* Algorithme */
    C_CR("crypteAES128", OWL_CRYPTE_AES128,
        "Chiffrement AES avec une clé de 128 bits.",
        "AES encryption with a 128-bit key.", "encryptAES128"),
    C_CR("crypteAES256", OWL_CRYPTE_AES256,
        "Chiffrement AES avec une clé de 256 bits.",
        "AES encryption with a 256-bit key.", "encryptAES256"),
    /* Mode */
    C_CR("crypteCBC", OWL_CRYPTE_MODE_CBC,
        "Mode CBC : chiffrement par blocs enchaînés, avec remplissage du dernier bloc.",
        "CBC mode: cipher-block chaining, with padding on the last block.", "encryptCBC"),
    C_CR("crypteCFB", OWL_CRYPTE_MODE_CFB,
        "Mode CFB : chiffrement en flux, sans remplissage.",
        "CFB mode: stream cipher, no padding.", "encryptCFB"),
    C_CR("crypteCTR", OWL_CRYPTE_MODE_CTR,
        "Mode CTR : chiffrement en flux par compteur, sans remplissage.",
        "CTR mode: counter-based stream cipher, no padding.", "encryptCTR"),
    C_CR("crypteGCM", OWL_CRYPTE_MODE_GCM,
        "Mode GCM : chiffrement authentifié ; toute altération du message chiffré est détectée au déchiffrement.",
        "GCM mode: authenticated encryption; any tampering with the ciphertext is detected on decryption.", "encryptGCM"),
    /* Remplissage */
    C_CR("cryptePaddingPKCS", OWL_CRYPTE_PAD_PKCS,
        "Remplissage PKCS7 du dernier bloc en mode CBC.",
        "PKCS7 padding of the last block in CBC mode.", "encryptPaddingPKCS"),
    C_CR("cryptePaddingZ\xc3\xa9ro", OWL_CRYPTE_PAD_ZERO,  /* cryptePaddingZéro */
        "Remplissage par zéros du dernier bloc en mode CBC.",
        "Zero padding of the last block in CBC mode.", "encryptPaddingZero"),
};
#undef C_CR

void stdlib_crypto_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums(CRYPTO_ENUMS,
        (int)(sizeof(CRYPTO_ENUMS)/sizeof(CRYPTO_ENUMS[0])));
    /* Constantes HA_* : register_consts auto-enregistre dans vm.globals. */
    owl_registry_register_consts(HA_CONSTS,
        (int)(sizeof(HA_CONSTS)/sizeof(HA_CONSTS[0])));
    /* Constantes crypte* : selection algo/mode/remplissage de CrypteStandard. */
    owl_registry_register_consts(CRYPTE_CONSTS,
        (int)(sizeof(CRYPTE_CONSTS)/sizeof(CRYPTE_CONSTS[0])));
}
