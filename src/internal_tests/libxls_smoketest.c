// SPDX-License-Identifier: AGPL-3.0-only
/* libxls_smoketest.c — smoke test du vendoring libxls 1.6.3 (BSD-2-Clause).
 *
 * But : valider que le lecteur .xls binaire herite (BIFF8 dans conteneur OLE2)
 * vendore compile, linke et DECODE CORRECTEMENT sur la toolchain OWL, AVANT
 * qu'une native OWL le consomme (le cablage dans stdlib_xls.c est un lot
 * feature separe). Aucun wrapping OWL ici : on tape directement l'API publique
 * xls_open_file / xls_getWorkSheet / xls_parseWorkSheet, sans framework
 * (pattern zint_smoketest / argon2_smoketest / llhttp_smoketest).
 *
 * Le point dur du vendoring etait le charset : BIFF8 stocke les chaines soit
 * compressees (latin1, 1 octet/char) soit en UTF-16LE. libxls convertit vers
 * l'encodage cible ("UTF-8") via iconv SI HAVE_ICONV. On a DELIBEREMENT bati
 * SANS iconv (aucune dependance externe, cf. vendor/libxls/src/config.h). Le
 * chemin sans-iconv force une locale UTF-8 par plateforme
 * (_create_locale(".65001") Windows / newlocale "C.UTF-8" Linux). Ce test
 * PROUVE au runtime que ce chemin produit de l'UTF-8 correct, y compris sur des
 * chaines UTF-16 non-ASCII a diacritiques (vietnamien) — c'est la validation
 * qui a leve le doute iconv.
 *
 * Fixtures : vendor/libxls/testdata/{mtcars,vietnamese-utf8}.xls (corpus amont
 * BSD-2). Chemins RELATIFS a la racine repo (CWD des smoketests dans la
 * batterie). Un chemin de repertoire alternatif peut etre passe en argv[1].
 *
 * Verifs (exit 0 si tout passe, != 0 + message sinon) :
 *   1. mtcars.xls : 1 feuille, cellule [0,0] == "mpg" (SST/LABELSST), au moins
 *      une cellule numerique (NUMBER/RK) decodee.
 *   2. vietnamese-utf8.xls : cellule [0,0] == octets UTF-8 EXACTS de "Mo ta"
 *      diacrite (4d c3 b4 20 74 e1 ba a3) -> preuve UTF-16 -> UTF-8 sans iconv.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "xls.h"

static int fail(const char* msg) {
    fprintf(stderr, "libxls_smoketest ECHEC : %s\n", msg);
    return 1;
}

/* Ouvre un .xls du repertoire de fixtures, renvoie le workbook ou NULL. */
static xlsWorkBook* open_fixture(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_file(path, "UTF-8", &err);
    if (!wb) fprintf(stderr, "  (ouverture '%s' : %s)\n", path, xls_getError(err));
    return wb;
}

static int test_mtcars(const char* dir) {
    xlsWorkBook* wb = open_fixture(dir, "mtcars.xls");
    if (!wb) return fail("mtcars.xls : ouverture impossible");
    if (wb->sheets.count != 1) { xls_close_WB(wb); return fail("mtcars.xls : nb feuilles != 1"); }

    xlsWorkSheet* ws = xls_getWorkSheet(wb, 0);
    if (!ws) { xls_close_WB(wb); return fail("mtcars.xls : getWorkSheet(0) NULL"); }
    xls_parseWorkSheet(ws);

    struct st_row_data* row0 = &ws->rows.row[0];
    xlsCell* c00 = &row0->cells.cell[0];
    if (!c00->str || strcmp(c00->str, "mpg") != 0) {
        xls_close_WS(ws); xls_close_WB(wb);
        return fail("mtcars.xls : [0,0] attendu 'mpg'");
    }

    /* Au moins une cellule numerique decodee quelque part dans la feuille. */
    int sawNumber = 0;
    for (WORD r = 0; r <= ws->rows.lastrow && !sawNumber; r++) {
        struct st_row_data* row = &ws->rows.row[r];
        for (WORD col = 0; col <= ws->rows.lastcol; col++) {
            xlsCell* cell = &row->cells.cell[col];
            if (cell->id == XLS_RECORD_NUMBER || cell->id == XLS_RECORD_RK ||
                cell->id == XLS_RECORD_MULRK) { sawNumber = 1; break; }
        }
    }
    xls_close_WS(ws);
    xls_close_WB(wb);
    if (!sawNumber) return fail("mtcars.xls : aucune cellule numerique decodee");
    return 0;
}

static int test_vietnamese(const char* dir) {
    /* "Mo ta" diacrite : M o(U+00F4) espace t a(U+1EA3), en UTF-8. */
    static const unsigned char expected[] = {
        0x4d, 0xc3, 0xb4, 0x20, 0x74, 0xe1, 0xba, 0xa3, 0x00
    };
    xlsWorkBook* wb = open_fixture(dir, "vietnamese-utf8.xls");
    if (!wb) return fail("vietnamese-utf8.xls : ouverture impossible");

    xlsWorkSheet* ws = xls_getWorkSheet(wb, 0);
    if (!ws) { xls_close_WB(wb); return fail("vietnamese-utf8.xls : getWorkSheet(0) NULL"); }
    xls_parseWorkSheet(ws);

    struct st_row_data* row0 = &ws->rows.row[0];
    xlsCell* c00 = &row0->cells.cell[0];
    int ok = c00->str && strcmp(c00->str, (const char*)expected) == 0;
    if (!ok && c00->str) {
        fprintf(stderr, "  (obtenu bytes=");
        for (const unsigned char* p = (const unsigned char*)c00->str; *p; p++)
            fprintf(stderr, "%02x ", (unsigned)*p);
        fprintf(stderr, ")\n");
    }
    xls_close_WS(ws);
    xls_close_WB(wb);
    if (!ok) return fail("vietnamese-utf8.xls : [0,0] != UTF-8 attendu (conversion UTF-16 sans iconv cassee)");
    return 0;
}

int main(int argc, char** argv) {
    const char* dir = (argc >= 2) ? argv[1] : "vendor/libxls/testdata";
    if (test_mtcars(dir) != 0) return 1;
    if (test_vietnamese(dir) != 0) return 1;
    printf("libxls_smoketest OK (mtcars + vietnamese UTF-8, sans iconv)\n");
    return 0;
}
