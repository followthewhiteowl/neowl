/*  (ST1 de l'umbrella , ) — le lien fiche->guide
 * n'existe que sous forme d'HEURISTIQUE d'occurrences (>= 2 occurrences du
 * symbole dans un bloc ```owl du guide embarque). Un lien topiquement
 * pertinent mais qui n'utilise jamais le NOM de la native dans un bloc de
 * code passe totalement inapercu.
 *
 * Cas choisi : `RegexRemplace`. Le guide `05_operateurs.md` renvoie deja
 * l'utilisateur vers "les expressions regulieres via le module regex" juste
 * apres avoir presente les operateurs de comparaison positionnelle — lien
 * topique legitime — mais ne mentionne JAMAIS les natives RegexRemplace /
 * RegexVerifie / RegexCherche dans un bloc ```owl (verifie : aucun guide ne
 * les mentionne). L'heuristique par occurrence ne peut donc pas le trouver :
 * `owl explain RegexRemplace --json` a ce jour renvoie
 * `"public_documentation": []` (confirme avant impl, cf. rapport ticket).
 *
 * Ce scanner verrouille qu'UNE FOIS la table declarative de cross-reference
 * branchee, le lien RegexRemplace -> 05_operateurs apparait :
 *   - en mode texte : `owl explain RegexRemplace --lang fr` liste
 *     "05_operateurs" dans la ligne "fiches".
 *   - en JSON : `owl explain RegexRemplace --json` liste "05_operateurs"
 *     dans `related.public_documentation[].name`.
 *
 * ROUGE attendu tant que la table declarative n'existe pas : la ligne
 * "fiches" est absente en texte (section "Voir aussi" masque le bucket
 * vide), et `public_documentation` est un tableau vide en JSON.
 *
 * Boite noire, calque table_seealso_scan.c / explain_json_lang_scan.c.
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

    /* --- texte : owl explain RegexRemplace --lang fr --- */
    char* txt = run_owl(owlBin, "explain RegexRemplace --lang fr");
    if (!txt || !*txt) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain RegexRemplace --lang fr'\n");
        free(txt);
        return 1;
    }
    /* On isole la section "Voir aussi" pour ne pas matcher "05_operateurs"
     * ailleurs par accident (peu probable mais on reste precis). */
    const char* voirAussi = strstr(txt, "Voir aussi");
    if (!voirAussi) {
        fprintf(stderr, "FAIL: explain RegexRemplace --lang fr : section \"Voir aussi\" absente\n");
        fails++;
    } else {
        fails += must_contain(voirAussi, "05_operateurs",
                               "explain RegexRemplace --lang fr (section Voir aussi)");
    }
    free(txt);

    /* --- JSON : owl explain RegexRemplace --json --- */
    char* js = run_owl(owlBin, "explain RegexRemplace --json");
    if (!js || !*js) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain RegexRemplace --json'\n");
        free(js);
        return 1;
    }
    const char* relatedJs = strstr(js, "\"related\"");
    if (!relatedJs) {
        fprintf(stderr, "FAIL: explain RegexRemplace --json : champ \"related\" absent\n");
        fails++;
    } else {
        fails += must_contain(relatedJs, "\"05_operateurs\"",
                               "explain RegexRemplace --json (related.public_documentation)");
    }
    free(js);

    printf("=== feat/2140 pin declaratif fiche->guide (RegexRemplace -> 05_operateurs) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — table declarative pas encore branchee\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (lien declaratif RegexRemplace -> 05_operateurs present CLI texte+JSON)\n");
    return 0;
}
