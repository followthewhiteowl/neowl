// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test Login7 : password obfuscation, packing offsets/longueurs,
 * UCS-2 encoding, parsing reponse (LOGINACK / ERROR / ENVCHANGE / DONE). */

#include "sql_mssql_login.h"

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
 * Password obfuscation : valeurs connues et invariants
 * ================================================================ */

static void test_obfuscate_known_vectors(void) {
    /* Obfusque : swap_nibbles(b) XOR 0xA5
     *   0x00 -> swap=0x00 XOR A5 = 0xA5
     *   0xFF -> swap=0xFF XOR A5 = 0x5A
     *   0x12 -> swap=0x21 XOR A5 = 0x84
     *   0xA5 -> swap=0x5A XOR A5 = 0xFF
     *   0x5A -> swap=0xA5 XOR A5 = 0x00
     */
    uint8_t data[] = {0x00, 0xFF, 0x12, 0xA5, 0x5A};
    owl_mssql_login_obfuscate(data, sizeof(data));
    CHECK(data[0] == 0xA5, "obfuscate(0x00) = 0xA5");
    CHECK(data[1] == 0x5A, "obfuscate(0xFF) = 0x5A");
    CHECK(data[2] == 0x84, "obfuscate(0x12) = 0x84");
    CHECK(data[3] == 0xFF, "obfuscate(0xA5) = 0xFF");
    CHECK(data[4] == 0x00, "obfuscate(0x5A) = 0x00");
}

/* ================================================================
 * Pack : structure d'un Login7 minimal
 * ================================================================ */

static void test_pack_minimal_structure(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);

    OwlMssqlLoginRequest req = {
        .hostname    = "H",      /* 1 char  -> 2 octets UCS-2 */
        .username    = "sa",     /* 2 chars -> 4 octets */
        .password    = "x",      /* 1 char  -> 2 octets obfusques */
        .appname     = NULL,     /* defaut "OWL" 3 chars -> 6 octets */
        .servername  = NULL,
        .database    = "db",     /* 2 chars -> 4 octets */
        .language    = NULL,
        .clt_int_name= NULL,     /* defaut "owl-mssql" 9 chars -> 18 octets */
        .packet_size = 4096,
        .client_pid  = 1234,
        .client_id   = {0,0,0,0,0,0},
    };
    CHECK(owl_mssql_login_pack(&b, &req), "pack minimal OK");

    /* Length total declared = 94 fixe + (1+2+1+3+0+9+0+2) chars * 2
     *                       = 94 + 18*2 = 94 + 36 = 130. */
    uint32_t total = (uint32_t)(b.data[0] | (b.data[1] << 8)
                                | (b.data[2] << 16) | (b.data[3] << 24));
    CHECK(total == 130,                       "Length field = 130");
    CHECK(b.length == 130,                     "buffer size == Length");

    /* TDSVersion @ offset 4 = 0x74000004 LE */
    CHECK(b.data[4] == 0x04 && b.data[5] == 0x00
       && b.data[6] == 0x00 && b.data[7] == 0x74,  "TDSVersion 7.4");

    /* PacketSize @ offset 8 = 4096 LE = 00 10 00 00 */
    CHECK(b.data[8] == 0x00 && b.data[9] == 0x10
       && b.data[10] == 0x00 && b.data[11] == 0x00, "PacketSize 4096");

    /* ClientPID @ offset 16 = 1234 LE = D2 04 00 00 */
    CHECK(b.data[16] == 0xD2 && b.data[17] == 0x04, "ClientPID 1234");

    /* HostName descriptor : offset @ 36 (u16 LE), length-in-chars @ 38 (u16 LE).
     * HostName data commence apres la zone fixe a offset 94, length-in-chars = 1. */
    uint16_t host_off = (uint16_t)(b.data[36] | (b.data[37] << 8));
    uint16_t host_len = (uint16_t)(b.data[38] | (b.data[39] << 8));
    CHECK(host_off == 94,                      "HostNameOffset = 94");
    CHECK(host_len == 1,                       "HostNameLength = 1 (en chars)");

    /* UserNameLength = 2 */
    uint16_t user_len = (uint16_t)(b.data[42] | (b.data[43] << 8));
    CHECK(user_len == 2,                       "UserNameLength = 2");

    /* PasswordOffset = 94 + 1*2 + 2*2 = 100 */
    uint16_t pwd_off = (uint16_t)(b.data[44] | (b.data[45] << 8));
    CHECK(pwd_off == 100,                      "PasswordOffset = 100");

    /* Data area: HostName "H" en UCS-2 LE = 0x48 0x00 a offset 94. */
    CHECK(b.data[94] == 0x48 && b.data[95] == 0x00, "Host[0] = 'H' UCS-2");

    /* UserName "sa" en UCS-2 a offset 96 : 's'=0x73 0x00, 'a'=0x61 0x00 */
    CHECK(b.data[96] == 0x73 && b.data[97] == 0x00, "User[0] = 's'");
    CHECK(b.data[98] == 0x61 && b.data[99] == 0x00, "User[1] = 'a'");

    /* Password "x" en UCS-2 a offset 100, MAIS obfusque.
     * 'x' = 0x78, en UCS-2 = 0x78 0x00.
     * obfuscate(0x78) : swap = 0x87, XOR A5 = 0x22
     * obfuscate(0x00) : swap = 0x00, XOR A5 = 0xA5 */
    CHECK(b.data[100] == 0x22,                  "Pass[0] obfusque");
    CHECK(b.data[101] == 0xA5,                  "Pass[1] obfusque");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Pack : appname/clt_int defaults sont bien injectes
 * ================================================================ */

static void test_pack_defaults_applied(void) {
    OwlLeBuffer b;
    owl_le_buffer_init(&b, 256);

    OwlMssqlLoginRequest req = {
        .hostname    = "h",
        .username    = "u",
        .password    = "p",
        .appname     = NULL,        /* doit devenir "OWL" */
        .clt_int_name= NULL,        /* doit devenir "owl-mssql" */
        .packet_size = 4096,
    };
    CHECK(owl_mssql_login_pack(&b, &req), "pack defaults OK");

    uint16_t app_len = (uint16_t)(b.data[50] | (b.data[51] << 8));
    CHECK(app_len == 3,                        "AppNameLength = 3 ('OWL')");

    uint16_t clt_len = (uint16_t)(b.data[62] | (b.data[63] << 8));
    CHECK(clt_len == 9,                        "CltIntNameLength = 9 ('owl-mssql')");

    owl_le_buffer_free(&b);
}

/* ================================================================
 * Parse : reponse Login7 reussie (LOGINACK + DONE)
 * ================================================================ */

static void test_parse_success(void) {
    /* Reponse construite a la main :
     *   LOGINACK : interface=1, TDSVersion 0x74000004 (BE), ProgName="SQL Server"
     *              ProgVersion 12.0.4100 BE (build).
     *   DONE     : status=0, curCmd=0, rowCount=0.
     *
     * Length LOGINACK :
     *   1 (interf) + 4 (tdsver) + 1 (prog_len) + 10*2 (UCS-2 "SQL Server") + 4 (progver)
     *   = 30 octets
     *
     * Token byte:  AD
     * Length:      1E 00  (30 LE)
     * Interface:   01
     * TDSVersion:  74 00 00 04 (BE comme dans la spec MS-TDS)
     * ProgNameLen: 0A  (10 chars)
     * ProgName:    53 00 51 00 4C 00 20 00 53 00 65 00 72 00 76 00 65 00 72 00  ("SQL Server")
     * ProgVersion: 0C 00 10 04  (12.0, build=0x1004=4100 BE)
     *
     * DONE :
     * Token: FD
     * Status u16 LE: 00 00
     * CurCmd u16 LE: 00 00
     * RowCount u64 LE: 00*8
     */
    uint8_t payload[] = {
        /* LOGINACK */
        0xAD,
        0x1E, 0x00,
        0x01,
        0x74, 0x00, 0x00, 0x04,
        0x0A,
        'S',0, 'Q',0, 'L',0, ' ',0,
        'S',0, 'e',0, 'r',0, 'v',0, 'e',0, 'r',0,
        0x0C, 0x00, 0x10, 0x04,
        /* DONE */
        0xFD,
        0x00, 0x00,
        0x00, 0x00,
        0x00,0,0,0,0,0,0,0,
    };

    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse success OK");
    CHECK(r.success,                                       "login.success = true");
    CHECK(r.tds_version_negotiated == 0x74000004,          "TDS version negotiated");
    CHECK(strcmp(r.server_program, "SQL Server") == 0,     "server_program");
    CHECK(r.server_version_major == 12,                    "version major 12");
    CHECK(r.server_version_minor == 0,                     "version minor 0");
    CHECK(r.server_version_build == 4100,                  "version build 4100");
}

/* ================================================================
 * Parse : reponse Login7 echouee (ERROR)
 * ================================================================ */

static void test_parse_error(void) {
    /* Token ERROR (0xAA) :
     *   Length u16 : valeur computed
     *   Number u32 LE = 18456 (login failed for user)
     *   State u8 = 1
     *   Class u8 = 14
     *   MsgLen u16 LE = 5 (chars)
     *   MsgText UCS-2 = "Hello"
     *   ServerLen u8 = 0
     *   ProcLen u8 = 0
     *   LineNumber u32 LE = 1
     *
     * Length total apres len field = 4+1+1+2 + 10 + 1+0 + 1+0 + 4 = 24
     *
     * Suivi du DONE pour finir le stream proprement
     * (DONE.Status = 2 = ERROR mais on s'en fout, le ERROR token suffit).
     */
    uint8_t payload[] = {
        0xAA,
        0x18, 0x00,                            /* Length = 24 */
        0x18, 0x48, 0x00, 0x00,                /* Number 18456 LE */
        0x01,                                   /* State */
        0x0E,                                   /* Class */
        0x05, 0x00,                             /* MsgLen 5 */
        'H',0, 'e',0, 'l',0, 'l',0, 'o',0,     /* MsgText */
        0x00,                                   /* ServerLen 0 */
        0x00,                                   /* ProcLen 0 */
        0x01, 0x00, 0x00, 0x00,                /* LineNumber 1 */
        /* DONE */
        0xFD,
        0x02, 0x00, 0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };

    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse error OK");
    CHECK(!r.success,                                      "success = false");
    CHECK(r.error_number == 18456,                          "error_number 18456");
    CHECK(r.error_state == 1,                               "error_state 1");
    CHECK(r.error_class == 14,                              "error_class 14");
    CHECK(strcmp(r.error_message, "Hello") == 0,            "error message decoded");
    CHECK(r.error_line == 1,                                "error_line 1");
}

/* ================================================================
 * Parse : ENVCHANGE PACKET_SIZE
 * ================================================================ */

static void test_parse_envchange_packetsize(void) {
    /* ENVCHANGE token avec type=4 (PACKET_SIZE), new="8192", old="4096".
     *   Length = 1 (type) + 1 (newLen) + 4*2 (UCS-2 "8192") + 1 (oldLen) + 4*2 = 19
     */
    uint8_t payload[] = {
        0xE3,
        0x13, 0x00,                            /* Length = 19 */
        0x04,                                   /* Type = PACKET_SIZE */
        0x04,                                   /* newLen = 4 chars */
        '8',0, '1',0, '9',0, '2',0,
        0x04,                                   /* oldLen = 4 */
        '4',0, '0',0, '9',0, '6',0,
        /* LOGINACK minimal pour valider la suite */
        0xAD,
        0x1E, 0x00,
        0x01,
        0x74, 0x00, 0x00, 0x04,
        0x0A,
        'S',0, 'Q',0, 'L',0, ' ',0,
        'S',0, 'e',0, 'r',0, 'v',0, 'e',0, 'r',0,
        0x0C, 0x00, 0x10, 0x04,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };

    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "parse envchange OK");
    CHECK(r.packet_size_changed,                            "packet_size_changed=true");
    CHECK(r.negotiated_packet_size == 8192,                 "new packet size 8192");
    CHECK(r.success,                                        "login still success");
}

/* ================================================================
 * Parse : token inconnu -> erreur
 * ================================================================ */

static void test_parse_unknown_token(void) {
    uint8_t payload[] = { 0x42, 0x00 };
    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(!owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "token inconnu rejette");
    CHECK(err[0] != '\0',                                   "err message fourni");
}

/* ================================================================
 * Parse : stream tronque -> erreur
 * ================================================================ */

static void test_parse_truncated(void) {
    /* LOGINACK avec length=30 mais on coupe a mi-chemin. */
    uint8_t payload[] = {
        0xAD,
        0x1E, 0x00,                            /* annonce 30 octets */
        0x01,
        0x74, 0x00, 0x00, 0x04,
        0x0A,
        'S',0, 'Q',0,                          /* tronque ici */
    };
    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(!owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "tronque rejette");
}

/* ================================================================
 * Parse : INFO + LOGINACK + DONE (cas normal post-login)
 * ================================================================ */

static void test_parse_info_then_success(void) {
    uint8_t payload[] = {
        /* INFO : "Changed database context to 'master'." (5851) */
        0xAB,
        0x12, 0x00,                            /* Length = 18 */
        0xDB, 0x16, 0x00, 0x00,                /* Number 5851 */
        0x01, 0x00,                             /* State, Class */
        0x02, 0x00,                             /* MsgLen 2 */
        'H',0, 'i',0,                          /* MsgText "Hi" */
        0x00, 0x00,                             /* server/proc len */
        0x01, 0x00, 0x00, 0x00,                /* LineNumber */
        /* LOGINACK */
        0xAD, 0x1E, 0x00,
        0x01, 0x74, 0x00, 0x00, 0x04,
        0x0A,
        'S',0,'Q',0,'L',0,' ',0,'S',0,'e',0,'r',0,'v',0,'e',0,'r',0,
        0x0C, 0x00, 0x10, 0x04,
        /* DONE */
        0xFD,
        0x00, 0x00, 0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };
    OwlMssqlLoginResponse r;
    char err[128];
    CHECK(owl_mssql_login_parse_response(payload, sizeof(payload), &r, err),
                                                          "INFO+LOGINACK OK");
    CHECK(r.info_present,                                  "info_present");
    CHECK(strcmp(r.info_message, "Hi") == 0,               "info message");
    CHECK(r.success,                                        "success");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_login_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_obfuscate_known_vectors();
    test_pack_minimal_structure();
    test_pack_defaults_applied();
    test_parse_success();
    test_parse_error();
    test_parse_envchange_packetsize();
    test_parse_unknown_token();
    test_parse_truncated();
    test_parse_info_then_success();

    printf("sql_mssql_login self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
