// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test : connexion MySQL state machine, sans mock server.
 *
 * Sans serveur reel, on ne peut tester que les chemins d'erreur transport
 * (refus connexion). Les vrais tests bout-en-bout passent par un mock
 * server (sql_mysql_mock_server, etape suivante du chantier). */

#include "sql_mysql_conn.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_tcp_refused(void) {
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    /* Port 1 sur localhost : refus immediat. */
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", 1,
                                    "u", "p", "db", OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(!ok,                                  "connect 127.0.0.1:1 echec");
    CHECK(c.last_error[0] != '\0',              "last_error rempli");
    CHECK(strstr(c.last_error, "tcp connect") != NULL,
                                                 "last_error stage TCP");
    owl_mysql_conn_close(&c);
}

static void test_init_state(void) {
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    CHECK(!c.connected,                         "init: !connected");
    CHECK(!c.use_tls,                           "init: !use_tls");
    CHECK(c.seq_num == 0,                       "init: seq=0");
    CHECK(c.capabilities == 0,                  "init: caps=0");
    CHECK(c.last_error[0] == '\0',              "init: error vide");
    owl_mysql_conn_close(&c);  /* idempotent sur init seul */
}

int sql_mysql_conn_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    test_init_state();
    test_tcp_refused();
    printf("sql_mysql_conn self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
