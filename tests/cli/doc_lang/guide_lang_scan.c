/*  — digue sur la localisation des "guides" (chapitres thematiques
 * de l'aide HTML, `owl --dump-doc-html`).
 *
 * Support #389 / volet #300 : les guides longs (dont celui sur les prefixes
 * de nommage) n'existent aujourd'hui qu'en francais. En mode EN, le viewer
 * affiche donc du FR brut sous une etiquette EN, sans avertir l'utilisateur.
 * L'infra cible (convention `NN_slug.en.md` a cote du FR, cf. decision
 * ticket) n'existe pas encore sur HEAD : ce scanner pose le contrat attendu
 * AVANT l'implementation.
 *
 * Exigences verifiees sur le HTML reellement produit par owl.exe :
 *
 *   1. L'emission `GUIDES` apparie une variante EN au stem FR
 *      `12_prefixes_de_nommage` : l'entree JSON de ce stem porte les cles
 *      `titleEN` ET `mdEN` (en plus de `title`/`md`). Absent aujourd'hui
 *      (aucune des deux cles n'existe nulle part dans l'emission) -> rouge.
 *
 *   2. Garde anti-doublon : aucune des 3 variantes EN du premier lot
 *      (`05_operateurs`, `12_prefixes_de_nommage`, `14_sql_connecteurs`)
 *      n'apparait comme cle de guide AUTONOME dans `GUIDES` (un `.en.md`
 *      ne doit jamais se voir attribuer sa propre fiche navigable). Verifie
 *      en cherchant la forme `"<stem>.en":{` — absente aujourd'hui (comme
 *      apres impl), donc cette assertion-ci est verte des le depart : c'est
 *      un filet de non-regression, pas une attente rouge.
 *
 *   3. Le viewer est language-aware sur le rendu d'un guide :
 *      a. `renderGuidePage` porte une condition qui choisit la variante EN
 *         uniquement quand elle existe : la sous-chaine exacte
 *         `lang==="en"&&g.titleEN&&g.mdEN` doit apparaitre dans le template.
 *      b. Quand le guide n'a pas de variante EN et qu'on est en mode EN, un
 *         bandeau localise honnete est affiche via la cle i18n
 *         `t("guide_untranslated")` — appel absent aujourd'hui.
 *      c. Le dictionnaire `I18N.fr` porte l'entree
 *         `guide_untranslated:"Pas encore disponible en anglais."`
 *      d. Le dictionnaire `I18N.en` porte l'entree
 *         `guide_untranslated:"Not yet available in English."`
 *      Ces 4 ancres sont absentes du template actuel (aucune n'existe
 *      aujourd'hui, meme partiellement) : ce sont des attentes rouges, pas
 *      des tautologies sur de l'existant. L'implementer est libre d'ajouter
 *      la condition EN et l'appel bandeau dans un ordre different pourvu que
 *      ces sous-chaines litterales apparaissent verbatim.
 *
 * Principe (calque sur category_filter_lang_scan.c / list_scroll_reset_scan.c) :
 * on invoque le VRAI binaire owl.exe (chemin en argv[1]) en `--dump-doc-html`,
 * on capture le HTML (qui embarque le JS du viewer + les consts GUIDES/I18N
 * verbatim), et on verifie ces sous-chaines. Le chemin du template est
 * derive du chemin de l'exe.
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

/* Extrait la fenetre texte de l'objet JSON associe a la cle `"<stem>":{...}`
 * dans l'emission `GUIDES`, en respectant l'imbrication d'accolades et les
 * guillemets echappes a l'interieur des chaines JSON (title/md peuvent
 * contenir des `\"`). Renvoie un buffer alloue (a liberer par l'appelant) ou
 * NULL si la cle n'est pas trouvee. */
static char* extract_guide_entry(const char* out, const char* stem) {
    char key[256];
    snprintf(key, sizeof(key), "\"%s\":{", stem);
    const char* start = strstr(out, key);
    if (!start) return NULL;
    const char* p = start + strlen(key) - 1; /* positionne sur le '{' ouvrant */
    const char* entryStart = p;
    int depth = 0;
    int inStr = 0;
    for (; *p; p++) {
        char c = *p;
        if (inStr) {
            if (c == '\\') { p++; continue; }
            if (c == '"') inStr = 0;
            continue;
        }
        if (c == '"') { inStr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { p++; break; }
        }
    }
    size_t len = (size_t)(p - entryStart);
    char* win = (char*)malloc(len + 1);
    if (!win) return NULL;
    memcpy(win, entryStart, len);
    win[len] = '\0';
    return win;
}

static const char* LOT_STEMS[] = {
    "05_operateurs", "12_prefixes_de_nommage", "14_sql_connecteurs"
};
#define LOT_STEMS_COUNT ((int)(sizeof(LOT_STEMS)/sizeof(LOT_STEMS[0])))

typedef struct { const char* needle; const char* note; } Expect;
static const Expect POSITIVE_VIEWER[] = {
    { "lang===\"en\"&&g.titleEN&&g.mdEN",
      "renderGuidePage choisit la variante EN uniquement quand titleEN+mdEN existent" },
    { "t(\"guide_untranslated\")",
      "bandeau de repli localise appele via la cle i18n guide_untranslated" },
    { "guide_untranslated:\"Pas encore disponible en anglais.\"",
      "I18N.fr porte le libelle du bandeau de repli" },
    { "guide_untranslated:\"Not yet available in English.\"",
      "I18N.en porte le libelle du bandeau de repli" },
};
#define POSITIVE_VIEWER_COUNT ((int)(sizeof(POSITIVE_VIEWER)/sizeof(POSITIVE_VIEWER[0])))

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

    /* 1. GUIDES["12_prefixes_de_nommage"] porte titleEN + mdEN. */
    {
        const char* stem = "12_prefixes_de_nommage";
        char* win = extract_guide_entry(out, stem);
        if (!win) {
            fprintf(stderr, "FAIL: stem GUIDES introuvable — %s\n", stem);
            fails++;
        } else {
            if (strstr(win, "\"titleEN\":") && strstr(win, "\"mdEN\":")) {
                printf("  OK  present (GUIDES[%s] porte titleEN+mdEN)\n", stem);
            } else {
                fprintf(stderr,
                        "FAIL: attendu absent — GUIDES[%s] sans titleEN/mdEN\n",
                        stem);
                fails++;
            }
            free(win);
        }
    }

    /* 2. Garde anti-doublon : aucun .en.md n'est un guide autonome. */
    {
        int dup = 0;
        for (int i = 0; i < LOT_STEMS_COUNT; i++) {
            char key[300];
            snprintf(key, sizeof(key), "\"%s.en\":{", LOT_STEMS[i]);
            if (strstr(out, key)) {
                fprintf(stderr,
                        "FAIL: cle de guide autonome inattendue — %s\n", key);
                dup++;
            }
        }
        if (dup == 0) {
            printf("  OK  absent (aucun .en.md n'apparait comme guide autonome)\n");
        }
        fails += dup;
    }

    /* 3. Viewer language-aware (condition EN + bandeau de repli bilingue). */
    for (int i = 0; i < POSITIVE_VIEWER_COUNT; i++) {
        if (strstr(out, POSITIVE_VIEWER[i].needle)) {
            printf("  OK  present (%s)\n", POSITIVE_VIEWER[i].note);
        } else {
            fprintf(stderr, "FAIL: attendu absent — %s\n", POSITIVE_VIEWER[i].note);
            fails++;
        }
    }

    free(out);

    printf("=== fix/2238 localisation des guides (dump-doc-html) ===\n");
    printf("Attentes : %d positives, echecs : %d\n",
           1 + 1 + POSITIVE_VIEWER_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s))\n", fails);
        return 1;
    }
    printf("RESULT: GREEN\n");
    return 0;
}
