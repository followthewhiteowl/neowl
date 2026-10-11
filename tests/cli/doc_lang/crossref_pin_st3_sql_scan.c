/*  (ST3 de l'umbrella , ) — peuplement de la table
 * de cross-reference declarative avec un lien concret famille SQL.
 *
 * Cas choisi : `SQLConnexionOuvre`. `SQLEnvoie` a deja un lien vers
 * `13_sql_modele` via l'heuristique d'occurrences (score 5, confirme avant
 * depot : `owl explain SQLEnvoie --json` -> related.public_documentation
 * contient deja "13_sql_modele"), donc epingler ce symbole n'ajouterait rien
 * d'observable. `SQLConnexionOuvre`, en revanche, n'a AUCUN lien fiche->guide
 * aujourd'hui (confirme : `owl explain SQLConnexionOuvre --json` ->
 * related.public_documentation = [] vide) alors que le guide
 * `14_sql_connecteurs.md` documente precisement les champs de connexion
 * (Provider/Source/...) que cette native consomme.
 *
 * Ce scanner verrouille le pin SQLConnexionOuvre -> 14_sql_connecteurs :
 *   - en mode texte : `owl explain SQLConnexionOuvre --lang fr` liste
 *     "14_sql_connecteurs" dans la section "Voir aussi".
 *   - en JSON : `owl explain SQLConnexionOuvre --json` liste
 *     "14_sql_connecteurs" dans `related.public_documentation[].name`.
 *
 * ROUGE attendu tant que l'entree n'est pas ajoutee a OWL_CROSSREF_TABLE
 * (cli_doc.c) : aucune des deux sorties ne mentionne "14_sql_connecteurs".
 *
 * Boite noire, calque tests/cli/doc_lang/crossref_pin_scan.c (ST1).
 * Compile a la volee par tests/cli/doc_lang/run.sh.
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

static int must_contain(const char* out, const char* needle, const char* ctx) {
    if (out && strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present mais absent : %s\n", ctx, needle);
    return 1;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- texte : owl explain SQLConnexionOuvre --lang fr --- */
    char* txt = run_owl(owlBin, "explain SQLConnexionOuvre --lang fr");
    if (!txt || !*txt) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain SQLConnexionOuvre --lang fr'\n");
        free(txt);
        return 1;
    }
    const char* voirAussi = strstr(txt, "Voir aussi");
    if (!voirAussi) {
        fprintf(stderr, "FAIL: explain SQLConnexionOuvre --lang fr : section \"Voir aussi\" absente\n");
        fails++;
    } else {
        fails += must_contain(voirAussi, "14_sql_connecteurs",
                               "explain SQLConnexionOuvre --lang fr (section Voir aussi)");
    }
    free(txt);

    /* --- JSON : owl explain SQLConnexionOuvre --json --- */
    char* js = run_owl(owlBin, "explain SQLConnexionOuvre --json");
    if (!js || !*js) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain SQLConnexionOuvre --json'\n");
        free(js);
        return 1;
    }
    const char* relatedJs = strstr(js, "\"related\"");
    if (!relatedJs) {
        fprintf(stderr, "FAIL: explain SQLConnexionOuvre --json : champ \"related\" absent\n");
        fails++;
    } else {
        fails += must_contain(relatedJs, "\"14_sql_connecteurs\"",
                               "explain SQLConnexionOuvre --json (related.public_documentation)");
    }
    free(js);

    printf("=== feat/2142 pin declaratif fiche->guide (SQLConnexionOuvre -> 14_sql_connecteurs) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — entree pas encore ajoutee a OWL_CROSSREF_TABLE\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (lien declaratif SQLConnexionOuvre -> 14_sql_connecteurs present CLI texte+JSON)\n");
    return 0;
}
