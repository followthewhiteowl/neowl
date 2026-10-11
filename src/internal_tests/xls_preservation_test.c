// SPDX-License-Identifier: AGPL-3.0-only
/* xls_preservation_test.c —  : preuve de la préservation ZIP-level
 * des parts qu'OWL ne modélise pas (macros, graphiques, médias...).
 *
 * Exécuté via `owl --self-test-xls-preservation` (cf. main.c). Ce que le
 * harnais de scénarios .owl NE PEUT PAS voir : le contenu binaire réel du
 * .xlsx. Ici on fabrique un classeur qui contient une part custom bidon
 * (`xl/media/img.bin`) — comme l'aurait produit un autre outil — puis :
 *   1. on l'ouvre via le moteur (xlsOuvre),
 *   2. on modifie une cellule,
 *   3. on ré-enregistre (xlsSauve, même chemin),
 *   4. on dézippe et on asserte que la part custom a survécu OCTET POUR OCTET.
 *
 * Sans la capture/reinjection des parts non modélisées, xlsSauve régénère un
 * .xlsx « propre » et la part custom disparaît silencieusement.
 *
 * Retourne 0 si tous les cas passent, sinon le nombre d'échecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"
#include "miniz.h"
#include "fixture_unique.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Blob custom reconnaissable (non-ASCII pour attraper toute corruption). */
static const unsigned char CUSTOM_BLOB[] = {
    0x00, 0x01, 0x02, 0xFD, 0xFE, 0xFF, 0xDE, 0xAD,
    0xBE, 0xEF, 0x10, 0x20, 0x30, 0x40, 0x7F, 0x80
};
#define CUSTOM_NAME "xl/media/img.bin"

/* Recherche naïve d'un motif dans un buffer (memmem non portable mingw). */
static bool buf_contains(const char* hay, size_t hlen, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    return false;
}

/* Échappe les backslashes Windows d'un chemin pour un littéral OWL. */
static void esc_path(const char* path, char* out, size_t outSize) {
    size_t i = 0;
    for (const char* p = path; *p && i < outSize - 2; p++) {
        if (*p == '\\') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = '\0';
}

int xls_preservation_test_main(void) {
    fprintf(stderr, "\n==== XLS PRESERVATION TESTS (feat/1792) ====\n");

    char path[160];
    fixture_unique(path, sizeof path, "_owl_xls_preserv_", ".xlsx");
    char esc[320];
    esc_path(path, esc, sizeof esc);

    /* --- 1. Le moteur écrit un classeur valide (2 cellules, style sur B1). --- */
    char src[1024];
    snprintf(src, sizeof(src),
        "doc est un xlsDocument\n"
        "doc[1, 1] = \"A1 origine\"\n"
        "doc[1, 2] = \"B1 origine\"\n"
        "doc..Cellule[1, 2]..CouleurFond = Bleu\n"
        "xlsSauve(doc, \"%s\")\n", esc);
    CHECK(vm_interpret(src) == INTERPRET_OK, "create_ok");

    /* --- 2. Injection d'une part custom, comme un outil tiers. --- */
    {
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        bool okr = mz_zip_reader_init_file(&zip, path, 0);
        CHECK(okr, "reopen_for_inject");
        if (okr) {
            bool okw = mz_zip_writer_init_from_reader(&zip, path);
            CHECK(okw, "writer_from_reader");
            if (okw) {
                bool added = mz_zip_writer_add_mem(&zip, CUSTOM_NAME, CUSTOM_BLOB,
                                                   sizeof(CUSTOM_BLOB), MZ_DEFAULT_COMPRESSION);
                CHECK(added, "inject_custom_part");
                mz_zip_writer_finalize_archive(&zip);
                mz_zip_writer_end(&zip);
            } else {
                mz_zip_reader_end(&zip);
            }
        }
    }

    /* --- 3. Ouverture moteur -> modif cellule -> ré-enregistrement même chemin. --- */
    char src2[1024];
    snprintf(src2, sizeof(src2),
        "relu est un xlsDocument = xlsOuvre(\"%s\")\n"
        "relu[1, 1] = \"A1 modifie\"\n"
        "xlsSauve(relu, \"%s\")\n"
        "xlsFerme(relu)\n", esc, esc);
    CHECK(vm_interpret(src2) == INTERPRET_OK, "reopen_modify_save_ok");

    /* --- 4. La part custom a-t-elle survécu octet pour octet ? --- */
    {
        mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
        bool okr = mz_zip_reader_init_file(&zip, path, 0);
        CHECK(okr, "reopen_after_save");
        if (okr) {
            size_t len = 0;
            void* data = mz_zip_reader_extract_file_to_heap(&zip, CUSTOM_NAME, &len, 0);
            CHECK(data != NULL, "custom_part_survives");
            CHECK(data && len == sizeof(CUSTOM_BLOB), "custom_part_size");
            CHECK(data && len == sizeof(CUSTOM_BLOB) &&
                  memcmp(data, CUSTOM_BLOB, sizeof(CUSTOM_BLOB)) == 0,
                  "custom_part_bytes_identical");
            if (data) mz_free(data);

            /* Contrôle témoin : le contenu modélisé a bien été régénéré (la
             * modif de A1 est présente -> ce n'est pas un simple no-op copie). */
            size_t wlen = 0;
            char* wxml = (char*)mz_zip_reader_extract_file_to_heap(
                &zip, "xl/sharedStrings.xml", &wlen, 0);
            CHECK(wxml != NULL, "sharedstrings_present");
            if (wxml) {
                CHECK(buf_contains(wxml, wlen, "A1 modifie"), "cell_modif_regenerated");
                mz_free(wxml);
            }
            mz_zip_reader_end(&zip);
        }
    }

    remove(path);

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "xls-preservation: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    return g_fail;
}
