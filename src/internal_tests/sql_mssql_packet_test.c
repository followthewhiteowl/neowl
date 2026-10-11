// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du framing TDS MSSQL : header pack/parse, sender chunking,
 * reassembler. Vecteurs construits a la main (header format documente
 * dans MS-TDS section 2.2.3) + verifs de coherence sur les invariants. */

#include "sql_mssql_packet.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Header pack
 * ================================================================ */

static void test_header_write(void) {
    uint8_t hdr[TDS_HEADER_SIZE];

    /* Type=Login7 (0x10), Status=EOM, length=80 (header 8 + payload 72),
     * packet_id=0. Length doit etre encode en BE: 0x0050 -> [0x00, 0x50]. */
    owl_tds_packet_write_header(hdr, TDS_PKT_LOGIN7, TDS_STATUS_EOM, 80, 0);
    CHECK(hdr[0] == 0x10,                       "type byte Login7");
    CHECK(hdr[1] == TDS_STATUS_EOM,             "status EOM");
    CHECK(hdr[2] == 0x00 && hdr[3] == 0x50,     "length 80 BE");
    CHECK(hdr[4] == 0x00 && hdr[5] == 0x00,     "spid = 0 (client)");
    CHECK(hdr[6] == 0x00,                       "packet_id 0");
    CHECK(hdr[7] == 0x00,                       "window = 0");

    /* Length > 256 force le high byte non-zero : 0x1234 -> [0x12, 0x34] */
    owl_tds_packet_write_header(hdr, TDS_PKT_SQL_BATCH, TDS_STATUS_NORMAL, 0x1234, 7);
    CHECK(hdr[0] == 0x01,                       "type SQLBatch");
    CHECK(hdr[1] == TDS_STATUS_NORMAL,          "status normal");
    CHECK(hdr[2] == 0x12 && hdr[3] == 0x34,     "length 0x1234 BE");
    CHECK(hdr[6] == 7,                          "packet_id 7");
}

/* ================================================================
 * Header parse
 * ================================================================ */

static void test_header_read(void) {
    /* Header construit a la main : PreLogin response avec EOM,
     * length=44, spid=0x1234, packet_id=0. */
    uint8_t raw[TDS_HEADER_SIZE] = {
        0x04,        /* Type = TabularResult */
        0x01,        /* Status = EOM */
        0x00, 0x2C,  /* Length = 44 (BE) */
        0x12, 0x34,  /* SPID = 0x1234 (BE) */
        0x00,        /* PacketID */
        0x00,        /* Window */
    };
    OwlTdsPacketHeader h;
    CHECK(owl_tds_packet_read_header(raw, sizeof(raw), &h), "parse OK");
    CHECK(h.type == TDS_PKT_TABULAR_RESULT,                 "type tabular");
    CHECK(h.status & TDS_STATUS_EOM,                        "status has EOM");
    CHECK(h.length == 44,                                    "length 44");
    CHECK(h.spid == 0x1234,                                  "spid 0x1234");
    CHECK(h.packet_id == 0,                                  "packet_id 0");

    /* Buffer trop court -> false */
    uint8_t tiny[3] = {0x04, 0x01, 0x00};
    OwlTdsPacketHeader h2;
    CHECK(!owl_tds_packet_read_header(tiny, sizeof(tiny), &h2),
                                                             "buf trop court");

    /* Length aberrante (< header) -> false */
    uint8_t bad[TDS_HEADER_SIZE] = {0x04, 0x01, 0x00, 0x05, 0,0,0,0};
    CHECK(!owl_tds_packet_read_header(bad, sizeof(bad), &h2),
                                                             "length < header rejected");

    /* Length > max -> false (TDS_MAX_PKT_SIZE+TDS_HEADER_SIZE = 32775) */
    uint8_t huge[TDS_HEADER_SIZE] = {0x04, 0x01, 0xFF, 0xFF, 0,0,0,0};
    CHECK(!owl_tds_packet_read_header(huge, sizeof(huge), &h2),
                                                             "length > max rejected");
}

/* ================================================================
 * Header round-trip
 * ================================================================ */

static void test_header_roundtrip(void) {
    /* On ecrit puis on relit -> doit retrouver toutes les valeurs. */
    uint8_t hdr[TDS_HEADER_SIZE];
    owl_tds_packet_write_header(hdr, TDS_PKT_PRELOGIN, TDS_STATUS_EOM, 1024, 42);

    OwlTdsPacketHeader h;
    CHECK(owl_tds_packet_read_header(hdr, sizeof(hdr), &h), "roundtrip read");
    CHECK(h.type == TDS_PKT_PRELOGIN,                       "rt type");
    CHECK(h.status == TDS_STATUS_EOM,                       "rt status");
    CHECK(h.length == 1024,                                  "rt length");
    CHECK(h.packet_id == 42,                                 "rt packet_id");
}

/* ================================================================
 * Sender : emission simple (1 paquet)
 * ================================================================ */

static void test_sender_single_packet(void) {
    OwlTdsPacketSender s;
    owl_tds_packet_sender_init(&s, TDS_PKT_PRELOGIN, 4088);

    uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t out[64];
    size_t n = owl_tds_packet_sender_emit(&s, out, sizeof(out),
                                           payload, sizeof(payload), true);
    CHECK(n == TDS_HEADER_SIZE + 4,         "emit returned hdr+4");
    CHECK(out[0] == TDS_PKT_PRELOGIN,        "type byte");
    CHECK(out[1] & TDS_STATUS_EOM,           "EOM set on last");
    CHECK(out[2] == 0 && out[3] == 12,       "length 12 BE");
    CHECK(out[6] == 0,                        "packet_id 0 for first emit");
    CHECK(memcmp(out + TDS_HEADER_SIZE, payload, 4) == 0, "payload copied");
    CHECK(s.next_packet_id == 1,             "packet_id incremented");
}

/* ================================================================
 * Sender : chunking automatique
 * ================================================================ */

static void test_sender_chunking(void) {
    OwlTdsPacketSender s;
    /* max payload = 4 octets -> un message de 10 octets se split en 3 paquets
     * (4 + 4 + 2). Seul le dernier porte EOM. PacketID 0, 1, 2. */
    owl_tds_packet_sender_init(&s, TDS_PKT_SQL_BATCH, 4);

    uint8_t msg[10] = {1,2,3,4, 5,6,7,8, 9,10};
    uint8_t out[64];
    size_t total = owl_tds_packet_sender_emit_full(&s, out, sizeof(out), msg, 10);
    /* 3 paquets : (8+4) + (8+4) + (8+2) = 12 + 12 + 10 = 34 */
    CHECK(total == 34,                        "total bytes 34");

    /* Paquet 1 : offset 0-11, header [SQLBatch, NORMAL, 12, 0,0, 0, 0] */
    CHECK(out[0] == TDS_PKT_SQL_BATCH,        "p1 type");
    CHECK(out[1] == TDS_STATUS_NORMAL,        "p1 not EOM");
    CHECK(out[3] == 12,                        "p1 length 12");
    CHECK(out[6] == 0,                         "p1 packet_id 0");
    CHECK(out[TDS_HEADER_SIZE] == 1
       && out[TDS_HEADER_SIZE+3] == 4,         "p1 payload [1..4]");

    /* Paquet 2 : offset 12-23 */
    CHECK(out[12] == TDS_PKT_SQL_BATCH,       "p2 type");
    CHECK(out[13] == TDS_STATUS_NORMAL,        "p2 not EOM");
    CHECK(out[15] == 12,                       "p2 length 12");
    CHECK(out[18] == 1,                        "p2 packet_id 1");
    CHECK(out[12+TDS_HEADER_SIZE] == 5
       && out[12+TDS_HEADER_SIZE+3] == 8,       "p2 payload [5..8]");

    /* Paquet 3 : offset 24-33 (10 octets) */
    CHECK(out[24] == TDS_PKT_SQL_BATCH,       "p3 type");
    CHECK(out[25] == TDS_STATUS_EOM,           "p3 EOM");
    CHECK(out[27] == 10,                       "p3 length 10");
    CHECK(out[30] == 2,                        "p3 packet_id 2");
    CHECK(out[24+TDS_HEADER_SIZE]   == 9
       && out[24+TDS_HEADER_SIZE+1] == 10,      "p3 payload [9,10]");
}

/* ================================================================
 * Sender : message vide (Attention packet)
 * ================================================================ */

static void test_sender_empty(void) {
    OwlTdsPacketSender s;
    owl_tds_packet_sender_init(&s, TDS_PKT_ATTENTION, 4088);
    uint8_t out[16];
    size_t n = owl_tds_packet_sender_emit_full(&s, out, sizeof(out), NULL, 0);
    CHECK(n == TDS_HEADER_SIZE,               "empty msg = header only");
    CHECK(out[0] == TDS_PKT_ATTENTION,         "type Attention");
    CHECK(out[1] & TDS_STATUS_EOM,             "EOM on empty");
    CHECK(out[3] == TDS_HEADER_SIZE,           "length = 8");
}

/* ================================================================
 * Sender : packet_id wrap a 256
 * ================================================================ */

static void test_sender_packet_id_wrap(void) {
    OwlTdsPacketSender s;
    owl_tds_packet_sender_init(&s, TDS_PKT_SQL_BATCH, 4088);
    s.next_packet_id = 255;
    uint8_t out[16];
    owl_tds_packet_sender_emit(&s, out, sizeof(out), NULL, 0, true);
    CHECK(out[6] == 255,                       "emitted with id 255");
    CHECK(s.next_packet_id == 0,               "wraps to 0");
}

/* ================================================================
 * Sender : dst trop court -> retourne 0
 * ================================================================ */

static void test_sender_dst_too_small(void) {
    OwlTdsPacketSender s;
    owl_tds_packet_sender_init(&s, TDS_PKT_SQL_BATCH, 4088);
    uint8_t payload[4] = {1,2,3,4};
    uint8_t out[5];  /* 5 < 8+4 = 12 */
    size_t n = owl_tds_packet_sender_emit(&s, out, sizeof(out), payload, 4, true);
    CHECK(n == 0,                              "dst too small returns 0");
}

/* ================================================================
 * Reassembler : single packet message
 * ================================================================ */

static void test_reassembler_single(void) {
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);

    /* Header + payload "ABCD" */
    uint8_t pkt[TDS_HEADER_SIZE + 4];
    owl_tds_packet_write_header(pkt, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_EOM, 12, 0);
    memcpy(pkt + TDS_HEADER_SIZE, "ABCD", 4);

    CHECK(owl_tds_packet_reassembler_push(&r, pkt, sizeof(pkt)), "push OK");
    CHECK(r.complete,                                              "complete");
    CHECK(r.msg_type == TDS_PKT_TABULAR_RESULT,                    "type captured");
    CHECK(r.payload.length == 4,                                    "payload 4 bytes");
    CHECK(memcmp(r.payload.data, "ABCD", 4) == 0,                   "payload content");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Reassembler : multi-packet message
 * ================================================================ */

static void test_reassembler_multi(void) {
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);

    /* Trois paquets de meme type, dernier avec EOM. */
    uint8_t p1[TDS_HEADER_SIZE + 3];
    owl_tds_packet_write_header(p1, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_NORMAL, 11, 0);
    memcpy(p1 + TDS_HEADER_SIZE, "abc", 3);

    uint8_t p2[TDS_HEADER_SIZE + 4];
    owl_tds_packet_write_header(p2, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_NORMAL, 12, 1);
    memcpy(p2 + TDS_HEADER_SIZE, "defg", 4);

    uint8_t p3[TDS_HEADER_SIZE + 2];
    owl_tds_packet_write_header(p3, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_EOM, 10, 2);
    memcpy(p3 + TDS_HEADER_SIZE, "hi", 2);

    CHECK(owl_tds_packet_reassembler_push(&r, p1, sizeof(p1)),     "p1");
    CHECK(!r.complete,                                              "not complete after p1");
    CHECK(r.payload.length == 3,                                    "3 bytes after p1");

    CHECK(owl_tds_packet_reassembler_push(&r, p2, sizeof(p2)),     "p2");
    CHECK(!r.complete,                                              "not complete after p2");
    CHECK(r.payload.length == 7,                                    "7 bytes after p2");

    CHECK(owl_tds_packet_reassembler_push(&r, p3, sizeof(p3)),     "p3");
    CHECK(r.complete,                                                "complete after p3");
    CHECK(r.payload.length == 9,                                    "9 bytes total");
    CHECK(memcmp(r.payload.data, "abcdefghi", 9) == 0,              "assembled content");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Reassembler : type mismatch -> error
 * ================================================================ */

static void test_reassembler_type_mismatch(void) {
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);

    uint8_t p1[TDS_HEADER_SIZE + 2];
    owl_tds_packet_write_header(p1, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_NORMAL, 10, 0);
    memcpy(p1 + TDS_HEADER_SIZE, "ok", 2);
    CHECK(owl_tds_packet_reassembler_push(&r, p1, sizeof(p1)),     "p1 OK");

    uint8_t p2[TDS_HEADER_SIZE + 2];
    owl_tds_packet_write_header(p2, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_EOM, 10, 1);
    memcpy(p2 + TDS_HEADER_SIZE, "no", 2);
    CHECK(!owl_tds_packet_reassembler_push(&r, p2, sizeof(p2)),    "p2 rejected (type change)");
    CHECK(r.error[0] != '\0',                                       "error message set");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Reassembler : length declaree > buffer recu -> error
 * ================================================================ */

static void test_reassembler_truncated(void) {
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);

    /* Header annonce length=100 mais on lui passe que 16 octets. */
    uint8_t pkt[16];
    owl_tds_packet_write_header(pkt, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_EOM, 100, 0);
    CHECK(!owl_tds_packet_reassembler_push(&r, pkt, sizeof(pkt)),
                                                                    "tronque rejette");
    CHECK(r.error[0] != '\0',                                       "error message set");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Reassembler : reset
 * ================================================================ */

static void test_reassembler_reset(void) {
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);

    uint8_t pkt[TDS_HEADER_SIZE + 2];
    owl_tds_packet_write_header(pkt, TDS_PKT_TABULAR_RESULT,
                                  TDS_STATUS_EOM, 10, 0);
    memcpy(pkt + TDS_HEADER_SIZE, "ab", 2);
    owl_tds_packet_reassembler_push(&r, pkt, sizeof(pkt));
    CHECK(r.complete,                                                "1st msg complete");

    owl_tds_packet_reassembler_reset(&r);
    CHECK(!r.complete && !r.started,                                "reset clears state");
    CHECK(r.payload.length == 0,                                    "payload empty after reset");

    /* On peut pousser un nouveau message d'un autre type apres reset. */
    uint8_t pkt2[TDS_HEADER_SIZE + 1];
    owl_tds_packet_write_header(pkt2, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_EOM, 9, 0);
    pkt2[TDS_HEADER_SIZE] = 0xFF;
    CHECK(owl_tds_packet_reassembler_push(&r, pkt2, sizeof(pkt2)),  "new msg after reset");
    CHECK(r.msg_type == TDS_PKT_PRELOGIN,                           "new type captured");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Round-trip : sender -> reassembler
 * ================================================================ */

static void test_sender_reassembler_roundtrip(void) {
    OwlTdsPacketSender s;
    owl_tds_packet_sender_init(&s, TDS_PKT_TABULAR_RESULT, 8);  /* tres petit chunks */

    uint8_t msg[40];
    for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = (uint8_t)(i * 3 + 7);

    uint8_t serial[128];
    size_t total = owl_tds_packet_sender_emit_full(&s, serial, sizeof(serial),
                                                     msg, sizeof(msg));
    CHECK(total > sizeof(msg),                              "serialisation > msg (header overhead)");

    /* Reassemble en parsant chaque paquet du buffer serialise. */
    OwlTdsPacketReassembler r;
    owl_tds_packet_reassembler_init(&r);
    size_t cursor = 0;
    while (cursor < total) {
        OwlTdsPacketHeader h;
        if (!owl_tds_packet_read_header(serial + cursor, total - cursor, &h)) {
            CHECK(false, "parse header during roundtrip");
            break;
        }
        bool ok = owl_tds_packet_reassembler_push(&r, serial + cursor, h.length);
        CHECK(ok, "push during roundtrip");
        cursor += h.length;
    }
    CHECK(r.complete,                                       "rt complete");
    CHECK(r.payload.length == sizeof(msg),                  "rt payload len");
    CHECK(memcmp(r.payload.data, msg, sizeof(msg)) == 0,    "rt payload content");

    owl_tds_packet_reassembler_free(&r);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_packet_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_header_write();
    test_header_read();
    test_header_roundtrip();
    test_sender_single_packet();
    test_sender_chunking();
    test_sender_empty();
    test_sender_packet_id_wrap();
    test_sender_dst_too_small();
    test_reassembler_single();
    test_reassembler_multi();
    test_reassembler_type_mismatch();
    test_reassembler_truncated();
    test_reassembler_reset();
    test_sender_reassembler_roundtrip();

    printf("sql_mssql_packet self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
