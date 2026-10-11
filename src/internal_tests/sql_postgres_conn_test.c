// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test connexion PostgreSQL.
 *
 * Tests minimaux deterministes (pas de dependance reseau externe) :
 *   - init/close idempotent
 *   - connect a host/port impossible -> echec propre + last_error rempli
 *
 * Le vrai test handshake (Startup + Auth + Ready) sera fait en Phase 2.5
 * avec le mini-serveur mock PG. Ici on valide juste l'initialisation et
 * les chemins d'erreur transport. */

#include "sql_postgres_conn.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define C_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_init_close(void) {
    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    C_CHECK(c.connected == false, "init: connected = false");
    C_CHECK(c.last_error[0] == '\0', "init: last_error vide");
    /* Close sur conn jamais ouverte : idempotent. */
    owl_postgres_conn_close(&c);
    owl_postgres_conn_close(&c);
    C_CHECK(c.connected == false, "close idempotent");
}

static void test_connect_refused(void) {
    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    /* Port 1 sur localhost : refus immediat. */
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", 1, "u", "p", "db", OWL_SQL_TLS_DESACTIVE, NULL);
    C_CHECK(!ok, "connect 127.0.0.1:1 echec");
    C_CHECK(c.last_error[0] != '\0', "last_error rempli sur echec connect");
    C_CHECK(strstr(c.last_error, "tcp connect") != NULL,
            "last_error identifie comme tcp stage");
    owl_postgres_conn_close(&c);
}

int sql_postgres_conn_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_init_close();
    test_connect_refused();

    printf("sql_postgres_conn self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
