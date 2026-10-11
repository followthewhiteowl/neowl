// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_conn_h
#define owl_sql_mssql_conn_h

/* ================================================================
 * MSSQL TDS 7.4 — connection layer
 *
 * Orchestre les briques Lot 1-5 pour realiser une session SQL Server
 * complete :
 *
 *   1. TCP connect (port 1433 par defaut)
 *   2. PreLogin                       (Lot 2)
 *   3. [TLS handshake]                (Lot 6b — non implemente V1)
 *   4. Login7 + auth SQL Server       (Lot 3)
 *   5. SQLBatch + parse TabularResult (Lots 4-5)
 *
 * V1 : SQL Server Authentication seul (user/password). Pas de TLS.
 * Le serveur doit accepter ENCRYPT_NOT_SUP du client (option "Force
 * Encryption" desactivee, defaut sur SQL Server 2019- et instances
 * on-prem typiques).
 *
 * TLS (Lot 6b) : reuse sql_mssql_tls.c BIO callbacks pour wrapper le
 * handshake TLS dans des paquets TDS 0x12, puis basculer en raw TLS
 * sur TCP pour les messages app (mode ENCRYPT_ON).
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"
#include "value.h"
#include "sql_mssql_query.h"
#include "sql_mssql_tls.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#define OWL_MSSQL_LAST_ERROR_SIZE 512
#define OWL_MSSQL_DEFAULT_PORT    1433

typedef struct {
    OwlTcpSocket tcp;
    bool         connected;

    /* TLS post-handshake : mbedTLS routera ses recv/send via le BIO
     * (OwlTdsTlsBio) qui wrappe en TDS pendant handshake puis passthrough. */
    bool                 tls_active;
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   ssl_conf;
    mbedtls_x509_crt     extra_ca;     /* CA custom du user (CertificatCA) */
    bool                 extra_ca_loaded;
    OwlTdsTlsBio         bio;

    /* Infos serveur capturees depuis LOGINACK et ENVCHANGE. */
    uint8_t      server_version_major;
    uint8_t      server_version_minor;
    uint16_t     server_version_build;
    uint32_t     packet_size;          /* taille payload effective post-ENVCHANGE */

    char         last_error[OWL_MSSQL_LAST_ERROR_SIZE];
} OwlMssqlConn;

void owl_mssql_conn_init(OwlMssqlConn* c);

/* Ouvre une session complete vers host:port.
 *   user/password   : SQL Server Auth (V1).
 *   database        : base initiale a utiliser (optionnel, "" ou NULL = master).
 *   tls_mode        : V1 supporte seulement 0 (tlsDesactive). Autre valeur
 *                     -> echec avec message explicite.
 *   timeout_ms      : pour le connect TCP.
 *
 * Retourne true si CONNECTED. Sinon last_error decrit l'echec. */
bool owl_mssql_conn_open(OwlMssqlConn* c,
                           const char* host, int port,
                           const char* user, const char* password,
                           const char* database,
                           int tls_mode,
                           const char* extra_ca_path,
                           int timeout_ms);

/* Envoie une requete SQL et materialise le resultat dans r.
 * r doit etre owl_mssql_result_init avant l'appel, et free apres usage. */
bool owl_mssql_conn_query(OwlMssqlConn* c, const char* sql_utf8,
                            OwlMssqlResult* r);

/* Envoie une requete parametree via RPC sp_executesql.
 * sql_utf8 doit utiliser les placeholders @P1, @P2... dans l'ordre.
 * params[i].name = "P1", "P2"... (sans @).
 * Anti-injection garanti : les valeurs ne sont jamais concatenees au SQL. */
bool owl_mssql_conn_query_rpc(OwlMssqlConn* c,
                                const char* sql_utf8,
                                const OwlMssqlRpcParam* params, size_t n_params,
                                OwlMssqlResult* r);

void owl_mssql_conn_close(OwlMssqlConn* c);

const char* owl_mssql_conn_last_error(const OwlMssqlConn* c);

#endif
