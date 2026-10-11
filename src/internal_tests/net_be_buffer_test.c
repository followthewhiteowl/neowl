// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du buffer big-endian.
 *
 * Exécuté via `owl --self-test-net` (cf main.c). Couvre :
 *   - Round-trip de chaque type (u8/16/32/64/i16/32/64)
 *   - Encoding BE correct (vecteurs hex hardcodés)
 *   - read_bytes / read_cstring / read_skip
 *   - EOF detection (read past length)
 *   - patch_u32 (back-patching pour PG length)
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'échecs.
 * Imprime un résumé sur stdout. */

#include "net_be_buffer.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

#define CHECK_BYTES(buf, expected_hex, n, label) do { \
    bool ok = (buf->length == (n)); \
    if (ok) { \
        for (size_t _i = 0; _i < (size_t)(n); _i++) { \
            if (buf->data[_i] != (uint8_t)expected_hex[_i]) { ok = false; break; } \
        } \
    } \
    CHECK(ok, label); \
} while (0)

static void test_write_be_encoding(void) {
    OwlBeBuffer buf;
    owl_be_buffer_init(&buf, 0);

    /* u8 */
    owl_be_buffer_write_u8(&buf, 0x42);
    CHECK_BYTES((&buf), ((const uint8_t[]){0x42}), 1, "u8 encoding");
    owl_be_buffer_reset(&buf);

    /* u16 BE : 0x1234 -> 12 34 */
    owl_be_buffer_write_u16(&buf, 0x1234);
    CHECK_BYTES((&buf), ((const uint8_t[]){0x12, 0x34}), 2, "u16 BE encoding");
    owl_be_buffer_reset(&buf);

    /* u32 BE : 0xDEADBEEF -> DE AD BE EF */
    owl_be_buffer_write_u32(&buf, 0xDEADBEEFu);
    CHECK_BYTES((&buf), ((const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF}), 4, "u32 BE encoding");
    owl_be_buffer_reset(&buf);

    /* u64 BE : 0x0123456789ABCDEF */
    owl_be_buffer_write_u64(&buf, 0x0123456789ABCDEFull);
    CHECK_BYTES((&buf),
        ((const uint8_t[]){0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}),
        8, "u64 BE encoding");
    owl_be_buffer_reset(&buf);

    /* cstring : "hi" -> 'h' 'i' 0x00 */
    owl_be_buffer_write_cstring(&buf, "hi");
    CHECK_BYTES((&buf), ((const uint8_t[]){'h', 'i', 0x00}), 3, "cstring encoding");

    owl_be_buffer_free(&buf);
}

static void test_round_trip(void) {
    OwlBeBuffer buf;
    owl_be_buffer_init(&buf, 0);

    owl_be_buffer_write_u8 (&buf, 0xAB);
    owl_be_buffer_write_u16(&buf, 0x1234);
    owl_be_buffer_write_u32(&buf, 0xDEADBEEFu);
    owl_be_buffer_write_u64(&buf, 0x0123456789ABCDEFull);
    owl_be_buffer_write_i16(&buf, -1);
    owl_be_buffer_write_i32(&buf, -42);
    owl_be_buffer_write_i64(&buf, -1234567890LL);
    owl_be_buffer_write_cstring(&buf, "world");
    owl_be_buffer_write_bytes(&buf, "\xCA\xFE", 2);

    /* Bascule en mode lecture sur le contenu écrit. */
    OwlBeBuffer rd;
    owl_be_buffer_attach_read(&rd, buf.data, buf.length);

    uint8_t  u8;   CHECK(owl_be_buffer_read_u8 (&rd, &u8 ) && u8  == 0xAB, "round-trip u8");
    uint16_t u16;  CHECK(owl_be_buffer_read_u16(&rd, &u16) && u16 == 0x1234, "round-trip u16");
    uint32_t u32;  CHECK(owl_be_buffer_read_u32(&rd, &u32) && u32 == 0xDEADBEEFu, "round-trip u32");
    uint64_t u64;  CHECK(owl_be_buffer_read_u64(&rd, &u64) && u64 == 0x0123456789ABCDEFull, "round-trip u64");
    int16_t  i16;  CHECK(owl_be_buffer_read_i16(&rd, &i16) && i16 == -1, "round-trip i16 neg");
    int32_t  i32;  CHECK(owl_be_buffer_read_i32(&rd, &i32) && i32 == -42, "round-trip i32 neg");
    int64_t  i64;  CHECK(owl_be_buffer_read_i64(&rd, &i64) && i64 == -1234567890LL, "round-trip i64 neg");

    const char* s = owl_be_buffer_read_cstring(&rd);
    CHECK(s != NULL && strcmp(s, "world") == 0, "round-trip cstring");

    uint8_t bytes[2] = {0};
    CHECK(owl_be_buffer_read_bytes(&rd, bytes, 2)
       && bytes[0] == 0xCA && bytes[1] == 0xFE, "round-trip bytes");

    CHECK(owl_be_buffer_eof(&rd), "EOF apres read complet");

    owl_be_buffer_free(&buf);
}

static void test_eof_detection(void) {
    OwlBeBuffer rd;
    uint8_t data[] = {0x12, 0x34};
    owl_be_buffer_attach_read(&rd, data, 2);

    uint16_t u16; CHECK(owl_be_buffer_read_u16(&rd, &u16) && u16 == 0x1234, "u16 OK");
    /* Buffer epuise. */
    uint8_t u8;   CHECK(!owl_be_buffer_read_u8(&rd, &u8), "u8 EOF detecte");
    CHECK(owl_be_buffer_eof(&rd), "EOF flag");
    CHECK(owl_be_buffer_remaining(&rd) == 0, "remaining=0");

    /* Read trop gros : doit échouer sans avancer pos. */
    OwlBeBuffer rd2;
    owl_be_buffer_attach_read(&rd2, data, 2);
    uint32_t u32; CHECK(!owl_be_buffer_read_u32(&rd2, &u32), "u32 trop gros rejette");
    /* pos doit etre encore a 0. */
    uint16_t v; CHECK(owl_be_buffer_read_u16(&rd2, &v) && v == 0x1234, "pos preserve apres echec");
}

static void test_cstring_no_null(void) {
    /* cstring sans '\0' final → retourne NULL et ne consomme rien. */
    OwlBeBuffer rd;
    uint8_t data[] = {'a', 'b', 'c'};
    owl_be_buffer_attach_read(&rd, data, 3);

    const char* s = owl_be_buffer_read_cstring(&rd);
    CHECK(s == NULL, "cstring sans \\0 -> NULL");
    CHECK(rd.pos == 0, "pos preserve apres echec cstring");
}

static void test_patch_u32(void) {
    /* Cas usage PG : on écrit un header de message avec length = 0 placeholder,
     * on remplit le payload, puis on back-patche la length. */
    OwlBeBuffer buf;
    owl_be_buffer_init(&buf, 0);

    owl_be_buffer_write_u8(&buf, 'Q');         /* type de message (Query) */
    size_t lenOffset = buf.length;
    owl_be_buffer_write_u32(&buf, 0);          /* placeholder length */
    owl_be_buffer_write_cstring(&buf, "SELECT 1");

    /* Total length du message PG = inclut le u32 length lui-meme mais PAS le
     * byte de type. Soit (buf.length - 1). */
    uint32_t msgLen = (uint32_t)(buf.length - 1);
    owl_be_buffer_patch_u32(&buf, lenOffset, msgLen);

    /* Verifie que les 4 bytes a lenOffset sont bien le BE de msgLen. */
    uint32_t patched = ((uint32_t)buf.data[lenOffset    ] << 24)
                     | ((uint32_t)buf.data[lenOffset + 1] << 16)
                     | ((uint32_t)buf.data[lenOffset + 2] <<  8)
                     | ((uint32_t)buf.data[lenOffset + 3]);
    CHECK(patched == msgLen, "patch_u32 back-patches correctement");

    owl_be_buffer_free(&buf);
}

static void test_grow(void) {
    /* Force plusieurs grow successifs en remplissant au-dela de la
     * capacite initiale. */
    OwlBeBuffer buf;
    owl_be_buffer_init(&buf, 4);
    for (int i = 0; i < 1000; i++) {
        owl_be_buffer_write_u8(&buf, (uint8_t)(i & 0xFF));
    }
    CHECK(buf.length == 1000, "grow capacity to 1000 bytes");
    CHECK(buf.capacity >= 1000, "capacity follows");
    /* Verifie un octet au hasard. */
    CHECK(buf.data[500] == (uint8_t)(500 & 0xFF), "contenu preserve apres grow");
    owl_be_buffer_free(&buf);
}

int net_be_buffer_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_write_be_encoding();
    test_round_trip();
    test_eof_detection();
    test_cstring_no_null();
    test_patch_u32();
    test_grow();

    printf("net_be_buffer self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
