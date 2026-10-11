// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test simple_query MySQL via mock server.
 * Valide : COM_QUERY -> ColumnCount -> ColumnDef -> EOF -> DataRow -> EOF. */

#include "sql_mysql_query.h"
#include "sql_mysql_conn.h"
#include "sql_mysql_mock_server.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_simple_select_42(void) {
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_OK,
                                                  MYSQL_MOCK_PLUGIN_NATIVE,
                                                  "s3cret");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "alice", "s3cret", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(ok, "conn_open OK");
    if (!ok) { mysql_mock_server_free(m); return; }

    OwlMysqlResult r;
    owl_mysql_result_init(&r);
    bool qok = owl_mysql_simple_query(&c, "SELECT n FROM x", &r);
    CHECK(qok,          "simple_query reussit");
    CHECK(r.success,    "r.success = true");
    CHECK(r.column_count == 1,   "1 colonne");
    CHECK(r.columns && strcmp(r.columns[0].name, "n") == 0,
                                 "column.name = 'n'");
    CHECK(r.columns && r.columns[0].type == 0x03 /* MYSQL_TYPE_LONG */,
                                 "column.type = LONG (INT4)");
    CHECK(r.row_count == 1,      "1 ligne");
    CHECK(r.rows && r.rows[0][0].length == 2,
                                 "row[0][0].length = 2 (chars '42')");
    CHECK(r.rows && r.rows[0][0].data &&
          memcmp(r.rows[0][0].data, "42", 2) == 0,
                                 "row[0][0].data = '42'");

    owl_mysql_result_free(&r);
    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);
    CHECK(strcmp(mysql_mock_server_last_query(m), "SELECT n FROM x") == 0,
                                 "mock saw the right query");
    mysql_mock_server_free(m);
}

int sql_mysql_query_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    test_simple_select_42();
    printf("sql_mysql_query self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
