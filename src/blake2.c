// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * BLAKE2b reference implementation -- adapte de RFC 7693 Appendix A.
 *
 * Algorithme :
 *   - State : 8 lanes 64-bit (h[0..7])
 *   - Block size : 128 bytes (16 lanes * 8 bytes)
 *   - 12 rounds de mixing par bloc, 8 calls a G() chacun
 *   - G() : 4 ops (add+xor+rotate) sur 4 lanes du working state v[16]
 */

#include "blake2.h"
#include <string.h>

/* Initial Vector -- meme que SHA-512 / RFC 7693 */
static const uint64_t BLAKE2B_IV[8] = {
    0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL,
    0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
    0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
    0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL
};

/* SIGMA -- 12 permutations de 0..15 utilisees pour selectionner les words
 * du message a chaque round. */
static const uint8_t BLAKE2B_SIGMA[12][16] = {
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    {14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    {11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    { 7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    { 9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    { 2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    {12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    {13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    { 6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    {10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    /* round 11+12 reutilisent rounds 0+1 */
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    {14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

/* Mixing function G : 4 ops sur 4 lanes du working state */
#define G(v, a, b, c, d, x, y) do {       \
    v[a] = v[a] + v[b] + (x);              \
    v[d] = ROTR64(v[d] ^ v[a], 32);        \
    v[c] = v[c] + v[d];                    \
    v[b] = ROTR64(v[b] ^ v[c], 24);        \
    v[a] = v[a] + v[b] + (y);              \
    v[d] = ROTR64(v[d] ^ v[a], 16);        \
    v[c] = v[c] + v[d];                    \
    v[b] = ROTR64(v[b] ^ v[c], 63);        \
} while (0)

static void blake2b_compress(OwlBlake2bCtx* ctx, const uint8_t block[128]) {
    uint64_t v[16];
    uint64_t m[16];

    /* Decode block en 16 lanes 64-bit little-endian */
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint64_t)block[i*8    ])      |
               ((uint64_t)block[i*8 + 1] <<  8) |
               ((uint64_t)block[i*8 + 2] << 16) |
               ((uint64_t)block[i*8 + 3] << 24) |
               ((uint64_t)block[i*8 + 4] << 32) |
               ((uint64_t)block[i*8 + 5] << 40) |
               ((uint64_t)block[i*8 + 6] << 48) |
               ((uint64_t)block[i*8 + 7] << 56);
    }

    /* Initialise working state v = h || IV ; XOR le counter dans v[12..13]
     * et le finalization flag dans v[14..15]. */
    for (int i = 0; i < 8; i++) v[i]     = ctx->h[i];
    for (int i = 0; i < 8; i++) v[i + 8] = BLAKE2B_IV[i];
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    v[14] ^= ctx->f[0];
    v[15] ^= ctx->f[1];

    /* 12 rounds */
    for (int r = 0; r < 12; r++) {
        const uint8_t* s = BLAKE2B_SIGMA[r];
        /* Column step */
        G(v, 0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);
        G(v, 1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
        G(v, 2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
        G(v, 3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
        /* Diagonal step */
        G(v, 0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);
        G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        G(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }

    /* h = h XOR low(v) XOR high(v) */
    for (int i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

static void blake2b_increment(OwlBlake2bCtx* ctx, uint64_t inc) {
    ctx->t[0] += inc;
    if (ctx->t[0] < inc) ctx->t[1]++;  /* overflow vers high */
}

int owl_blake2b_init(OwlBlake2bCtx* ctx, int hashBytes) {
    if (hashBytes < 1 || hashBytes > 64) return 0;
    /* Param block (RFC 7693 §3.1) -- on configure :
     *   digest length = hashBytes
     *   key length    = 0  (pas de keyed mode en V1)
     *   fanout/depth  = 1  (sequential mode)
     * Tout le reste a 0. Le 1er word du param XOR avec IV[0]. */
    for (int i = 0; i < 8; i++) ctx->h[i] = BLAKE2B_IV[i];
    ctx->h[0] ^= 0x01010000ULL ^ (uint64_t)hashBytes;
    /* (depth=1 << 16) | (fanout=1 << 8) | (keyLen=0 << 8) | (digestLen=hashBytes) */
    ctx->t[0] = 0;
    ctx->t[1] = 0;
    ctx->f[0] = 0;
    ctx->f[1] = 0;
    ctx->bufLen = 0;
    ctx->hashBytes = hashBytes;
    return 1;
}

void owl_blake2b_update(OwlBlake2bCtx* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    if (len == 0) return;

    /* Si on a deja des bytes en buffer, completer le bloc */
    if (ctx->bufLen > 0) {
        int avail = 128 - ctx->bufLen;
        if ((int)len > avail) {
            memcpy(ctx->buf + ctx->bufLen, p, (size_t)avail);
            blake2b_increment(ctx, 128);
            blake2b_compress(ctx, ctx->buf);
            ctx->bufLen = 0;
            p += avail;
            len -= (size_t)avail;
        } else {
            memcpy(ctx->buf + ctx->bufLen, p, len);
            ctx->bufLen += (int)len;
            return;
        }
    }
    /* Process full blocks directement depuis l'input -- mais on doit
     * laisser le DERNIER bloc pour final() (qui doit etre marque comme
     * tel via f[0]). Donc on s'arrete avec >= 1 byte restant. */
    while (len > 128) {
        blake2b_increment(ctx, 128);
        blake2b_compress(ctx, p);
        p += 128;
        len -= 128;
    }
    /* Buffer le reste (entre 1 et 128 bytes) */
    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->bufLen = (int)len;
    }
}

void owl_blake2b_final(OwlBlake2bCtx* ctx, unsigned char* out) {
    /* Increment counter avec le contenu restant du buffer (peut etre
     * partiel). Pad le buffer a 128 bytes avec des zeros. */
    blake2b_increment(ctx, (uint64_t)ctx->bufLen);
    while (ctx->bufLen < 128) ctx->buf[ctx->bufLen++] = 0;

    /* Marquer comme dernier bloc */
    ctx->f[0] = 0xFFFFFFFFFFFFFFFFULL;
    blake2b_compress(ctx, ctx->buf);

    /* Extraire les hashBytes premiers bytes du state h, little-endian */
    for (int i = 0; i < ctx->hashBytes; i++) {
        out[i] = (uint8_t)(ctx->h[i / 8] >> ((i % 8) * 8));
    }
}

int owl_blake2b(int hashBytes, const void* data, size_t len, unsigned char* out) {
    OwlBlake2bCtx ctx;
    if (!owl_blake2b_init(&ctx, hashBytes)) return 0;
    owl_blake2b_update(&ctx, data, len);
    owl_blake2b_final(&ctx, out);
    return 1;
}
