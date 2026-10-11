// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * SHA-3 / Keccak — implementation compacte pour OWL.
 *
 * Couvre les variantes hash standardisees FIPS 202 :
 *   SHA3-224, SHA3-256, SHA3-384, SHA3-512
 *
 * API streaming (init/update/final) pour permettre le hashing de gros
 * fichiers sans tout charger en RAM. Une API one-shot est aussi exposee.
 *
 * Implementation publique : derivee de tiny_sha3 par Markku-Juhani O.
 * Saarinen (https://github.com/mjosaarinen/tiny_sha3) -- domaine public.
 *
 * Note : pas de support SHAKE / cSHAKE / Keccak (pre-FIPS) en V1 ; les
 * 4 variantes hash standardisees couvrent toute la doc HA_SHA3_*.
 */

#ifndef owl_sha3_h
#define owl_sha3_h

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t state[25];      /* 5x5 lanes de 64 bits = 1600 bits */
    int rate_bytes;          /* taille du bloc d'absorption */
    int byteIndex;           /* offset courant dans le bloc */
    int hashBytes;           /* taille du digest final */
} OwlSha3Ctx;

/* hashBits = 224, 256, 384 ou 512. Retourne 1 OK, 0 si hashBits invalide. */
int owl_sha3_init(OwlSha3Ctx* ctx, int hashBits);

void owl_sha3_update(OwlSha3Ctx* ctx, const void* data, size_t len);

/* Ecrit hashBytes octets (hashBits/8) dans `out`. Apres cet appel, ctx
 * doit etre re-init avant tout autre usage. */
void owl_sha3_final(OwlSha3Ctx* ctx, unsigned char* out);

/* One-shot : hash complet en un appel. Convenience wrapper. */
int owl_sha3(int hashBits, const void* data, size_t len, unsigned char* out);

#endif
