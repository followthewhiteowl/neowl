// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * RIPEMD-128 / RIPEMD-160 -- reference implementation.
 *
 * Adapte de la specification publique RIPEMD par Bosselaers/Dobbertin/Preneel.
 * Test vectors : https://homes.esat.kuleuven.be/~bosselae/ripemd160.html
 *
 * L'algorithme :
 *   - Block size 512 bits (64 bytes).
 *   - 5 rounds parallels (left/right) de 16 sub-rounds chacun = 80 sub-ops/branch.
 *   - 5 fonctions non-lineaires f1..f5 alternees.
 *   - Output 160 bits : combine state[0..4] des branches L et R.
 *
 *   RIPEMD-128 = meme principe mais 4 rounds, 4 fonctions, output 128 bits.
 */

#include "ripemd.h"
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* ================================================================
 * Fonctions f1..f5 (boolean nonlinear)
 * ================================================================ */
#define F1(x, y, z) ((x) ^ (y) ^ (z))
#define F2(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define F3(x, y, z) (((x) | ~(y)) ^ (z))
#define F4(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define F5(x, y, z) ((x) ^ ((y) | ~(z)))

/* ================================================================
 * RIPEMD-160 -- 5 rounds, 20 bytes output
 * ================================================================ */

#define R160_FF(a, b, c, d, e, x, s) do { \
    (a) += F1((b),(c),(d)) + (x);          \
    (a) = ROL((a),(s)) + (e);              \
    (c) = ROL((c), 10);                    \
} while (0)

#define R160_GG(a, b, c, d, e, x, s) do { \
    (a) += F2((b),(c),(d)) + (x) + 0x5A827999UL; \
    (a) = ROL((a),(s)) + (e);              \
    (c) = ROL((c), 10);                    \
} while (0)

#define R160_HH(a, b, c, d, e, x, s) do { \
    (a) += F3((b),(c),(d)) + (x) + 0x6ED9EBA1UL; \
    (a) = ROL((a),(s)) + (e);              \
    (c) = ROL((c), 10);                    \
} while (0)

#define R160_II(a, b, c, d, e, x, s) do { \
    (a) += F4((b),(c),(d)) + (x) + 0x8F1BBCDCUL; \
    (a) = ROL((a),(s)) + (e);              \
    (c) = ROL((c), 10);                    \
} while (0)

#define R160_JJ(a, b, c, d, e, x, s) do { \
    (a) += F5((b),(c),(d)) + (x) + 0xA953FD4EUL; \
    (a) = ROL((a),(s)) + (e);              \
    (c) = ROL((c), 10);                    \
} while (0)

#define R160_FFF(a, b, c, d, e, x, s) do {  /* parallel F1 */ \
    (a) += F1((b),(c),(d)) + (x);            \
    (a) = ROL((a),(s)) + (e);                \
    (c) = ROL((c), 10);                      \
} while (0)

#define R160_GGG(a, b, c, d, e, x, s) do {  /* parallel F2 with const */ \
    (a) += F2((b),(c),(d)) + (x) + 0x7A6D76E9UL; \
    (a) = ROL((a),(s)) + (e);                \
    (c) = ROL((c), 10);                      \
} while (0)

#define R160_HHH(a, b, c, d, e, x, s) do {  /* parallel F3 */ \
    (a) += F3((b),(c),(d)) + (x) + 0x6D703EF3UL; \
    (a) = ROL((a),(s)) + (e);                \
    (c) = ROL((c), 10);                      \
} while (0)

#define R160_III(a, b, c, d, e, x, s) do {  /* parallel F4 */ \
    (a) += F4((b),(c),(d)) + (x) + 0x5C4DD124UL; \
    (a) = ROL((a),(s)) + (e);                \
    (c) = ROL((c), 10);                      \
} while (0)

#define R160_JJJ(a, b, c, d, e, x, s) do {  /* parallel F5 */ \
    (a) += F5((b),(c),(d)) + (x) + 0x50A28BE6UL; \
    (a) = ROL((a),(s)) + (e);                \
    (c) = ROL((c), 10);                      \
} while (0)

static void ripemd160_compress(uint32_t state[5], const uint8_t block[64]) {
    uint32_t X[16];
    for (int i = 0; i < 16; i++) {
        X[i] = ((uint32_t)block[i*4    ])      |
               ((uint32_t)block[i*4 + 1] <<  8) |
               ((uint32_t)block[i*4 + 2] << 16) |
               ((uint32_t)block[i*4 + 3] << 24);
    }
    uint32_t aa = state[0], bb = state[1], cc = state[2], dd = state[3], ee = state[4];
    uint32_t aaa = aa, bbb = bb, ccc = cc, ddd = dd, eee = ee;

    /* Round 1 (left) -- F1 */
    R160_FF(aa, bb, cc, dd, ee, X[ 0], 11);
    R160_FF(ee, aa, bb, cc, dd, X[ 1], 14);
    R160_FF(dd, ee, aa, bb, cc, X[ 2], 15);
    R160_FF(cc, dd, ee, aa, bb, X[ 3], 12);
    R160_FF(bb, cc, dd, ee, aa, X[ 4],  5);
    R160_FF(aa, bb, cc, dd, ee, X[ 5],  8);
    R160_FF(ee, aa, bb, cc, dd, X[ 6],  7);
    R160_FF(dd, ee, aa, bb, cc, X[ 7],  9);
    R160_FF(cc, dd, ee, aa, bb, X[ 8], 11);
    R160_FF(bb, cc, dd, ee, aa, X[ 9], 13);
    R160_FF(aa, bb, cc, dd, ee, X[10], 14);
    R160_FF(ee, aa, bb, cc, dd, X[11], 15);
    R160_FF(dd, ee, aa, bb, cc, X[12],  6);
    R160_FF(cc, dd, ee, aa, bb, X[13],  7);
    R160_FF(bb, cc, dd, ee, aa, X[14],  9);
    R160_FF(aa, bb, cc, dd, ee, X[15],  8);
    /* Round 2 (left) -- F2 */
    R160_GG(ee, aa, bb, cc, dd, X[ 7],  7);
    R160_GG(dd, ee, aa, bb, cc, X[ 4],  6);
    R160_GG(cc, dd, ee, aa, bb, X[13],  8);
    R160_GG(bb, cc, dd, ee, aa, X[ 1], 13);
    R160_GG(aa, bb, cc, dd, ee, X[10], 11);
    R160_GG(ee, aa, bb, cc, dd, X[ 6],  9);
    R160_GG(dd, ee, aa, bb, cc, X[15],  7);
    R160_GG(cc, dd, ee, aa, bb, X[ 3], 15);
    R160_GG(bb, cc, dd, ee, aa, X[12],  7);
    R160_GG(aa, bb, cc, dd, ee, X[ 0], 12);
    R160_GG(ee, aa, bb, cc, dd, X[ 9], 15);
    R160_GG(dd, ee, aa, bb, cc, X[ 5],  9);
    R160_GG(cc, dd, ee, aa, bb, X[ 2], 11);
    R160_GG(bb, cc, dd, ee, aa, X[14],  7);
    R160_GG(aa, bb, cc, dd, ee, X[11], 13);
    R160_GG(ee, aa, bb, cc, dd, X[ 8], 12);
    /* Round 3 (left) -- F3 */
    R160_HH(dd, ee, aa, bb, cc, X[ 3], 11);
    R160_HH(cc, dd, ee, aa, bb, X[10], 13);
    R160_HH(bb, cc, dd, ee, aa, X[14],  6);
    R160_HH(aa, bb, cc, dd, ee, X[ 4],  7);
    R160_HH(ee, aa, bb, cc, dd, X[ 9], 14);
    R160_HH(dd, ee, aa, bb, cc, X[15],  9);
    R160_HH(cc, dd, ee, aa, bb, X[ 8], 13);
    R160_HH(bb, cc, dd, ee, aa, X[ 1], 15);
    R160_HH(aa, bb, cc, dd, ee, X[ 2], 14);
    R160_HH(ee, aa, bb, cc, dd, X[ 7],  8);
    R160_HH(dd, ee, aa, bb, cc, X[ 0], 13);
    R160_HH(cc, dd, ee, aa, bb, X[ 6],  6);
    R160_HH(bb, cc, dd, ee, aa, X[13],  5);
    R160_HH(aa, bb, cc, dd, ee, X[11], 12);
    R160_HH(ee, aa, bb, cc, dd, X[ 5],  7);
    R160_HH(dd, ee, aa, bb, cc, X[12],  5);
    /* Round 4 (left) -- F4 */
    R160_II(cc, dd, ee, aa, bb, X[ 1], 11);
    R160_II(bb, cc, dd, ee, aa, X[ 9], 12);
    R160_II(aa, bb, cc, dd, ee, X[11], 14);
    R160_II(ee, aa, bb, cc, dd, X[10], 15);
    R160_II(dd, ee, aa, bb, cc, X[ 0], 14);
    R160_II(cc, dd, ee, aa, bb, X[ 8], 15);
    R160_II(bb, cc, dd, ee, aa, X[12],  9);
    R160_II(aa, bb, cc, dd, ee, X[ 4],  8);
    R160_II(ee, aa, bb, cc, dd, X[13],  9);
    R160_II(dd, ee, aa, bb, cc, X[ 3], 14);
    R160_II(cc, dd, ee, aa, bb, X[ 7],  5);
    R160_II(bb, cc, dd, ee, aa, X[15],  6);
    R160_II(aa, bb, cc, dd, ee, X[14],  8);
    R160_II(ee, aa, bb, cc, dd, X[ 5],  6);
    R160_II(dd, ee, aa, bb, cc, X[ 6],  5);
    R160_II(cc, dd, ee, aa, bb, X[ 2], 12);
    /* Round 5 (left) -- F5 */
    R160_JJ(bb, cc, dd, ee, aa, X[ 4],  9);
    R160_JJ(aa, bb, cc, dd, ee, X[ 0], 15);
    R160_JJ(ee, aa, bb, cc, dd, X[ 5],  5);
    R160_JJ(dd, ee, aa, bb, cc, X[ 9], 11);
    R160_JJ(cc, dd, ee, aa, bb, X[ 7],  6);
    R160_JJ(bb, cc, dd, ee, aa, X[12],  8);
    R160_JJ(aa, bb, cc, dd, ee, X[ 2], 13);
    R160_JJ(ee, aa, bb, cc, dd, X[10], 12);
    R160_JJ(dd, ee, aa, bb, cc, X[14],  5);
    R160_JJ(cc, dd, ee, aa, bb, X[ 1], 12);
    R160_JJ(bb, cc, dd, ee, aa, X[ 3], 13);
    R160_JJ(aa, bb, cc, dd, ee, X[ 8], 14);
    R160_JJ(ee, aa, bb, cc, dd, X[11], 11);
    R160_JJ(dd, ee, aa, bb, cc, X[ 6],  8);
    R160_JJ(cc, dd, ee, aa, bb, X[15],  5);
    R160_JJ(bb, cc, dd, ee, aa, X[13],  6);

    /* Parallel branch -- right -- starts at F5 then descends to F1 */
    /* Round 1 (right) -- F5 */
    R160_JJJ(aaa, bbb, ccc, ddd, eee, X[ 5],  8);
    R160_JJJ(eee, aaa, bbb, ccc, ddd, X[14],  9);
    R160_JJJ(ddd, eee, aaa, bbb, ccc, X[ 7],  9);
    R160_JJJ(ccc, ddd, eee, aaa, bbb, X[ 0], 11);
    R160_JJJ(bbb, ccc, ddd, eee, aaa, X[ 9], 13);
    R160_JJJ(aaa, bbb, ccc, ddd, eee, X[ 2], 15);
    R160_JJJ(eee, aaa, bbb, ccc, ddd, X[11], 15);
    R160_JJJ(ddd, eee, aaa, bbb, ccc, X[ 4],  5);
    R160_JJJ(ccc, ddd, eee, aaa, bbb, X[13],  7);
    R160_JJJ(bbb, ccc, ddd, eee, aaa, X[ 6],  7);
    R160_JJJ(aaa, bbb, ccc, ddd, eee, X[15],  8);
    R160_JJJ(eee, aaa, bbb, ccc, ddd, X[ 8], 11);
    R160_JJJ(ddd, eee, aaa, bbb, ccc, X[ 1], 14);
    R160_JJJ(ccc, ddd, eee, aaa, bbb, X[10], 14);
    R160_JJJ(bbb, ccc, ddd, eee, aaa, X[ 3], 12);
    R160_JJJ(aaa, bbb, ccc, ddd, eee, X[12],  6);
    /* Round 2 (right) -- F4 */
    R160_III(eee, aaa, bbb, ccc, ddd, X[ 6],  9);
    R160_III(ddd, eee, aaa, bbb, ccc, X[11], 13);
    R160_III(ccc, ddd, eee, aaa, bbb, X[ 3], 15);
    R160_III(bbb, ccc, ddd, eee, aaa, X[ 7],  7);
    R160_III(aaa, bbb, ccc, ddd, eee, X[ 0], 12);
    R160_III(eee, aaa, bbb, ccc, ddd, X[13],  8);
    R160_III(ddd, eee, aaa, bbb, ccc, X[ 5],  9);
    R160_III(ccc, ddd, eee, aaa, bbb, X[10], 11);
    R160_III(bbb, ccc, ddd, eee, aaa, X[14],  7);
    R160_III(aaa, bbb, ccc, ddd, eee, X[15],  7);
    R160_III(eee, aaa, bbb, ccc, ddd, X[ 8], 12);
    R160_III(ddd, eee, aaa, bbb, ccc, X[12],  7);
    R160_III(ccc, ddd, eee, aaa, bbb, X[ 4],  6);
    R160_III(bbb, ccc, ddd, eee, aaa, X[ 9], 15);
    R160_III(aaa, bbb, ccc, ddd, eee, X[ 1], 13);
    R160_III(eee, aaa, bbb, ccc, ddd, X[ 2], 11);
    /* Round 3 (right) -- F3 */
    R160_HHH(ddd, eee, aaa, bbb, ccc, X[15],  9);
    R160_HHH(ccc, ddd, eee, aaa, bbb, X[ 5],  7);
    R160_HHH(bbb, ccc, ddd, eee, aaa, X[ 1], 15);
    R160_HHH(aaa, bbb, ccc, ddd, eee, X[ 3], 11);
    R160_HHH(eee, aaa, bbb, ccc, ddd, X[ 7],  8);
    R160_HHH(ddd, eee, aaa, bbb, ccc, X[14],  6);
    R160_HHH(ccc, ddd, eee, aaa, bbb, X[ 6],  6);
    R160_HHH(bbb, ccc, ddd, eee, aaa, X[ 9], 14);
    R160_HHH(aaa, bbb, ccc, ddd, eee, X[11], 12);
    R160_HHH(eee, aaa, bbb, ccc, ddd, X[ 8], 13);
    R160_HHH(ddd, eee, aaa, bbb, ccc, X[12],  5);
    R160_HHH(ccc, ddd, eee, aaa, bbb, X[ 2], 14);
    R160_HHH(bbb, ccc, ddd, eee, aaa, X[10], 13);
    R160_HHH(aaa, bbb, ccc, ddd, eee, X[ 0], 13);
    R160_HHH(eee, aaa, bbb, ccc, ddd, X[ 4],  7);
    R160_HHH(ddd, eee, aaa, bbb, ccc, X[13],  5);
    /* Round 4 (right) -- F2 */
    R160_GGG(ccc, ddd, eee, aaa, bbb, X[ 8], 15);
    R160_GGG(bbb, ccc, ddd, eee, aaa, X[ 6],  5);
    R160_GGG(aaa, bbb, ccc, ddd, eee, X[ 4],  8);
    R160_GGG(eee, aaa, bbb, ccc, ddd, X[ 1], 11);
    R160_GGG(ddd, eee, aaa, bbb, ccc, X[ 3], 14);
    R160_GGG(ccc, ddd, eee, aaa, bbb, X[11], 14);
    R160_GGG(bbb, ccc, ddd, eee, aaa, X[15],  6);
    R160_GGG(aaa, bbb, ccc, ddd, eee, X[ 0], 14);
    R160_GGG(eee, aaa, bbb, ccc, ddd, X[ 5],  6);
    R160_GGG(ddd, eee, aaa, bbb, ccc, X[12],  9);
    R160_GGG(ccc, ddd, eee, aaa, bbb, X[ 2], 12);
    R160_GGG(bbb, ccc, ddd, eee, aaa, X[13],  9);
    R160_GGG(aaa, bbb, ccc, ddd, eee, X[ 9], 12);
    R160_GGG(eee, aaa, bbb, ccc, ddd, X[ 7],  5);
    R160_GGG(ddd, eee, aaa, bbb, ccc, X[10], 15);
    R160_GGG(ccc, ddd, eee, aaa, bbb, X[14],  8);
    /* Round 5 (right) -- F1 */
    R160_FFF(bbb, ccc, ddd, eee, aaa, X[12],  8);
    R160_FFF(aaa, bbb, ccc, ddd, eee, X[15],  5);
    R160_FFF(eee, aaa, bbb, ccc, ddd, X[10], 12);
    R160_FFF(ddd, eee, aaa, bbb, ccc, X[ 4],  9);
    R160_FFF(ccc, ddd, eee, aaa, bbb, X[ 1], 12);
    R160_FFF(bbb, ccc, ddd, eee, aaa, X[ 5],  5);
    R160_FFF(aaa, bbb, ccc, ddd, eee, X[ 8], 14);
    R160_FFF(eee, aaa, bbb, ccc, ddd, X[ 7],  6);
    R160_FFF(ddd, eee, aaa, bbb, ccc, X[ 6],  8);
    R160_FFF(ccc, ddd, eee, aaa, bbb, X[ 2], 13);
    R160_FFF(bbb, ccc, ddd, eee, aaa, X[13],  6);
    R160_FFF(aaa, bbb, ccc, ddd, eee, X[14],  5);
    R160_FFF(eee, aaa, bbb, ccc, ddd, X[ 0], 15);
    R160_FFF(ddd, eee, aaa, bbb, ccc, X[ 3], 13);
    R160_FFF(ccc, ddd, eee, aaa, bbb, X[ 9], 11);
    R160_FFF(bbb, ccc, ddd, eee, aaa, X[11], 11);

    /* Combine */
    uint32_t T = state[1] + cc + ddd;
    state[1] = state[2] + dd + eee;
    state[2] = state[3] + ee + aaa;
    state[3] = state[4] + aa + bbb;
    state[4] = state[0] + bb + ccc;
    state[0] = T;
}

void owl_ripemd160_init(OwlRipemd160Ctx* ctx) {
    ctx->h[0] = 0x67452301UL;
    ctx->h[1] = 0xEFCDAB89UL;
    ctx->h[2] = 0x98BADCFEUL;
    ctx->h[3] = 0x10325476UL;
    ctx->h[4] = 0xC3D2E1F0UL;
    ctx->totalLen = 0;
    ctx->bufLen = 0;
}

void owl_ripemd160_update(OwlRipemd160Ctx* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->totalLen += len;
    while (len > 0) {
        int chunk = 64 - ctx->bufLen;
        if ((int)len < chunk) chunk = (int)len;
        memcpy(ctx->buffer + ctx->bufLen, p, (size_t)chunk);
        ctx->bufLen += chunk;
        p += chunk;
        len -= chunk;
        if (ctx->bufLen == 64) {
            ripemd160_compress(ctx->h, ctx->buffer);
            ctx->bufLen = 0;
        }
    }
}

void owl_ripemd160_final(OwlRipemd160Ctx* ctx, unsigned char out[20]) {
    /* Padding : 0x80 puis zeros, puis longueur (bits) en little-endian sur 8 bytes. */
    uint64_t bits = ctx->totalLen * 8;
    ctx->buffer[ctx->bufLen++] = 0x80;
    if (ctx->bufLen > 56) {
        while (ctx->bufLen < 64) ctx->buffer[ctx->bufLen++] = 0;
        ripemd160_compress(ctx->h, ctx->buffer);
        ctx->bufLen = 0;
    }
    while (ctx->bufLen < 56) ctx->buffer[ctx->bufLen++] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (i * 8));
    }
    ripemd160_compress(ctx->h, ctx->buffer);
    /* Sortie little-endian */
    for (int i = 0; i < 5; i++) {
        out[i*4    ] = (uint8_t)(ctx->h[i]      );
        out[i*4 + 1] = (uint8_t)(ctx->h[i] >>  8);
        out[i*4 + 2] = (uint8_t)(ctx->h[i] >> 16);
        out[i*4 + 3] = (uint8_t)(ctx->h[i] >> 24);
    }
}

void owl_ripemd160(const void* data, size_t len, unsigned char out[20]) {
    OwlRipemd160Ctx ctx;
    owl_ripemd160_init(&ctx);
    owl_ripemd160_update(&ctx, data, len);
    owl_ripemd160_final(&ctx, out);
}

/* ================================================================
 * RIPEMD-128 -- 4 rounds, 16 bytes output
 * ================================================================ */

#define R128_FF(a, b, c, d, x, s) do { \
    (a) += F1((b),(c),(d)) + (x);       \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_GG(a, b, c, d, x, s) do { \
    (a) += F2((b),(c),(d)) + (x) + 0x5A827999UL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_HH(a, b, c, d, x, s) do { \
    (a) += F3((b),(c),(d)) + (x) + 0x6ED9EBA1UL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_II(a, b, c, d, x, s) do { \
    (a) += F4((b),(c),(d)) + (x) + 0x8F1BBCDCUL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_FFF(a, b, c, d, x, s) do { /* parallel F4 */ \
    (a) += F4((b),(c),(d)) + (x) + 0x50A28BE6UL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_GGG(a, b, c, d, x, s) do { /* parallel F3 */ \
    (a) += F3((b),(c),(d)) + (x) + 0x5C4DD124UL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_HHH(a, b, c, d, x, s) do { /* parallel F2 */ \
    (a) += F2((b),(c),(d)) + (x) + 0x6D703EF3UL; \
    (a) = ROL((a),(s));                 \
} while (0)

#define R128_III(a, b, c, d, x, s) do { /* parallel F1 */ \
    (a) += F1((b),(c),(d)) + (x);       \
    (a) = ROL((a),(s));                 \
} while (0)

static void ripemd128_compress(uint32_t state[4], const uint8_t block[64]) {
    uint32_t X[16];
    for (int i = 0; i < 16; i++) {
        X[i] = ((uint32_t)block[i*4    ])      |
               ((uint32_t)block[i*4 + 1] <<  8) |
               ((uint32_t)block[i*4 + 2] << 16) |
               ((uint32_t)block[i*4 + 3] << 24);
    }
    uint32_t aa = state[0], bb = state[1], cc = state[2], dd = state[3];
    uint32_t aaa = aa, bbb = bb, ccc = cc, ddd = dd;

    /* Round 1 (left) -- F1 */
    R128_FF(aa, bb, cc, dd, X[ 0], 11);
    R128_FF(dd, aa, bb, cc, X[ 1], 14);
    R128_FF(cc, dd, aa, bb, X[ 2], 15);
    R128_FF(bb, cc, dd, aa, X[ 3], 12);
    R128_FF(aa, bb, cc, dd, X[ 4],  5);
    R128_FF(dd, aa, bb, cc, X[ 5],  8);
    R128_FF(cc, dd, aa, bb, X[ 6],  7);
    R128_FF(bb, cc, dd, aa, X[ 7],  9);
    R128_FF(aa, bb, cc, dd, X[ 8], 11);
    R128_FF(dd, aa, bb, cc, X[ 9], 13);
    R128_FF(cc, dd, aa, bb, X[10], 14);
    R128_FF(bb, cc, dd, aa, X[11], 15);
    R128_FF(aa, bb, cc, dd, X[12],  6);
    R128_FF(dd, aa, bb, cc, X[13],  7);
    R128_FF(cc, dd, aa, bb, X[14],  9);
    R128_FF(bb, cc, dd, aa, X[15],  8);
    /* Round 2 (left) -- F2 */
    R128_GG(aa, bb, cc, dd, X[ 7],  7);
    R128_GG(dd, aa, bb, cc, X[ 4],  6);
    R128_GG(cc, dd, aa, bb, X[13],  8);
    R128_GG(bb, cc, dd, aa, X[ 1], 13);
    R128_GG(aa, bb, cc, dd, X[10], 11);
    R128_GG(dd, aa, bb, cc, X[ 6],  9);
    R128_GG(cc, dd, aa, bb, X[15],  7);
    R128_GG(bb, cc, dd, aa, X[ 3], 15);
    R128_GG(aa, bb, cc, dd, X[12],  7);
    R128_GG(dd, aa, bb, cc, X[ 0], 12);
    R128_GG(cc, dd, aa, bb, X[ 9], 15);
    R128_GG(bb, cc, dd, aa, X[ 5],  9);
    R128_GG(aa, bb, cc, dd, X[ 2], 11);
    R128_GG(dd, aa, bb, cc, X[14],  7);
    R128_GG(cc, dd, aa, bb, X[11], 13);
    R128_GG(bb, cc, dd, aa, X[ 8], 12);
    /* Round 3 (left) -- F3 */
    R128_HH(aa, bb, cc, dd, X[ 3], 11);
    R128_HH(dd, aa, bb, cc, X[10], 13);
    R128_HH(cc, dd, aa, bb, X[14],  6);
    R128_HH(bb, cc, dd, aa, X[ 4],  7);
    R128_HH(aa, bb, cc, dd, X[ 9], 14);
    R128_HH(dd, aa, bb, cc, X[15],  9);
    R128_HH(cc, dd, aa, bb, X[ 8], 13);
    R128_HH(bb, cc, dd, aa, X[ 1], 15);
    R128_HH(aa, bb, cc, dd, X[ 2], 14);
    R128_HH(dd, aa, bb, cc, X[ 7],  8);
    R128_HH(cc, dd, aa, bb, X[ 0], 13);
    R128_HH(bb, cc, dd, aa, X[ 6],  6);
    R128_HH(aa, bb, cc, dd, X[13],  5);
    R128_HH(dd, aa, bb, cc, X[11], 12);
    R128_HH(cc, dd, aa, bb, X[ 5],  7);
    R128_HH(bb, cc, dd, aa, X[12],  5);
    /* Round 4 (left) -- F4 */
    R128_II(aa, bb, cc, dd, X[ 1], 11);
    R128_II(dd, aa, bb, cc, X[ 9], 12);
    R128_II(cc, dd, aa, bb, X[11], 14);
    R128_II(bb, cc, dd, aa, X[10], 15);
    R128_II(aa, bb, cc, dd, X[ 0], 14);
    R128_II(dd, aa, bb, cc, X[ 8], 15);
    R128_II(cc, dd, aa, bb, X[12],  9);
    R128_II(bb, cc, dd, aa, X[ 4],  8);
    R128_II(aa, bb, cc, dd, X[13],  9);
    R128_II(dd, aa, bb, cc, X[ 3], 14);
    R128_II(cc, dd, aa, bb, X[ 7],  5);
    R128_II(bb, cc, dd, aa, X[15],  6);
    R128_II(aa, bb, cc, dd, X[14],  8);
    R128_II(dd, aa, bb, cc, X[ 5],  6);
    R128_II(cc, dd, aa, bb, X[ 6],  5);
    R128_II(bb, cc, dd, aa, X[ 2], 12);

    /* Round 1 (right) -- F4 */
    R128_FFF(aaa, bbb, ccc, ddd, X[ 5],  8);
    R128_FFF(ddd, aaa, bbb, ccc, X[14],  9);
    R128_FFF(ccc, ddd, aaa, bbb, X[ 7],  9);
    R128_FFF(bbb, ccc, ddd, aaa, X[ 0], 11);
    R128_FFF(aaa, bbb, ccc, ddd, X[ 9], 13);
    R128_FFF(ddd, aaa, bbb, ccc, X[ 2], 15);
    R128_FFF(ccc, ddd, aaa, bbb, X[11], 15);
    R128_FFF(bbb, ccc, ddd, aaa, X[ 4],  5);
    R128_FFF(aaa, bbb, ccc, ddd, X[13],  7);
    R128_FFF(ddd, aaa, bbb, ccc, X[ 6],  7);
    R128_FFF(ccc, ddd, aaa, bbb, X[15],  8);
    R128_FFF(bbb, ccc, ddd, aaa, X[ 8], 11);
    R128_FFF(aaa, bbb, ccc, ddd, X[ 1], 14);
    R128_FFF(ddd, aaa, bbb, ccc, X[10], 14);
    R128_FFF(ccc, ddd, aaa, bbb, X[ 3], 12);
    R128_FFF(bbb, ccc, ddd, aaa, X[12],  6);
    /* Round 2 (right) -- F3 */
    R128_GGG(aaa, bbb, ccc, ddd, X[ 6],  9);
    R128_GGG(ddd, aaa, bbb, ccc, X[11], 13);
    R128_GGG(ccc, ddd, aaa, bbb, X[ 3], 15);
    R128_GGG(bbb, ccc, ddd, aaa, X[ 7],  7);
    R128_GGG(aaa, bbb, ccc, ddd, X[ 0], 12);
    R128_GGG(ddd, aaa, bbb, ccc, X[13],  8);
    R128_GGG(ccc, ddd, aaa, bbb, X[ 5],  9);
    R128_GGG(bbb, ccc, ddd, aaa, X[10], 11);
    R128_GGG(aaa, bbb, ccc, ddd, X[14],  7);
    R128_GGG(ddd, aaa, bbb, ccc, X[15],  7);
    R128_GGG(ccc, ddd, aaa, bbb, X[ 8], 12);
    R128_GGG(bbb, ccc, ddd, aaa, X[12],  7);
    R128_GGG(aaa, bbb, ccc, ddd, X[ 4],  6);
    R128_GGG(ddd, aaa, bbb, ccc, X[ 9], 15);
    R128_GGG(ccc, ddd, aaa, bbb, X[ 1], 13);
    R128_GGG(bbb, ccc, ddd, aaa, X[ 2], 11);
    /* Round 3 (right) -- F2 */
    R128_HHH(aaa, bbb, ccc, ddd, X[15],  9);
    R128_HHH(ddd, aaa, bbb, ccc, X[ 5],  7);
    R128_HHH(ccc, ddd, aaa, bbb, X[ 1], 15);
    R128_HHH(bbb, ccc, ddd, aaa, X[ 3], 11);
    R128_HHH(aaa, bbb, ccc, ddd, X[ 7],  8);
    R128_HHH(ddd, aaa, bbb, ccc, X[14],  6);
    R128_HHH(ccc, ddd, aaa, bbb, X[ 6],  6);
    R128_HHH(bbb, ccc, ddd, aaa, X[ 9], 14);
    R128_HHH(aaa, bbb, ccc, ddd, X[11], 12);
    R128_HHH(ddd, aaa, bbb, ccc, X[ 8], 13);
    R128_HHH(ccc, ddd, aaa, bbb, X[12],  5);
    R128_HHH(bbb, ccc, ddd, aaa, X[ 2], 14);
    R128_HHH(aaa, bbb, ccc, ddd, X[10], 13);
    R128_HHH(ddd, aaa, bbb, ccc, X[ 0], 13);
    R128_HHH(ccc, ddd, aaa, bbb, X[ 4],  7);
    R128_HHH(bbb, ccc, ddd, aaa, X[13],  5);
    /* Round 4 (right) -- F1 */
    R128_III(aaa, bbb, ccc, ddd, X[ 8], 15);
    R128_III(ddd, aaa, bbb, ccc, X[ 6],  5);
    R128_III(ccc, ddd, aaa, bbb, X[ 4],  8);
    R128_III(bbb, ccc, ddd, aaa, X[ 1], 11);
    R128_III(aaa, bbb, ccc, ddd, X[ 3], 14);
    R128_III(ddd, aaa, bbb, ccc, X[11], 14);
    R128_III(ccc, ddd, aaa, bbb, X[15],  6);
    R128_III(bbb, ccc, ddd, aaa, X[ 0], 14);
    R128_III(aaa, bbb, ccc, ddd, X[ 5],  6);
    R128_III(ddd, aaa, bbb, ccc, X[12],  9);
    R128_III(ccc, ddd, aaa, bbb, X[ 2], 12);
    R128_III(bbb, ccc, ddd, aaa, X[13],  9);
    R128_III(aaa, bbb, ccc, ddd, X[ 9], 12);
    R128_III(ddd, aaa, bbb, ccc, X[ 7],  5);
    R128_III(ccc, ddd, aaa, bbb, X[10], 15);
    R128_III(bbb, ccc, ddd, aaa, X[14],  8);

    /* Combine */
    uint32_t T = state[1] + cc + ddd;
    state[1] = state[2] + dd + aaa;
    state[2] = state[3] + aa + bbb;
    state[3] = state[0] + bb + ccc;
    state[0] = T;
}

void owl_ripemd128_init(OwlRipemd128Ctx* ctx) {
    ctx->h[0] = 0x67452301UL;
    ctx->h[1] = 0xEFCDAB89UL;
    ctx->h[2] = 0x98BADCFEUL;
    ctx->h[3] = 0x10325476UL;
    ctx->totalLen = 0;
    ctx->bufLen = 0;
}

void owl_ripemd128_update(OwlRipemd128Ctx* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->totalLen += len;
    while (len > 0) {
        int chunk = 64 - ctx->bufLen;
        if ((int)len < chunk) chunk = (int)len;
        memcpy(ctx->buffer + ctx->bufLen, p, (size_t)chunk);
        ctx->bufLen += chunk;
        p += chunk;
        len -= chunk;
        if (ctx->bufLen == 64) {
            ripemd128_compress(ctx->h, ctx->buffer);
            ctx->bufLen = 0;
        }
    }
}

void owl_ripemd128_final(OwlRipemd128Ctx* ctx, unsigned char out[16]) {
    uint64_t bits = ctx->totalLen * 8;
    ctx->buffer[ctx->bufLen++] = 0x80;
    if (ctx->bufLen > 56) {
        while (ctx->bufLen < 64) ctx->buffer[ctx->bufLen++] = 0;
        ripemd128_compress(ctx->h, ctx->buffer);
        ctx->bufLen = 0;
    }
    while (ctx->bufLen < 56) ctx->buffer[ctx->bufLen++] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (i * 8));
    }
    ripemd128_compress(ctx->h, ctx->buffer);
    for (int i = 0; i < 4; i++) {
        out[i*4    ] = (uint8_t)(ctx->h[i]      );
        out[i*4 + 1] = (uint8_t)(ctx->h[i] >>  8);
        out[i*4 + 2] = (uint8_t)(ctx->h[i] >> 16);
        out[i*4 + 3] = (uint8_t)(ctx->h[i] >> 24);
    }
}

void owl_ripemd128(const void* data, size_t len, unsigned char out[16]) {
    OwlRipemd128Ctx ctx;
    owl_ripemd128_init(&ctx);
    owl_ripemd128_update(&ctx, data, len);
    owl_ripemd128_final(&ctx, out);
}
