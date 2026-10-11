// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test wire protocol PostgreSQL.
 *
 * Vecteurs hex hardcodés issus de la doc officielle PG + captures
 * Wireshark validées. Pas de dépendance réseau — tout est binaire
 * déterministe. */

#include "sql_postgres_wire.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define WIRE_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

/* ================================================================
 * SSLRequest
 *
 * Format fixe : 4 bytes length=8 + 4 bytes code=80877103.
 *   00 00 00 08 04 d2 16 2f
 * ================================================================ */
static void test_ssl_request(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_ssl_request(&buf);

    const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x08,  /* length = 8 */
        0x04, 0xD2, 0x16, 0x2F   /* code = 80877103 */
    };
    WIRE_CHECK(buf.length == sizeof(expected), "SSLRequest taille = 8");
    WIRE_CHECK(bytes_equal(buf.data, expected, sizeof(expected)),
                "SSLRequest bytes exacts");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * StartupMessage
 *
 * user=alice, database=mydb. Doit produire :
 *   length (4) = 34
 *   version (4) = 196608
 *   "user\0alice\0database\0mydb\0\0"
 * ================================================================ */
static void test_startup(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_startup(&buf, "alice", "mydb");

    const uint8_t expected[] = {
        /* length = 34 = 0x22 */
        0x00, 0x00, 0x00, 0x22,
        /* protocol version 3.0 */
        0x00, 0x03, 0x00, 0x00,
        /* "user\0" "alice\0" */
        'u', 's', 'e', 'r', 0x00,
        'a', 'l', 'i', 'c', 'e', 0x00,
        /* "database\0" "mydb\0" */
        'd', 'a', 't', 'a', 'b', 'a', 's', 'e', 0x00,
        'm', 'y', 'd', 'b', 0x00,
        /* terminator */
        0x00
    };
    WIRE_CHECK(buf.length == sizeof(expected),
                "StartupMessage taille = 34");
    WIRE_CHECK(bytes_equal(buf.data, expected, sizeof(expected)),
                "StartupMessage bytes exacts");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * Query "SELECT 1"
 *
 *   Type 'Q' (0x51), length = 13, payload = "SELECT 1\0"
 * ================================================================ */
static void test_query(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_query(&buf, "SELECT 1");

    const uint8_t expected[] = {
        0x51,                            /* 'Q' */
        0x00, 0x00, 0x00, 0x0D,          /* length = 13 */
        'S','E','L','E','C','T',' ','1', 0x00
    };
    WIRE_CHECK(buf.length == sizeof(expected), "Query taille = 14");
    WIRE_CHECK(bytes_equal(buf.data, expected, sizeof(expected)),
                "Query bytes exacts");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * Terminate
 *
 *   Type 'X' (0x58), length = 4 (juste le header).
 * ================================================================ */
static void test_terminate(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_terminate(&buf);

    const uint8_t expected[] = {
        0x58,                          /* 'X' */
        0x00, 0x00, 0x00, 0x04         /* length = 4 */
    };
    WIRE_CHECK(buf.length == sizeof(expected), "Terminate taille = 5");
    WIRE_CHECK(bytes_equal(buf.data, expected, sizeof(expected)),
                "Terminate bytes exacts");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * PasswordMessage cleartext
 *
 *   Type 'p', length = 4 + len(pwd) + 1
 *   "s3cret" -> 'p' 00 00 00 0B 's' '3' 'c' 'r' 'e' 't' 00
 * ================================================================ */
static void test_password(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_password(&buf, "s3cret");

    const uint8_t expected[] = {
        0x70,                          /* 'p' */
        0x00, 0x00, 0x00, 0x0B,        /* length = 11 (4 + 6 + 1) */
        's','3','c','r','e','t', 0x00
    };
    WIRE_CHECK(buf.length == sizeof(expected), "Password taille");
    WIRE_CHECK(bytes_equal(buf.data, expected, sizeof(expected)),
                "Password bytes exacts");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * SASLInitialResponse
 *
 *   mechanism = "SCRAM-SHA-256"
 *   sasl-first = "n,,n=user,r=foo"  (15 bytes)
 *   Payload : "SCRAM-SHA-256\0" + i32(15) + 15 bytes
 *   Length = 4 + 14 + 4 + 15 = 37
 * ================================================================ */
static void test_sasl_initial(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    postgres_write_sasl_initial_response(&buf, "SCRAM-SHA-256", "n,,n=user,r=foo");

    /* Verifie header + structure. */
    WIRE_CHECK(buf.data[0] == 'p', "SASLInitial type = p");
    /* length BE @ offset 1..5 */
    uint32_t L = ((uint32_t)buf.data[1] << 24) | ((uint32_t)buf.data[2] << 16)
               | ((uint32_t)buf.data[3] <<  8) |  (uint32_t)buf.data[4];
    WIRE_CHECK(L == (uint32_t)(buf.length - 1),
                "SASLInitial length coherente");
    /* Verifie mechanism cstring. */
    WIRE_CHECK(memcmp(buf.data + 5, "SCRAM-SHA-256\0", 14) == 0,
                "SASLInitial mechanism = SCRAM-SHA-256");
    /* i32(15) BE après mechanism. */
    uint32_t saslLen = ((uint32_t)buf.data[19] << 24) | ((uint32_t)buf.data[20] << 16)
                     | ((uint32_t)buf.data[21] <<  8) |  (uint32_t)buf.data[22];
    WIRE_CHECK(saslLen == 15, "SASLInitial sasl-first len = 15");
    WIRE_CHECK(memcmp(buf.data + 23, "n,,n=user,r=foo", 15) == 0,
                "SASLInitial sasl-first bytes");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * postgres_msg_begin / postgres_msg_end + back-patch length
 * ================================================================ */
static void test_msg_begin_end(void) {
    OwlBeBuffer buf; owl_be_buffer_init(&buf, 0);
    size_t off = postgres_msg_begin(&buf, 'X');  /* type X */
    /* Append 6 bytes de body. */
    owl_be_buffer_write_bytes(&buf, "ABCDEF", 6);
    postgres_msg_end(&buf, off);

    /* Total: 1 (type) + 4 (length) + 6 (body) = 11.
     * length = 4 + 6 = 10 */
    WIRE_CHECK(buf.length == 11, "msg_begin/end total bytes");
    uint32_t L = ((uint32_t)buf.data[1] << 24) | ((uint32_t)buf.data[2] << 16)
               | ((uint32_t)buf.data[3] <<  8) |  (uint32_t)buf.data[4];
    WIRE_CHECK(L == 10, "msg_end back-patche length correctement");
    owl_be_buffer_free(&buf);
}

/* ================================================================
 * postgres_read_msg_header
 * ================================================================ */
static void test_read_header(void) {
    /* "Q" + length=13 + "SELECT 1\0" */
    const uint8_t data[] = {
        0x51, 0x00, 0x00, 0x00, 0x0D,
        'S','E','L','E','C','T',' ','1', 0x00
    };
    OwlBeBuffer in;
    owl_be_buffer_attach_read(&in, data, sizeof(data));

    char type;
    uint32_t payload_len;
    WIRE_CHECK(postgres_read_msg_header(&in, &type, &payload_len),
                "read_header succeeds");
    WIRE_CHECK(type == 'Q', "type = Q");
    WIRE_CHECK(payload_len == 9, "payload_len = 9 (length 13 - 4)");
    /* pos doit etre sur le payload maintenant. */
    WIRE_CHECK(in.pos == 5, "pos avance apres header");

    /* Buffer trop court → echec. */
    const uint8_t shortBuf[] = { 0x51, 0x00, 0x00 };
    owl_be_buffer_attach_read(&in, shortBuf, sizeof(shortBuf));
    WIRE_CHECK(!postgres_read_msg_header(&in, &type, &payload_len),
                "read_header echec sur buffer tronque");
}

/* ================================================================
 * postgres_parse_auth — different sub-codes
 * ================================================================ */
static void test_parse_auth(void) {
    /* AuthenticationOK : payload = 00 00 00 00. */
    const uint8_t ok[] = { 0x00, 0x00, 0x00, 0x00 };
    int code = postgres_parse_auth(ok, sizeof(ok), NULL, NULL);
    WIRE_CHECK(code == POSTGRES_AUTH_OK, "parse_auth OK = 0");

    /* AuthenticationSASL : payload = 00 00 00 0A "SCRAM-SHA-256\0\0". */
    const uint8_t sasl[] = {
        0x00, 0x00, 0x00, 0x0A,
        'S','C','R','A','M','-','S','H','A','-','2','5','6', 0x00,
        0x00
    };
    const uint8_t* saslData = NULL;
    uint32_t saslLen = 0;
    code = postgres_parse_auth(sasl, sizeof(sasl), &saslData, &saslLen);
    WIRE_CHECK(code == POSTGRES_AUTH_SASL, "parse_auth SASL = 10");
    WIRE_CHECK(saslLen == sizeof(sasl) - 4, "parse_auth SASL data len");
    WIRE_CHECK(saslData == sasl + 4, "parse_auth SASL data ptr");
    WIRE_CHECK(memcmp(saslData, "SCRAM-SHA-256\0\0", 15) == 0,
                "parse_auth SASL mechanism list");

    /* Payload trop court. */
    code = postgres_parse_auth((const uint8_t*)"\x00\x00", 2, NULL, NULL);
    WIRE_CHECK(code == -1, "parse_auth rejette payload < 4 bytes");
}

/* ================================================================
 * postgres_parse_error_message
 *
 *   Format ErrorResponse payload :
 *     [code:1] [value:cstring] ... [0]
 *
 *   Vecteur : "SFATAL\0M\database does not exist\0\0"
 * ================================================================ */
static void test_parse_error(void) {
    const uint8_t payload[] = {
        'S', 'F','A','T','A','L', 0x00,
        'C', '3','D','0','0','0', 0x00,
        'M', 'd','a','t','a','b','a','s','e',' ','"','f','o','o','"',
             ' ','d','o','e','s',' ','n','o','t',' ','e','x','i','s','t', 0x00,
        0x00
    };
    const char* m = postgres_parse_error_message(payload, sizeof(payload));
    WIRE_CHECK(m != NULL, "parse_error trouve le champ M");
    WIRE_CHECK(m && strcmp(m, "database \"foo\" does not exist") == 0,
                "parse_error extrait le message exact");

    /* Pas de champ M → NULL. */
    const uint8_t no_m[] = {
        'S', 'F','A','T','A','L', 0x00,
        'C', '3','D','0','0','0', 0x00,
        0x00
    };
    WIRE_CHECK(postgres_parse_error_message(no_m, sizeof(no_m)) == NULL,
                "parse_error retourne NULL si pas de M");
}

int sql_postgres_wire_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_ssl_request();
    test_startup();
    test_query();
    test_terminate();
    test_password();
    test_sasl_initial();
    test_msg_begin_end();
    test_read_header();
    test_parse_auth();
    test_parse_error();

    printf("sql_postgres_wire self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
