// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * SHA-3 / Keccak-f[1600] — derived from tiny_sha3 (public domain).
 * Source originelle : https://github.com/mjosaarinen/tiny_sha3
 *
 * Couvre FIPS 202 : SHA3-224, SHA3-256, SHA3-384, SHA3-512.
 */

#include "sha3.h"
#include <string.h>

/* ================================================================
 * Keccak-f[1600] permutation
 * ================================================================ */

#define KECCAK_ROUNDS 24

static const uint64_t KECCAK_RC[KECCAK_ROUNDS] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL
};

static const int KECCAK_ROTC[24] = {
    1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
    27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44
};

static const int KECCAK_PI[24] = {
    10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1
};

#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static void keccak_f(uint64_t s[25]) {
    uint64_t bc[5], t;
    int i, j, r;

    for (r = 0; r < KECCAK_ROUNDS; r++) {
        /* Theta */
        for (i = 0; i < 5; i++) {
            bc[i] = s[i] ^ s[i+5] ^ s[i+10] ^ s[i+15] ^ s[i+20];
        }
        for (i = 0; i < 5; i++) {
            t = bc[(i+4)%5] ^ ROTL64(bc[(i+1)%5], 1);
            for (j = 0; j < 25; j += 5) s[j+i] ^= t;
        }
        /* Rho + Pi */
        t = s[1];
        for (i = 0; i < 24; i++) {
            j = KECCAK_PI[i];
            bc[0] = s[j];
            s[j] = ROTL64(t, KECCAK_ROTC[i]);
            t = bc[0];
        }
        /* Chi */
        for (j = 0; j < 25; j += 5) {
            for (i = 0; i < 5; i++) bc[i] = s[j+i];
            for (i = 0; i < 5; i++) {
                s[j+i] = bc[i] ^ ((~bc[(i+1)%5]) & bc[(i+2)%5]);
            }
        }
        /* Iota */
        s[0] ^= KECCAK_RC[r];
    }
}

/* ================================================================
 * SHA-3 sponge API
 * ================================================================ */

int owl_sha3_init(OwlSha3Ctx* ctx, int hashBits) {
    if (hashBits != 224 && hashBits != 256 && hashBits != 384 && hashBits != 512) {
        return 0;
    }
    memset(ctx->state, 0, sizeof(ctx->state));
    ctx->byteIndex = 0;
    ctx->hashBytes = hashBits / 8;
    /* rate = 1600 - 2*hashBits (en bits) */
    ctx->rate_bytes = (1600 - 2 * hashBits) / 8;
    return 1;
}

/* XOR un byte dans le state au position byteIdx (little-endian lane layout). */
static void absorb_byte(uint64_t state[25], int byteIdx, unsigned char b) {
    int laneIdx = byteIdx >> 3;       /* /8 */
    int byteInLane = byteIdx & 7;     /* %8 */
    state[laneIdx] ^= ((uint64_t)b) << (byteInLane * 8);
}

void owl_sha3_update(OwlSha3Ctx* ctx, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    while (len > 0) {
        absorb_byte(ctx->state, ctx->byteIndex, *p++);
        ctx->byteIndex++;
        len--;
        if (ctx->byteIndex == ctx->rate_bytes) {
            keccak_f(ctx->state);
            ctx->byteIndex = 0;
        }
    }
}

void owl_sha3_final(OwlSha3Ctx* ctx, unsigned char* out) {
    /* SHA-3 padding : append 0x06 puis 0x80 au dernier offset du bloc. */
    absorb_byte(ctx->state, ctx->byteIndex, 0x06);
    absorb_byte(ctx->state, ctx->rate_bytes - 1, 0x80);
    keccak_f(ctx->state);

    /* Squeeze : extraire hashBytes octets depuis le state (little-endian) */
    int extracted = 0;
    while (extracted < ctx->hashBytes) {
        int chunk = ctx->hashBytes - extracted;
        if (chunk > ctx->rate_bytes) chunk = ctx->rate_bytes;
        for (int i = 0; i < chunk; i++) {
            int laneIdx = i >> 3;
            int byteInLane = i & 7;
            out[extracted + i] = (unsigned char)(ctx->state[laneIdx] >> (byteInLane * 8));
        }
        extracted += chunk;
        if (extracted < ctx->hashBytes) {
            keccak_f(ctx->state);
        }
    }
}

int owl_sha3(int hashBits, const void* data, size_t len, unsigned char* out) {
    OwlSha3Ctx ctx;
    if (!owl_sha3_init(&ctx, hashBits)) return 0;
    owl_sha3_update(&ctx, data, len);
    owl_sha3_final(&ctx, out);
    return 1;
}
