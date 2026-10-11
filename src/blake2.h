// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * BLAKE2b -- impl OWL.
 *
 * Reference : RFC 7693 (BLAKE2 cryptographic hash and MAC).
 *   https://www.rfc-editor.org/rfc/rfc7693
 *
 * BLAKE2b est plus rapide que SHA-2/SHA-3 sur CPU 64-bit. Output de
 * taille variable (1-64 bytes). En V1 OWL on expose 256/384/512 bits
 * (32/48/64 bytes) via les constantes HA_BLAKE2B_*.
 *
 * Note : le mode "keyed hash" natif de BLAKE2 (RFC 7693 §3.2) n'est
 * pas expose en V1 -- on utilise le pattern HMAC standard pour les
 * variantes HMAC, par coherence avec le reste du module crypto.
 *
 * Stack-only, pas d'allocation dynamique.
 */

#ifndef owl_blake2_h
#define owl_blake2_h

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t h[8];        /* hash state */
    uint64_t t[2];        /* counter (bytes processed, 128 bits) */
    uint64_t f[2];        /* finalization flags */
    uint8_t  buf[128];    /* input buffer (1 block = 128 bytes) */
    int      bufLen;
    int      hashBytes;   /* 1..64 */
} OwlBlake2bCtx;

/* hashBytes : 1..64. Retourne 1 OK, 0 si hashBytes invalide. */
int owl_blake2b_init(OwlBlake2bCtx* ctx, int hashBytes);

void owl_blake2b_update(OwlBlake2bCtx* ctx, const void* data, size_t len);

void owl_blake2b_final(OwlBlake2bCtx* ctx, unsigned char* out);

/* One-shot. */
int owl_blake2b(int hashBytes, const void* data, size_t len, unsigned char* out);

#endif
