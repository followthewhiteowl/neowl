// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test DuckDB bout-en-bout : open :memory:, INSERT, SELECT, types. */

#include "sql_duckdb_conn.h"
#include "sql_duckdb_query.h"

#include "vm.h"
#include "object.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static bool g_vm_inited_here = false;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void ensure_vm(void) {
    if (!g_vm_inited_here) { vm_init(); g_vm_inited_here = true; }
}

static void test_open_close_memory(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    CHECK(owl_duckdb_conn_open(&c, ":memory:"), "open :memory:");
    CHECK(c.connected, "c.connected = true");
    owl_duckdb_conn_close(&c);
    CHECK(!c.connected, "close -> not connected");
}

static void test_select_constant(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    if (!owl_duckdb_conn_open(&c, ":memory:")) { CHECK(false, "open"); return; }

    OwlDuckdbResult r;
    bool ok = owl_duckdb_simple_query(&c, "SELECT 42 AS n", &r);
    CHECK(ok && r.success,                              "SELECT 42 reussit");
    CHECK(r.column_count == 1,                          "1 colonne");
    CHECK(r.columns && strcmp(r.columns[0].name, "n") == 0, "col name 'n'");
    CHECK(r.row_count == 1,                              "1 ligne");
    CHECK(r.row_count == 1 && IS_INTEGER(r.rows[0][0]) &&
          AS_INTEGER(r.rows[0][0]) == 42,                "value = 42");
    owl_duckdb_result_free(&r);
    owl_duckdb_conn_close(&c);
}

static void test_create_insert_select(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    owl_duckdb_conn_open(&c, ":memory:");

    OwlDuckdbResult r;

    bool ok = owl_duckdb_simple_query(&c,
        "CREATE TABLE t (id INTEGER, nom VARCHAR, montant DOUBLE)", &r);
    CHECK(ok, "CREATE TABLE");
    owl_duckdb_result_free(&r);

    ok = owl_duckdb_simple_query(&c,
        "INSERT INTO t VALUES (1, 'Alice', 100.5), (2, 'Bob', 250.0), (3, 'Carol', NULL)",
        &r);
    CHECK(ok,                              "INSERT reussit");
    CHECK(r.affected_rows == 3,            "3 lignes affectees");
    owl_duckdb_result_free(&r);

    ok = owl_duckdb_simple_query(&c,
        "SELECT id, nom, montant FROM t ORDER BY id", &r);
    CHECK(ok && r.column_count == 3 && r.row_count == 3,
                                            "SELECT 3 cols x 3 rows");
    CHECK(IS_INTEGER(r.rows[0][0]) && AS_INTEGER(r.rows[0][0]) == 1,
                                            "row[0].id = 1");
    CHECK(IS_STRING(r.rows[0][1]) && strcmp(AS_STRING(r.rows[0][1])->chars, "Alice") == 0,
                                            "row[0].nom = Alice");
    CHECK(IS_REAL(r.rows[0][2]) && AS_REAL(r.rows[0][2]) > 100.4 && AS_REAL(r.rows[0][2]) < 100.6,
                                            "row[0].montant = 100.5");
    CHECK(IS_NIL(r.rows[2][2]),             "row[2].montant = NULL");
    owl_duckdb_result_free(&r);

    ok = owl_duckdb_simple_query(&c,
        "SELECT COUNT(*) AS n FROM t", &r);
    CHECK(ok && r.row_count == 1,           "aggregat COUNT reussit");
    if (ok && r.row_count == 1) {
        CHECK(IS_INTEGER(r.rows[0][0]) && AS_INTEGER(r.rows[0][0]) == 3,
                                                "COUNT = 3");
    }
    owl_duckdb_result_free(&r);
    owl_duckdb_conn_close(&c);
}

/* Verifie que les 4 extensions (core_functions, parquet, json, icu) lies
 * statiquement sont effectivement disponibles. core_functions porte SUM/
 * MIN/MAX/AVG, json porte json_extract, icu porte les timezones. */
static void test_linked_extensions(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    owl_duckdb_conn_open(&c, ":memory:");

    OwlDuckdbResult r;
    bool ok;

    /* core_functions : SUM/MIN/MAX/AVG. DuckDB SUM(INTEGER) renvoie HUGEINT
     * (128-bit) — on cast en BIGINT pour rester sur INT64 mappable. */
    ok = owl_duckdb_simple_query(&c,
        "SELECT CAST(SUM(n) AS BIGINT), MIN(n), MAX(n), AVG(n) "
        "FROM (VALUES (1),(2),(3),(4),(5)) AS v(n)", &r);
    CHECK(ok && r.row_count == 1 && r.column_count == 4,
                                            "core_functions: SUM/MIN/MAX/AVG dispo");
    if (ok && r.row_count == 1 && r.column_count == 4) {
        CHECK(IS_INTEGER(r.rows[0][0]) && AS_INTEGER(r.rows[0][0]) == 15, "SUM = 15");
        CHECK(IS_INTEGER(r.rows[0][1]) && AS_INTEGER(r.rows[0][1]) == 1,  "MIN = 1");
        CHECK(IS_INTEGER(r.rows[0][2]) && AS_INTEGER(r.rows[0][2]) == 5,  "MAX = 5");
        CHECK(IS_REAL(r.rows[0][3]) && AS_REAL(r.rows[0][3]) > 2.9 && AS_REAL(r.rows[0][3]) < 3.1,
                                                                          "AVG = 3.0");
    }
    owl_duckdb_result_free(&r);

    /* json : json_extract */
    ok = owl_duckdb_simple_query(&c,
        "SELECT json_extract('{\"x\":42,\"y\":\"hello\"}', '$.x') AS v", &r);
    CHECK(ok && r.row_count == 1,           "json: json_extract dispo");
    owl_duckdb_result_free(&r);

    /* icu : timezone-aware timestamp */
    ok = owl_duckdb_simple_query(&c,
        "SELECT TIMESTAMPTZ '2026-05-13 12:00:00+02' AS t", &r);
    CHECK(ok && r.row_count == 1,           "icu: TIMESTAMPTZ avec offset dispo");
    owl_duckdb_result_free(&r);

    /* parquet : verifie que la fonction read_parquet est resolue (catalog).
     * On ne lit pas reellement un fichier — juste qu'elle n'est pas inconnue. */
    ok = owl_duckdb_simple_query(&c,
        "SELECT function_name FROM duckdb_functions() WHERE function_name = 'read_parquet' LIMIT 1", &r);
    CHECK(ok && r.row_count >= 1,           "parquet: read_parquet enregistre");
    owl_duckdb_result_free(&r);

    owl_duckdb_conn_close(&c);
}

static void test_date_timestamp(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    owl_duckdb_conn_open(&c, ":memory:");

    OwlDuckdbResult r;
    bool ok = owl_duckdb_simple_query(&c,
        "SELECT DATE '2026-05-13' AS d, TIMESTAMP '2026-05-13 14:30:00' AS ts", &r);
    CHECK(ok && r.row_count == 1, "SELECT date+ts");
    CHECK(IS_STRING(r.rows[0][0]) &&
          strncmp(AS_STRING(r.rows[0][0])->chars, "2026-05-13", 10) == 0,
                                  "DATE rendue YYYY-MM-DD");
    CHECK(IS_STRING(r.rows[0][1]) &&
          strncmp(AS_STRING(r.rows[0][1])->chars, "2026-05-13 14:30:00", 19) == 0,
                                  "TIMESTAMP rendu YYYY-MM-DD HH:MM:SS");
    owl_duckdb_result_free(&r);
    owl_duckdb_conn_close(&c);
}

static void test_error_handling(void) {
    OwlDuckdbConn c;
    owl_duckdb_conn_init(&c);
    owl_duckdb_conn_open(&c, ":memory:");

    OwlDuckdbResult r;
    bool ok = owl_duckdb_simple_query(&c,
        "SELECT * FROM table_qui_nexiste_pas", &r);
    CHECK(!ok,                          "SQL invalide -> echec");
    CHECK(r.error_message[0] != '\0',   "error_message rempli");
    owl_duckdb_result_free(&r);
    owl_duckdb_conn_close(&c);
}

int sql_duckdb_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    ensure_vm();
    test_open_close_memory();
    test_select_constant();
    test_create_insert_select();
    test_date_timestamp();
    test_error_handling();
    test_linked_extensions();
    printf("sql_duckdb self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
