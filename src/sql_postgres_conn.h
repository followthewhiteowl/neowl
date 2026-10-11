// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_postgres_conn_h
#define owl_sql_postgres_conn_h

/* ================================================================
 * PostgreSQL connection — state machine de handshake
 *
 * Orchestre les briques infra (net_tcp / net_tcp_tls / auth_scram /
 * sql_postgres_wire / sql_postgres_types) pour realiser un dialogue PG complet
 * de bout en bout :
 *
 *   1. TCP connect
 *   2. SSL negotiation (SSLRequest -> 'S' = upgrade TLS, 'N' = clair)
 *   3. StartupMessage (user, database)
 *   4. Auth loop selon ce que le serveur demande :
 *        - AuthenticationOK              : direct ready
 *        - AuthenticationCleartextPassword : envoyer password en clair
 *        - AuthenticationMD5Password     : md5(md5(pwd+user)+salt)
 *        - AuthenticationSASL            : SCRAM-SHA-256 (canon PG ≥ 10)
 *   5. ParameterStatus / BackendKeyData : consigne dans la conn
 *   6. ReadyForQuery -> CONNECTED
 *
 * Erreur a tout moment : ErrorResponse 'E' du serveur extrait via
 * postgres_parse_error_message, last_error rempli, conn fermee.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"
#include "net_be_buffer.h"
#include "sentinels.h"

#define OWL_POSTGRES_LAST_ERROR_SIZE 512

/* Mode TLS pour SQLConnexion.ModeTLS — aligne Postgres sslmode.
 * Valeurs entieres definies en zone sentinel dans sentinels.h
 * (OWL_SQL_TLS_BASE). Type opaque ici, le runtime traite ca comme int. */
typedef int OwlSqlTlsMode;

typedef struct {
    /* Transport : socket TCP simple, OU TLS au-dessus si upgrade reussi. */
    OwlTlsSocket tls;             /* contient l'OwlTcpSocket via .tcp */
    bool         use_tls;
    bool         connected;

    /* BackendKeyData : pid + key pour eventuellement annuler une query
     * en cours via une connexion separee (CancelRequest message). */
    int32_t      backend_pid;
    int32_t      cancel_key;

    /* ParameterStatus accumule (server_version, server_encoding, etc.).
     * V1 : on les ignore pour la simplicite. */

    char         last_error[OWL_POSTGRES_LAST_ERROR_SIZE];
} OwlPostgresConn;

void owl_postgres_conn_init(OwlPostgresConn* c);

/* Ouvre une connexion complete (TCP -> [TLS] -> Startup -> Auth -> Ready).
 *
 *   host, port    : endpoint reseau
 *   user          : nom d'utilisateur PG
 *   password      : mot de passe (utilise selon ce que le serveur demande)
 *   database      : base a connecter
 *   tls_mode      : OwlSqlTlsMode. DESACTIVE = clair. Autres -> SSL upgrade
 *                   avec niveau de verification du cert serveur. Si serveur
 *                   refuse ('N') alors qu'on a demande TLS, echec (anti
 *                   downgrade attack).
 *
 * Retourne true si connection ready for query. Sinon last_error
 * decrit l'echec (auth, TLS, network, ...). */
bool owl_postgres_conn_open(OwlPostgresConn* c,
                       const char* host, int port,
                       const char* user, const char* password,
                       const char* database,
                       int tls_mode,
                       const char* extra_ca_path);

/* Termine proprement la session : send Terminate ('X') puis close
 * socket. Idempotent. */
void owl_postgres_conn_close(OwlPostgresConn* c);

const char* owl_postgres_conn_last_error(const OwlPostgresConn* c);

/* Helpers internes exposes pour tests + reuse en Phase 2.4 :
 *   send_buffer / recv_message font le transport unifie TLS ou clair. */

/* Envoie integralement le contenu d'un OwlBeBuffer (write-mode). */
bool owl_postgres_send(OwlPostgresConn* c, const OwlBeBuffer* out);

/* Recoit un message PG complet (header + payload).
 *   out_type      : char 1 byte
 *   out_payload   : buffer alloue, caller doit free(*out_payload)
 *   out_payload_len: longueur du payload
 * Retourne false en cas d'erreur transport ou message tronque. */
bool owl_postgres_recv_msg(OwlPostgresConn* c,
                      char* out_type,
                      uint8_t** out_payload,
                      uint32_t* out_payload_len);

#endif
