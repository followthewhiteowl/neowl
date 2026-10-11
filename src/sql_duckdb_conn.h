// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_duckdb_conn_h
#define owl_sql_duckdb_conn_h

/* ================================================================
 * DuckDB connection — thin wrapper sur l'API C officielle.
 *
 * Contrairement a PG/MySQL :
 *   - pas de socket / TCP / TLS (embedded)
 *   - pas de user / password / database (ouvert avec un chemin fichier
 *     ou ":memory:")
 *   - pas de seq_num / handshake
 *
 * Lifecycle :
 *   1. duckdb_open(path, &db)       -> alloue la base
 *   2. duckdb_connect(db, &conn)    -> alloue une connexion sur la base
 *   3. queries via owl_duckdb_simple_query (cf sql_duckdb_query.h)
 *   4. duckdb_disconnect(&conn)
 *   5. duckdb_close(&db)
 * ================================================================ */

#include "common.h"
#include "duckdb.h"

#define OWL_DUCKDB_LAST_ERROR_SIZE 512

typedef struct {
    duckdb_database   db;
    duckdb_connection conn;
    bool              connected;
    char              last_error[OWL_DUCKDB_LAST_ERROR_SIZE];
} OwlDuckdbConn;

void owl_duckdb_conn_init(OwlDuckdbConn* c);

/* Ouvre une base DuckDB.
 *   path : ":memory:" ou chemin fichier .duckdb.
 *          NULL/"" -> ":memory:".
 * Retourne true si OK. Sinon last_error rempli. */
bool owl_duckdb_conn_open(OwlDuckdbConn* c, const char* path);

void owl_duckdb_conn_close(OwlDuckdbConn* c);

const char* owl_duckdb_conn_last_error(const OwlDuckdbConn* c);

#endif
