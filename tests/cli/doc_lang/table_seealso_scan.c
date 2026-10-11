/*  — la page d'aide sur le widget Table ne renvoyait vers AUCUNE
 * des natives qui le manipulent (support #360). Ce scanner verrouille le
 * "Voir aussi" (maillage reverse-lookup type-based, cf. cli_doc.c) attendu
 * apres retypage du 1er param des ~12 natives Table de "tout" vers
 * "Champ Table" :
 *
 *   - `owl explain Table --lang fr` : la ligne "voisins" liste au moins
 *     TableAjouteLigne, TableSupprime, TableSelect (formes FR canon).
 *   - `owl explain Table --lang en` : meme chose en formes EN canon
 *     (TableAddLine, TableDelete, TableSelected) — cf. neighbor_name_lang
 *     dans cli_doc.c, deja le mecanisme de traduction du bucket "voisins"
 *     pour toutes les fiches (verifie empiriquement sur TableCherche).
 *   - Dans les deux langues, aucune fonction Tableau* (arrays, sans
 *     rapport avec le widget Table) ne doit apparaitre dans "voisins" —
 *     piege d'exclusion explicite du ticket.
 *
 * Mode TEXTE choisi plutot que --json : le champ JSON `related.neighbors`
 * n'est aujourd'hui JAMAIS traduit par langue (verifie sur TableCherche —
 * meme "Info, Erreur..." en --lang en qu'en --lang fr), alors que le mode
 * texte traduit deja via neighbor_name_lang(). Tester le JSON pour le
 * critere bilingue aurait donc verrouille un comportement absent du
 * systeme par ailleurs, hors-scope de ce ticket.
 *
 * ROUGE attendu tant que le retypage + le reverse-lookup ne sont pas
 * implementes : aujourd'hui la ligne "voisins"/"neighbors" n'apparait
 * meme pas pour Table (neighbors vide => section masquee).
 *
 * Boite noire, calque explain_json_lang_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh. Exit code 0 si toutes les assertions
 * tiennent, 1 sinon.
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

/* Isole la ligne "voisins"/"neighbors" de la section "Voir aussi"/"See
 * also", bornee a la fin de ligne (buffer statique, un seul appel actif
 * a la fois suffit pour ce scanner). NULL si la ligne n'existe pas. */
static char g_line_buf[2048];
static const char* neighbors_line(const char* out, const char* marker) {
    const char* p = strstr(out, marker);
    if (!p) return NULL;
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(g_line_buf)) len = sizeof(g_line_buf) - 1;
    memcpy(g_line_buf, p, len);
    g_line_buf[len] = '\0';
    return g_line_buf;
}

static int must_contain(const char* hay, const char* needle, const char* ctx) {
    if (hay && strstr(hay, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present dans la ligne voisins mais absent : %s\n",
            ctx, needle);
    return 1;
}

static int must_not_contain(const char* hay, const char* needle, const char* ctx) {
    if (!hay || !strstr(hay, needle)) return 0;
    fprintf(stderr, "FAIL: %s : fonction Tableau* (array) presente a tort dans le see-also : %s\n",
            ctx, needle);
    return 1;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- FR : owl explain Table --lang fr --- */
    char* fr = run_owl(owlBin, "explain Table --lang fr");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain Table --lang fr'\n");
        free(fr);
        return 1;
    }
    const char* lineFR = neighbors_line(fr, "voisins    ");
    if (!lineFR) {
        fprintf(stderr, "FAIL: ligne \"voisins\" absente de 'explain Table --lang fr' "
                "(see-also Table pas encore implemente)\n");
        fails++;
    } else {
        const char* ctxFR = "explain Table --lang fr";
        fails += must_contain(lineFR, "TableAjouteLigne", ctxFR);
        fails += must_contain(lineFR, "TableSupprime",    ctxFR);
        fails += must_contain(lineFR, "TableSelect",      ctxFR);
        fails += must_not_contain(lineFR, "TableauAjoute",  ctxFR);
        fails += must_not_contain(lineFR, "TableauCherche", ctxFR);
    }
    free(fr);

    /* --- EN : owl explain Table --lang en (formes EN canon) --- */
    char* en = run_owl(owlBin, "explain Table --lang en");
    if (!en || !*en) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain Table --lang en'\n");
        free(en);
        return 1;
    }
    const char* lineEN = neighbors_line(en, "neighbors  ");
    if (!lineEN) {
        fprintf(stderr, "FAIL: ligne \"neighbors\" absente de 'explain Table --lang en' "
                "(see-also Table pas encore implemente)\n");
        fails++;
    } else {
        const char* ctxEN = "explain Table --lang en";
        fails += must_contain(lineEN, "TableAddLine",  ctxEN);
        fails += must_contain(lineEN, "TableDelete",   ctxEN);
        fails += must_contain(lineEN, "TableSelected", ctxEN);
        fails += must_not_contain(lineEN, "TableauAjoute",  ctxEN);
        fails += must_not_contain(lineEN, "TableauCherche", ctxEN);
    }
    free(en);

    printf("=== fix/2101 see-also natives Table (owl explain Table) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — see-also Table pas encore implemente\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (see-also Table liste les natives, FR+EN, exclut Tableau*)\n");
    return 0;
}
