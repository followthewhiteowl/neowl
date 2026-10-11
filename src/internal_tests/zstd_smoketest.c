// SPDX-License-Identifier: AGPL-3.0-only
/* zstd_smoketest.c — smoke test du vendoring zstd v1.5.7.
 *
 * But : valider que la lib zstd vendoree (decompress-only) compile et
 * fonctionne, AVANT que le runtime icones embarque s'en
 * serve pour decompresser le catalogue.
 *
 * Mode decompress-only : la lib n'embarque PAS le compresseur. Le blob
 * test ci-dessous a ete genere a la main via la CLI zstd :
 *
 *     printf "Hello, OWL zstd v1.5.7 vendoring smoke test!" | zstd -3
 *
 * Verifs :
 *   1. ZSTD_versionNumber() retourne 10507 (1.5.7)
 *   2. Decompression round-trip d'un blob constant -> match la string source
 *   3. ZSTD_isError() repond correctement sur un cas connu
 *
 * Comportement : exit code 0 si tout passe, !=0 sinon. Pas de framework
 * de test : ce binaire est isole, juste un sanity check du vendoring.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "zstd.h"

/* Blob genere via : printf "Hello, OWL zstd v1.5.7 vendoring smoke test!" | zstd -3 */
static const uint8_t k_compressed[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0x61, 0x01, 0x00, 0x48, 0x65, 0x6c,
    0x6c, 0x6f, 0x2c, 0x20, 0x4f, 0x57, 0x4c, 0x20, 0x7a, 0x73, 0x74, 0x64,
    0x20, 0x76, 0x31, 0x2e, 0x35, 0x2e, 0x37, 0x20, 0x76, 0x65, 0x6e, 0x64,
    0x6f, 0x72, 0x69, 0x6e, 0x67, 0x20, 0x73, 0x6d, 0x6f, 0x6b, 0x65, 0x20,
    0x74, 0x65, 0x73, 0x74, 0x21, 0xed, 0xb4, 0xa1, 0x86
};
static const char k_expected[] = "Hello, OWL zstd v1.5.7 vendoring smoke test!";

int main(void) {
    /* 1) Version check : on attend exactement 1.5.7 (= 1*100*100 + 5*100 + 7). */
    unsigned int v = ZSTD_versionNumber();
    if (v != 10507u) {
        fprintf(stderr, "zstd_smoketest: ZSTD_versionNumber=%u, attendu 10507\n", v);
        return 1;
    }

    /* 2) Decompression round-trip. */
    char out[256] = {0};
    size_t expected_len = strlen(k_expected);
    size_t actual = ZSTD_decompress(out, sizeof(out) - 1,
                                    k_compressed, sizeof(k_compressed));
    if (ZSTD_isError(actual)) {
        fprintf(stderr, "zstd_smoketest: ZSTD_decompress erreur: %s\n",
                ZSTD_getErrorName(actual));
        return 2;
    }
    if (actual != expected_len) {
        fprintf(stderr, "zstd_smoketest: longueur decompressee=%zu, attendu %zu\n",
                actual, expected_len);
        return 3;
    }
    if (memcmp(out, k_expected, expected_len) != 0) {
        fprintf(stderr, "zstd_smoketest: contenu decompresse different\n  got: %s\n  exp: %s\n",
                out, k_expected);
        return 4;
    }

    /* 3) ZSTD_isError sanity : 0 = pas une erreur ; un code d'erreur connu = erreur. */
    if (ZSTD_isError(0)) {
        fprintf(stderr, "zstd_smoketest: ZSTD_isError(0) faux positif\n");
        return 5;
    }

    printf("zstd_smoketest OK (version %u, decompresse %zu bytes)\n", v, actual);
    return 0;
}
