// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du mapping de types PostgreSQL.
 *
 * Vecteurs deterministes : strings d'entree connues, valeurs attendues
 * en sortie. Pas de dependance reseau ni runtime OWL. */

#include "sql_postgres_types.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int g_pass = 0;
static int g_fail = 0;

#define T_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_oid_category(void) {
    T_CHECK(postgres_oid_category(POSTGRES_OID_BOOL)        == POSTGRES_CAT_BOOL,      "BOOL -> CAT_BOOL");
    T_CHECK(postgres_oid_category(POSTGRES_OID_INT4)        == POSTGRES_CAT_INTEGER,   "INT4 -> CAT_INTEGER");
    T_CHECK(postgres_oid_category(POSTGRES_OID_INT8)        == POSTGRES_CAT_INTEGER,   "INT8 -> CAT_INTEGER");
    T_CHECK(postgres_oid_category(POSTGRES_OID_OID)         == POSTGRES_CAT_INTEGER,   "OID -> CAT_INTEGER");
    T_CHECK(postgres_oid_category(POSTGRES_OID_FLOAT4)      == POSTGRES_CAT_REAL,      "FLOAT4 -> CAT_REAL");
    T_CHECK(postgres_oid_category(POSTGRES_OID_FLOAT8)      == POSTGRES_CAT_REAL,      "FLOAT8 -> CAT_REAL");
    T_CHECK(postgres_oid_category(POSTGRES_OID_TEXT)        == POSTGRES_CAT_STRING,    "TEXT -> CAT_STRING");
    T_CHECK(postgres_oid_category(POSTGRES_OID_NUMERIC)     == POSTGRES_CAT_STRING,    "NUMERIC -> CAT_STRING");
    T_CHECK(postgres_oid_category(POSTGRES_OID_BYTEA)       == POSTGRES_CAT_BYTEA,     "BYTEA -> CAT_BYTEA");
    T_CHECK(postgres_oid_category(POSTGRES_OID_DATE)        == POSTGRES_CAT_DATE,      "DATE -> CAT_DATE");
    T_CHECK(postgres_oid_category(POSTGRES_OID_TIMESTAMP)   == POSTGRES_CAT_TIMESTAMP, "TIMESTAMP -> CAT_TIMESTAMP");
    T_CHECK(postgres_oid_category(99999)              == POSTGRES_CAT_UNKNOWN,   "unknown Oid -> CAT_UNKNOWN");

    T_CHECK(strcmp(postgres_oid_name(POSTGRES_OID_TEXT), "text") == 0,  "name(TEXT)");
    T_CHECK(postgres_oid_name(99999) == NULL, "name(unknown) = NULL");
}

static void test_parse_bool(void) {
    bool b = false;
    T_CHECK(postgres_parse_bool("t",     1, &b) && b == true,  "parse 't'");
    T_CHECK(postgres_parse_bool("f",     1, &b) && b == false, "parse 'f'");
    T_CHECK(postgres_parse_bool("T",     1, &b) && b == true,  "parse 'T' (case insensitive)");
    T_CHECK(postgres_parse_bool("true",  4, &b) && b == true,  "parse 'true'");
    T_CHECK(postgres_parse_bool("false", 5, &b) && b == false, "parse 'false'");
    T_CHECK(postgres_parse_bool("yes",   3, &b) && b == true,  "parse 'yes'");
    T_CHECK(postgres_parse_bool("no",    2, &b) && b == false, "parse 'no'");
    T_CHECK(postgres_parse_bool("1",     1, &b) && b == true,  "parse '1'");
    T_CHECK(postgres_parse_bool("0",     1, &b) && b == false, "parse '0'");
    T_CHECK(!postgres_parse_bool("",     0, &b), "parse '' echec");
    T_CHECK(!postgres_parse_bool("zzz",  3, &b), "parse 'zzz' echec");
}

static void test_parse_int64(void) {
    int64_t v = 0;
    T_CHECK(postgres_parse_int64("42",   2, &v) && v == 42,   "parse '42'");
    T_CHECK(postgres_parse_int64("-1",   2, &v) && v == -1,   "parse '-1'");
    T_CHECK(postgres_parse_int64("0",    1, &v) && v == 0,    "parse '0'");
    T_CHECK(postgres_parse_int64("9223372036854775807", 19, &v)
                                          && v == 9223372036854775807LL, "parse INT64_MAX");
    T_CHECK(!postgres_parse_int64("",    0, &v), "parse '' echec");
    T_CHECK(!postgres_parse_int64("abc", 3, &v), "parse 'abc' echec");
    T_CHECK(!postgres_parse_int64("12x", 3, &v), "parse '12x' trailing echec");
}

static void test_parse_double(void) {
    double d = 0;
    T_CHECK(postgres_parse_double("3.14",     4, &d) && fabs(d - 3.14) < 1e-9,   "parse '3.14'");
    T_CHECK(postgres_parse_double("-0.5",     4, &d) && fabs(d - (-0.5)) < 1e-9, "parse '-0.5'");
    T_CHECK(postgres_parse_double("1e10",     4, &d) && fabs(d - 1e10) < 1e-3,   "parse '1e10'");
    T_CHECK(postgres_parse_double("0",        1, &d) && d == 0.0,                 "parse '0'");
    T_CHECK(!postgres_parse_double("abc",     3, &d), "parse 'abc' echec");
    T_CHECK(!postgres_parse_double("1.2.3",   5, &d), "parse '1.2.3' echec");
}

static void test_parse_bytea_hex(void) {
    uint8_t* out = NULL;
    size_t out_len = 0;

    /* "\xDEADBEEF" */
    T_CHECK(postgres_parse_bytea_hex("\\xDEADBEEF", 10, &out, &out_len),
            "parse '\\xDEADBEEF'");
    T_CHECK(out_len == 4, "bytea len = 4");
    T_CHECK(out && out[0] == 0xDE && out[1] == 0xAD && out[2] == 0xBE && out[3] == 0xEF,
            "bytea contenu = DEADBEEF");
    free(out); out = NULL;

    /* "\x" (vide) */
    T_CHECK(postgres_parse_bytea_hex("\\x", 2, &out, &out_len),
            "parse '\\x' (empty)");
    T_CHECK(out_len == 0, "bytea vide");
    free(out); out = NULL;

    /* Lowercase aussi accepte */
    T_CHECK(postgres_parse_bytea_hex("\\xff", 4, &out, &out_len),
            "parse '\\xff' lowercase");
    T_CHECK(out_len == 1 && out[0] == 0xFF, "bytea ff");
    free(out); out = NULL;

    /* Prefix manquant */
    T_CHECK(!postgres_parse_bytea_hex("DEADBEEF", 8, &out, &out_len),
            "rejette sans prefix \\x");

    /* Longueur impaire */
    T_CHECK(!postgres_parse_bytea_hex("\\xDEA", 5, &out, &out_len),
            "rejette longueur impaire");

    /* Char invalide */
    T_CHECK(!postgres_parse_bytea_hex("\\xZZ", 4, &out, &out_len),
            "rejette caractere invalide");
}

static void test_format_bool(void) {
    char buf[8];
    T_CHECK(postgres_format_bool(true,  buf, sizeof buf) == 1 && strcmp(buf, "t") == 0, "format true");
    T_CHECK(postgres_format_bool(false, buf, sizeof buf) == 1 && strcmp(buf, "f") == 0, "format false");
    T_CHECK(postgres_format_bool(true,  buf, 1) == -1, "format echec si buf trop petit");
}

static void test_format_int64(void) {
    char buf[32];
    T_CHECK(postgres_format_int64(42,   buf, sizeof buf) > 0 && strcmp(buf, "42") == 0, "format 42");
    T_CHECK(postgres_format_int64(-1,   buf, sizeof buf) > 0 && strcmp(buf, "-1") == 0, "format -1");
    T_CHECK(postgres_format_int64(9223372036854775807LL, buf, sizeof buf) > 0
            && strcmp(buf, "9223372036854775807") == 0, "format INT64_MAX");
}

static void test_format_double(void) {
    char buf[64];
    /* %.17g round-trip preserve. */
    double v = 3.141592653589793;
    int n = postgres_format_double(v, buf, sizeof buf);
    T_CHECK(n > 0, "format double OK");
    /* Reparse + recompare bit-exact. */
    double parsed = 0;
    postgres_parse_double(buf, (size_t)n, &parsed);
    T_CHECK(parsed == v, "double round-trip preserve");
}

static void test_format_bytea_hex(void) {
    char buf[64];
    const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    int n = postgres_format_bytea_hex(data, 4, buf, sizeof buf);
    T_CHECK(n == 10 && strcmp(buf, "\\xdeadbeef") == 0,
            "format bytea \\xdeadbeef");
    /* Vide */
    n = postgres_format_bytea_hex(NULL, 0, buf, sizeof buf);
    T_CHECK(n == 2 && strcmp(buf, "\\x") == 0, "format bytea vide");
    /* Buf trop petit */
    T_CHECK(postgres_format_bytea_hex(data, 4, buf, 5) == -1, "echec si buf trop petit");
}

int sql_postgres_types_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_oid_category();
    test_parse_bool();
    test_parse_int64();
    test_parse_double();
    test_parse_bytea_hex();
    test_format_bool();
    test_format_int64();
    test_format_double();
    test_format_bytea_hex();

    printf("sql_postgres_types self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
