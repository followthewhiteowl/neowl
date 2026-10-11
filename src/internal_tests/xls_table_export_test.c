// SPDX-License-Identifier: AGPL-3.0-only
/* xls_table_export_test.c —  : round-trip de l'export « table -> .xlsx ».
 *
 * Exécuté via `owl --self-test-xls-table-export` (cf. main.c). Le menu du corps
 * de table (clic droit) propose « Exporter vers Excel… » ; le rendu Qt lui-même
 * n'est pas headless-testable, mais la logique data->.xlsx l'est : elle vit dans
 * owl_ui_table_export_xlsx() (src/stdlib_ui.c), une fonction pure-données qui ne
 * dépend d'aucun widget Qt. Le sélecteur de fichier du menu ne fait que fournir
 * le chemin à cette fonction.
 *
 * Ici on fabrique un tableau (entêtes + lignes), on l'exporte, puis :
 *   1. on rouvre le .xlsx via le moteur (xlsOuvre) — preuve qu'il est parseable,
 *   2. on dézippe et on asserte le contenu exact (entêtes en ligne 1, données en
 *      ligne 2+, placement des cellules A1/B1/A2…).
 *
 * Retourne 0 si tous les cas passent, sinon le nombre d'échecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"
#include "../owl_ui.h"
#include "miniz.h"
#include "fixture_unique.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

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

/* Lit une part du .xlsx dans un buffer heap (caller mz_free). NULL si absente. */
static char* read_zip_part(const char* path, const char* part, size_t* outLen) {
    mz_zip_archive zip; memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) return NULL;
    char* data = (char*)mz_zip_reader_extract_file_to_heap(&zip, part, outLen, 0);
    mz_zip_reader_end(&zip);
    return data;
}

int xls_table_export_test_main(void) {
    fprintf(stderr, "\n==== XLS TABLE EXPORT TESTS (feat/2146) ====\n");

    char path[160];
    fixture_unique(path, sizeof path, "_owl_xls_tblexport_", ".xlsx");
    char esc[320];
    esc_path(path, esc, sizeof esc);

    /* --- 1. Export d'un tableau : 2 colonnes, 2 lignes de données. --- */
    const char* headers[2] = { "Nom", "Age" };
    /* row-major : 2 lignes x 2 colonnes. */
    const char* cells[4] = {
        "Alice", "30",
        "Bob",   "25"
    };
    bool ok = owl_ui_table_export_xlsx(path, headers, 2, cells, 2);
    CHECK(ok, "export_returns_true");

    /* --- 2. Le fichier se rouvre via le moteur (xlsOuvre) sans erreur. --- */
    char src[512];
    snprintf(src, sizeof(src),
        "relu est un xlsDocument = xlsOuvre(\"%s\")\n"
        "xlsFerme(relu)\n", esc);
    CHECK(vm_interpret(src) == INTERPRET_OK, "reopen_via_xlsOuvre_ok");

    /* --- 3. sharedStrings.xml contient les entêtes ET les valeurs exactes. --- */
    {
        size_t len = 0;
        char* ss = read_zip_part(path, "xl/sharedStrings.xml", &len);
        CHECK(ss != NULL, "sharedstrings_present");
        if (ss) {
            CHECK(buf_contains(ss, len, "Nom"),   "header_nom_present");
            CHECK(buf_contains(ss, len, "Age"),   "header_age_present");
            CHECK(buf_contains(ss, len, "Alice"), "value_alice_present");
            CHECK(buf_contains(ss, len, "Bob"),   "value_bob_present");
            mz_free(ss);
        }
    }

    /* --- 4. La worksheet place entêtes en ligne 1, données en ligne 2/3. --- */
    {
        size_t len = 0;
        char* ws = read_zip_part(path, "xl/worksheets/sheet1.xml", &len);
        CHECK(ws != NULL, "worksheet_present");
        if (ws) {
            CHECK(buf_contains(ws, len, "<row r=\"1\""), "row1_present");
            CHECK(buf_contains(ws, len, "<row r=\"2\""), "row2_present");
            CHECK(buf_contains(ws, len, "<row r=\"3\""), "row3_present");
            /* Entêtes en A1/B1, première donnée en A2/B2. */
            CHECK(buf_contains(ws, len, "<c r=\"A1\""), "cell_A1_present");
            CHECK(buf_contains(ws, len, "<c r=\"B1\""), "cell_B1_present");
            CHECK(buf_contains(ws, len, "<c r=\"A2\""), "cell_A2_present");
            CHECK(buf_contains(ws, len, "<c r=\"B3\""), "cell_B3_present");
            /* Pas de 4e ligne : on n'a exporté que 2 lignes de données. */
            CHECK(!buf_contains(ws, len, "<row r=\"4\""), "no_row4");
            mz_free(ws);
        }
    }

    /* --- 5. Cas dégénéré : 0 ligne de données = seule la ligne d'entêtes. --- */
    {
        char path2[160];
        fixture_unique(path2, sizeof path2, "_owl_xls_tblexport_hdr_", ".xlsx");
        bool ok2 = owl_ui_table_export_xlsx(path2, headers, 2, NULL, 0);
        CHECK(ok2, "export_headers_only_ok");
        size_t len = 0;
        char* ws = read_zip_part(path2, "xl/worksheets/sheet1.xml", &len);
        CHECK(ws != NULL, "hdronly_worksheet_present");
        if (ws) {
            CHECK(buf_contains(ws, len, "<row r=\"1\""),  "hdronly_row1_present");
            CHECK(!buf_contains(ws, len, "<row r=\"2\""), "hdronly_no_row2");
            mz_free(ws);
        }
        remove(path2);
    }

    /* --- 6. Garde-fous : chemin NULL / ncols<=0 => échec propre. --- */
    CHECK(!owl_ui_table_export_xlsx(NULL, headers, 2, cells, 2), "reject_null_path");
    CHECK(!owl_ui_table_export_xlsx(path, headers, 0, cells, 2),  "reject_zero_cols");

    remove(path);

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "xls-table-export: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    return g_fail;
}
