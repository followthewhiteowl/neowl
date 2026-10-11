// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "auth_mysql_caching.h"

#include <mbedtls/sha256.h>
#include <string.h>

#define SHA256_LEN 32

int owl_mysql_caching_compute_token(const char* password, size_t password_len,
                                     const uint8_t salt[MYSQL_CACHING_SALT_LEN],
                                     uint8_t out_token[MYSQL_CACHING_TOKEN_LEN]) {
    if (password == NULL || out_token == NULL || salt == NULL) return -1;

    uint8_t hash1[SHA256_LEN];   /* SHA256(password) */
    uint8_t hash2[SHA256_LEN];   /* SHA256(SHA256(password)) */
    uint8_t mix[SHA256_LEN + MYSQL_CACHING_SALT_LEN];
    uint8_t challenge[SHA256_LEN]; /* SHA256( SHA256(SHA256(password)) || salt ) */

    /* hash1 = SHA256(password). is224=0 pour SHA-256 (vs SHA-224). */
    if (mbedtls_sha256((const uint8_t*)password, password_len, hash1, 0) != 0) return -1;
    /* hash2 = SHA256(hash1) */
    if (mbedtls_sha256(hash1, SHA256_LEN, hash2, 0) != 0) return -1;
    /* challenge = SHA256(hash2 || salt) */
    memcpy(mix,            hash2, SHA256_LEN);
    memcpy(mix + SHA256_LEN, salt,  MYSQL_CACHING_SALT_LEN);
    if (mbedtls_sha256(mix, sizeof(mix), challenge, 0) != 0) return -1;

    /* token = hash1 XOR challenge */
    for (int i = 0; i < SHA256_LEN; i++) {
        out_token[i] = hash1[i] ^ challenge[i];
    }
    return 0;
}
