/*  (ST1 de l'umbrella , ) — non-regression de
 * l'heuristique d'occurrences existante (`find_related_public_docs`,
 * cf. src/cli_doc.c).  §Decision(1) : la table declarative EPINGLE
 * des liens en plus de l'heuristique, elle ne la REMPLACE pas — le fallback
 * par comptage d'occurrences doit rester actif.
 *
 * `owl explain Projet` est le cas de reference documente par le ticket
 *  lui-meme : le guide `24_projet` et le guide
 * `12_prefixes_de_nommage` apparaissent deja aujourd'hui via l'heuristique
 * (chacun >= 2 occurrences de "Projet" dans un bloc ```owl). Ce scanner
 * verrouille que l'ajout de la table declarative ne les fait PAS disparaitre
 * (regression de fusion possible si l'impl remplace au lieu de fusionner).
 *
 * VERT des aujourd'hui (garde anti-regression, pas un rouge attendu) :
 * verifie empiriquement avant depot (cf. rapport ticket).
 *
 * Boite noire, calque table_seealso_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh.
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

    /* --- texte : owl explain Projet --lang fr --- */
    char* txt = run_owl(owlBin, "explain Projet --lang fr");
    if (!txt || !*txt) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain Projet --lang fr'\n");
        free(txt);
        return 1;
    }
    const char* voirAussi = strstr(txt, "Voir aussi");
    if (!voirAussi) {
        fprintf(stderr, "FAIL: explain Projet --lang fr : section \"Voir aussi\" absente "
                "(regression : l'heuristique ne trouve plus rien)\n");
        fails++;
    } else {
        fails += must_contain(voirAussi, "24_projet",
                               "explain Projet --lang fr (section Voir aussi)");
        fails += must_contain(voirAussi, "12_prefixes_de_nommage",
                               "explain Projet --lang fr (section Voir aussi)");
    }
    free(txt);

    /* --- JSON : owl explain Projet --json --- */
    char* js = run_owl(owlBin, "explain Projet --json");
    if (!js || !*js) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain Projet --json'\n");
        free(js);
        return 1;
    }
    const char* relatedJs = strstr(js, "\"related\"");
    if (!relatedJs) {
        fprintf(stderr, "FAIL: explain Projet --json : champ \"related\" absent\n");
        fails++;
    } else {
        fails += must_contain(relatedJs, "\"24_projet\"",
                               "explain Projet --json (related.public_documentation)");
        fails += must_contain(relatedJs, "\"12_prefixes_de_nommage\"",
                               "explain Projet --json (related.public_documentation)");
    }
    free(js);

    printf("=== feat/2140 non-regression heuristique occurrences (owl explain Projet) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — l'heuristique a regresse\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (heuristique d'occurrences toujours active en fallback)\n");
    return 0;
}
