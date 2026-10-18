/*  — digue sur le tri des membres (proprietes + evenements) de la
 * fiche d'un controle dans le doc-browser HTML (`owl --dump-doc-html`).
 *
 * Les noms affiches sont deja language-aware : `xref(p.n,...)` passe par
 * `dispName`, qui rend la forme EN en mode EN. Mais le TRI des deux listes
 * utilisait `a.n.localeCompare(b.n)` — soit le nom FR canon, quel que soit
 * le mode. Resultat en mode EN : noms EN affiches mais ordonnes par leur
 * equivalent FR -> ordre percu comme aleatoire (ticket support #388).
 *
 * Exigences verifiees sur le HTML reellement produit par owl.exe :
 *   NEGATIF : la cle de tri FR-only `.sort((a,b)=>a.n.localeCompare(b.n))`
 *             n'existe plus (c'etait l'unique porteur de ce pattern, sur les
 *             deux listes widget).
 *   POSITIF : un helper `memberSortName` miroir de dispName, applique au tri
 *             des deux listes (propsByWidget + eventsByWidget).
 *
 * Principe (calque sur list_scroll_reset_scan.c) : on invoque le VRAI binaire
 * owl.exe (chemin en argv[1]) en `--dump-doc-html`, on capture le HTML (qui
 * embarque le JS du viewer verbatim), et on verifie ces sous-chaines. Le
 * chemin du template est derive du chemin de l'exe.
 *
 * Ce n'est PAS un self-test compile dans owl.exe : harness boite noire
 * compile a la volee par tests/cli/doc_lang/run.sh.
 *
 * Exit code : 0 si toutes les attentes tiennent, 1 sinon.
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

/* Derive le chemin du template a partir du chemin de l'exe :
 * <racine>/build/owl.exe -> <racine>/docs/reference/_owl-doc-template.html. */
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

static int count_occurrences(const char* hay, const char* needle) {
    int c = 0;
    const char* p = hay;
    size_t ln = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { c++; p += ln; }
    return c;
}

/* NEGATIF : plus aucune occurrence du tri FR-only sur les membres widget. */
static const char* NEG_NEEDLE = ".slice().sort((a,b)=>a.n.localeCompare(b.n))";

/* POSITIF : helper language-aware + application aux deux listes. */
typedef struct { const char* needle; const char* note; } Expect;
static const Expect POSITIVE[] = {
    { "function memberSortName(x){ return lang===\"en\"&&x.en?x.en:x.n; }",
      "helper de tri language-aware (miroir dispName)" },
    { "(propsByWidget[d.ascii]||[]).slice().sort((a,b)=>memberSortName(a).localeCompare(memberSortName(b)))",
      "tri language-aware des proprietes widget" },
    { "(eventsByWidget[d.ascii]||[]).slice().sort((a,b)=>memberSortName(a).localeCompare(memberSortName(b)))",
      "tri language-aware des evenements widget" },
};
#define POSITIVE_COUNT ((int)(sizeof(POSITIVE)/sizeof(POSITIVE[0])))

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

    int neg = count_occurrences(out, NEG_NEEDLE);
    if (neg > 0) {
        fprintf(stderr, "FAIL: tri FR-only encore present (%d occurrence(s)) — "
                "les membres widget doivent trier sur le nom affiche, pas sur "
                ".n FR canon\n", neg);
        fails++;
    } else {
        printf("  OK  tri FR-only absent (a.n.localeCompare retire des membres widget)\n");
    }

    for (int i = 0; i < POSITIVE_COUNT; i++) {
        if (strstr(out, POSITIVE[i].needle)) {
            printf("  OK  present (%s)\n", POSITIVE[i].note);
        } else {
            fprintf(stderr, "FAIL: attendu absent — %s\n", POSITIVE[i].note);
            fails++;
        }
    }

    free(out);

    printf("=== fix/2179 tri language-aware des membres widget (dump-doc-html) ===\n");
    printf("Attentes : 1 negative + %d positives, echecs : %d\n",
           POSITIVE_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s))\n", fails);
        return 1;
    }
    printf("RESULT: GREEN\n");
    return 0;
}
