// SPDX-License-Identifier: AGPL-3.0-only
/* zint_smoketest.c — smoke test du vendoring zint 2.16.0 (backend BSD-3).
 *
 * But : valider que la brique code-barres vendoree compile, linke et produit
 * un raster RGB en memoire sur la toolchain OWL (host gcc + cross zig), AVANT
 * que les natives CodeBarre la consomment via un OBJ wrappe au GC.
 *
 * Ici aucun wrapping OWL : la lib tourne sur son propre malloc/free, on ne
 * touche QUE la sortie buffer (ZBarcode_Buffer -> symbol->bitmap RGB), jamais
 * la sortie fichier. Sanity check isole, sans framework (pattern
 * mpdecimal_smoketest / zstd_smoketest).
 *
 * Verifs (exit code 0 si tout passe, !=0 + message sinon) :
 *   1. QR Code (BARCODE_QRCODE)   : encode + buffer RGB non vide
 *   2. Code 128 (BARCODE_CODE128) : encode + buffer RGB non vide
 * On asserte a chaque fois error_number < ZINT_ERROR, bitmap != NULL et
 * bitmap_width/height > 0.
 */

#include <stdio.h>
#include <string.h>
#include "zint.h"

/* Encode `data` avec la symbologie `symbology`, verifie le buffer RGB.
   Retourne 0 si OK, sinon un code d'erreur distinct (base + offset). */
static int check_symbol(int symbology, const char *name, const char *data, int base_code) {
    struct zint_symbol *symbol = ZBarcode_Create();
    if (!symbol) {
        fprintf(stderr, "zint_smoketest: ZBarcode_Create a echoue (%s)\n", name);
        return base_code + 1;
    }
    symbol->symbology = symbology;

    int err = ZBarcode_Encode(symbol, (const unsigned char *)data, (int)strlen(data));
    if (err >= ZINT_ERROR) {
        fprintf(stderr, "zint_smoketest: %s ZBarcode_Encode err=%d (%s)\n",
                name, err, symbol->errtxt);
        ZBarcode_Delete(symbol);
        return base_code + 2;
    }

    err = ZBarcode_Buffer(symbol, 0);
    if (err >= ZINT_ERROR) {
        fprintf(stderr, "zint_smoketest: %s ZBarcode_Buffer err=%d (%s)\n",
                name, err, symbol->errtxt);
        ZBarcode_Delete(symbol);
        return base_code + 3;
    }

    if (symbol->bitmap == NULL || symbol->bitmap_width <= 0 || symbol->bitmap_height <= 0) {
        fprintf(stderr, "zint_smoketest: %s bitmap vide (ptr=%p, %dx%d)\n",
                name, (void *)symbol->bitmap, symbol->bitmap_width, symbol->bitmap_height);
        ZBarcode_Delete(symbol);
        return base_code + 4;
    }

    printf("  %s OK : %dx%d px RGB\n", name, symbol->bitmap_width, symbol->bitmap_height);
    ZBarcode_Delete(symbol);
    return 0;
}

int main(void) {
    int rc;

    /* 1) QR Code (2D). */
    rc = check_symbol(BARCODE_QRCODE, "QRCODE", "https://owl.example/facture/42", 10);
    if (rc) return rc;

    /* 2) Code 128 (1D). */
    rc = check_symbol(BARCODE_CODE128, "CODE128", "OWL-0899-2026", 20);
    if (rc) return rc;

    /* ZBarcode_Version() encode la version en MMmmpp (ex. 21600 = 2.16.0). */
    int v = ZBarcode_Version();
    printf("zint_smoketest OK (zint %d.%d.%d, QR + Code128 en buffer RGB)\n",
           v / 10000, (v / 100) % 100, v % 100);
    return 0;
}
