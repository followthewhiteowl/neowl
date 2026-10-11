// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du mapping types MYSQL -> Value OWL.
 *
 * Requiert vm_init() pour les allocations d'objets (ObjString, ObjBuffer).
 * Le harness --self-test-net ne fait pas vm_init -> on initialise nous-meme. */

#include "sql_mysql_types.h"

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
    if (!g_vm_inited_here) {
        vm_init();
        g_vm_inited_here = true;
    }
}

static void test_null(void) {
    Value v = owl_mysql_value_to_owl(MYSQL_TYPE_LONG, 0, NULL, -1);
    CHECK(IS_NIL(v), "value_len == -1 -> NIL");
}

static void test_integers(void) {
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_LONG, 0,
                                          (const uint8_t*)"42", 2);
        CHECK(IS_INTEGER(v) && AS_INTEGER(v) == 42, "LONG '42' -> 42");
    }
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_LONGLONG, 0,
                                          (const uint8_t*)"-9876543210", 11);
        CHECK(IS_INTEGER(v) && AS_INTEGER(v) == -9876543210LL,
              "LONGLONG '-9876543210' -> -9876543210");
    }
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_TINY, 0,
                                          (const uint8_t*)"1", 1);
        CHECK(IS_INTEGER(v) && AS_INTEGER(v) == 1, "TINY '1' -> 1 (booleen-like)");
    }
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_YEAR, 0,
                                          (const uint8_t*)"2026", 4);
        CHECK(IS_INTEGER(v) && AS_INTEGER(v) == 2026, "YEAR '2026'");
    }
}

static void test_reals(void) {
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_DOUBLE, 0,
                                          (const uint8_t*)"3.14159", 7);
        CHECK(IS_REAL(v) && AS_REAL(v) > 3.14 && AS_REAL(v) < 3.15,
              "DOUBLE '3.14159'");
    }
    {
        /* DECIMAL precision : on accepte la perte (mapping vers reel). */
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_NEWDECIMAL, 0,
                                          (const uint8_t*)"1234.56", 7);
        CHECK(IS_REAL(v) && AS_REAL(v) > 1234.0 && AS_REAL(v) < 1235.0,
              "NEWDECIMAL '1234.56'");
    }
}

static void test_strings(void) {
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_VARCHAR, 0,
                                          (const uint8_t*)"hello", 5);
        CHECK(IS_STRING(v) && AS_STRING(v)->length == 5 &&
              memcmp(AS_STRING(v)->chars, "hello", 5) == 0,
              "VARCHAR 'hello'");
    }
    {
        /* String vide (length=0). */
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_VARCHAR, 0, NULL, 0);
        CHECK(IS_STRING(v) && AS_STRING(v)->length == 0,
              "VARCHAR len=0 -> ''");
    }
    {
        /* DATE en text mode = string canonique MySQL */
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_DATE, 0,
                                          (const uint8_t*)"2026-05-12", 10);
        CHECK(IS_STRING(v) && AS_STRING(v)->length == 10,
              "DATE -> string '2026-05-12'");
    }
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_DATETIME, 0,
                                          (const uint8_t*)"2026-05-12 14:30:00", 19);
        CHECK(IS_STRING(v) && AS_STRING(v)->length == 19,
              "DATETIME -> string");
    }
    {
        Value v = owl_mysql_value_to_owl(MYSQL_TYPE_JSON, 0,
                                          (const uint8_t*)"{\"a\":1}", 7);
        CHECK(IS_STRING(v) && AS_STRING(v)->length == 7,
              "JSON -> string brute (user parse)");
    }
}

static void test_blob(void) {
    uint8_t raw[] = { 0x00, 0xFF, 0x42, 0xAB };
    Value v = owl_mysql_value_to_owl(MYSQL_TYPE_BLOB, 0, raw, 4);
    CHECK(IS_BUFFER(v) && AS_BUFFER(v)->length == 4,
          "BLOB -> buffer length 4");
    CHECK(IS_BUFFER(v) && memcmp(AS_BUFFER(v)->bytes, raw, 4) == 0,
          "BLOB bytes preserves");
}

static void test_unknown_type_fallback(void) {
    /* Type byte arbitraire non liste : fallback string. */
    Value v = owl_mysql_value_to_owl(0xAB, 0,
                                      (const uint8_t*)"raw", 3);
    CHECK(IS_STRING(v), "type inconnu fallback string");
}

int sql_mysql_types_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    ensure_vm();
    test_null();
    test_integers();
    test_reals();
    test_strings();
    test_blob();
    test_unknown_type_fallback();
    printf("sql_mysql_types self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
