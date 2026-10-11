// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test simple query : mock server + owl_postgres_simple_query.
 * Valide le cycle complet Query -> RowDescription -> DataRow -> CommandComplete
 * -> ReadyForQuery + parsing des résultats. */

#include "sql_postgres_query.h"
#include "sql_postgres_conn.h"
#include "sql_postgres_mock_server.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define Q_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_simple_query_select(void) {
    /* Le mock répond toujours : RowDescription(INT4 "n") + DataRow "42" +
     * CommandComplete "SELECT 1". */
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_OK, NULL);
    if (!m || !postgres_mock_server_is_ready(m)) {
        Q_CHECK(false, "mock setup");
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", postgres_mock_server_port(m),
                                "alice", NULL, "mydb", OWL_SQL_TLS_DESACTIVE, NULL);
    Q_CHECK(ok, "conn_open OK");
    if (!ok) {
        fprintf(stderr, "    last_error: %s\n", owl_postgres_conn_last_error(&c));
        postgres_mock_server_free(m);
        return;
    }

    OwlPostgresResult r;
    owl_postgres_result_init(&r);
    ok = owl_postgres_simple_query(&c, "SELECT 1", &r);
    Q_CHECK(ok, "simple_query reussit");
    Q_CHECK(r.success, "result.success = true");

    /* Schema. */
    Q_CHECK(r.column_count == 1, "1 colonne");
    Q_CHECK(r.columns && r.columns[0].name &&
            strcmp(r.columns[0].name, "n") == 0,
            "column[0].name = 'n'");
    Q_CHECK(r.columns && r.columns[0].type_oid == 23,
            "column[0].oid = 23 (INT4)");
    Q_CHECK(r.columns && r.columns[0].format == 0,
            "column[0].format = 0 (text)");

    /* Data. */
    Q_CHECK(r.row_count == 1, "1 ligne");
    Q_CHECK(r.rows && r.rows[0][0].length == 2,
            "rows[0][0].length = 2 (chars '42')");
    Q_CHECK(r.rows && r.rows[0][0].data &&
            memcmp(r.rows[0][0].data, "42", 2) == 0,
            "rows[0][0].data = '42'");

    /* CommandComplete tag. */
    Q_CHECK(strcmp(r.command_tag, "SELECT 1") == 0,
            "command_tag = 'SELECT 1'");

    /* Close AVANT join : sinon le mock reste bloque dans recv_msg
     * en attente du Terminate du client. */
    owl_postgres_result_free(&r);
    owl_postgres_conn_close(&c);   /* envoie Terminate */
    postgres_mock_server_join(m);   /* mock recoit X, return */

    Q_CHECK(strcmp(postgres_mock_server_last_query(m), "SELECT 1") == 0,
            "mock a recu 'SELECT 1'");
    postgres_mock_server_free(m);
}

static void test_extended_query_with_param(void) {
    PostgresMockServer* m = postgres_mock_server_start(POSTGRES_MOCK_AUTH_OK, NULL);
    if (!m || !postgres_mock_server_is_ready(m)) {
        Q_CHECK(false, "mock setup (extended)");
        if (m) postgres_mock_server_free(m);
        return;
    }

    OwlPostgresConn c;
    owl_postgres_conn_init(&c);
    bool ok = owl_postgres_conn_open(&c, "127.0.0.1", postgres_mock_server_port(m),
                                "alice", NULL, "mydb", OWL_SQL_TLS_DESACTIVE, NULL);
    Q_CHECK(ok, "conn_open OK (extended)");
    if (!ok) { postgres_mock_server_free(m); return; }

    /* 1 parametre text = "hello". */
    const char* params[] = { "hello" };
    OwlPostgresResult r;
    owl_postgres_result_init(&r);
    ok = owl_postgres_extended_query(&c, "SELECT $1::text", 1, params, &r);
    Q_CHECK(ok, "extended_query reussit");
    Q_CHECK(r.success, "result.success = true (extended)");
    Q_CHECK(r.column_count == 1, "1 colonne (extended)");
    Q_CHECK(r.row_count == 1, "1 ligne (extended)");
    Q_CHECK(strcmp(r.command_tag, "SELECT 1") == 0,
            "command_tag = 'SELECT 1' (extended)");

    owl_postgres_result_free(&r);
    owl_postgres_conn_close(&c);
    postgres_mock_server_join(m);

    Q_CHECK(strcmp(postgres_mock_server_last_parse_sql(m), "SELECT $1::text") == 0,
            "mock a recu Parse 'SELECT $1::text'");
    Q_CHECK(strcmp(postgres_mock_server_last_bind_param0(m), "hello") == 0,
            "mock a recu Bind param0 = 'hello'");
    postgres_mock_server_free(m);
}

int sql_postgres_query_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_simple_query_select();
    test_extended_query_with_param();

    printf("sql_postgres_query self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
