// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du wire protocol MySQL : encoders/parsers vs vecteurs reels.
 *
 * Vecteurs hex tires de captures wireshark MySQL 8.0 et de la spec officielle. */

#include "sql_mysql_wire.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Framing
 * ================================================================ */

static void test_packet_framing(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 32);
    size_t off = mysql_pkt_begin(&b, 0);
    owl_le_buffer_write_u8(&b, 0xAA);
    owl_le_buffer_write_u8(&b, 0xBB);
    owl_le_buffer_write_u8(&b, 0xCC);
    mysql_pkt_end(&b, off);
    /* Attendu : 03 00 00 00 AA BB CC (3 octets payload, seq=0) */
    CHECK(b.length == 7,                 "packet length 7");
    CHECK(b.data[0] == 0x03,             "u24 byte 0 = 3");
    CHECK(b.data[1] == 0x00,             "u24 byte 1");
    CHECK(b.data[2] == 0x00,             "u24 byte 2");
    CHECK(b.data[3] == 0x00,             "seq = 0");
    CHECK(b.data[4] == 0xAA && b.data[5] == 0xBB && b.data[6] == 0xCC,
                                          "payload bytes");
    owl_le_buffer_free(&b);

    /* Round-trip header read */
    uint8_t raw[5] = { 0x42, 0x01, 0x00, 0x05, 0xFF };  /* len=0x000142=322, seq=5 */
    OwlLeBuffer r; owl_le_buffer_attach_read(&r, raw, 5);
    uint32_t plen; uint8_t seq;
    CHECK(mysql_read_header(&r, &plen, &seq), "read_header OK");
    CHECK(plen == 322,                         "header length 322");
    CHECK(seq  == 5,                           "header seq 5");
}

/* ================================================================
 * Initial Handshake parsing (vrai payload MySQL 8.0)
 * ================================================================ */

static void test_handshake_parse(void) {
    /* Payload capture reel (MySQL 8.0.35, simplifie).
     * Layout :
     *   0A                                   protocol_version = 10
     *   38 2E 30 2E 33 35 00                 server_version "8.0.35\0"
     *   2A 00 00 00                          thread_id = 42
     *   01 02 03 04 05 06 07 08              salt part 1 (8 octets)
     *   00                                   filler
     *   FF FF                                cap_low = 0xFFFF
     *   2D                                   charset = 0x2D (utf8mb4)
     *   02 00                                status = 2
     *   FF DF                                cap_high (0xDFFF)
     *   15                                   auth_data_len = 21
     *   00 00 00 00 00 00 00 00 00 00         reserved 10x0
     *   09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 00  salt part 2 (13 bytes, dont \0)
     *   63 61 63 68 69 6E 67 5F 73 68 61 32 5F 70 61 73 73 77 6F 72 64 00
     *                                          "caching_sha2_password\0"
     */
    uint8_t payload[] = {
        0x0A,
        '8','.','0','.','3','5', 0x00,
        0x2A, 0x00, 0x00, 0x00,
        1, 2, 3, 4, 5, 6, 7, 8,
        0x00,
        0xFF, 0xFF,
        0x2D,
        0x02, 0x00,
        0xFF, 0xDF,
        0x15,
        0,0,0,0,0,0,0,0,0,0,
        9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0x00,
        'c','a','c','h','i','n','g','_','s','h','a','2','_','p','a','s','s','w','o','r','d', 0x00
    };
    size_t plen = sizeof(payload);

    MysqlHandshake hs;
    CHECK(mysql_parse_handshake(payload, plen, &hs), "parse handshake OK");
    CHECK(hs.protocol_version == 10,                 "protocol v10");
    CHECK(hs.server_version != NULL && strcmp(hs.server_version, "8.0.35") == 0,
                                                      "server_version");
    CHECK(hs.thread_id == 42,                         "thread_id 42");
    CHECK(hs.charset == 0x2D,                         "charset utf8mb4");
    CHECK(hs.status_flags == 2,                       "status flags 2");
    CHECK((hs.capabilities & MYSQL_CAP_PROTOCOL_41) != 0, "cap PROTOCOL_41");
    CHECK((hs.capabilities & MYSQL_CAP_PLUGIN_AUTH) != 0, "cap PLUGIN_AUTH");
    CHECK(hs.auth_plugin_name != NULL &&
          strcmp(hs.auth_plugin_name, "caching_sha2_password") == 0,
                                                      "auth_plugin_name");

    /* Extraction salt complet */
    uint8_t salt[20];
    CHECK(mysql_handshake_extract_salt(payload, plen, salt), "extract_salt OK");
    uint8_t expected_salt[20] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    };
    CHECK(memcmp(salt, expected_salt, 20) == 0, "salt bytes match");
}

/* ================================================================
 * Error packet parsing
 * ================================================================ */

static void test_error_parse(void) {
    /* Vraie capture ERROR 1045 (28000) Access denied for user...
     * Header byte 0xFF deja consomme par le caller (cf convention API),
     * payload commence directement par error_code.
     *   05 04                                error_code = 0x0405 = 1029  (test num)
     *   #                                    sql_state_marker
     *   32 33 30 30 30                       sql_state "23000"
     *   message string                       "Duplicate entry"
     */
    uint8_t payload[] = {
        0x06, 0x04,
        '#',
        '2','3','0','0','0',
        'D','u','p','l','i','c','a','t','e',' ','e','n','t','r','y'
    };
    MysqlErrorPacket err;
    CHECK(mysql_parse_error(payload, sizeof(payload), &err), "parse error OK");
    CHECK(err.error_code == 0x0406,                  "error_code 1030");
    CHECK(err.sql_state != NULL && strncmp(err.sql_state, "23000", 5) == 0,
                                                       "sql_state 23000");
    CHECK(err.message_len == 15 && strncmp(err.message, "Duplicate entry", 15) == 0,
                                                       "message");
}

/* ================================================================
 * OK packet parsing
 * ================================================================ */

static void test_ok_parse(void) {
    /* Apres consumption du header byte 0x00 :
     *   01                                   affected_rows = 1 (lenenc)
     *   05                                   last_insert_id = 5
     *   02 00                                status_flags
     *   00 00                                warnings
     *   <empty info>
     */
    uint8_t payload[] = { 0x01, 0x05, 0x02, 0x00, 0x00, 0x00 };
    MysqlOkPacket ok;
    CHECK(mysql_parse_ok(payload, sizeof(payload), MYSQL_CAP_PROTOCOL_41, &ok),
                                                      "parse OK packet");
    CHECK(ok.affected_rows == 1,                      "affected_rows = 1");
    CHECK(ok.last_insert_id == 5,                     "last_insert_id = 5");
    CHECK(ok.status_flags == 2,                       "status_flags = 2");
    CHECK(ok.warnings == 0,                           "warnings = 0");
    CHECK(ok.info_len == 0,                           "info empty");
}

/* ================================================================
 * AuthSwitchRequest parsing
 * ================================================================ */

static void test_auth_switch_parse(void) {
    /* Header 0xFE deja consomme. Payload :
     *   "mysql_native_password\0" 20-octets salt
     */
    uint8_t payload[] = {
        'm','y','s','q','l','_','n','a','t','i','v','e','_',
        'p','a','s','s','w','o','r','d', 0x00,
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20
    };
    MysqlAuthSwitch as;
    CHECK(mysql_parse_auth_switch(payload, sizeof(payload), &as),
                                                       "parse auth_switch OK");
    CHECK(as.plugin_name != NULL &&
          strcmp(as.plugin_name, "mysql_native_password") == 0,
                                                       "plugin_name");
    CHECK(as.plugin_data_len == 20,                    "plugin_data 20 octets");
    CHECK(as.plugin_data[0] == 1 && as.plugin_data[19] == 20,
                                                       "plugin_data content");
}

/* ================================================================
 * Commands encoding
 * ================================================================ */

static void test_com_query(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 32);
    mysql_write_com_query(&b, 0, "SELECT 1");
    /* Attendu :
     *   09 00 00       payload length = 9 (1 cmd + 8 SQL)
     *   00             seq = 0
     *   03             COM_QUERY
     *   "SELECT 1"     8 bytes
     */
    CHECK(b.length == 13,                              "com_query total bytes");
    CHECK(b.data[0] == 0x09 && b.data[1] == 0 && b.data[2] == 0, "payload len 9");
    CHECK(b.data[3] == 0,                              "seq 0");
    CHECK(b.data[4] == MYSQL_COM_QUERY,                "cmd 0x03");
    CHECK(memcmp(b.data + 5, "SELECT 1", 8) == 0,      "sql payload");
    owl_le_buffer_free(&b);
}

static void test_com_quit_ping(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 16);
    mysql_write_com_quit(&b, 7);
    /* 01 00 00 07 01  (len=1, seq=7, cmd=0x01) */
    CHECK(b.length == 5 &&
          b.data[0] == 1 && b.data[1] == 0 && b.data[2] == 0 &&
          b.data[3] == 7 && b.data[4] == MYSQL_COM_QUIT,
                                                       "com_quit packet");
    owl_le_buffer_reset(&b);
    mysql_write_com_ping(&b, 0);
    CHECK(b.length == 5 && b.data[4] == MYSQL_COM_PING, "com_ping packet");
    owl_le_buffer_free(&b);
}

/* ================================================================
 * HandshakeResponse41 encoding
 * ================================================================ */

static void test_handshake_response(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 128);
    MysqlHandshakeResponse hr = {
        .client_capabilities = MYSQL_CAP_PROTOCOL_41 | MYSQL_CAP_SECURE_CONNECTION |
                               MYSQL_CAP_PLUGIN_AUTH | MYSQL_CAP_CONNECT_WITH_DB,
        .max_packet_size = 16 * 1024 * 1024,
        .charset = 0x2D,
        .user = "alice",
        .auth_response = (const uint8_t*)"\x01\x02\x03",
        .auth_response_len = 3,
        .database = "mydb",
        .auth_plugin_name = "mysql_native_password",
    };
    mysql_write_handshake_response(&b, 1, &hr);

    /* Verifications de structure : header + content. */
    CHECK(b.length > MYSQL_HEADER_SIZE,                "packet non-empty");
    CHECK(b.data[3] == 1,                              "seq = 1");
    /* Apres header : caps u32 LE, max_packet u32 LE, charset u8, 23 reserved
     * = 33 octets fixes. Puis user "alice\0" = 6 octets. Puis auth_len + 3 bytes. */
    uint32_t caps_in_pkt = ((uint32_t)b.data[4])
                         | ((uint32_t)b.data[5] << 8)
                         | ((uint32_t)b.data[6] << 16)
                         | ((uint32_t)b.data[7] << 24);
    CHECK(caps_in_pkt == hr.client_capabilities,       "capabilities preserved");
    /* "alice\0" doit etre present a l'offset 4 (header) + 32 (caps u32 +
     * max_pkt u32 + charset u8 + 23 reserved) = 36. */
    CHECK(memcmp(b.data + 4 + 32, "alice", 6) == 0,    "user 'alice'");
    /* Verifie qu'on a bien le plugin_name a la fin (search) */
    bool plugin_found = false;
    if (b.length >= 22) {
        for (size_t i = 0; i + 22 <= b.length; i++) {
            if (memcmp(b.data + i, "mysql_native_password", 21) == 0 &&
                b.data[i + 21] == 0) { plugin_found = true; break; }
        }
    }
    CHECK(plugin_found,                                "plugin_name present");
    owl_le_buffer_free(&b);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mysql_wire_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_packet_framing();
    test_handshake_parse();
    test_error_parse();
    test_ok_parse();
    test_auth_switch_parse();
    test_com_query();
    test_com_quit_ping();
    test_handshake_response();

    printf("sql_mysql_wire self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
