/*  — digue sur le chemin d'export JSON de `owl explain`, qui
 * n'honorait pas `--lang en` sur `category` / `return_type` / `return_desc`
 * alors que la sortie plain les traduit deja (support #330 : la fiche d'aide
 * du viewer, qui consomme ce JSON, restait en partie en francais — categorie
 * "Fichiers", type de retour "chaine" — meme en anglais).
 *
 * Repro figee (avant fix) :
 *   owl explain fExeDir --json --lang en  ->  "category": "Fichiers",
 *                                             "return_type": "chaine",
 *                                             "return_desc": "chemin"
 * alors que la sortie plain EN rend deja "Files" / "string".
 *
 * Ce scanner verrouille, en --lang en, la traduction des trois champs dans
 * le JSON (reutilise le meme mapping que le plain : owl_category_display +
 * type_str_lang cote impl), et verifie qu'en --lang fr la forme FR reste
 * intacte (non-regression bilingue).
 *
 * Harness boite noire compile a la volee par tests/cli/doc_lang/run.sh
 * (calque date_returntype_scan.c). Invoque le VRAI owl.exe (chemin en argv[1]).
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
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>NUL\"", owlBin, args);
    return run_capture(cmdline);
}

/* Assertion de presence : `needle` DOIT etre dans `out`. */
static int must_contain(const char* out, const char* needle, const char* ctx) {
    if (strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present mais absent : %s\n", ctx, needle);
    return 1;
}

/* Assertion d'absence : `needle` NE DOIT PAS etre dans `out`. */
static int must_not_contain(const char* out, const char* needle, const char* ctx) {
    if (!strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : residu FR interdit trouve : %s\n", ctx, needle);
    return 1;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- fExeDir --json --lang en : tout doit etre traduit --- */
    char* en = run_owl(owlBin, "explain fExeDir --json --lang en");
    if (!en || !*en) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain fExeDir --json --lang en'\n");
        free(en);
        return 1;
    }
    const char* ctxEN = "fExeDir --json --lang en";
    /* categorie traduite */
    fails += must_contain(en,     "\"category\": \"Files\"",      ctxEN);
    fails += must_not_contain(en, "\"category\": \"Fichiers\"",   ctxEN);
    /* type de retour traduit */
    fails += must_contain(en,     "\"return_type\": \"string\"",  ctxEN);
    fails += must_not_contain(en, "\"return_type\": \"cha",       ctxEN); /* prefixe de "chaine"/"chaine" accentue */
    /* description de retour mono-langue FR : non imprimee en EN (null),
     * coherent avec le plain qui la supprime plutot que de fuiter du FR. */
    fails += must_not_contain(en, "\"return_desc\": \"chemin\"",  ctxEN);
    free(en);

    /* --- fExeDir --json --lang fr : la forme FR reste intacte --- */
    char* fr = run_owl(owlBin, "explain fExeDir --json --lang fr");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain fExeDir --json --lang fr'\n");
        free(fr);
        return 1;
    }
    const char* ctxFR = "fExeDir --json --lang fr";
    fails += must_contain(fr, "\"category\": \"Fichiers\"",    ctxFR);
    fails += must_contain(fr, "\"return_type\": \"cha",        ctxFR); /* "chaine" accentue */
    free(fr);

    printf("=== fix/2022 export JSON de `owl explain` honore --lang ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (category/return_type/return_desc suivent --lang dans le JSON)\n");
    return 0;
}
