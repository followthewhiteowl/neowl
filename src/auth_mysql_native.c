// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "auth_mysql_native.h"

#include <mbedtls/sha1.h>
#include <string.h>

#define SHA1_LEN 20

int owl_mysql_native_compute_token(const char* password, size_t password_len,
                                    const uint8_t salt[MYSQL_NATIVE_SALT_LEN],
                                    uint8_t out_token[MYSQL_NATIVE_TOKEN_LEN]) {
    if (password == NULL || out_token == NULL || salt == NULL) return -1;

    uint8_t hash1[SHA1_LEN];      /* SHA1(password) */
    uint8_t hash2[SHA1_LEN];      /* SHA1(SHA1(password)) */
    uint8_t mix[SHA1_LEN + MYSQL_NATIVE_SALT_LEN];
    uint8_t challenge[SHA1_LEN];  /* SHA1(salt || SHA1(SHA1(password))) */

    /* hash1 = SHA1(password) */
    if (mbedtls_sha1((const uint8_t*)password, password_len, hash1) != 0) return -1;
    /* hash2 = SHA1(hash1) */
    if (mbedtls_sha1(hash1, SHA1_LEN, hash2) != 0) return -1;
    /* challenge = SHA1(salt || hash2) */
    memcpy(mix,                       salt,  MYSQL_NATIVE_SALT_LEN);
    memcpy(mix + MYSQL_NATIVE_SALT_LEN, hash2, SHA1_LEN);
    if (mbedtls_sha1(mix, sizeof(mix), challenge) != 0) return -1;

    /* token = hash1 XOR challenge */
    for (int i = 0; i < SHA1_LEN; i++) {
        out_token[i] = hash1[i] ^ challenge[i];
    }
    return 0;
}
