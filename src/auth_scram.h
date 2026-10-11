// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_auth_scram_h
#define owl_auth_scram_h

/* ================================================================
 * SCRAM-SHA-256 client (RFC 5802 + RFC 7677)
 *
 * Implémente le côté client du Salted Challenge Response Authentication
 * Mechanism, mécanisme SASL standard pour les bases de données modernes
 * (PostgreSQL ≥ 10, MongoDB, Cassandra, etc.) et IMAP/SMTP/XMPP.
 *
 * Sécurité : le mot de passe utilisateur ne traverse JAMAIS le wire. À
 * la place, le client prouve qu'il connait le mot de passe en envoyant
 * un `ClientProof` calculé via PBKDF2-HMAC-SHA-256 + HMAC. Le serveur
 * répond avec une `ServerSignature` que le client peut vérifier, ce qui
 * authentifie mutuellement les deux parties.
 *
 * Cette implémentation utilise mbedTLS pour SHA-256, HMAC-SHA-256,
 * PBKDF2-HMAC, et base64 — pas de duplication crypto.
 *
 * Usage typique (côté PG wire) :
 *
 *   OwlScramSha256 s;
 *   owl_scram_init(&s, "alice", "s3cret");
 *
 *   // 1. Envoi client-first-message au serveur
 *   char first[512];
 *   owl_scram_client_first(&s, NULL, first, sizeof first);
 *
 *   // 2. Le serveur répond server-first-message
 *   char final[512];
 *   owl_scram_client_final(&s, server_first, final, sizeof final);
 *
 *   // 3. Le serveur répond server-final-message
 *   if (!owl_scram_verify_server_final(&s, server_final)) { ... auth fail ... }
 *
 * Non thread-safe par instance (séquentiel).
 * ================================================================ */

#include "common.h"

typedef struct {
    char    username[256];
    char    password[256];
    char    client_nonce[64];           /* ASCII printable, ≥ 16 chars */
    char    auth_message[2048];         /* concat des 3 messages pour HMAC */
    size_t  auth_message_len;
    /* Signature attendue du serveur, calculée pendant client_final.
     * Verifiée contre ce que le serveur enverra en server-final. */
    uint8_t expected_server_sig[32];
    bool    has_expected_server_sig;
    char    last_error[256];
} OwlScramSha256;

void owl_scram_init(OwlScramSha256* s, const char* username, const char* password);

/* Étape 1 : produit client-first-message au format SCRAM.
 *   Forme : "n,,n=<user>,r=<client-nonce>"
 *
 * Si client_nonce_override == NULL, génère un nonce aléatoire (24 chars
 * printable depuis /dev/urandom ou BCryptGenRandom).
 * Si non-NULL, utilise la valeur fournie (utile pour tests reproductibles
 * avec vecteurs RFC).
 *
 * Le message est écrit dans out_msg (avec '\0' terminal), out_msg_size
 * doit être ≥ 64 + len(username). Retourne true si OK, false sur OOM
 * ou erreur RNG. */
bool owl_scram_client_first(OwlScramSha256* s,
                             const char* client_nonce_override,
                             char* out_msg, size_t out_msg_size);

/* Étape 2 : parse server-first-message et produit client-final-message.
 *   Server-first format : "r=<combined-nonce>,s=<salt-b64>,i=<iter>"
 *   Client-final format : "c=biws,r=<combined-nonce>,p=<proof-b64>"
 *
 * Calcule les valeurs intermédiaires :
 *   - SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, iter, 32 bytes)
 *   - ClientKey      = HMAC(SaltedPassword, "Client Key")
 *   - StoredKey      = SHA-256(ClientKey)
 *   - AuthMessage    = client-first-bare + "," + server-first + "," + client-final-without-proof
 *   - ClientSignature = HMAC(StoredKey, AuthMessage)
 *   - ClientProof    = ClientKey XOR ClientSignature
 *   - ServerKey      = HMAC(SaltedPassword, "Server Key")
 *   - ServerSignature attendue = HMAC(ServerKey, AuthMessage)  [stocké]
 *
 * Retourne true si OK, false si parse error (last_error rempli).
 * Validation : la combined-nonce doit commencer par notre client_nonce
 * (anti-replay). */
bool owl_scram_client_final(OwlScramSha256* s,
                             const char* server_first_msg,
                             char* out_msg, size_t out_msg_size);

/* Étape 3 : vérifie server-final-message.
 *   Server-final format : "v=<server-signature-b64>"
 *
 * Compare la signature reçue avec celle calculée en étape 2 (en
 * temps constant pour résister aux timing attacks). Retourne true
 * si auth mutual réussie. */
bool owl_scram_verify_server_final(OwlScramSha256* s,
                                    const char* server_final_msg);

const char* owl_scram_last_error(const OwlScramSha256* s);

#endif
