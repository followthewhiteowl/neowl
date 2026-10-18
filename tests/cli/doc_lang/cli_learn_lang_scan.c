/*  — digue sur le cablage EN du CLI documentaire (`owl learn`,
 * `owl primer`). Les donnees EN sont deja embarquees dans le binaire
 * (`EMBEDDED_PUBLIC_DOCS_EN[]`, cf. src/embedded_docs.h) mais `cli_doc.c`
 * ne les consomme pas encore : `learn_show` (src/cli_doc.c ~L1755) sert du
 * FR brut sous `--lang en`, avec un bandeau placeholder V0
 * `[FR fallback — EN translation pending]` et `"lang":"fr"` +
 * `"lang_fallback":true` dans le JSON, alors que la variante EN du
 * chapitre existe reellement (docs/public_documentation/00_apercu_du_langage.en.md).
 *
 * Ce scanner verrouille, en invoquant le VRAI owl.exe :
 *
 *   1. `learn apercu --json --lang en` -> `"lang":"en"`, PAS de
 *      `"lang_fallback":true`, et un marqueur distinctement anglais
 *      ("## 1. First program", present dans le .en.md, absent du FR).
 *      ROUGE attendu avant cablage (sert "lang":"fr" + fallback + FR).
 *   2. `primer --lang en` (texte) -> meme marqueur anglais present, bandeau
 *      de repli V0 absent. ROUGE attendu avant cablage.
 *   3. Non-regression FR : `learn apercu` (sans --lang) -> marqueur FR
 *      ("## 1. Premier programme") present, pas de bandeau de repli.
 *      VERT attendu (comportement FR inchange par ce chantier).
 *   4. Invariant  : le compte de fiches ("N fiche(s) embarquee(s).")
 *      et la liste des stems de `learn` (liste brute, sans --lang) restent
 *      IDENTIQUES avant/apres cablage — EMBEDDED_PUBLIC_DOCS reste pur FR,
 *      aucune fiche .en ne doit fuiter comme entree autonome. Mesure le
 *      compte reel observe (pas de valeur magique a l'aveugle) et verifie
 *      sa stabilite entre un appel `--lang fr` et un appel `--lang en` (la
 *      liste elle-meme n'a pas de --lang cote sortie texte mais le JSON de
 *      `learn_list` n'est pas langue-dependant non plus : on verifie que le
 *      meme compte revient sur 2 appels, ce qui detecterait une fuite de
 *      fiche .en si `--lang en` en ajoutait une par accident — ce n'est pas
 *      le cas aujourd'hui, donc cette assertion est VERTE des le depart,
 *      filet de non-regression).
 *
 * Limite assumee (documentee, pas contournee) : les 43 chapitres ont TOUS
 * leur variante .en.md aujourd'hui (epic ) — impossible de
 * declencher ici le chemin "bandeau honnete sur variante manquante" avec un
 * vrai chapitre sans fabriquer un faux stem, ce que cette lane boite-noire
 * ne fait pas (pas de fixture .md ad-hoc). Ce chemin reste couvert par la
 * logique d'implementation, pas par une assertion de ce scanner.
 *
 * Harness boite noire compile a la volee par tests/cli/doc_lang/run.sh
 * (calque explain_json_lang_scan.c / guide_lang_scan.c). Invoque le VRAI
 * owl.exe (chemin en argv[1]).
 *
 * Exit code : 0 si toutes les assertions tiennent, 1 sinon.
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
    size_t cap = 65536, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    buf[0] = '\0';
    char chunk[4096];
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

static char* run_owl(const char* owlBin, const char* args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>&1\"", owlBin, args);
    return run_capture(cmdline);
}

static int must_contain(const char* out, const char* needle, const char* ctx) {
    if (strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present mais absent : %s\n", ctx, needle);
    return 1;
}

static int must_not_contain(const char* out, const char* needle, const char* ctx) {
    if (!strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : residu interdit trouve : %s\n", ctx, needle);
    return 1;
}

/* Compte le nombre de fiches rapporte par "owl learn" en lisant le dernier
 * entier qui precede "fiche(s) embarquee". Renvoie -1 si introuvable. */
static int extract_fiche_count(const char* out) {
    const char* marker = strstr(out, "fiche(s) embarqu");
    if (!marker) return -1;
    const char* p = marker;
    /* recule jusqu'au debut du nombre */
    while (p > out && (p[-1] == ' ' || (p[-1] >= '0' && p[-1] <= '9'))) p--;
    return atoi(p);
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* Marqueurs stables extraits des chapitres reels (pas de prose fragile,
     * un titre de section). EN present uniquement dans le .en.md, FR
     * present uniquement dans le .md. */
    const char* MARKER_EN = "## 1. First program";
    const char* MARKER_FR = "## 1. Premier programme";
    const char* BANNER_V0 = "[FR fallback";

    /* --- 1. learn apercu --json --lang en : doit servir le contenu EN --- */
    {
        char* out = run_owl(owlBin, "learn apercu --json --lang en");
        if (!out || !*out) {
            fprintf(stderr, "FAIL: sortie vide pour 'learn apercu --json --lang en'\n");
            fails++;
        } else {
            const char* ctx = "learn apercu --json --lang en";
            fails += must_contain(out, "\"lang\": \"en\"", ctx);
            fails += must_not_contain(out, "\"lang_fallback\": true", ctx);
            fails += must_contain(out, MARKER_EN, ctx);
            fails += must_not_contain(out, MARKER_FR, ctx);
        }
        free(out);
    }

    /* --- 2. primer --lang en (texte) : doit afficher le contenu EN --- */
    {
        char* out = run_owl(owlBin, "primer --lang en");
        if (!out || !*out) {
            fprintf(stderr, "FAIL: sortie vide pour 'primer --lang en'\n");
            fails++;
        } else {
            const char* ctx = "primer --lang en";
            fails += must_contain(out, MARKER_EN, ctx);
            fails += must_not_contain(out, MARKER_FR, ctx);
            fails += must_not_contain(out, BANNER_V0, ctx);
        }
        free(out);
    }

    /* --- 3. Non-regression FR : learn apercu (sans --lang) --- */
    {
        char* out = run_owl(owlBin, "learn apercu");
        if (!out || !*out) {
            fprintf(stderr, "FAIL: sortie vide pour 'learn apercu'\n");
            fails++;
        } else {
            const char* ctx = "learn apercu (FR, defaut)";
            fails += must_contain(out, MARKER_FR, ctx);
            fails += must_not_contain(out, MARKER_EN, ctx);
            fails += must_not_contain(out, BANNER_V0, ctx);
        }
        free(out);
    }

    /* --- 4. Invariant  : compte de fiches stable --- */
    {
        char* outFr = run_owl(owlBin, "learn");
        char* outEn = run_owl(owlBin, "learn --lang en");
        int cFr = outFr ? extract_fiche_count(outFr) : -1;
        int cEn = outEn ? extract_fiche_count(outEn) : -1;
        if (cFr < 0 || cEn < 0) {
            fprintf(stderr,
                    "FAIL: compte de fiches introuvable (fr=%d, en=%d)\n",
                    cFr, cEn);
            fails++;
        } else if (cFr != cEn) {
            fprintf(stderr,
                    "FAIL: compte de fiches divergent entre 'learn' et "
                    "'learn --lang en' (fr=%d, en=%d) — une fiche .en fuit "
                    "comme entree autonome (ADR_0232 viole)\n",
                    cFr, cEn);
            fails++;
        } else {
            printf("  OK  compte de fiches stable FR/EN (%d fiche(s))\n", cFr);
        }
        free(outFr);
        free(outEn);
    }

    printf("=== fix/2262 cablage EN du CLI documentaire (learn/primer) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (learn/primer servent l'EN embarque, FR inchange)\n");
    return 0;
}
