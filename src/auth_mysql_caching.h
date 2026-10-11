// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_auth_mysql_caching_h
#define owl_auth_mysql_caching_h

/* ================================================================
 * MySQL caching_sha2_password authentication (SHA-256, default 8.0+)
 *
 * Algorithme :
 *   token = SHA256(password) XOR SHA256( SHA256(SHA256(password)) || salt )
 *
 * Le serveur stocke SHA256(SHA256(SHA256(password))) (triple SHA-256).
 * Le serveur maintient un CACHE en RAM des utilisateurs deja authentifies
 * pour eviter le slow path (qui demande le password en clair via TLS) a
 * chaque connexion. D'ou le nom "caching_sha2".
 *
 * Apres l'envoi du token, le serveur envoie un AuthMoreData :
 *   0x03 : fast auth success — auth termine, attendre OK packet
 *   0x04 : full auth needed (cache miss) — envoyer password en clair
 *          (uniquement si TLS active, sinon refuser pour eviter leak)
 *
 * Salt = 20 octets (utilise meme si SHA256 produit 32 — MySQL tronque).
 * Token = 32 octets (longueur d'un SHA-256).
 * ================================================================ */

#include "common.h"

#define MYSQL_CACHING_TOKEN_LEN 32
#define MYSQL_CACHING_SALT_LEN  20

/* Compute the auth token for caching_sha2_password (fast path).
 *   password / password_len : mot de passe en UTF-8
 *   salt : 20 octets fournis par le serveur
 *   out_token : 32 octets ecrits ici
 * Retourne 0 si OK, -1 si erreur crypto interne. */
int owl_mysql_caching_compute_token(const char* password, size_t password_len,
                                     const uint8_t salt[MYSQL_CACHING_SALT_LEN],
                                     uint8_t out_token[MYSQL_CACHING_TOKEN_LEN]);

#endif
