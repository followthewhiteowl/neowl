/*  — digue F1 sur les proprietes du bloc Projet dans le doc-browser
 * HTML (`owl --dump-doc-html`). F1 dans l'editeur ouvre owl-doc.html a
 * l'ancre `#<langue>/<mot>` ; la SPA resout le mot via `byProp` (fallback
 * mot-nu) qui est alimente par le tableau JS `DSLPROPS`. Or ce tableau
 * n'emettait AUCUNE entree pour les 7 champs du type composite `Projet`
 * (Description, Auteur, Licence, Langues, Theme, FenetreDemarrage,
 * ActiveAvertissementConventionDeNommage) -> F1 sur `StartupWindow` (ou
 * toute autre propriete projet, FR comme EN) tombait sur un 404.
 *
 * Ce harness invoque le VRAI binaire owl.exe (chemin en argv[1]) en
 * `--dump-doc-html`, capture le JS embarque, et verifie que chaque objet
 * DSLPROPS `"w":"Projet"` expose bien les 7 proprietes avec leur forme EN
 * (source de verite : nameEN de FIELDS_Projet, src/stdlib_builtin_types.c).
 * Les formes EN sont pur ASCII -> pas d'UTF-8 en dur dans ce source (les
 * noms FR Theme/FenetreDemarrage sont accentues, on ne les teste pas ici).
 *
 * Ce n'est PAS un self-test compile dans owl.exe : harness boite noire
 * compile a la volee par tests/cli/doc_lang/run.sh (calque keyword_en_scan.c).
 *
 * Exit code : 0 si les 7 proprietes projet sont presentes (w=Projet, en
 * attendu), 1 sinon (rouge attendu tant que dump_doc_html n'emet pas les
 * champs de Projet dans DSLPROPS).
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

/* Cherche, parmi les objets DSLPROPS `{"w":"Projet",...}`, celui qui porte
 * `"en":"<enForm>"`. Chaque objet est borne par le prochain `{"w":` (les
 * entrees DSLPROPS sont `{...},\n{...}`). Retourne 1 si trouve, 0 sinon. */
static int projet_prop_has_en(const char* json, const char* enForm) {
    const char* p = json;
    const char* WNEEDLE = "{\"w\":\"Projet\"";
    char en[96];
    snprintf(en, sizeof(en), "\"en\":\"%s\"", enForm);
    while ((p = strstr(p, WNEEDLE)) != NULL) {
        const char* start = p;
        p += strlen(WNEEDLE);
        /* borne = prochain objet DSLPROPS (ou fin) */
        const char* bound = strstr(p, "{\"w\":\"");
        const char* hit = strstr(start, en);
        if (hit && (!bound || hit < bound)) return 1;
    }
    return 0;
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

typedef struct {
    const char* en;    /* forme EN attendue (nameEN de FIELDS_Projet) */
    const char* note;  /* nom FR de rappel */
} Expect;

/* Les 7 champs du type Projet, cle EN (src/stdlib_builtin_types.c). */
static const Expect EXPECTED[] = {
    { "Description",                    "Description" },
    { "Author",                        "Auteur" },
    { "License",                       "Licence" },
    { "Languages",                     "Langues" },
    { "Theme",                         "Theme" },
    { "StartupWindow",                 "FenetreDemarrage" },
    { "EnableNamingConventionWarning", "ActiveAvertissementConventionDeNommage" },
};
#define EXPECTED_COUNT ((int)(sizeof(EXPECTED)/sizeof(EXPECTED[0])))

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
    for (int i = 0; i < EXPECTED_COUNT; i++) {
        if (projet_prop_has_en(out, EXPECTED[i].en)) {
            printf("  OK  Projet.%-38s en=%s\n", EXPECTED[i].note, EXPECTED[i].en);
        } else {
            fprintf(stderr,
                    "FAIL: propriete Projet \"%s\" (en=\"%s\") absente de DSLPROPS "
                    "(w=Projet)\n", EXPECTED[i].note, EXPECTED[i].en);
            fails++;
        }
    }
    free(out);

    printf("=== fix/1775 proprietes du bloc Projet navigables F1 (dump-doc-html) ===\n");
    printf("Proprietes verifiees : %d, echecs : %d\n", EXPECTED_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d absente(s)) — attendu tant que dump_doc_html "
               "n'emet pas les champs de Projet dans DSLPROPS.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (%d proprietes projet emises, formes EN conformes)\n",
           EXPECTED_COUNT);
    return 0;
}
