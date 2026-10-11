// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du PreLogin TDS : pack/parse + round-trip + edge cases. */

#include "sql_mssql_prelogin.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Pack : structure minimale (4 tokens, sans INSTOPT)
 * ================================================================ */

static void test_pack_minimal(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 64);
    OwlTdsPreloginRequest req = {
        .version_major   = 1,
        .version_minor   = 0,
        .version_build   = 0,
        .version_subbuild = 0,
        .encryption      = TDS_ENCRYPT_ON,
        .instance        = NULL,
        .thread_id       = 0,
        .mars            = false,
    };
    CHECK(owl_tds_prelogin_pack(&b, &req), "pack minimal OK");

    /* 4 descripteurs * 5 + 1 terminator = 21 octets desc
     * + 6 (VERSION) + 1 (ENCRYPTION) + 4 (THREADID) + 1 (MARS) = 12 octets data
     * Total = 33 */
    CHECK(b.length == 33, "taille totale 33 octets");

    /* Descripteur 1 : VERSION token 0x00, offset 21, length 6 */
    CHECK(b.data[0] == 0x00,                    "desc1.type = VERSION");
    CHECK((b.data[1] << 8 | b.data[2]) == 21,   "desc1.offset = 21 (BE)");
    CHECK((b.data[3] << 8 | b.data[4]) == 6,    "desc1.length = 6 (BE)");

    /* Descripteur 2 : ENCRYPTION token 0x01, offset 27, length 1 */
    CHECK(b.data[5] == 0x01,                     "desc2.type = ENCRYPTION");
    CHECK((b.data[6] << 8 | b.data[7]) == 27,    "desc2.offset = 27");
    CHECK((b.data[8] << 8 | b.data[9]) == 1,     "desc2.length = 1");

    /* Descripteur 3 : THREADID token 0x03, offset 28, length 4 */
    CHECK(b.data[10] == 0x03,                    "desc3.type = THREADID");
    CHECK((b.data[11] << 8 | b.data[12]) == 28,  "desc3.offset = 28");

    /* Descripteur 4 : MARS token 0x04, offset 32, length 1 */
    CHECK(b.data[15] == 0x04,                    "desc4.type = MARS");
    CHECK((b.data[16] << 8 | b.data[17]) == 32,  "desc4.offset = 32");

    /* Terminator */
    CHECK(b.data[20] == 0xFF,                    "terminator @ offset 20");

    /* Data zone : VERSION a partir de l'offset 21 */
    CHECK(b.data[21] == 1 && b.data[22] == 0,    "VERSION major.minor = 1.0");

    /* ENCRYPTION = ON (0x01) a l'offset 27 */
    CHECK(b.data[27] == 0x01,                    "ENCRYPTION = ON");

    /* MARS = 0 a l'offset 32 */
    CHECK(b.data[32] == 0x00,                    "MARS = 0");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Pack : avec INSTOPT
 * ================================================================ */

static void test_pack_with_instance(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 128);
    OwlTdsPreloginRequest req = {
        .version_major   = 4,
        .version_minor   = 0,
        .version_build   = 1234,
        .encryption      = TDS_ENCRYPT_ON,
        .instance        = "SQLEXPRESS",
        .thread_id       = 0xCAFEBABE,
        .mars            = false,
    };
    CHECK(owl_tds_prelogin_pack(&b, &req), "pack with instance OK");

    /* 5 desc * 5 + 1 term = 26 octets desc
     * + 6 VERSION + 1 ENCRYPTION + 11 INSTOPT("SQLEXPRESS\0") + 4 THREADID + 1 MARS = 23
     * Total = 49 */
    CHECK(b.length == 49,                          "taille totale 49 octets");

    /* Le 3eme descripteur doit etre INSTOPT (0x02) */
    CHECK(b.data[10] == 0x02,                       "desc3 = INSTOPT");

    /* THREADID en BE : 0xCAFEBABE -> CA FE BA BE
     * Offset THREADID = desc_size (26) + 6 + 1 + 11 = 44 */
    CHECK(b.data[44] == 0xCA && b.data[45] == 0xFE
       && b.data[46] == 0xBA && b.data[47] == 0xBE, "THREADID BE bytes");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Parse : reponse serveur typique
 * ================================================================ */

static void test_parse_typical_response(void) {
    /* Reponse construite a la main : VERSION + ENCRYPTION=ON + MARS=0.
     *
     *   Desc table (3 tokens) :
     *     00 00 10 00 06        VERSION   offset=16 len=6
     *     01 00 16 00 01        ENCRYPT   offset=22 len=1
     *     04 00 17 00 01        MARS      offset=23 len=1
     *     FF                    terminator
     *   Data :
     *     0F 00 0B B8 00 00     VERSION 15.0.3000 sub=0
     *     01                    ENCRYPT_ON
     *     00                    MARS off
     */
    uint8_t resp[] = {
        0x00, 0x00,0x10, 0x00,0x06,
        0x01, 0x00,0x16, 0x00,0x01,
        0x04, 0x00,0x17, 0x00,0x01,
        0xFF,
        /* offset 16 */
        0x0F, 0x00, 0x0B,0xB8, 0x00,0x00,
        /* offset 22 */
        0x01,
        /* offset 23 */
        0x00,
    };
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(owl_tds_prelogin_parse(resp, sizeof(resp), &r, err),  "parse OK");
    CHECK(r.version_present,                                     "VERSION present");
    CHECK(r.version_major == 15 && r.version_minor == 0,         "VERSION 15.0");
    CHECK(r.version_build == 3000,                                "VERSION build 3000");
    CHECK(r.encryption_present,                                   "ENCRYPTION present");
    CHECK(r.encryption == TDS_ENCRYPT_ON,                         "ENCRYPT = ON");
    CHECK(r.mars_present && r.mars_value == false,                "MARS = off");
    CHECK(r.fedauth_present == false,                             "FEDAUTH absent");
}

/* ================================================================
 * Parse : reponse minimaliste (VERSION + ENCRYPTION seuls)
 * ================================================================ */

static void test_parse_minimal(void) {
    uint8_t resp[] = {
        0x00, 0x00,0x0B, 0x00,0x06,   /* VERSION desc, offset 11 */
        0x01, 0x00,0x11, 0x00,0x01,   /* ENCRYPT desc, offset 17 */
        0xFF,                          /* terminator */
        0x10, 0x00, 0x07,0xD0, 0x00,0x00,   /* VERSION 16.0.2000 */
        0x03,                          /* ENCRYPT_REQ */
    };
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(owl_tds_prelogin_parse(resp, sizeof(resp), &r, err),
                                                                  "parse minimal OK");
    CHECK(r.version_major == 16 && r.version_minor == 0,           "v16.0");
    CHECK(r.encryption == TDS_ENCRYPT_REQ,                          "ENCRYPT_REQ");
    CHECK(!r.mars_present && !r.fedauth_present,                    "optionals absent");
}

/* ================================================================
 * Parse : tokens inconnus ignores (forward compat)
 * ================================================================ */

static void test_parse_unknown_tokens_ignored(void) {
    uint8_t resp[] = {
        0x42, 0x00,0x0B, 0x00,0x02,   /* token 0x42 inconnu */
        0x01, 0x00,0x0D, 0x00,0x01,   /* ENCRYPT */
        0xFF,
        /* offset 11 */
        0xAA, 0xBB,                   /* donnees du token inconnu */
        /* offset 13 */
        0x00,                          /* ENCRYPT_OFF */
    };
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(owl_tds_prelogin_parse(resp, sizeof(resp), &r, err),
                                                                  "parse ignore token inconnu");
    CHECK(r.encryption == TDS_ENCRYPT_OFF,                          "ENCRYPT decoded");
}

/* ================================================================
 * Parse : descripteur tronque
 * ================================================================ */

static void test_parse_truncated_desc(void) {
    /* Descripteur incomplet (3 octets au lieu de 5) avant terminator */
    uint8_t bad[] = {
        0x00, 0x00, 0x10,
    };
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(!owl_tds_prelogin_parse(bad, sizeof(bad), &r, err),
                                                                  "tronque rejete");
    CHECK(err[0] != '\0',                                           "err msg fourni");
}

/* ================================================================
 * Parse : offset hors-bornes
 * ================================================================ */

static void test_parse_offset_out_of_bounds(void) {
    /* VERSION declare offset=100 length=6 mais payload fait 12 octets. */
    uint8_t bad[] = {
        0x00, 0x00, 0x64, 0x00, 0x06,
        0xFF,
        0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED,
    };
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(!owl_tds_prelogin_parse(bad, sizeof(bad), &r, err),
                                                                  "OOB rejete");
}

/* ================================================================
 * Round-trip : on emet puis on parse, on doit retrouver les valeurs
 * ================================================================ */

static void test_roundtrip(void) {
    OwlTdsPreloginRequest req = {
        .version_major    = 0x12,
        .version_minor    = 0x34,
        .version_build    = 0x5678,
        .version_subbuild = 0x9ABC,
        .encryption       = TDS_ENCRYPT_ON,
        .instance         = NULL,
        .thread_id        = 0,
        .mars             = false,
    };
    OwlLeBuffer b; owl_le_buffer_init(&b, 64);
    CHECK(owl_tds_prelogin_pack(&b, &req), "rt pack");

    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(owl_tds_prelogin_parse(b.data, b.length, &r, err), "rt parse");

    CHECK(r.version_major == 0x12,         "rt version_major");
    CHECK(r.version_minor == 0x34,         "rt version_minor");
    CHECK(r.version_build == 0x5678,       "rt version_build BE");
    CHECK(r.version_subbuild == 0x9ABC,    "rt version_subbuild LE");
    CHECK(r.encryption == TDS_ENCRYPT_ON,  "rt encryption");
    CHECK(r.mars_present && !r.mars_value, "rt mars = off");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Parse : payload vide
 * ================================================================ */

static void test_parse_empty(void) {
    OwlTdsPreloginResponse r;
    char err[128];
    CHECK(!owl_tds_prelogin_parse(NULL, 0, &r, err),
                                                                  "vide rejete");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_prelogin_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_pack_minimal();
    test_pack_with_instance();
    test_parse_typical_response();
    test_parse_minimal();
    test_parse_unknown_tokens_ignored();
    test_parse_truncated_desc();
    test_parse_offset_out_of_bounds();
    test_roundtrip();
    test_parse_empty();

    printf("sql_mssql_prelogin self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
