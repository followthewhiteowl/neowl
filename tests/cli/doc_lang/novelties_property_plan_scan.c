/*  — la page "Nouveautes" de l'aide doit lister les PROPRIETES de
 * widget nouvellement apparues, pas seulement les fiches nommees
 * (natives/types/enums/consts/widgets/chapitres). Regression reelle, de bout
 * en bout (support #439) : `..Plan`/`..Plane` (propriete de plan de fenetre,
 * nouveaute surfacee tardivement) etait absente des Nouveautes parce que
 * `collect_doc_fiches` n'enumerait pas les proprietes -> l'univers du diff ne
 * les contenait jamais.
 *
 * Verification en deux temps sur la sortie reelle de `--dump-doc-html` (le
 * meme generateur que la page livree, baseline chargee depuis
 * tools/doc_fiches_baseline.tsv) :
 *   1. NOVELTIES contient bien l'entree propriete `{"k":"property","n":"Plan"}`
 *      -> `..Plan` ressort (la baseline seedee contient toutes les autres
 *      proprietes SAUF Plan, donc seule Plan surface : pas de flood).
 *   2. DSLPROPS porte la propriete Plan avec son alias EN `"en":"Plane"`
 *      -> la fiche est navigable sous les DEUX formes (FR ..Plan / EN ..Plane),
 *      l'invariant bilingue tient.
 *
 * Boite noire, calque widget_prop_regression_scan.c. Rouge AVANT 
 * (aucune propriete dans l'univers du diff), vert apres. */

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
    size_t cap = 262144, len = 0;
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

static void derive_template(const char* owlBin, char* out, size_t outsz) {
    char root[1024];
    snprintf(root, sizeof(root), "%s", owlBin);
    for (int seg = 0; seg < 2; seg++) {
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

    int fails = 0;

    /* 1. Plan ressort comme nouveaute propriete. */
    if (strstr(out, "{\"k\":\"property\",\"n\":\"Plan\"") != NULL) {
        printf("  OK  NOVELTIES contient la propriete nouvelle ..Plan\n");
    } else {
        fprintf(stderr, "FAIL: `{\"k\":\"property\",\"n\":\"Plan\"}` absent de NOVELTIES "
                "(..Plan ne ressort pas dans les Nouveautes)\n");
        fails++;
    }

    /* 2. Navigable FR ..Plan ET EN ..Plane (invariant bilingue). */
    if (strstr(out, "\"n\":\"Plan\",\"en\":\"Plane\"") != NULL) {
        printf("  OK  DSLPROPS porte la propriete Plan avec alias EN Plane\n");
    } else {
        fprintf(stderr, "FAIL: `\"n\":\"Plan\",\"en\":\"Plane\"` absent de DSLPROPS "
                "(fiche Plan non navigable sous sa forme EN ..Plane)\n");
        fails++;
    }

    free(out);

    printf("=== feat/2392 ..Plan dans les Nouveautes de l'aide ===\n");
    printf("Echecs : %d\n", fails);
    if (fails > 0) {
        printf("RESULT: RED (%d)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (..Plan listee en Nouveautes, bilingue)\n");
    return 0;
}
