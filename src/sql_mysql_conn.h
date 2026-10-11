// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mysql_conn_h
#define owl_sql_mysql_conn_h

/* ================================================================
 * MySQL connection — state machine handshake
 *
 * Orchestre :
 *   1. TCP connect
 *   2. Recv Initial Handshake (server speaks first)
 *   3. Optional : SSLRequest + TLS upgrade
 *   4. Send HandshakeResponse41 avec auth token (native ou caching_sha2)
 *   5. Optional : AuthSwitchRequest (server demande autre plugin)
 *   6. Optional : AuthMoreData (caching_sha2 fast/slow path)
 *   7. Recv OK packet -> connected
 *
 * Caching_sha2 slow path (password en clair via TLS) : REFUSE par
 * defaut pour V2.2 (security : pas de leak password possible). Le
 * caller obtient une erreur claire.
 *
 * Erreur a tout moment : ErrorPacket -> last_error rempli, conn fermee.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"
#include "net_le_buffer.h"

#define OWL_MYSQL_LAST_ERROR_SIZE 512

/* Modes TLS — alignes sur sql_postgres_conn.h (meme enum OwlSqlTlsMode
 * deja defini la-bas, on s'en sert). Inclus pour ne pas casser. */
#include "sql_postgres_conn.h"

typedef struct {
    OwlTlsSocket tls;           /* TCP + TLS layer */
    bool         use_tls;
    bool         connected;

    /* Capabilities negociees (intersection client/serveur). */
    uint32_t     capabilities;

    /* thread_id annonce par le serveur (utile pour KILL CONNECTION). */
    uint32_t     thread_id;

    /* Counter seq_num : reset a 0 a chaque nouvelle requete cliente.
     * Pendant le handshake : 0 (server Handshake), 1 (client SSL/Response),
     * 2 (server OK)... */
    uint8_t      seq_num;

    char         last_error[OWL_MYSQL_LAST_ERROR_SIZE];
} OwlMysqlConn;

void owl_mysql_conn_init(OwlMysqlConn* c);

/* Ouverture complete : TCP -> [TLS upgrade] -> Handshake -> Auth -> OK.
 *
 * host, port : endpoint reseau
 * user/password : credentials. password peut etre NULL ou "" si pas
 *   d'auth requise (compte sans password — rare).
 * database : base initiale (NULL pour aucune, on l'inclut dans CONNECT_WITH_DB).
 * tls_mode : OwlSqlTlsMode (DESACTIVE / ACCEPTE_TOUT / VERIFIE_CA / VERIFIE_COMPLET).
 *
 * Retourne true si auth reussie et serveur READY. Sinon last_error rempli. */
bool owl_mysql_conn_open(OwlMysqlConn* c,
                          const char* host, int port,
                          const char* user, const char* password,
                          const char* database,
                          int tls_mode,
                          const char* extra_ca_path);

/* Ferme proprement : COM_QUIT + close socket. Idempotent. */
void owl_mysql_conn_close(OwlMysqlConn* c);

const char* owl_mysql_conn_last_error(const OwlMysqlConn* c);

/* Helpers transport unifies (clear ou TLS selon use_tls). */
bool owl_mysql_send(OwlMysqlConn* c, const OwlLeBuffer* out);

/* Recv UN paquet complet : lit le header (4 octets) puis exactement
 * `payload_len` octets. *out_payload est malloc'd (caller doit free).
 * *out_seq = numero de sequence recu (utile pour tracker). */
bool owl_mysql_recv_packet(OwlMysqlConn* c,
                            uint8_t** out_payload, uint32_t* out_payload_len,
                            uint8_t* out_seq);

#endif
