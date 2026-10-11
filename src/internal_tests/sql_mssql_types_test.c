// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du module types MSSQL : conversions cell -> Value OWL.
 *
 * Pour chaque type on construit une OwlMssqlCell + OwlMssqlColumn artificiels
 * (donnees binaires craftees) puis on verifie la Value produite. */

#include "sql_mssql_types.h"
#include "object.h"
#include "vm.h"

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

/* Helper construit une colonne avec type_code + scale. */
static OwlMssqlColumn make_col(uint8_t type_code, OwlTdsLenCategory cat,
                                 uint32_t fixed_size, uint8_t scale) {
    OwlMssqlColumn c;
    memset(&c, 0, sizeof(c));
    c.type_code = type_code;
    c.len_cat = cat;
    c.fixed_size = fixed_size;
    c.scale = scale;
    return c;
}

static OwlMssqlCell make_cell(const uint8_t* data, size_t len) {
    OwlMssqlCell cell = { .data = data, .len = len, .is_null = false };
    return cell;
}

/* ================================================================
 * Integers
 * ================================================================ */

static void test_int4(void) {
    uint8_t bytes[] = { 0x2A, 0x00, 0x00, 0x00 };   /* 42 */
    OwlMssqlColumn col = make_col(0x38, TDS_LEN_FIXED, 4, 0);
    OwlMssqlCell cell = make_cell(bytes, 4);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_INTEGER(v) && AS_INTEGER(v) == 42, "INT4=42");
}

static void test_int4_negative(void) {
    uint8_t bytes[] = { 0xFF, 0xFF, 0xFF, 0xFF };   /* -1 */
    OwlMssqlColumn col = make_col(0x38, TDS_LEN_FIXED, 4, 0);
    OwlMssqlCell cell = make_cell(bytes, 4);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_INTEGER(v) && AS_INTEGER(v) == -1, "INT4=-1");
}

static void test_bigint(void) {
    /* 1000000000000 (10^12) = E8 D4 A5 10 00 00 00 00 */
    uint8_t bytes[] = { 0x00, 0x10, 0xA5, 0xD4, 0xE8, 0x00, 0x00, 0x00 };
    OwlMssqlColumn col = make_col(0x7F, TDS_LEN_FIXED, 8, 0);
    OwlMssqlCell cell = make_cell(bytes, 8);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_INTEGER(v) && AS_INTEGER(v) == 1000000000000LL, "BIGINT 10^12");
}

static void test_intn_variants(void) {
    OwlMssqlColumn col = make_col(0x26, TDS_LEN_BYTE, 4, 0);

    uint8_t b1[] = { 0x7F };
    OwlMssqlCell c1 = make_cell(b1, 1);
    Value v1 = owl_mssql_cell_to_owl(&col, &c1);
    CHECK(IS_INTEGER(v1) && AS_INTEGER(v1) == 127, "INTN(1)=127");

    uint8_t b2[] = { 0x00, 0x80 };   /* -32768 signed */
    OwlMssqlCell c2 = make_cell(b2, 2);
    Value v2 = owl_mssql_cell_to_owl(&col, &c2);
    CHECK(IS_INTEGER(v2) && AS_INTEGER(v2) == -32768, "INTN(2)=-32768");

    uint8_t b4[] = { 0xFF, 0xFF, 0xFF, 0xFF };
    OwlMssqlCell c4 = make_cell(b4, 4);
    Value v4 = owl_mssql_cell_to_owl(&col, &c4);
    CHECK(IS_INTEGER(v4) && AS_INTEGER(v4) == -1, "INTN(4)=-1");
}

/* ================================================================
 * Bool
 * ================================================================ */

static void test_bit(void) {
    uint8_t b1[] = { 0x01 };
    OwlMssqlColumn col = make_col(0x32, TDS_LEN_FIXED, 1, 0);
    OwlMssqlCell c1 = make_cell(b1, 1);
    Value v1 = owl_mssql_cell_to_owl(&col, &c1);
    CHECK(IS_BOOL(v1) && AS_BOOL(v1) == true, "BIT=1 -> true");

    uint8_t b0[] = { 0x00 };
    OwlMssqlCell c0 = make_cell(b0, 1);
    Value v0 = owl_mssql_cell_to_owl(&col, &c0);
    CHECK(IS_BOOL(v0) && AS_BOOL(v0) == false, "BIT=0 -> false");
}

/* ================================================================
 * Float
 * ================================================================ */

static void test_float(void) {
    /* 1.5 en IEEE 754 double = 3FF8000000000000 BE = 00 00 00 00 00 00 F8 3F LE */
    uint8_t bytes[] = { 0,0,0,0, 0,0,0xF8,0x3F };
    OwlMssqlColumn col = make_col(0x3E, TDS_LEN_FIXED, 8, 0);
    OwlMssqlCell cell = make_cell(bytes, 8);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_REAL(v) && AS_REAL(v) > 1.49 && AS_REAL(v) < 1.51, "FLOAT 1.5");
}

/* ================================================================
 * Money
 * ================================================================ */

static void test_money(void) {
    /* SMALLMONEY 12.3400 -> raw = 123400, /10000 = 12.34
     * 123400 LE = 48 E2 01 00 */
    uint8_t bytes[] = { 0x48, 0xE2, 0x01, 0x00 };
    OwlMssqlColumn col = make_col(0x7A, TDS_LEN_FIXED, 4, 0);
    OwlMssqlCell cell = make_cell(bytes, 4);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_REAL(v) && AS_REAL(v) > 12.33 && AS_REAL(v) < 12.35, "SMALLMONEY 12.34");
}

/* ================================================================
 * GUID
 * ================================================================ */

static void test_guid(void) {
    /* GUID {12345678-9ABC-DEF0-1234-56789ABCDEF0} :
     *   bytes : 78 56 34 12  BC 9A  F0 DE  12 34  56 78 9A BC DE F0
     */
    uint8_t bytes[] = {
        0x78, 0x56, 0x34, 0x12,
        0xBC, 0x9A,
        0xF0, 0xDE,
        0x12, 0x34,
        0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
    };
    OwlMssqlColumn col = make_col(0x24, TDS_LEN_BYTE, 16, 0);
    OwlMssqlCell cell = make_cell(bytes, 16);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v),
                                                                "GUID -> string");
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars,
        "12345678-9abc-def0-1234-56789abcdef0") == 0,           "GUID format");
}

/* ================================================================
 * DATE
 * ================================================================ */

static void test_date(void) {
    /* 2024-01-15 :
     *   days from 1970-01-01 = 19737
     *   days from 0001-01-01 = 19737 + 719162 = 738899
     *   738899 = 0x0B4653 (BE) = 0x53 0x46 0x0B (LE) */
    uint8_t bytes[] = { 0x53, 0x46, 0x0B };
    OwlMssqlColumn col = make_col(0x28, TDS_LEN_BYTE, 3, 0);
    OwlMssqlCell cell = make_cell(bytes, 3);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "2024-01-15") == 0,
                                                                "DATE 2024-01-15");
}

/* ================================================================
 * TIME
 * ================================================================ */

static void test_time_scale7(void) {
    /* 12:34:56.7890123 en scale 7
     * total ticks (100ns) = 12*3600 + 34*60 + 56 = 45296 secondes
     *                     + 0.7890123 sec = 0.7890123 * 10^7 = 7890123 ticks
     * total = 45296 * 10^7 + 7890123 = 452960000000 + 7890123 = 452967890123
     * 452967890123 = 0x69 78 27 0E 7C LE (5 bytes)
     */
    uint64_t total = 452967890123ULL;
    uint8_t bytes[5];
    bytes[0] = (uint8_t)(total);
    bytes[1] = (uint8_t)(total >> 8);
    bytes[2] = (uint8_t)(total >> 16);
    bytes[3] = (uint8_t)(total >> 24);
    bytes[4] = (uint8_t)(total >> 32);
    OwlMssqlColumn col = make_col(0x29, TDS_LEN_BYTE, 5, 7);
    OwlMssqlCell cell = make_cell(bytes, 5);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v),                                          "TIME -> string");
    if (IS_STRING(v)) {
        CHECK(strcmp(AS_STRING(v)->chars, "12:34:56.7890123") == 0, "TIME format scale 7");
    }
}

static void test_time_scale0(void) {
    /* 09:30:00, scale 0, ticks in seconds = 34200, 3 bytes LE */
    uint8_t bytes[3];
    uint64_t t = 34200;
    bytes[0] = (uint8_t)t; bytes[1] = (uint8_t)(t >> 8); bytes[2] = (uint8_t)(t >> 16);
    OwlMssqlColumn col = make_col(0x29, TDS_LEN_BYTE, 3, 0);
    OwlMssqlCell cell = make_cell(bytes, 3);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "09:30:00") == 0,
                                                                "TIME scale 0 = 09:30:00");
}

/* ================================================================
 * DATETIME (legacy)
 * ================================================================ */

static void test_datetime(void) {
    /* 2024-01-15 12:00:00, days from 1900 = 45304 (1900..2024 = 19737+25567)
     * ticks (1/300) at 12:00 = 12*3600*300 = 12960000 = 0x00C5C100 LE
     *
     * 8 bytes: B0FA0000 00C1C500 LE
     */
    int32_t days_1900 = 45304;
    uint32_t ticks = 12960000;
    uint8_t bytes[8];
    for (int i = 0; i < 4; ++i) bytes[i] = (uint8_t)((days_1900 >> (i*8)) & 0xFF);
    for (int i = 0; i < 4; ++i) bytes[4 + i] = (uint8_t)((ticks >> (i*8)) & 0xFF);
    OwlMssqlColumn col = make_col(0x3D, TDS_LEN_FIXED, 8, 0);
    OwlMssqlCell cell = make_cell(bytes, 8);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v),                                          "DATETIME -> string");
    if (IS_STRING(v)) {
        CHECK(strncmp(AS_STRING(v)->chars, "2024-01-15 12:00:00", 19) == 0,
                                                                "DATETIME 2024-01-15 12:00:00");
    }
}

/* ================================================================
 * NULL
 * ================================================================ */

static void test_null(void) {
    OwlMssqlColumn col = make_col(0x38, TDS_LEN_FIXED, 4, 0);
    OwlMssqlCell cell = { .data = NULL, .len = 0, .is_null = true };
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_NIL(v),                                             "NULL -> NIL_VAL");
}

/* ================================================================
 * NVARCHAR -> UTF-8
 * ================================================================ */

static void test_nvarchar(void) {
    /* "Bonjour" en UCS-2 LE = 7 chars * 2 = 14 bytes */
    uint8_t bytes[] = {
        'B',0, 'o',0, 'n',0, 'j',0, 'o',0, 'u',0, 'r',0
    };
    OwlMssqlColumn col = make_col(0xE7, TDS_LEN_USHORT, 0, 0);
    OwlMssqlCell cell = make_cell(bytes, sizeof(bytes));
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "Bonjour") == 0,
                                                                "NVARCHAR ASCII -> Bonjour");
}

static void test_nvarchar_accent(void) {
    /* "Café" en UCS-2 LE : C=0x43 a=0x61 f=0x66 é=0xE9 (BMP)
     * Bytes: 43 00 61 00 66 00 E9 00 */
    uint8_t bytes[] = { 'C',0, 'a',0, 'f',0, 0xE9, 0 };
    OwlMssqlColumn col = make_col(0xE7, TDS_LEN_USHORT, 0, 0);
    OwlMssqlCell cell = make_cell(bytes, sizeof(bytes));
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    /* "Café" en UTF-8 = 43 61 66 C3 A9 (e-aigu = 2 bytes UTF-8) */
    CHECK(IS_STRING(v),                                          "NVARCHAR accent");
    if (IS_STRING(v)) {
        const char* s = AS_STRING(v)->chars;
        CHECK(s[0] == 'C' && s[1] == 'a' && s[2] == 'f'
           && (uint8_t)s[3] == 0xC3 && (uint8_t)s[4] == 0xA9,
                                                                "UTF-8 encoding e-aigu");
    }
}

/* ================================================================
 * VARCHAR (ASCII)
 * ================================================================ */

static void test_varchar(void) {
    OwlMssqlColumn col = make_col(0xA7, TDS_LEN_USHORT, 0, 0);
    OwlMssqlCell cell = make_cell((const uint8_t*)"hello", 5);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "hello") == 0,
                                                                "VARCHAR ascii hello");
}

/* ================================================================
 * VARBINARY -> Buffer
 * ================================================================ */

static void test_varbinary(void) {
    uint8_t bytes[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    OwlMssqlColumn col = make_col(0xA5, TDS_LEN_USHORT, 0, 0);
    OwlMssqlCell cell = make_cell(bytes, 4);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_BUFFER(v),                                          "VARBINARY -> Buffer");
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        CHECK(b->length == 4 && b->bytes[0] == 0xDE && b->bytes[3] == 0xEF,
                                                                "Buffer contenu");
    }
}

/* ================================================================
 * DECIMAL
 * ================================================================ */

static void test_decimal_simple(void) {
    /* DECIMAL(10,2) value = 123.45
     * mantissa = 12345 (decimal). In LE: 39 30 00 00 (4 bytes for precision 1-9, but
     * 12345 needs precision 5 -> still 4 byte mantissa)
     * Bytes: [sign=1] [39 30 00 00] = 5 bytes total
     */
    uint8_t bytes[] = { 0x01, 0x39, 0x30, 0x00, 0x00 };
    OwlMssqlColumn col = make_col(0x6A, TDS_LEN_BYTE, 5, 2);
    col.precision = 5;
    OwlMssqlCell cell = make_cell(bytes, 5);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v),                                          "DECIMAL -> string");
    if (IS_STRING(v)) {
        CHECK(strcmp(AS_STRING(v)->chars, "123.45") == 0,        "DECIMAL 123.45");
    }
}

static void test_decimal_negative(void) {
    /* DECIMAL(5,2) value = -7.50
     * mantissa = 750 = 02EE LE = EE 02 00 00 (4 bytes)
     * sign = 0 (negative)
     */
    uint8_t bytes[] = { 0x00, 0xEE, 0x02, 0x00, 0x00 };
    OwlMssqlColumn col = make_col(0x6A, TDS_LEN_BYTE, 5, 2);
    col.precision = 5;
    OwlMssqlCell cell = make_cell(bytes, 5);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "-7.50") == 0,
                                                                "DECIMAL -7.50");
}

static void test_decimal_zero(void) {
    uint8_t bytes[] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
    OwlMssqlColumn col = make_col(0x6A, TDS_LEN_BYTE, 5, 2);
    col.precision = 5;
    OwlMssqlCell cell = make_cell(bytes, 5);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_STRING(v) && strcmp(AS_STRING(v)->chars, "0.00") == 0,
                                                                "DECIMAL 0.00");
}

/* ================================================================
 * Type non supporte -> NIL
 * ================================================================ */

static void test_unsupported_returns_nil(void) {
    uint8_t bytes[] = { 0, 0, 0, 0 };
    OwlMssqlColumn col = make_col(0xF0 /* UDT non supporte */,
                                    TDS_LEN_FIXED, 4, 0);
    OwlMssqlCell cell = make_cell(bytes, 4);
    Value v = owl_mssql_cell_to_owl(&col, &cell);
    CHECK(IS_NIL(v),                                             "Type non supporte -> NIL");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_types_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    ensure_vm();

    test_int4();
    test_int4_negative();
    test_bigint();
    test_intn_variants();
    test_bit();
    test_float();
    test_money();
    test_guid();
    test_date();
    test_time_scale7();
    test_time_scale0();
    test_datetime();
    test_null();
    test_nvarchar();
    test_nvarchar_accent();
    test_varchar();
    test_varbinary();
    test_decimal_simple();
    test_decimal_negative();
    test_decimal_zero();
    test_unsupported_returns_nil();

    printf("sql_mssql_types self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
