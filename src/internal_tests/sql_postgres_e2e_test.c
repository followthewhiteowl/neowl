// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test end-to-end : mock server + OwlPostgresConn complete handshake.
 *
 * Validation reelle du pipeline complet : TCP -> Startup -> Auth ->
 * Ready -> Query -> response -> Terminate, sans dependance externe
 * (mock server local dans un thread). */

#include "sql_postgres_conn.h"
#include "sql_postgres_mock_server.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define E2E_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_handshake_auth_ok(void) {
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_OK, NULL);
    E2E_CHECK(m != NULL, "mock server start");
    E2E_CHECK(postgres_mock_server_is_ready(m), "mock server ready in 2s");
    if (!m || !postgres_mock_server_is_ready(m)) {
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c,
                                "127.0.0.1", postgres_mock_server_port(m),
                                "alice", NULL, "mydb",
                                OWL_SQL_TLS_DESACTIVE /* mock ne fait pas TLS */, NULL);
    E2E_CHECK(ok, "owl_postgres_conn_open auth=OK reussit");
    if (!ok) {
        fprintf(stderr, "    last_error: %s\n", owl_postgres_conn_last_error(&c));
    }
    E2E_CHECK(c.connected, "conn.connected = true apres open");
    E2E_CHECK(c.backend_pid == 12345, "backend_pid recu du mock");
    E2E_CHECK(c.cancel_key == 67890, "cancel_key recu du mock");

    owl_postgres_conn_close(&c);
    postgres_mock_server_join(m);
    postgres_mock_server_free(m);
}

static void test_handshake_auth_cleartext(void) {
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_CLEARTEXT, "s3cret");
    if (!m || !postgres_mock_server_is_ready(m)) {
        E2E_CHECK(false, "mock cleartext setup");
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", postgres_mock_server_port(m),
                                "alice", "s3cret", "mydb", OWL_SQL_TLS_DESACTIVE, NULL);
    E2E_CHECK(ok, "handshake auth=cleartext password match");
    if (!ok) {
        fprintf(stderr, "    last_error: %s\n", owl_postgres_conn_last_error(&c));
    }
    owl_postgres_conn_close(&c);
    postgres_mock_server_join(m);
    /* Verifie que le mock a recu le bon password. */
    E2E_CHECK(strcmp(postgres_mock_server_last_password(m), "s3cret") == 0,
              "mock a recu le bon password");
    postgres_mock_server_free(m);
}

static void test_handshake_auth_fail(void) {
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_FAIL, NULL);
    if (!m || !postgres_mock_server_is_ready(m)) {
        E2E_CHECK(false, "mock fail setup");
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", postgres_mock_server_port(m),
                                "alice", "wrong", "mydb", OWL_SQL_TLS_DESACTIVE, NULL);
    E2E_CHECK(!ok, "handshake auth=fail echec proprement");
    E2E_CHECK(strstr(owl_postgres_conn_last_error(&c), "auth failed") != NULL,
              "last_error contient le message du serveur");
    owl_postgres_conn_close(&c);
    postgres_mock_server_join(m);
    postgres_mock_server_free(m);
}

static void test_handshake_cleartext_wrong_pwd(void) {
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_CLEARTEXT, "right");
    if (!m || !postgres_mock_server_is_ready(m)) {
        E2E_CHECK(false, "mock cleartext setup");
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", postgres_mock_server_port(m),
                                "alice", "wrong", "mydb", OWL_SQL_TLS_DESACTIVE, NULL);
    E2E_CHECK(!ok, "handshake cleartext mauvais password -> echec");
    E2E_CHECK(strstr(owl_postgres_conn_last_error(&c), "bad password") != NULL,
              "last_error mentionne bad password");
    owl_postgres_conn_close(&c);
    postgres_mock_server_join(m);
    postgres_mock_server_free(m);
}

int sql_postgres_e2e_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_handshake_auth_ok();
    test_handshake_auth_cleartext();
    test_handshake_auth_fail();
    test_handshake_cleartext_wrong_pwd();

    printf("sql_postgres_e2e self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
