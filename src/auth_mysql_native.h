// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_auth_mysql_native_h
#define owl_auth_mysql_native_h

/* ================================================================
 * MySQL native_password authentication (SHA1-based)
 *
 * Algorithme :
 *   token = SHA1(password) XOR SHA1( salt || SHA1(SHA1(password)) )
 *
 * Le serveur stocke SHA1(SHA1(password)) (deux passes SHA1 du mot de
 * passe en clair). Il calcule le meme XOR cote serveur et compare.
 *
 * SHA1 est obsolete cryptographiquement (collisions 2017) — `caching_sha2_password`
 * est le defaut MySQL 8.0+. Mais native_password reste support sur les
 * vieux comptes et MariaDB.
 *
 * Salt = 20 octets fournis par le serveur (Initial Handshake ou AuthSwitch).
 * Token = 20 octets (= longueur d'un SHA1).
 * ================================================================ */

#include "common.h"

#define MYSQL_NATIVE_TOKEN_LEN 20
#define MYSQL_NATIVE_SALT_LEN  20

/* Compute the auth token for native_password.
 *   password / password_len : mot de passe utilisateur en UTF-8
 *   salt : 20 octets fournis par le serveur
 *   out_token : 20 octets ecrits ici
 * Retourne 0 si OK, -1 si erreur crypto interne. */
int owl_mysql_native_compute_token(const char* password, size_t password_len,
                                    const uint8_t salt[MYSQL_NATIVE_SALT_LEN],
                                    uint8_t out_token[MYSQL_NATIVE_TOKEN_LEN]);

#endif
