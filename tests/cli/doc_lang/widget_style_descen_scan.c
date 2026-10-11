/*  — anti-franglais sur les fiches d'aide (F1 -> owl-doc.html) des
 * proprietes `Style` des champs stylables. Symptome (support #316) : en aide
 * ANGLAISE, la descEN de la prop Style d'un champ liste ses sous-elements avec
 * leurs noms FRANCAIS (ex. Table : « Sub-elements: CadreExterieur, TitreColonne,
 * LibelleLignes... »). La descFR reste, elle, en francais (correct).
 *
 * Ce harness verifie, pour chaque objet DSLPROPS `{"w":"...","n":"Style",
 * "t":"style..."}` (les proprietes Style des widgets, portees par
 * src/ui_catalog.c), que la descEN ne contient AUCUN jeton de sous-element FR
 * (les formes EN canoniques du registre — OuterFrame, ColumnTitle, RowLabel...
 * — doivent etre utilisees a la place). Assertion de valeur exacte en plus :
 * la descEN de Table cite les 8 formes EN attendues.
 *
 * Mutation : remettre un nom FR (ex. CadreExterieur) dans une descEN de Style
 * -> ce test rougit. Les fiches menu (styleMenuPrincipal / styleMenuContextuel)
 * etaient deja en anglais, donc couvertes gratuitement.
 *
 * Boite noire, calque projet_prop_descen_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh. Exit code 0 si aucune fuite FR, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 1048576, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    buf[0] = '\0';
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap + n + 1) * 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
        buf[len] = '\0';
    }
    PCLOSE(fp);
    return buf;
}

static int extract_str(const char* start, const char* end,
                       const char* key, char* out, size_t outsz) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(start, needle);
    if (!p || p >= end) return 0;
    p += strlen(needle);
    size_t j = 0;
    while (p < end && *p) {
        if (*p == '\\' && p + 1 < end) {
            if (j + 2 < outsz) { out[j++] = p[0]; out[j++] = p[1]; }
            p += 2;
            continue;
        }
        if (*p == '"') break;
        if (j + 1 < outsz) out[j++] = *p;
        p++;
    }
    out[j] = '\0';
    return 1;
}

static void derive_template(const char* owlBin, char* out, size_t outsz) {
    char root[1024];
    snprintf(root, sizeof(root), "%s", owlBin);
    for (int seg = 0; seg < 2; seg++) {   /* strip "owl.exe" puis "build" */
        char* slash = NULL;
        for (char* q = root; *q; q++) if (*q == '/' || *q == '\\') slash = q;
        if (!slash) { root[0] = '\0'; break; }
        *slash = '\0';
    }
    if (root[0])
        snprintf(out, outsz, "%s/docs/reference/_owl-doc-template.html", root);
    else
        snprintf(out, outsz, "docs/reference/_owl-doc-template.html");
}

/* Jetons FR de sous-elements bannis dans une descEN de Style. Prefixes ASCII
 * (avant tout accent) pour matcher sans dependre de l'encodage des bytes
 * accentues : "CadreExt" couvre CadreExterieur, "Lignes" couvre LibelleLignes /
 * LignesPaires / LignesImpaires, "LigneS" couvre LigneSelectionnee /
 * LigneSurvolee, "parateur" couvre Separateur, "Coche"/"Puce" couvrent aussi
 * CadreCoche / CadrePuce. Aucun de ces jetons n'est un mot anglais legitime. */
static const char* const BANNED[] = {
    "CadreExt", "TitreColonne", "Lignes", "LigneS", "Quadrillage",
    "ZoneSaisie", "TexteOptions", "Coche", "Puce", "CadreInterne",
    "parateur", "Remplissage", "Volet", "Retrait", "survol",
    NULL
};

/* Formes EN attendues dans la descEN de Table (valeur exacte). */
static const char* const TABLE_EN[] = {
    "OuterFrame", "ColumnTitle", "RowLabel", "EvenRows", "OddRows",
    "SelectedRow", "HoveredRow", "Gridlines", NULL
};

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char tpl[1200];
    derive_template(owlBin, tpl, sizeof(tpl));

    char cmdline[3072];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s\" 2>NUL\"", owlBin, tpl);
    char* out = run_capture(cmdline);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide (dump-doc-html non execute) : %s\n",
                cmdline);
        free(out);
        printf("RESULT: RED — dump-doc-html sans sortie (template: %s)\n", tpl);
        return 1;
    }

    int fails = 0, styleProps = 0;
    const char* WNEEDLE = "{\"w\":\"";
    const char* p = out;
    while ((p = strstr(p, WNEEDLE)) != NULL) {
        const char* start = p;
        const char* bound = strstr(p + strlen(WNEEDLE), WNEEDLE);
        const char* end = bound ? bound : out + strlen(out);
        p += strlen(WNEEDLE);

        /* Ne cibler que les proprietes Style d'un type styleXxx. */
        if (!strstr(start, "\"n\":\"Style\"") ||
            strstr(start, "\"n\":\"Style\"") >= end) continue;
        if (!strstr(start, "\"t\":\"style") ||
            strstr(start, "\"t\":\"style") >= end) continue;

        char w[64] = {0}, descEN[4096] = {0};
        extract_str(start, end, "w", w, sizeof(w));
        if (!extract_str(start, end, "descEN", descEN, sizeof(descEN)) ||
            descEN[0] == '\0') {
            fprintf(stderr, "FAIL: %s.Style : descEN absente/vide\n", w);
            fails++;
            continue;
        }
        styleProps++;

        for (int b = 0; BANNED[b]; b++) {
            if (strstr(descEN, BANNED[b])) {
                fprintf(stderr,
                    "FAIL: %s.Style : jeton FR \"%s\" present dans la descEN "
                    "(aide EN montre du francais)\n", w, BANNED[b]);
                fails++;
            }
        }

        /* Assertion de valeur exacte sur le widget signale (Table). */
        if (strcmp(w, "Table") == 0) {
            for (int e = 0; TABLE_EN[e]; e++) {
                if (!strstr(descEN, TABLE_EN[e])) {
                    fprintf(stderr,
                        "FAIL: Table.Style : forme EN \"%s\" attendue absente "
                        "de la descEN\n", TABLE_EN[e]);
                    fails++;
                }
            }
        }
    }
    free(out);

    printf("=== fix/2019 descEN des proprietes Style des widgets (anti-franglais) ===\n");
    printf("Proprietes Style scannees : %d, echecs : %d\n", styleProps, fails);
    if (styleProps < 15) {
        fprintf(stderr, "FAIL: trop peu de proprietes Style trouvees (%d) — "
                "emission DSLPROPS cassee ?\n", styleProps);
        fails++;
    }
    if (fails > 0) {
        printf("RESULT: RED (%d) — l'aide EN d'une prop Style montre du FR.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (%d proprietes Style, descEN 100%% anglaises)\n", styleProps);
    return 0;
}
