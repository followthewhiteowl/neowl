// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du module query MSSQL : pack SQLBatch + parse response stream
 * (COLMETADATA, ROW, NBCROW, DONE, ERROR). */

#include "sql_mssql_query.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * SQLBatch pack
 * ================================================================ */

static void test_pack_simple_query(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 128);

    CHECK(owl_mssql_query_pack(&b, "SELECT 1", 0),  "pack SELECT 1");

    /* ALL_HEADERS : TotalLength=22, Header Length=18, Type=0x0002,
     * TxnDesc=0, OutstandingReq=1 -> 22 octets.
     * Puis "SELECT 1" en UCS-2 LE = 8 chars * 2 = 16 octets.
     * Total = 38. */
    CHECK(b.length == 38,                            "total 38 octets");

    /* TotalLength u32 LE = 22 */
    CHECK(b.data[0] == 22 && b.data[1] == 0
       && b.data[2] == 0  && b.data[3] == 0,         "ALL_HEADERS.TotalLength=22");

    /* Header.Length u32 LE = 18 a offset 4 */
    CHECK(b.data[4] == 18,                            "Header.Length=18");

    /* Header.Type u16 LE = 0x0002 a offset 8 */
    CHECK(b.data[8] == 0x02 && b.data[9] == 0x00,    "Header.Type=TransactionDesc");

    /* TransactionDescriptor u64 LE = 0 a offset 10 */
    bool td_zero = true;
    for (int i = 0; i < 8; ++i) if (b.data[10 + i] != 0) td_zero = false;
    CHECK(td_zero,                                    "TransactionDesc=0");

    /* OutstandingRequestCount u32 LE = 1 a offset 18 */
    CHECK(b.data[18] == 1 && b.data[19] == 0,        "OutstandingReq=1");

    /* SQL text "SELECT 1" en UCS-2 LE a partir de offset 22 */
    CHECK(b.data[22] == 'S' && b.data[23] == 0,      "SQL[0]='S' UCS-2");
    CHECK(b.data[24] == 'E' && b.data[25] == 0,      "SQL[1]='E' UCS-2");
    CHECK(b.data[36] == '1' && b.data[37] == 0,      "SQL[7]='1' UCS-2");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Parse : DML simple (CREATE TABLE) → DONE + rowcount
 * ================================================================ */

static void test_parse_empty_done(void) {
    /* Juste un DONE (status=0, curCmd=0, rowCount=5). */
    uint8_t payload[] = {
        0xFD,
        0x00, 0x00,   /* status */
        0x00, 0x00,   /* curCmd */
        0x05, 0, 0, 0, 0, 0, 0, 0,   /* rowCount u64 LE = 5 */
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse DONE OK");
    CHECK(r.kind == TDS_RESULT_EMPTY,                      "kind=EMPTY");
    CHECK(r.done_received,                                  "done_received");
    CHECK(r.rows_affected == 5,                             "rows_affected=5");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : ERROR token
 * ================================================================ */

static void test_parse_error(void) {
    uint8_t payload[] = {
        0xAA,
        0x18, 0x00,                            /* Length=24 */
        0xD2, 0x00, 0x00, 0x00,                /* Number=210 */
        0x01,                                   /* State */
        0x0E,                                   /* Class */
        0x05, 0x00,                             /* MsgLen=5 */
        'O',0, 'o',0, 'p',0, 's',0, '!',0,
        0x00,                                   /* ServerLen=0 */
        0x00,                                   /* ProcLen=0 */
        0x01, 0x00, 0x00, 0x00,                /* LineNumber=1 */
        0xFD,
        0x02, 0x00, 0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse ERROR OK");
    CHECK(r.kind == TDS_RESULT_ERROR,                      "kind=ERROR");
    CHECK(r.error_number == 210,                            "err number 210");
    CHECK(strcmp(r.error_message, "Oops!") == 0,            "err msg");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : SELECT avec 1 colonne INT4 + 2 lignes
 * ================================================================ */

static void test_parse_select_int4(void) {
    /* COLMETADATA:
     *   token 0x81, count=1
     *   col0: UserType u32=0, Flags u16=0x0009 (nullable+...), Type=0x38 (INT4)
     *         NameLen=2 chars, Name "id" UCS-2
     *
     * Row1 (INT4 = 4 bytes, value 42 LE)
     * Row2 (INT4 = 4 bytes, value 100 LE)
     * DONE rowCount=2
     */
    uint8_t payload[] = {
        /* COLMETADATA */
        0x81,
        0x01, 0x00,                       /* count = 1 */
        0x00, 0x00, 0x00, 0x00,           /* UserType = 0 */
        0x09, 0x00,                        /* Flags */
        0x38,                              /* Type = INT4 (FIXEDLEN 4) */
        0x02,                              /* Name length = 2 chars */
        'i',0, 'd',0,
        /* ROW 1 : value 42 LE = 2A 00 00 00 */
        0xD1,
        0x2A, 0x00, 0x00, 0x00,
        /* ROW 2 : value 100 LE = 64 00 00 00 */
        0xD1,
        0x64, 0x00, 0x00, 0x00,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse SELECT INT4 OK");
    CHECK(r.kind == TDS_RESULT_ROWS,                       "kind=ROWS");
    CHECK(r.column_count == 1,                              "1 col");
    CHECK(strcmp(r.columns[0].name, "id") == 0,             "col name 'id'");
    CHECK(r.columns[0].type_code == 0x38,                   "col type INT4");
    CHECK(r.columns[0].len_cat == TDS_LEN_FIXED,            "FIXEDLEN");
    CHECK(r.columns[0].fixed_size == 4,                     "fixed_size 4");
    CHECK(r.row_count == 2,                                 "2 rows");
    CHECK(!r.cells[0].is_null && r.cells[0].len == 4,       "row[0] not null, 4 bytes");
    /* Verifie valeur 42 LE */
    CHECK(r.cells[0].data[0] == 0x2A && r.cells[0].data[1] == 0x00, "row[0] = 42");
    CHECK(r.cells[1].data[0] == 0x64,                       "row[1] = 100");
    CHECK(r.rows_affected == 2,                             "rows_affected=2");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : SELECT avec NVARCHAR + NULL
 * ================================================================ */

static void test_parse_select_nvarchar_null(void) {
    /* COLMETADATA :
     *   token 0x81, count=1
     *   col0: usertype=0, flags=0x0009 (nullable)
     *         Type=0xE7 (NVARCHAR USHORTLEN, needs_collation)
     *         max_len u16 = 200 (0xC8 0x00)
     *         collation 5 octets (zero pour test)
     *         NameLen=4, name="name"
     *
     * Row1 : len=8 -> 4 chars UCS-2 "Alic"... hmm OK je vais faire "Hi"
     *        len=4 octets pour 2 chars UCS-2 "Hi" = 48 00 69 00
     * Row2 : NULL via len=0xFFFF
     *
     * DONE rowCount=2
     */
    uint8_t payload[] = {
        0x81,
        0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,           /* usertype */
        0x09, 0x00,                        /* flags */
        0xE7,                              /* NVARCHAR */
        0xC8, 0x00,                        /* max_len 200 */
        0x00, 0x00, 0x00, 0x00, 0x00,      /* collation 5 bytes */
        0x04,                              /* name len 4 */
        'n',0, 'a',0, 'm',0, 'e',0,
        /* ROW 1 : len=4 (octets), "Hi" UCS-2 */
        0xD1,
        0x04, 0x00,
        'H',0, 'i',0,
        /* ROW 2 : NULL via len=0xFFFF */
        0xD1,
        0xFF, 0xFF,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse NVARCHAR OK");
    CHECK(r.column_count == 1,                              "1 col");
    CHECK(r.columns[0].type_code == 0xE7,                   "NVARCHAR type");
    CHECK(r.columns[0].len_cat == TDS_LEN_USHORT,           "USHORTLEN");
    CHECK(r.columns[0].has_collation,                       "has collation");
    CHECK(r.row_count == 2,                                 "2 rows");
    CHECK(!r.cells[0].is_null && r.cells[0].len == 4,       "row[0] not null, 4 bytes");
    CHECK(r.cells[1].is_null,                               "row[1] = NULL");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : NBCROW (compressed null-bitmap)
 * ================================================================ */

static void test_parse_nbcrow(void) {
    /* 3 colonnes INT4. bitmap = 1 byte. Si col1=NULL et col0/col2 OK,
     * bitmap = 0b00000010 = 0x02. */
    uint8_t payload[] = {
        0x81,
        0x03, 0x00,                       /* 3 cols */
        /* col0 INT4 */
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x38,
        0x01, 'a', 0,
        /* col1 INT4 */
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x38,
        0x01, 'b', 0,
        /* col2 INT4 */
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x38,
        0x01, 'c', 0,
        /* NBCROW */
        0xD2,
        0x02,                              /* bitmap = 0b00000010 = col1 NULL */
        /* col0 = 10 */
        0x0A, 0x00, 0x00, 0x00,
        /* col1 = SKIPPED (NULL bitmap) */
        /* col2 = 20 */
        0x14, 0x00, 0x00, 0x00,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse NBCROW OK");
    CHECK(r.column_count == 3,                              "3 cols");
    CHECK(r.row_count == 1,                                 "1 row");
    CHECK(!r.cells[0].is_null,                              "cell[0] not null");
    CHECK(r.cells[1].is_null,                               "cell[1] NULL via bitmap");
    CHECK(!r.cells[2].is_null,                              "cell[2] not null");
    CHECK(r.cells[0].data[0] == 0x0A,                       "cell[0] = 10");
    CHECK(r.cells[2].data[0] == 0x14,                       "cell[2] = 20");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : INTN (BYTELEN, variable width per row)
 * ================================================================ */

static void test_parse_intn(void) {
    /* INTN (0x26) avec max_len=4. Lignes peuvent avoir len=0 (NULL),
     * len=1, len=2, len=4 selon valeur. */
    uint8_t payload[] = {
        0x81,
        0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
        0x26,                              /* INTN */
        0x04,                              /* max_len = 4 */
        0x01, 'n', 0,
        /* Row 1 : len=4, value 1000 LE */
        0xD1,
        0x04,                              /* bytelen */
        0xE8, 0x03, 0x00, 0x00,
        /* Row 2 : NULL via len=0 */
        0xD1,
        0x00,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse INTN OK");
    CHECK(r.columns[0].type_code == 0x26,                   "INTN");
    CHECK(r.columns[0].len_cat == TDS_LEN_BYTE,             "BYTELEN");
    CHECK(r.row_count == 2,                                 "2 rows");
    CHECK(!r.cells[0].is_null && r.cells[0].len == 4,       "row[0] 4 bytes");
    CHECK(r.cells[0].data[0] == 0xE8 && r.cells[0].data[1] == 0x03, "value 1000");
    CHECK(r.cells[1].is_null,                               "row[1] NULL");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : DECIMAL avec precision/scale
 * ================================================================ */

static void test_parse_decimal(void) {
    /* DECIMALN (0x6A) avec max_len=17, precision=10, scale=2.
     * Row1 : len=5 (sign+4 bytes mantissa) — on ne decode pas la valeur,
     * juste verifier que la metadata + len lecture marche. */
    uint8_t payload[] = {
        0x81,
        0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
        0x6A,                              /* DECIMALN */
        0x11,                              /* max_len = 17 */
        0x0A,                              /* precision = 10 */
        0x02,                              /* scale = 2 */
        0x01, 'd', 0,
        /* Row : len=5 */
        0xD1,
        0x05,
        0x01, 0xE8, 0x03, 0x00, 0x00,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse DECIMAL OK");
    CHECK(r.columns[0].type_code == 0x6A,                   "DECIMALN");
    CHECK(r.columns[0].precision == 10,                     "precision 10");
    CHECK(r.columns[0].scale == 2,                          "scale 2");
    CHECK(r.cells[0].len == 5,                              "cell 5 bytes");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : DATETIME2 (TDS_LEN_BYTE avec scale prefix)
 * ================================================================ */

static void test_parse_datetime2(void) {
    /* DATETIME2N (0x2A) : scale (u8) AVANT max_len (u8). */
    uint8_t payload[] = {
        0x81,
        0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
        0x2A,                              /* DATETIME2N */
        0x07,                              /* scale = 7 */
        0x08,                              /* max_len = 8 */
        0x01, 't', 0,
        /* Row : len=8 (DATETIME2(7) sur 8 octets : 5 time + 3 date) */
        0xD1,
        0x08,
        0,0,0,0,0,0, 0,0,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse DATETIME2 OK");
    CHECK(r.columns[0].type_code == 0x2A,                   "DATETIME2N");
    CHECK(r.columns[0].scale == 7,                          "scale 7");
    CHECK(r.cells[0].len == 8,                              "cell 8 bytes");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : token non supporte -> erreur claire
 * ================================================================ */

static void test_parse_unsupported_token(void) {
    uint8_t payload[] = { 0xF0 };   /* UDT — non supporte V1 */
    OwlMssqlResult r;
    char err[128];
    CHECK(!owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "unsupported token rejected");
    CHECK(err[0] != '\0',                                   "err msg fourni");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : ROW sans COLMETADATA prealable -> erreur
 * ================================================================ */

static void test_parse_row_without_metadata(void) {
    uint8_t payload[] = { 0xD1, 0x00, 0x00, 0x00, 0x00 };
    OwlMssqlResult r;
    char err[128];
    CHECK(!owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "row without meta rejected");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Parse : ENVCHANGE intercale (genre USE database) — skippe proprement
 * ================================================================ */

static void test_parse_envchange_skipped(void) {
    uint8_t payload[] = {
        /* ENVCHANGE database (type=1), new="db1", old="".
         * Length = 1(EnvType) + 1(newLen) + 6("db1") + 1(oldLen=0) = 9 */
        0xE3,
        0x09, 0x00,                            /* Length=9 */
        0x01,                                   /* type DATABASE */
        0x03,                                   /* newLen 3 chars */
        'd', 0, 'b', 0, '1', 0,
        0x00,                                   /* oldLen 0 */
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "envchange skipped");
    CHECK(r.kind == TDS_RESULT_EMPTY,                      "kind=EMPTY post-envchange");
    CHECK(r.done_received,                                  "done received");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * Round-trip simple : on construit un faux payload server-like et
 * verifie qu'on le reparse correctement
 * ================================================================ */

static void test_parse_two_columns_two_rows(void) {
    /* SELECT id, name FROM ... avec 2 lignes (1,"A") et (2,"BB")
     *
     * col 0 : INT4 (FIXEDLEN 4), name="id"
     * col 1 : NVARCHAR USHORTLEN, max=200, name="name"
     */
    uint8_t payload[] = {
        /* COLMETADATA count=2 */
        0x81,
        0x02, 0x00,
        /* col0 */
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x38,
        0x02, 'i', 0, 'd', 0,
        /* col1 */
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0xE7,
        0xC8, 0x00,
        0,0,0,0,0,
        0x04, 'n', 0, 'a', 0, 'm', 0, 'e', 0,
        /* ROW 1: id=1, name="A" */
        0xD1,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 'A', 0,
        /* ROW 2: id=2, name="BB" */
        0xD1,
        0x02, 0x00, 0x00, 0x00,
        0x04, 0x00, 'B', 0, 'B', 0,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0, 0, 0, 0, 0, 0, 0,
    };
    OwlMssqlResult r;
    char err[128];
    CHECK(owl_mssql_query_parse_response(payload, sizeof(payload), &r, err),
                                                          "2 cols 2 rows OK");
    CHECK(r.column_count == 2,                              "2 cols");
    CHECK(r.row_count == 2,                                 "2 rows");
    CHECK(strcmp(r.columns[0].name, "id") == 0,             "col0='id'");
    CHECK(strcmp(r.columns[1].name, "name") == 0,           "col1='name'");
    /* row[0].id = 1 */
    CHECK(r.cells[0].data[0] == 0x01,                       "r0.id=1");
    /* row[0].name = "A" (len=2) */
    CHECK(r.cells[1].len == 2 && r.cells[1].data[0] == 'A', "r0.name='A'");
    /* row[1].name = "BB" (len=4) */
    CHECK(r.cells[3].len == 4 && r.cells[3].data[2] == 'B', "r1.name='BB'");
    owl_mssql_result_free(&r);
}

/* ================================================================
 * RPC sp_executesql — pack
 * ================================================================ */

static void test_rpc_pack_no_params(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);
    bool ok = owl_mssql_query_pack_rpc(&b, "SELECT 1", NULL, 0, 0);
    CHECK(ok,                                                "rpc pack no params");

    /* Structure attendue :
     * Offset  Bytes
     * 0       ALL_HEADERS (22 octets) : 16 00 00 00 12 00 00 00 02 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00
     * 22      ProcID : FF FF 0A 00 (0xFFFF + procID=10)
     * 26      OptionFlags : 00 00
     * 28      param @stmt :
     *         ParamNameLen u8 = 5 ('@stmt')
     *         Name UCS-2 LE (10 octets) "@stmt"
     *         StatusFlags u8 = 0
     *         TypeInfo : E7 (NVARCHAR) + max_len u16 = 1F40 (8000)
     *         Collation 5 octets zero
     *         Length u16 LE = 16 (chars "SELECT 1" * 2 = 16 octets)
     *         UCS-2 LE "SELECT 1"
     * ...    @params = NULL
     */
    CHECK(b.data[0] == 22,                                    "ALL_HEADERS.TotalLength");
    CHECK(b.data[22] == 0xFF && b.data[23] == 0xFF,          "ProcID 0xFFFF");
    CHECK(b.data[24] == 10 && b.data[25] == 0,                "procID=10 (sp_executesql)");
    CHECK(b.data[26] == 0 && b.data[27] == 0,                 "OptionFlags=0");
    /* @stmt name */
    CHECK(b.data[28] == 5,                                    "param[0] name_len=5");
    CHECK(b.data[29] == '@' && b.data[30] == 0,               "name[0]='@'");
    CHECK(b.data[31] == 's' && b.data[33] == 't',             "name=stmt");
    /* StatusFlags + TypeInfo NVARCHAR */
    /* offset 28 + 1 + 10 = 39 */
    CHECK(b.data[39] == 0,                                    "StatusFlags=0");
    CHECK(b.data[40] == 0xE7,                                 "type NVARCHAR");
    /* max_len u16 LE = 8000 (max NVARCHAR sans MAX) */
    uint16_t maxlen = (uint16_t)(b.data[41] | (b.data[42] << 8));
    CHECK(maxlen == 8000,                                     "max_bytes=8000");
    /* Collation 5 octets a offset 43 */
    CHECK(b.data[43] == 0 && b.data[44] == 0 && b.data[47] == 0,
                                                              "collation zeros");

    owl_le_buffer_free(&b);
}

static void test_rpc_pack_with_int_param(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);
    OwlMssqlRpcParam params[1] = {
        { .name = "P1", .type = OWL_MSSQL_PARAM_INT, .i_value = 42 },
    };
    bool ok = owl_mssql_query_pack_rpc(&b,
        "SELECT * FROM t WHERE id = @P1", params, 1, 0);
    CHECK(ok,                                                 "rpc pack int param");

    /* Doit contenir le @P1 BIGINT param a la fin. INTNTYPE = 0x26.
     * Cherche le pattern 0x26 0x08 (type + max_len). */
    int found_intn = 0;
    for (size_t i = 30; i + 10 < b.length; ++i) {
        if (b.data[i] == 0x26 && b.data[i+1] == 0x08) {
            /* Le precedent doit etre StatusFlags=0, et avant ca name "@P1" */
            /* Verifie aussi qu'il y a un actual_len + valeur 42 LE apres */
            if (b.data[i+2] == 0x08 &&
                b.data[i+3] == 0x2A) {
                found_intn = 1;
                break;
            }
        }
    }
    CHECK(found_intn,                                         "INTN(8)=42 trouve dans le RPC");

    owl_le_buffer_free(&b);
}

static void test_rpc_pack_string_param(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);
    const char* hello = "Hi";
    OwlMssqlRpcParam params[1] = {
        { .name = "P1", .type = OWL_MSSQL_PARAM_STRING_UTF8,
          .bytes = (const uint8_t*)hello, .bytes_len = 2 },
    };
    bool ok = owl_mssql_query_pack_rpc(&b,
        "SELECT @P1", params, 1, 0);
    CHECK(ok,                                                 "rpc pack string");

    /* Cherche la sequence UCS-2 LE 'H', 0, 'i', 0 dans le payload. */
    int found = 0;
    for (size_t i = 0; i + 3 < b.length; ++i) {
        if (b.data[i] == 'H' && b.data[i+1] == 0
         && b.data[i+2] == 'i' && b.data[i+3] == 0) {
            found++;
        }
    }
    /* Devrait apparaitre 1x dans le SQL "SELECT @P1" (pas 'H' literal, donc 0
     * pour le SQL) et 1x dans la valeur du @P1. Le SQL ne contient pas 'Hi'
     * donc 1 occurrence attendue. */
    CHECK(found >= 1,                                         "valeur 'Hi' UCS-2 presente");

    owl_le_buffer_free(&b);
}

static void test_rpc_pack_null_param(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);
    OwlMssqlRpcParam params[1] = {
        { .name = "P1", .type = OWL_MSSQL_PARAM_NULL },
    };
    bool ok = owl_mssql_query_pack_rpc(&b,
        "UPDATE t SET x = @P1", params, 1, 0);
    CHECK(ok,                                                 "rpc pack NULL");

    /* NULL NVARCHAR : length u16 = 0xFFFF doit apparaitre quelque part. */
    int found_null = 0;
    for (size_t i = 30; i + 1 < b.length; ++i) {
        if (b.data[i] == 0xFF && b.data[i+1] == 0xFF) {
            found_null++;
        }
    }
    /* Au moins une (le NULL @P1). Le @params peut aussi etre NULL si pas
     * declare — dans notre code on declare quand n_params > 0, donc params
     * a "NVARCHAR" pas NULL. Donc exactement 1 NULL attendu. */
    CHECK(found_null >= 1,                                    "marqueur NULL 0xFFFF present");

    owl_le_buffer_free(&b);
}

static void test_rpc_pack_bool_real(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);
    OwlMssqlRpcParam params[2] = {
        { .name = "P1", .type = OWL_MSSQL_PARAM_BOOL, .b_value = true },
        { .name = "P2", .type = OWL_MSSQL_PARAM_REAL, .r_value = 3.14 },
    };
    bool ok = owl_mssql_query_pack_rpc(&b,
        "SELECT @P1, @P2", params, 2, 0);
    CHECK(ok,                                                 "rpc pack bool+real");

    /* Recherche BITN 0x68 et FLTN 0x6D dans le payload. */
    int has_bit = 0, has_flt = 0;
    for (size_t i = 0; i + 3 < b.length; ++i) {
        if (b.data[i] == 0x68 && b.data[i+1] == 0x01
         && b.data[i+2] == 0x01 && b.data[i+3] == 0x01) {
            has_bit = 1;
        }
        if (b.data[i] == 0x6D && b.data[i+1] == 0x08 && b.data[i+2] == 0x08) {
            has_flt = 1;
        }
    }
    CHECK(has_bit,                                            "BITN=1 (true) trouve");
    CHECK(has_flt,                                            "FLTN(8) trouve");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_query_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_pack_simple_query();
    test_parse_empty_done();
    test_parse_error();
    test_parse_select_int4();
    test_parse_select_nvarchar_null();
    test_parse_nbcrow();
    test_parse_intn();
    test_parse_decimal();
    test_parse_datetime2();
    test_parse_unsupported_token();
    test_parse_row_without_metadata();
    test_parse_envchange_skipped();
    test_parse_two_columns_two_rows();
    test_rpc_pack_no_params();
    test_rpc_pack_with_int_param();
    test_rpc_pack_string_param();
    test_rpc_pack_null_param();
    test_rpc_pack_bool_real();

    printf("sql_mssql_query self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
