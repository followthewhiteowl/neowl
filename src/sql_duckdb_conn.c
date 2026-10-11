// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_duckdb_conn.h"

#include <stdio.h>
#include <string.h>

void owl_duckdb_conn_init(OwlDuckdbConn* c) {
    memset(c, 0, sizeof(*c));
}

const char* owl_duckdb_conn_last_error(const OwlDuckdbConn* c) {
    return c->last_error;
}

bool owl_duckdb_conn_open(OwlDuckdbConn* c, const char* path) {
    if (c->connected) {
        snprintf(c->last_error, sizeof(c->last_error), "duckdb deja connecte");
        return false;
    }
    const char* effective_path = (path && path[0]) ? path : ":memory:";

    if (duckdb_open(effective_path, &c->db) != DuckDBSuccess) {
        snprintf(c->last_error, sizeof(c->last_error),
                 "duckdb_open(%s) echec", effective_path);
        c->db = NULL;
        return false;
    }
    if (duckdb_connect(c->db, &c->conn) != DuckDBSuccess) {
        snprintf(c->last_error, sizeof(c->last_error), "duckdb_connect echec");
        duckdb_close(&c->db);
        c->db = NULL;
        return false;
    }
    c->connected = true;
    c->last_error[0] = '\0';
    return true;
}

void owl_duckdb_conn_close(OwlDuckdbConn* c) {
    if (c->conn) {
        duckdb_disconnect(&c->conn);
        c->conn = NULL;
    }
    if (c->db) {
        duckdb_close(&c->db);
        c->db = NULL;
    }
    c->connected = false;
}
