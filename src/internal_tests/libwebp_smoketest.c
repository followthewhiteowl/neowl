// SPDX-License-Identifier: AGPL-3.0-only
/* libwebp_smoketest.c — smoke test du vendoring libwebp 1.5.0 (BSD-3, decodeur-seul).
 *
 * But : valider que le sous-ensemble DECODEUR cure de libwebp (dec/ + dsp/ +
 * utils/, encodeur/mux/demux ecartes) compile, linke et DECODE reellement une
 * image WebP sur la toolchain OWL, AVANT qu'une native OWL le consomme (le
 * branchement dans src/owl_image_stb.c est un lot feature separe). Aucun
 * wrapping OWL ici : on tape directement l'API publique WebPGetInfo /
 * WebPDecodeRGBA, sans framework (pattern zint/libxls/llhttp smoketest).
 *
 * Fixture embarquee : un WebP lossless 1x1 (VP8L) d'un unique pixel RGBA
 * (0x12,0x34,0x56,0xFF). 38 octets, genere hors-ligne (pas de dependance a un
 * fichier au runtime, cf pilier all-include). Le test prouve :
 *   1. WebPGetInfo renvoie les bonnes dimensions (1x1) sans decoder.
 *   2. WebPDecodeRGBA rend un buffer non nul de la bonne taille et le pixel
 *      attendu (preuve que le chemin lossless VP8L -> ARGB tourne vraiment,
 *      pas juste que ca linke).
 *
 * Verifs : exit 0 si tout passe, != 0 + message sinon.
 */

#include <stdio.h>
#include <string.h>
#include "webp/decode.h"

/* WebP lossless 1x1, pixel RGBA = (0x12, 0x34, 0x56, 0xFF). */
static const unsigned char kWebp1x1[] = {
    0x52, 0x49, 0x46, 0x46, 0x1e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x11, 0x00, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x50, 0x9a, 0x4a, 0xd4, 0xaa, 0xff, 0x81, 0x88, 0xe8, 0x7f,
    0x00, 0x00,
};

static int fail(const char* msg) {
    fprintf(stderr, "libwebp_smoketest ECHEC : %s\n", msg);
    return 1;
}

int main(void) {
    int w = 0, h = 0;

    /* 1) Entete seule : dimensions sans decoder. */
    if (!WebPGetInfo(kWebp1x1, sizeof(kWebp1x1), &w, &h))
        return fail("WebPGetInfo a refuse la fixture WebP");
    if (w != 1 || h != 1) {
        fprintf(stderr, "  (dimensions obtenues %dx%d)\n", w, h);
        return fail("WebPGetInfo : dimensions != 1x1");
    }

    /* 2) Decodage complet VP8L -> RGBA. */
    w = h = 0;
    uint8_t* rgba = WebPDecodeRGBA(kWebp1x1, sizeof(kWebp1x1), &w, &h);
    if (rgba == NULL) return fail("WebPDecodeRGBA a renvoye NULL");
    if (w != 1 || h != 1) { WebPFree(rgba); return fail("WebPDecodeRGBA : dimensions != 1x1"); }

    int ok = (rgba[0] == 0x12 && rgba[1] == 0x34 && rgba[2] == 0x56 && rgba[3] == 0xFF);
    if (!ok)
        fprintf(stderr, "  (pixel obtenu %02x %02x %02x %02x)\n",
                rgba[0], rgba[1], rgba[2], rgba[3]);
    WebPFree(rgba);
    if (!ok) return fail("WebPDecodeRGBA : pixel RGBA decode incorrect");

    printf("libwebp_smoketest OK (WebP 1x1 VP8L -> RGBA, decodeur-seul)\n");
    return 0;
}
