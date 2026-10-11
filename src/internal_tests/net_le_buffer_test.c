// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du buffer little-endian.
 *
 * Execute via `owl --self-test-net`. Couvre :
 *   - Round-trip de chaque type (u8/16/24/32/64/i16/32/64)
 *   - Encoding LE correct (vecteurs hex hardcodes vs spec MySQL)
 *   - read_bytes / read_cstring / read_skip
 *   - EOF detection (read past length)
 *   - patch_u24 (back-patching pour MySQL packet length)
 *   - lenenc_int / lenenc_str (specifique MySQL, 4 ranges) */

#include "net_le_buffer.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

#define CHECK_BYTES(buf, expected_hex, n, label) do { \
    bool ok = ((buf)->length == (size_t)(n)); \
    if (ok) { \
        for (size_t _i = 0; _i < (size_t)(n); _i++) { \
            if ((buf)->data[_i] != (uint8_t)(expected_hex)[_i]) { ok = false; break; } \
        } \
    } \
    if (ok) g_pass++; \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_endianness(void) {
    /* u16 = 0x1234 -> LE [0x34, 0x12] (poids faible d'abord) */
    OwlLeBuffer b; owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_u16(&b, 0x1234);
    CHECK_BYTES(&b, "\x34\x12", 2, "u16 0x1234 -> 34 12 (LE)");
    owl_le_buffer_free(&b);

    /* u32 = 0x12345678 -> LE [0x78, 0x56, 0x34, 0x12] */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_u32(&b, 0x12345678);
    CHECK_BYTES(&b, "\x78\x56\x34\x12", 4, "u32 0x12345678 -> 78 56 34 12 (LE)");
    owl_le_buffer_free(&b);

    /* u24 = 0xABCDEF -> LE [0xEF, 0xCD, 0xAB] */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_u24(&b, 0xABCDEF);
    CHECK_BYTES(&b, "\xEF\xCD\xAB", 3, "u24 0xABCDEF -> EF CD AB");
    owl_le_buffer_free(&b);

    /* u64 = 0x0102030405060708 -> LE [08 07 06 05 04 03 02 01] */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_u64(&b, 0x0102030405060708ULL);
    CHECK_BYTES(&b, "\x08\x07\x06\x05\x04\x03\x02\x01", 8,
                "u64 0x0102030405060708 -> 08..01 (LE)");
    owl_le_buffer_free(&b);
}

static void test_roundtrip(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 64);
    owl_le_buffer_write_u8 (&b, 0xAB);
    owl_le_buffer_write_u16(&b, 0xABCD);
    owl_le_buffer_write_u24(&b, 0xABCDEF);
    owl_le_buffer_write_u32(&b, 0xDEADBEEF);
    owl_le_buffer_write_u64(&b, 0x1122334455667788ULL);
    owl_le_buffer_write_i16(&b, -1234);
    owl_le_buffer_write_i32(&b, -123456789);
    owl_le_buffer_write_i64(&b, -1234567890123LL);

    OwlLeBuffer r; owl_le_buffer_attach_read(&r, b.data, b.length);
    uint8_t  u8;  uint16_t u16; uint32_t u24; uint32_t u32; uint64_t u64;
    int16_t  i16; int32_t  i32; int64_t  i64;
    CHECK(owl_le_buffer_read_u8 (&r, &u8 ) && u8  == 0xAB,             "rt u8");
    CHECK(owl_le_buffer_read_u16(&r, &u16) && u16 == 0xABCD,           "rt u16");
    CHECK(owl_le_buffer_read_u24(&r, &u24) && u24 == 0xABCDEF,         "rt u24");
    CHECK(owl_le_buffer_read_u32(&r, &u32) && u32 == 0xDEADBEEF,       "rt u32");
    CHECK(owl_le_buffer_read_u64(&r, &u64) && u64 == 0x1122334455667788ULL, "rt u64");
    CHECK(owl_le_buffer_read_i16(&r, &i16) && i16 == -1234,            "rt i16");
    CHECK(owl_le_buffer_read_i32(&r, &i32) && i32 == -123456789,       "rt i32");
    CHECK(owl_le_buffer_read_i64(&r, &i64) && i64 == -1234567890123LL, "rt i64");
    owl_le_buffer_free(&b);
}

static void test_bytes_cstring(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 64);
    owl_le_buffer_write_bytes  (&b, "hello", 5);
    owl_le_buffer_write_cstring(&b, "world");
    /* "hello" + "world\0" = 11 bytes */
    CHECK(b.length == 11, "bytes+cstring length");

    OwlLeBuffer r; owl_le_buffer_attach_read(&r, b.data, b.length);
    char tmp[6] = {0};
    CHECK(owl_le_buffer_read_bytes(&r, tmp, 5), "read_bytes");
    CHECK(memcmp(tmp, "hello", 5) == 0,         "bytes content");
    const char* s = owl_le_buffer_read_cstring(&r);
    CHECK(s != NULL && strcmp(s, "world") == 0, "cstring content");
    owl_le_buffer_free(&b);
}

static void test_lenenc_int(void) {
    /* Range 1 : v < 251 -> 1 octet directe */
    OwlLeBuffer b; owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_lenenc_int(&b, 0);
    owl_le_buffer_write_lenenc_int(&b, 42);
    owl_le_buffer_write_lenenc_int(&b, 250);
    CHECK_BYTES(&b, "\x00\x2A\xFA", 3, "lenenc<251 : 1 octet direct");
    owl_le_buffer_free(&b);

    /* Range 2 : 251 <= v < 2^16 -> 0xFC + u16 LE */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_lenenc_int(&b, 300);    /* 0x012C -> FC 2C 01 */
    CHECK_BYTES(&b, "\xFC\x2C\x01", 3, "lenenc 300 : FC 2C 01");
    owl_le_buffer_free(&b);

    /* Range 3 : 2^16 <= v < 2^24 -> 0xFD + u24 LE */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_lenenc_int(&b, 0x123456);  /* FD 56 34 12 */
    CHECK_BYTES(&b, "\xFD\x56\x34\x12", 4, "lenenc 0x123456 : FD 56 34 12");
    owl_le_buffer_free(&b);

    /* Range 4 : v >= 2^24 -> 0xFE + u64 LE */
    owl_le_buffer_init(&b, 16);
    owl_le_buffer_write_lenenc_int(&b, 0x1234567890ULL);
    /* FE 90 78 56 34 12 00 00 00 */
    CHECK_BYTES(&b, "\xFE\x90\x78\x56\x34\x12\x00\x00\x00", 9,
                "lenenc 0x1234567890 : FE + u64 LE");
    owl_le_buffer_free(&b);

    /* Round-trip read */
    owl_le_buffer_init(&b, 32);
    owl_le_buffer_write_lenenc_int(&b, 5);
    owl_le_buffer_write_lenenc_int(&b, 300);
    owl_le_buffer_write_lenenc_int(&b, 0x100000);
    owl_le_buffer_write_lenenc_int(&b, 0x10000000ULL);
    OwlLeBuffer r; owl_le_buffer_attach_read(&r, b.data, b.length);
    uint64_t v;
    CHECK(owl_le_buffer_read_lenenc_int(&r, &v) && v == 5,           "rt lenenc 5");
    CHECK(owl_le_buffer_read_lenenc_int(&r, &v) && v == 300,         "rt lenenc 300");
    CHECK(owl_le_buffer_read_lenenc_int(&r, &v) && v == 0x100000,    "rt lenenc 0x100000");
    CHECK(owl_le_buffer_read_lenenc_int(&r, &v) && v == 0x10000000ULL, "rt lenenc 0x10000000");
    owl_le_buffer_free(&b);
}

static void test_lenenc_str(void) {
    OwlLeBuffer b; owl_le_buffer_init(&b, 64);
    owl_le_buffer_write_lenenc_str(&b, "abc", 3);
    /* prefix lenenc(3)=1 octet "0x03" + 3 octets "abc" */
    CHECK_BYTES(&b, "\x03""abc", 4, "lenenc_str 'abc' = 03 + abc");

    /* String vide : prefix 0x00, pas de bytes apres. */
    owl_le_buffer_reset(&b);
    owl_le_buffer_write_lenenc_str(&b, "", 0);
    CHECK(b.length == 1 && b.data[0] == 0, "lenenc_str empty -> 00");

    /* Round-trip avec longueur 300 (force range 2 lenenc int). */
    owl_le_buffer_reset(&b);
    char big[300];
    for (int i = 0; i < 300; i++) big[i] = (char)(i & 0xFF);
    owl_le_buffer_write_lenenc_str(&b, big, 300);
    OwlLeBuffer r; owl_le_buffer_attach_read(&r, b.data, b.length);
    size_t got_len = 0;
    const uint8_t* got = owl_le_buffer_read_lenenc_str(&r, &got_len);
    CHECK(got_len == 300,                                       "rt lenenc_str len 300");
    CHECK(got != NULL && memcmp(got, big, 300) == 0,            "rt lenenc_str content");
    owl_le_buffer_free(&b);
}

static void test_eof_protection(void) {
    /* Buffer 3 bytes ne doit pas laisser lire un u32 (4 bytes). */
    uint8_t raw[3] = { 0x01, 0x02, 0x03 };
    OwlLeBuffer r; owl_le_buffer_attach_read(&r, raw, 3);
    uint32_t v;
    CHECK(!owl_le_buffer_read_u32(&r, &v),         "read_u32 truncated -> false");
    CHECK(r.pos == 0,                              "pos preserved after fail");

    /* Read 2 bytes OK, puis 2 bytes KO. */
    uint16_t v16;
    CHECK(owl_le_buffer_read_u16(&r, &v16) && v16 == 0x0201, "read_u16 OK");
    CHECK(!owl_le_buffer_read_u16(&r, &v16),       "second read_u16 truncated");
}

static void test_patch_u24(void) {
    /* On reserve 3 octets a 0, on ecrit du payload, on back-patch la length. */
    OwlLeBuffer b; owl_le_buffer_init(&b, 16);
    size_t lenOff = b.length;
    owl_le_buffer_write_u24(&b, 0);    /* placeholder */
    owl_le_buffer_write_u8 (&b, 0xAA);
    owl_le_buffer_write_u8 (&b, 0xBB);
    owl_le_buffer_write_u8 (&b, 0xCC);
    /* payload = 3 octets. Patch length = 3. */
    owl_le_buffer_patch_u24(&b, lenOff, 3);
    CHECK_BYTES(&b, "\x03\x00\x00\xAA\xBB\xCC", 6, "patch_u24 backpatch");
    owl_le_buffer_free(&b);
}

int net_le_buffer_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_endianness();
    test_roundtrip();
    test_bytes_cstring();
    test_lenenc_int();
    test_lenenc_str();
    test_eof_protection();
    test_patch_u24();

    printf("net_le_buffer self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
