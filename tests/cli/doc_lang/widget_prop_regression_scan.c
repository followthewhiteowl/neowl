/*  — non-regression : les proprietes de widgets deja navigables via
 * F1 (agregees dans DSLPROPS, resolues par `byProp` dans le doc-browser HTML)
 * doivent le rester une fois les champs de types composites ajoutes a cote
 * ( lot 1). Garde-fou anti-collision de nom d'array : si
 * l'implementation "recycle" DSLPROPS pour y verser aussi les champs de
 * types (au lieu d'un array frere), elle ne doit ni ecraser ni faire
 * disparaitre les entrees de widgets existantes.
 *
 * `Largeur` est choisi car c'est la propriete DSL la plus largement partagee
 * (accessible sur ~22 widgets a la date d'ecriture) : un bon canari si un
 * futur remaniement de l'emission DSLPROPS perd des entrees en route.
 *
 * Boite noire, calque projet_prop_scan.c / type_field_nav_scan.c. VERT
 * aujourd'hui ET apres  — sert de garde non-regression, pas de
 * digue rouge. */

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

/* Compte les entrees plates `{"w":"...","n":"Largeur",...}` (n'importe quel
 * porteur "w"), sans se soucier du reste du contenu de l'entree. */
static int count_prop_entries(const char* json, const char* fieldFR) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"n\":\"%s\"", fieldFR);
    int count = 0;
    const char* p = json;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
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

    /* seuil bas volontaire (10, pas 22) : marge si un widget est retire /
     * renomme un jour sans lien avec , ce garde reste sur son vrai
     * objet (non-regression massive), pas sur un compte exact fragile. */
    const int MIN_EXPECTED = 10;
    int n = count_prop_entries(out, "Largeur");
    free(out);

    printf("=== feat/1866 non-regression proprietes widgets (Largeur, dump-doc-html) ===\n");
    printf("Occurrences \"n\":\"Largeur\" trouvees : %d (seuil minimal : %d)\n", n, MIN_EXPECTED);
    if (n < MIN_EXPECTED) {
        fprintf(stderr, "FAIL: propriete widget \"Largeur\" a perdu des porteurs "
                "(regression emission DSLPROPS ?)\n");
        printf("RESULT: RED (regression)\n");
        return 1;
    }
    printf("RESULT: GREEN (%d occurrences, seuil %d respecte)\n", n, MIN_EXPECTED);
    return 0;
}
