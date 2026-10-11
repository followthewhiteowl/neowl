// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test bout-en-bout : mock server + owl_mysql_conn pour valider
 * tout le pipeline TCP -> Handshake -> Auth -> OK + une query simple. */

#include "sql_mysql_mock_server.h"
#include "sql_mysql_conn.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_handshake_native_ok(void) {
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_OK,
                                                  MYSQL_MOCK_PLUGIN_NATIVE,
                                                  "s3cret");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup native");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "alice", "s3cret", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(ok, "conn_open native auth=OK reussit");
    if (!ok) fprintf(stderr, "    last_error: %s\n", owl_mysql_conn_last_error(&c));

    CHECK(c.connected, "c.connected = true");
    CHECK(c.thread_id == 42, "thread_id du mock");

    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);

    CHECK(strcmp(mysql_mock_server_last_user(m), "alice") == 0, "mock saw user alice");
    CHECK(strcmp(mysql_mock_server_last_db(m), "testdb") == 0,  "mock saw db testdb");
    CHECK(mysql_mock_server_auth_token_matched(m),             "auth token matche");
    mysql_mock_server_free(m);
}

static void test_handshake_caching_ok(void) {
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_OK,
                                                  MYSQL_MOCK_PLUGIN_CACHING,
                                                  "s3cret");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup caching");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "bob", "s3cret", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(ok, "conn_open caching_sha2 fast path reussit");
    if (!ok) fprintf(stderr, "    last_error: %s\n", owl_mysql_conn_last_error(&c));

    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);
    CHECK(mysql_mock_server_auth_token_matched(m), "caching token matche");
    mysql_mock_server_free(m);
}

static void test_handshake_wrong_password(void) {
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_OK,
                                                  MYSQL_MOCK_PLUGIN_NATIVE,
                                                  "right");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup wrong pwd");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "alice", "wrong", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(!ok, "wrong password -> echec");
    CHECK(strstr(owl_mysql_conn_last_error(&c), "Access denied") != NULL,
                 "error message contient Access denied");
    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);
    CHECK(!mysql_mock_server_auth_token_matched(m), "mock voit token mismatch");
    mysql_mock_server_free(m);
}

static void test_handshake_auth_fail_mode(void) {
    /* Le serveur force un Access denied meme avec le bon password. */
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_FAIL,
                                                  MYSQL_MOCK_PLUGIN_NATIVE,
                                                  "s3cret");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup auth_fail");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "alice", "s3cret", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(!ok, "auth force-fail -> echec");
    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);
    mysql_mock_server_free(m);
}

int sql_mysql_e2e_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    test_handshake_native_ok();
    test_handshake_caching_ok();
    test_handshake_wrong_password();
    test_handshake_auth_fail_mode();
    printf("sql_mysql_e2e self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
