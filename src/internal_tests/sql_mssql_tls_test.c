// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du BIO TLS-in-TDS : verifie qu'en mode wrap les ecritures
 * sont encapsulees dans des paquets TDS 0x12, et que les lectures
 * deballent correctement les paquets recus. Sans vrai socket : transport
 * abstrait via buffers en memoire.
 *
 * On ne valide pas mbedTLS lui-meme — juste la couche pont. */

#include "sql_mssql_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Mock transport : un buffer "TCP" bidirectionnel
 * ================================================================ */

typedef struct {
    uint8_t* outbound;        /* ce que le BIO envoie */
    size_t   outbound_len;
    size_t   outbound_cap;

    uint8_t* inbound;          /* ce que le BIO doit lire */
    size_t   inbound_len;
    size_t   inbound_pos;
} MockTransport;

static void mock_init(MockTransport* m, size_t cap) {
    m->outbound     = (uint8_t*)malloc(cap);
    m->outbound_len = 0;
    m->outbound_cap = cap;
    m->inbound      = NULL;
    m->inbound_len  = 0;
    m->inbound_pos  = 0;
}

static void mock_free(MockTransport* m) {
    free(m->outbound);
    free(m->inbound);
    memset(m, 0, sizeof(*m));
}

static void mock_set_inbound(MockTransport* m, const uint8_t* data, size_t len) {
    free(m->inbound);
    m->inbound = (uint8_t*)malloc(len);
    memcpy(m->inbound, data, len);
    m->inbound_len = len;
    m->inbound_pos = 0;
}

static int mock_send(void* ctx, const uint8_t* data, size_t len) {
    MockTransport* m = (MockTransport*)ctx;
    if (m->outbound_len + len > m->outbound_cap) return -1;
    memcpy(m->outbound + m->outbound_len, data, len);
    m->outbound_len += len;
    return (int)len;
}

static int mock_recv(void* ctx, uint8_t* buf, size_t cap) {
    MockTransport* m = (MockTransport*)ctx;
    size_t remaining = m->inbound_len - m->inbound_pos;
    if (remaining == 0) return -1;  /* EOF en test */
    size_t n = (remaining < cap) ? remaining : cap;
    memcpy(buf, m->inbound + m->inbound_pos, n);
    m->inbound_pos += n;
    return (int)n;
}

/* Mock qui force des reads/writes morceles (1 octet par appel).
 * Simule un TCP avec MTU minuscule. */
static int mock_send_drip(void* ctx, const uint8_t* data, size_t len) {
    MockTransport* m = (MockTransport*)ctx;
    if (len == 0 || m->outbound_len >= m->outbound_cap) return -1;
    m->outbound[m->outbound_len++] = data[0];
    return 1;
}

static int mock_recv_drip(void* ctx, uint8_t* buf, size_t cap) {
    MockTransport* m = (MockTransport*)ctx;
    if (cap == 0) return 0;
    if (m->inbound_pos >= m->inbound_len) return -1;
    buf[0] = m->inbound[m->inbound_pos++];
    return 1;
}

/* ================================================================
 * Send : mode wrap encapsule dans un paquet TDS 0x12
 * ================================================================ */

static void test_send_wrap_single_packet(void) {
    MockTransport m;
    mock_init(&m, 1024);

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 8192);

    /* On envoie 16 octets faciles a reconnaitre. */
    uint8_t tls_record[16];
    for (size_t i = 0; i < sizeof(tls_record); ++i) tls_record[i] = (uint8_t)(i + 0xA0);

    int sent = owl_tds_tls_bio_send(&bio, tls_record, sizeof(tls_record));
    CHECK(sent == 16,                                        "send retourne 16");

    /* En wrap mode, le send n'emet PAS de paquet TDS immediatement —
     * il bufferise. Le flush intervient sur set_wrap(false) ou recv().
     * Le transport ne doit donc rien avoir recu pour l'instant. */
    CHECK(m.outbound_len == 0,                               "outbound vide (buffered)");

    /* On force le flush en basculant wrap a false (fin handshake). */
    owl_tds_tls_bio_set_wrap(&bio, false);

    /* Maintenant le transport a recu : header TDS 8 octets + 16 payload = 24. */
    CHECK(m.outbound_len == TDS_HEADER_SIZE + 16,            "post-flush hdr+16");
    CHECK(m.outbound[0] == TDS_PKT_PRELOGIN,                  "type TDS 0x12");
    CHECK(m.outbound[1] & TDS_STATUS_EOM,                     "EOM sur paquet unique");
    CHECK(((m.outbound[2] << 8) | m.outbound[3]) == 24,        "length BE = 24");

    /* Le payload doit etre identique aux octets envoyes. */
    CHECK(memcmp(m.outbound + TDS_HEADER_SIZE, tls_record, 16) == 0,
                                                              "payload preserve");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Send : plusieurs writes consecutifs sont bundled dans UN paquet
 * (= comportement attendu par SQL Server pour les flights handshake)
 * ================================================================ */

static void test_send_wrap_bundle_multiple_writes(void) {
    MockTransport m;
    mock_init(&m, 1024);

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 8192);

    /* 3 writes typiques d'une flight handshake : CKE, CCS, Finished. */
    uint8_t cke[] = {0x16, 0x03, 0x03, 0x00, 0x04, 1, 2, 3, 4};
    uint8_t ccs[] = {0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
    uint8_t fin[] = {0x16, 0x03, 0x03, 0x00, 0x08, 0xAA, 0xBB, 0xCC, 0xDD,
                     0xEE, 0xFF, 0x00, 0x11};

    owl_tds_tls_bio_send(&bio, cke, sizeof(cke));
    CHECK(m.outbound_len == 0,                                "buffer apres CKE");
    owl_tds_tls_bio_send(&bio, ccs, sizeof(ccs));
    CHECK(m.outbound_len == 0,                                "buffer apres CCS");
    owl_tds_tls_bio_send(&bio, fin, sizeof(fin));
    CHECK(m.outbound_len == 0,                                "buffer apres Finished");

    /* Flush via fin de wrap. */
    owl_tds_tls_bio_set_wrap(&bio, false);

    /* On doit avoir UN paquet TDS contenant les 3 records concatenes. */
    size_t total_payload = sizeof(cke) + sizeof(ccs) + sizeof(fin);
    CHECK(m.outbound_len == TDS_HEADER_SIZE + total_payload,  "1 paquet bundle");
    CHECK(m.outbound[0] == TDS_PKT_PRELOGIN,                   "type 0x12");
    CHECK(m.outbound[1] & TDS_STATUS_EOM,                      "EOM");
    CHECK(memcmp(m.outbound + TDS_HEADER_SIZE,                 cke, sizeof(cke)) == 0,
                                                               "CKE 1er");
    CHECK(memcmp(m.outbound + TDS_HEADER_SIZE + sizeof(cke),   ccs, sizeof(ccs)) == 0,
                                                               "CCS 2eme");
    CHECK(memcmp(m.outbound + TDS_HEADER_SIZE + sizeof(cke)
              + sizeof(ccs),                                   fin, sizeof(fin)) == 0,
                                                               "Finished 3eme");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Send : payload > max_payload -> plusieurs paquets, seul le dernier EOM
 * ================================================================ */

static void test_send_wrap_chunked(void) {
    MockTransport m;
    mock_init(&m, 4096);

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 8192);
    /* On limite max_payload artificiellement pour forcer 3 paquets. */
    bio.max_payload = 10;
    owl_tds_packet_sender_init(&bio.sender, TDS_PKT_PRELOGIN, 10);

    uint8_t data[25];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;

    int sent = owl_tds_tls_bio_send(&bio, data, sizeof(data));
    CHECK(sent == 25,                                       "send retourne 25");

    /* Pas de flush immediat — on est en wrap, buffer accumule. */
    CHECK(m.outbound_len == 0,                              "outbound vide (buffered)");

    /* Flush via set_wrap(false). Resultat : 25 bytes -> 3 paquets TDS
     * (max_payload=10, donc 10+10+5), seul le dernier EOM. */
    owl_tds_tls_bio_set_wrap(&bio, false);

    /* Trois paquets : (8+10) + (8+10) + (8+5) = 18 + 18 + 13 = 49 */
    CHECK(m.outbound_len == 49,                              "total transport 49 bytes");
    /* Paquet 1 : NOT EOM */
    CHECK(m.outbound[1] == TDS_STATUS_NORMAL,                "pkt1 not EOM");
    CHECK(m.outbound[3] == 18,                                "pkt1 length 18");
    /* Paquet 2 : NOT EOM (offset 18) */
    CHECK(m.outbound[18+1] == TDS_STATUS_NORMAL,             "pkt2 not EOM");
    /* Paquet 3 : EOM (offset 36) */
    CHECK(m.outbound[36+1] & TDS_STATUS_EOM,                 "pkt3 EOM");
    CHECK(m.outbound[36+3] == 13,                             "pkt3 length 13");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Send : passthrough (wrap=false) -> envoi direct sans wrapping
 * ================================================================ */

static void test_send_passthrough(void) {
    MockTransport m;
    mock_init(&m, 1024);

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 8192);
    owl_tds_tls_bio_set_wrap(&bio, false);

    uint8_t raw[8] = {0x16, 0x03, 0x03, 0x00, 0x04, 0xAA, 0xBB, 0xCC};
    int sent = owl_tds_tls_bio_send(&bio, raw, sizeof(raw));
    CHECK(sent == 8,                                         "passthrough send returns 8");
    CHECK(m.outbound_len == 8,                                "transport got exact 8");
    CHECK(memcmp(m.outbound, raw, 8) == 0,                    "no wrapping");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Recv : wrap mode deballe un paquet TDS et sert le payload
 * ================================================================ */

static void test_recv_wrap_single_packet(void) {
    /* On prepare un paquet TDS contenant 12 octets de payload TLS. */
    uint8_t packet[TDS_HEADER_SIZE + 12];
    owl_tds_packet_write_header(packet, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_EOM, 20, 0);
    for (size_t i = 0; i < 12; ++i) packet[TDS_HEADER_SIZE + i] = (uint8_t)(i + 0x50);

    MockTransport m;
    mock_init(&m, 256);
    mock_set_inbound(&m, packet, sizeof(packet));

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 4096);

    /* On demande 5 octets : doit recevoir les 5 premiers du payload. */
    uint8_t out[5];
    int got = owl_tds_tls_bio_recv(&bio, out, 5);
    CHECK(got == 5,                                          "1st recv = 5");
    for (size_t i = 0; i < 5; ++i) {
        CHECK(out[i] == (uint8_t)(i + 0x50),                  "byte match");
    }

    /* Le buffer interne a encore 7 octets a servir, on les lit. */
    uint8_t rest[7];
    got = owl_tds_tls_bio_recv(&bio, rest, 7);
    CHECK(got == 7,                                          "2nd recv = 7");
    CHECK(rest[0] == 0x55 && rest[6] == 0x5B,                "rest content");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Recv : sur 2 paquets successifs (mbedTLS appelle plusieurs fois)
 * ================================================================ */

static void test_recv_wrap_multi_packets(void) {
    /* Deux paquets TDS distincts. */
    uint8_t buf[256];
    size_t off = 0;
    owl_tds_packet_write_header(buf + off, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_NORMAL, TDS_HEADER_SIZE + 4, 0);
    memcpy(buf + off + TDS_HEADER_SIZE, "AAAA", 4);
    off += TDS_HEADER_SIZE + 4;
    owl_tds_packet_write_header(buf + off, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_EOM, TDS_HEADER_SIZE + 3, 1);
    memcpy(buf + off + TDS_HEADER_SIZE, "BBB", 3);
    off += TDS_HEADER_SIZE + 3;

    MockTransport m;
    mock_init(&m, 256);
    mock_set_inbound(&m, buf, off);

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 4096);

    /* mbedTLS lit le premier paquet (4 octets) puis demande encore. */
    uint8_t out[10];
    int got = owl_tds_tls_bio_recv(&bio, out, 10);
    CHECK(got == 4,                                          "1st recv = 4 (du paquet 1)");
    CHECK(memcmp(out, "AAAA", 4) == 0,                        "1er paquet content");

    got = owl_tds_tls_bio_recv(&bio, out, 10);
    CHECK(got == 3,                                          "2nd recv = 3 (paquet 2)");
    CHECK(memcmp(out, "BBB", 3) == 0,                         "2eme paquet content");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Recv : transport drippe (1 byte par recv) -> recv_exact accumule
 * ================================================================ */

static void test_recv_wrap_drip_transport(void) {
    uint8_t packet[TDS_HEADER_SIZE + 5];
    owl_tds_packet_write_header(packet, TDS_PKT_PRELOGIN,
                                  TDS_STATUS_EOM, 13, 0);
    memcpy(packet + TDS_HEADER_SIZE, "drip!", 5);

    MockTransport m;
    mock_init(&m, 256);
    mock_set_inbound(&m, packet, sizeof(packet));

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send_drip, mock_recv_drip, &m, 4096);

    uint8_t out[5];
    int got = owl_tds_tls_bio_recv(&bio, out, 5);
    CHECK(got == 5,                                          "drip recv 5");
    CHECK(memcmp(out, "drip!", 5) == 0,                       "drip content");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Recv : passthrough (apres bascule) -> transport direct
 * ================================================================ */

static void test_recv_passthrough(void) {
    uint8_t raw[] = {0x17, 0x03, 0x03, 0x00, 0x10};  /* TLS record header */
    MockTransport m;
    mock_init(&m, 256);
    mock_set_inbound(&m, raw, sizeof(raw));

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 4096);
    owl_tds_tls_bio_set_wrap(&bio, false);

    uint8_t out[10];
    int got = owl_tds_tls_bio_recv(&bio, out, sizeof(out));
    CHECK(got == 5,                                          "passthrough recv 5");
    CHECK(memcmp(out, raw, 5) == 0,                           "passthrough content");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Send : erreur transport (buffer outbound plein) -> retour negatif
 * ================================================================ */

static void test_send_transport_error(void) {
    MockTransport m;
    mock_init(&m, 5);   /* trop petit pour header + payload */

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 4096);

    uint8_t data[10] = {0};
    int sent = owl_tds_tls_bio_send(&bio, data, sizeof(data));
    CHECK(sent == 10,                                        "send buffered OK");

    /* L'erreur transport se manifeste au flush (recv ou set_wrap(false)). */
    owl_tds_tls_bio_set_wrap(&bio, false);
    CHECK(bio.error[0] != '\0',                               "error msg apres flush");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Recv : EOF transport -> echec
 * ================================================================ */

static void test_recv_eof(void) {
    MockTransport m;
    mock_init(&m, 16);
    /* inbound vide -> mock_recv retournera -1 */

    OwlTdsTlsBio bio;
    owl_tds_tls_bio_init(&bio, mock_send, mock_recv, &m, 4096);

    uint8_t out[4];
    int got = owl_tds_tls_bio_recv(&bio, out, sizeof(out));
    CHECK(got < 0,                                           "recv echec sur EOF");

    owl_tds_tls_bio_free(&bio);
    mock_free(&m);
}

/* ================================================================
 * Round-trip : send wrap -> simule cote serveur qui depile -> recv wrap
 * ================================================================ */

static void test_send_recv_roundtrip(void) {
    /* On envoie 50 octets en wrap. Le mock outbound est ensuite re-injecte
     * comme inbound — pour simuler un serveur qui renverrait exactement le
     * meme paquet. On verifie que recv recupere les 50 octets initiaux. */
    MockTransport m;
    mock_init(&m, 256);

    OwlTdsTlsBio bio_a;
    owl_tds_tls_bio_init(&bio_a, mock_send, mock_recv, &m, 4096);

    uint8_t msg[50];
    for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = (uint8_t)(i * 7 + 3);

    int sent = owl_tds_tls_bio_send(&bio_a, msg, sizeof(msg));
    CHECK(sent == 50,                                        "rt send 50");

    /* Forcer le flush du buffer pour materialiser le paquet TDS dans
     * outbound. */
    owl_tds_tls_bio_set_wrap(&bio_a, false);

    /* Recycle outbound -> inbound. */
    mock_set_inbound(&m, m.outbound, m.outbound_len);

    OwlTdsTlsBio bio_b;
    owl_tds_tls_bio_init(&bio_b, mock_send, mock_recv, &m, 4096);
    uint8_t recv_buf[50];
    int got = owl_tds_tls_bio_recv(&bio_b, recv_buf, sizeof(recv_buf));
    CHECK(got == 50,                                         "rt recv 50");
    CHECK(memcmp(recv_buf, msg, 50) == 0,                     "rt content");

    owl_tds_tls_bio_free(&bio_a);
    owl_tds_tls_bio_free(&bio_b);
    mock_free(&m);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_mssql_tls_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_send_wrap_single_packet();
    test_send_wrap_bundle_multiple_writes();
    test_send_wrap_chunked();
    test_send_passthrough();
    test_recv_wrap_single_packet();
    test_recv_wrap_multi_packets();
    test_recv_wrap_drip_transport();
    test_recv_passthrough();
    test_send_transport_error();
    test_recv_eof();
    test_send_recv_roundtrip();

    printf("sql_mssql_tls self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
