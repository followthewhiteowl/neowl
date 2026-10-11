// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * RIPEMD-128 / RIPEMD-160 — implementation compacte pour OWL.
 *
 * Reference : RIPEMD specification par Antoon Bosselaers, Hans Dobbertin
 * et Bart Preneel (1996). Test vectors officiels :
 *   https://homes.esat.kuleuven.be/~bosselae/ripemd160.html
 *
 * Couvre :
 *   - RIPEMD-160 : output 160 bits (20 bytes) -- le plus utilise (Bitcoin).
 *   - RIPEMD-128 : output 128 bits (16 bytes) -- variante 4-rounds.
 *
 * API streaming + one-shot. Stack-only, pas d'allocation dynamique.
 *
 * Note : RIPEMD-256/320 ne sont pas inclus en V1 (jamais demandes par doc).
 */

#ifndef owl_ripemd_h
#define owl_ripemd_h

#include <stddef.h>
#include <stdint.h>

/* RIPEMD-160 (5 rounds, output 20 bytes) */
typedef struct {
    uint32_t h[5];
    uint8_t  buffer[64];
    uint64_t totalLen;
    int      bufLen;
} OwlRipemd160Ctx;

void owl_ripemd160_init(OwlRipemd160Ctx* ctx);
void owl_ripemd160_update(OwlRipemd160Ctx* ctx, const void* data, size_t len);
void owl_ripemd160_final(OwlRipemd160Ctx* ctx, unsigned char out[20]);
void owl_ripemd160(const void* data, size_t len, unsigned char out[20]);

/* RIPEMD-128 (4 rounds, output 16 bytes) */
typedef struct {
    uint32_t h[4];
    uint8_t  buffer[64];
    uint64_t totalLen;
    int      bufLen;
} OwlRipemd128Ctx;

void owl_ripemd128_init(OwlRipemd128Ctx* ctx);
void owl_ripemd128_update(OwlRipemd128Ctx* ctx, const void* data, size_t len);
void owl_ripemd128_final(OwlRipemd128Ctx* ctx, unsigned char out[16]);
void owl_ripemd128(const void* data, size_t len, unsigned char out[16]);

#endif
