/*  — digue sur le reset de l'ascenseur de la liste de resultats
 * (#list) du doc-browser HTML (`owl --dump-doc-html`). Quand la requete /
 * le filtre change, la liste est reconstruite par renderList() : sans reset
 * de scrollTop l'ancienne position persiste et l'utilisateur voit la FIN de
 * la nouvelle liste (effet "liste vide/incomplete", ticket support #351).
 *
 * Deux exigences verifiees sur le HTML reellement produit par owl.exe :
 *   POSITIF : les handlers d'entree qui changent la requete/le filtre
 *             (input recherche, searchclear, resetAll) remettent
 *             `#list.scrollTop=0` apres renderList().
 *   NEGATIF : le reset n'est PAS dans le corps de renderList() lui-meme —
 *             cette fonction est aussi appelee par la nav clavier (select()
 *             sur ↑/↓), ou un reset casserait le scrollIntoView de
 *             l'element selectionne.
 *
 * Principe (calque sur keyword_en_scan.c) : on invoque le VRAI binaire
 * owl.exe (chemin en argv[1]) en `--dump-doc-html`, on capture le HTML, et
 * on verifie ces sous-chaines. Le chemin du template est derive du chemin
 * de l'exe (<racine>/build/owl.exe -> <racine>/docs/reference/...).
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

/* Sous-chaines POSITIVES attendues : chaque handler d'entree remet #list en
 * tete apres renderList(). Ancrees sur le contexte propre au handler pour
 * eviter les faux positifs (le reset data-assoc preexistant n'est pas ici). */
typedef struct { const char* needle; const char* note; } Expect;
static const Expect POSITIVE[] = {
    { "query=e.target.value; syncSearchClear(); renderList(); document.getElementById(\"list\").scrollTop=0;",
      "handler input recherche" },
    { "syncSearchClear(); renderList(); document.getElementById(\"list\").scrollTop=0; s.focus();",
      "handler searchclear" },
    { "renderList(); document.getElementById(\"list\").scrollTop=0; applyChrome();",
      "resetAll (clic titre)" },
    { "activeKinds.add(k); buildKindFilters(); renderList(); document.getElementById(\"list\").scrollTop=0;",
      "handler chip filtre KIND" },
    { "activeCats.add(c); buildFilters(); renderList(); document.getElementById(\"list\").scrollTop=0;",
      "handler chip filtre CAT" },
};
#define POSITIVE_COUNT ((int)(sizeof(POSITIVE)/sizeof(POSITIVE[0])))

/* NEGATIF : le corps de renderList() ne doit contenir aucun scrollTop.
 * Borne = de `function renderList(){` au prochain `\nfunction ` de plus haut
 * niveau (les fonctions du template sont declarees en colonne 0). */
static int render_list_has_scrolltop(const char* html) {
    const char* start = strstr(html, "function renderList(){");
    if (!start) return -1;   /* fonction introuvable : anomalie */
    const char* end = strstr(start + 1, "\nfunction ");
    if (!end) end = start + strlen(start);
    for (const char* p = start; p + 9 <= end; p++) {
        if (strncmp(p, "scrollTop", 9) == 0) return 1;
    }
    return 0;
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
    for (int i = 0; i < POSITIVE_COUNT; i++) {
        if (strstr(out, POSITIVE[i].needle)) {
            printf("  OK  reset present (%s)\n", POSITIVE[i].note);
        } else {
            fprintf(stderr, "FAIL: reset #list absent — %s\n", POSITIVE[i].note);
            fails++;
        }
    }

    int rl = render_list_has_scrolltop(out);
    if (rl < 0) {
        fprintf(stderr, "FAIL: renderList() introuvable dans le HTML\n");
        fails++;
    } else if (rl == 1) {
        fprintf(stderr, "FAIL: renderList() contient scrollTop — casserait la "
                        "nav clavier (scrollIntoView via select())\n");
        fails++;
    } else {
        printf("  OK  renderList() sans scrollTop (nav clavier preservee)\n");
    }

    free(out);

    printf("=== fix/2020 reset ascenseur #list du doc-browser (dump-doc-html) ===\n");
    printf("Attentes : %d positives + 1 negative, echecs : %d\n",
           POSITIVE_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s))\n", fails);
        return 1;
    }
    printf("RESULT: GREEN\n");
    return 0;
}
