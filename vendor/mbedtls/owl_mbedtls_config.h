/*
 * OWL mbedTLS configuration.
 * Enables:
 *   - Hashes: MD5, SHA1, SHA256, SHA384, SHA512
 *   - HMAC + PBKDF2
 *   - AES (CBC, GCM)
 *   - ChaCha20-Poly1305 AEAD (common in modern TLS)
 *   - RSA + ECC (for TLS certificate / key exchange)
 *   - X.509 certificate parsing
 *   - TLS 1.2 client
 *   - Entropy + CTR_DRBG (CSPRNG)
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ---- System ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_FS_IO            /* enables *_parse_file helpers for mTLS */

/* ---- Hashes ---- */
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C

/* ---- Symmetric ciphers ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_PADDING_PKCS7

/* ---- Asymmetric (for TLS) ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* ---- Elliptic curves enabled ---- */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* ---- X.509 ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C

/* ---- RNG ---- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C
/* Default entropy sources: CryptoAPI on Windows, /dev/urandom on Linux */

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* ---- Key exchange methods ---- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* ---- Key derivation ---- */
#define MBEDTLS_PKCS5_C
#define MBEDTLS_HKDF_C

/* ---- Encoding ---- */
#define MBEDTLS_BASE64_C

/* ---- Platform / infrastructure ---- */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ERROR_C

/* Note: do NOT include check_config.h here -- it is included automatically
 * by mbedtls/build_info.h after the config is finalized. */

#endif /* MBEDTLS_CONFIG_H */
