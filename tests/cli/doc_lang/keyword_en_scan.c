/*  — digue sur la forme EN des fiches de mot-cle du doc-browser
 * HTML (`owl --dump-doc-html`). La forme `en` d'une fiche keyword est
 * derivee par heuristique dans src/main.c::dump_doc_html : « premier alias
 * scanner dont l'ascii-minuscule differe du canon, remis en majuscules ».
 * Pour quatre tokens, ce premier alias different est une FLEXION FRANCAISE
 * (toute, publique, privee, protegee), pas la vraie forme anglaise — alors
 * que celle-ci existe deja comme alias scanner (each, public, private,
 * protected). Resultat : `TOUT` s'affichait `en:"TOUTE"` au lieu de `EACH`.
 *
 * Principe (calque sur leak_scan.c / internal_native_scan.c) : on invoque
 * le VRAI binaire owl.exe (chemin en argv[1]) en `--dump-doc-html`, on
 * capture le JSON embarque dans le HTML, et on verifie la forme `en` de
 * fiches keyword ciblees. Le chemin du template est derive du chemin de
 * l'exe (<racine>/build/owl.exe -> <racine>/docs/reference/...).
 *
 * Ce n'est PAS un self-test compile dans owl.exe (on ne touche pas a src/) :
 * harness boite noire compile a la volee par tests/cli/doc_lang/run.sh.
 *
 * Exit code : 0 si toutes les formes EN attendues correspondent, 1 sinon
 * (rouge attendu tant que dump_doc_html n'applique pas l'override curatif).
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

/* Extrait la forme `en` de la fiche keyword dont le champ `"ascii":"<ascii>"`
 * correspond. Recherche `"ascii":"<ascii>"` puis, dans les limites du meme
 * objet JSON (borne par le prochain `{"kind":"keyword"`), lit la valeur de
 * `"en":"..."`. Retourne 1 + remplit out si trouvee, 0 sinon (ascii absent
 * ou pas de champ `en` dans la fiche). L'ascii est pur ASCII (PRIVE,
 * PROTEGE) : evite d'ecrire les accents UTF-8 en dur dans ce source. */
static int keyword_en_of_ascii(const char* json, const char* ascii,
                               char* out, size_t outsz) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"ascii\":\"%s\"", ascii);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    /* borne = debut de la fiche keyword suivante (ou fin du texte). */
    const char* bound = strstr(p + 1, "{\"kind\":\"keyword\"");
    const char* en = strstr(p, "\"en\":\"");
    if (!en) return 0;
    if (bound && en >= bound) return 0;   /* le `en` appartient a la fiche suivante */
    en += strlen("\"en\":\"");
    size_t i = 0;
    while (en[i] && en[i] != '"' && i + 1 < outsz) { out[i] = en[i]; i++; }
    out[i] = '\0';
    return 1;
}

/* Derive le chemin du template a partir du chemin de l'exe :
 * <racine>/build/owl.exe -> <racine>/docs/reference/_owl-doc-template.html.
 * Gere les separateurs '/' et '\\'. */
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

typedef struct {
    const char* ascii;   /* champ "ascii" de la fiche (clé de recherche) */
    const char* en;      /* forme EN attendue (source de verite : alias scanner) */
    const char* note;    /* rappel humain */
} Expect;

/* Les quatre corrections (curatif ) + trois non-regressions. Les
 * formes EN sont des ALIAS SCANNER existants (src/scanner.c), pas une
 * invention : each/public/private/protected. */
static const Expect EXPECTED[] = {
    /* corrections : etaient des flexions FR (TOUTE/PUBLIQUE/PRIVEE/PROTEGEE) */
    { "TOUT",    "EACH",      "flexion FR TOUTE -> EACH (alias scanner)" },
    { "PUBLIC",  "PUBLIC",    "flexion FR PUBLIQUE -> PUBLIC (= canon)" },
    { "PRIVE",   "PRIVATE",   "flexion FR PRIVEE -> PRIVATE (alias scanner)" },
    { "PROTEGE", "PROTECTED", "flexion FR PROTEGEE -> PROTECTED (alias scanner)" },
    /* non-regression : formes EN correctes, l'heuristique doit rester intacte */
    { "SI",      "IF",        "non-regression heuristique" },
    { "POUR",    "FOR",       "non-regression heuristique" },
    { "TANTQUE", "WHILE",     "non-regression heuristique" },
};
#define EXPECTED_COUNT ((int)(sizeof(EXPECTED)/sizeof(EXPECTED[0])))

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char tpl[1200];
    derive_template(owlBin, tpl, sizeof(tpl));

    /* Wrapper de guillemets externe : sous Windows _popen appelle
     * `cmd /c "<cmd>"` et cmd.exe ne retire QUE la paire la plus externe
     * (cf. leak_scan.c). */
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
    for (int i = 0; i < EXPECTED_COUNT; i++) {
        char got[64];
        if (!keyword_en_of_ascii(out, EXPECTED[i].ascii, got, sizeof(got))) {
            fprintf(stderr,
                    "FAIL: fiche keyword ascii=\"%s\" : champ `en` introuvable [%s]\n",
                    EXPECTED[i].ascii, EXPECTED[i].note);
            fails++;
            continue;
        }
        if (strcmp(got, EXPECTED[i].en) != 0) {
            fprintf(stderr,
                    "FAIL: keyword \"%s\" en=\"%s\", attendu \"%s\" [%s]\n",
                    EXPECTED[i].ascii, got, EXPECTED[i].en, EXPECTED[i].note);
            fails++;
        } else {
            printf("  OK  %-8s en=%-9s (%s)\n",
                   EXPECTED[i].ascii, got, EXPECTED[i].note);
        }
    }
    free(out);

    printf("=== fix/1635 forme EN des fiches keyword (dump-doc-html) ===\n");
    printf("Fiches verifiees : %d, echecs : %d\n", EXPECTED_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s)) — attendu tant que dump_doc_html "
               "n'applique pas l'override curatif.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (%d fiches, formes EN conformes)\n", EXPECTED_COUNT);
    return 0;
}
