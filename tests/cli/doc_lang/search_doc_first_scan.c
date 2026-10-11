/*  (ST2 de l'umbrella ,  §3) — support #355 :
 * quand AUCUN filtre de categorie n'est actif, les fiches DOCUMENTATION
 * (guides / kind fondation-EN guide) doivent remonter EN TETE des resultats
 * `owl search`. Aujourd'hui `search_hit_cmp` (src/cli_doc.c) trie par
 * pass > tier > longueur > occ > ordre d'insertion, SANS aucune notion de
 * categorie documentation — un guide topiquement pertinent peut se retrouver
 * enterre sous 20 constantes/natives homonymes (`owl search sql`).
 *
 * Point de semantique CLI vs viewer verifie avant impl (cf. rapport ticket) :
 * `owl search` ne lit JAMAIS `opts.cat` (grep confirme : aucune occurrence
 * dans `cli_doc_search`). Le flag `--cat`/`--category` est parse generiquement
 * par `parse_doc_opts` mais silencieusement ignore par la sous-commande
 * `search` — il n'existe donc PAS de concept de "filtre de categorie actif"
 * cote CLI pour cette sous-commande : le CLI est TOUJOURS en etat "sans
 * filtre". Consequence directe : le boost documentation-first s'applique de
 * facon INCONDITIONNELLE cote CLI, il n'y a pas de branche "avec filtre" a
 * tester ici. Le mecanisme de filtre (chips activeKinds/activeCats) est un
 * concept PUREMENT viewer (`docs/reference/_owl-doc-template.html`,
 * fonctions `match()`/`buildFilters()`/`buildKindFilters()`), non execute
 * par ce harnais C (pas d'interpreteur JS dans la toolchain OWL) — la
 * non-regression "avec filtre" reste donc une verification de rendu manuelle
 * cote dev, hors perimetre de ce scanner (documente au rapport ticket).
 *
 * Ce scanner verrouille :
 *   1. `owl search naming` : le guide 12_prefixes_de_nommage doit ressortir
 *      AVANT la propriete Projet..(Enable)NamingConventionWarning — ordre
 *      inverse aujourd'hui. FR + EN (kind "fondation"/"guide").
 *   2. `owl search sql` : les guides 13_sql_modele et 14_sql_connecteurs
 *      doivent ressortir dans le TOP 5 — aujourd'hui enterres au-dela du
 *      rang 20 sous des constantes/types/natives homonymes.
 *   3. Invariant de non-regression intra-categorie (critere 4 du ticket) :
 *      parmi les hits NON-documentation, l'ordre relatif issu de
 *      pass/tier/longueur reste inchange par le boost — sqlODBC (tier
 *      prefixe, nom court) doit rester avant SQLEnvoie (tier prefixe, nom
 *      plus long) qu'importe le nombre de guides promus devant eux. Ce test
 *      est VERT des aujourd'hui (comportement courant) et doit RESTER vert
 *      apres impl : c'est le garde-fou anti-regression du critere 4.
 *
 * ROUGE attendu sur (1) et (2) tant que le boost documentation-first n'est
 * pas branche dans `search_hit_cmp`. VERT attendu sur (3) — invariant deja
 * respecte par le tri courant, a preserver.
 *
 * Boite noire, calque crossref_pin_scan.c. Compile a la volee par
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

/* Rang 0-based d'un hit : chaque ligne de resultat `owl search` porte
 * exactement un marqueur `[kind]`. Trouver la 1re occurrence de `name`
 * puis le `[` qui la suit (kind de sa propre ligne), et compter le nombre
 * de `[` rencontres AVANT ce point = nombre de hits qui la precedent. */
static int rank_of(const char* out, const char* name) {
    const char* p = strstr(out, name);
    if (!p) return -1;
    const char* br = strchr(p, '[');
    if (!br) return -1;
    int rank = 0;
    for (const char* q = out; q < br; q++) if (*q == '[') rank++;
    return rank;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- 1. owl search naming (FR) : guide avant propriete --- */
    char* fr = run_owl(owlBin, "search naming");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl search naming'\n");
        free(fr);
        return 1;
    }
    int r_guide_fr = rank_of(fr, "12_prefixes_de_nommage");
    int r_prop_fr  = rank_of(fr, "ActiveAvertissementConventionDeNommage");
    if (r_guide_fr < 0 || r_prop_fr < 0) {
        fprintf(stderr, "FAIL: 'owl search naming' : hit(s) attendu(s) introuvable(s) "
                "(guide rank=%d, propriete rank=%d) — needle(s) absent(s), verifier le nom canon\n",
                r_guide_fr, r_prop_fr);
        fails++;
    } else if (!(r_guide_fr < r_prop_fr)) {
        fprintf(stderr, "FAIL: 'owl search naming' : le guide 12_prefixes_de_nommage "
                "(rang %d) ne remonte pas avant la propriete (rang %d) — "
                "documentation-first pas encore applique\n", r_guide_fr, r_prop_fr);
        fails++;
    }
    free(fr);

    /* --- 1bis. owl search naming --en : meme invariant en anglais --- */
    char* en = run_owl(owlBin, "search naming --en");
    if (!en || !*en) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl search naming --en'\n");
        free(en);
        return 1;
    }
    int r_guide_en = rank_of(en, "12_prefixes_de_nommage");
    int r_prop_en  = rank_of(en, "EnableNamingConventionWarning");
    if (r_guide_en < 0 || r_prop_en < 0) {
        fprintf(stderr, "FAIL: 'owl search naming --en' : hit(s) attendu(s) introuvable(s) "
                "(guide rank=%d, propriete rank=%d)\n", r_guide_en, r_prop_en);
        fails++;
    } else if (!(r_guide_en < r_prop_en)) {
        fprintf(stderr, "FAIL: 'owl search naming --en' : le guide 12_prefixes_de_nommage "
                "(rang %d) ne remonte pas avant la propriete (rang %d)\n",
                r_guide_en, r_prop_en);
        fails++;
    }
    free(en);

    /* --- 2. owl search sql : les 2 guides SQL dans le top 5 --- */
    char* sql = run_owl(owlBin, "search sql");
    if (!sql || !*sql) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl search sql'\n");
        free(sql);
        return 1;
    }
    int r_modele = rank_of(sql, "13_sql_modele");
    int r_connect = rank_of(sql, "14_sql_connecteurs");
    if (r_modele < 0 || r_connect < 0) {
        fprintf(stderr, "FAIL: 'owl search sql' : guide(s) introuvable(s) "
                "(13_sql_modele rank=%d, 14_sql_connecteurs rank=%d)\n",
                r_modele, r_connect);
        fails++;
    } else {
        if (r_modele >= 5) {
            fprintf(stderr, "FAIL: 'owl search sql' : 13_sql_modele au rang %d, "
                    "attendu < 5 (documentation-first)\n", r_modele);
            fails++;
        }
        if (r_connect >= 5) {
            fprintf(stderr, "FAIL: 'owl search sql' : 14_sql_connecteurs au rang %d, "
                    "attendu < 5 (documentation-first)\n", r_connect);
            fails++;
        }
    }

    /* --- 3. invariant non-regression intra-categorie (critere 4) : ordre
     * relatif sqlODBC < SQLEnvoie preserve, boost ou pas. Doit rester VERT. */
    int r_odbc = rank_of(sql, "sqlODBC");
    int r_envoie = rank_of(sql, "SQLEnvoie");
    if (r_odbc < 0 || r_envoie < 0) {
        fprintf(stderr, "FAIL: 'owl search sql' : hits de reference introuvables "
                "pour l'invariant intra-categorie (sqlODBC rank=%d, SQLEnvoie rank=%d)\n",
                r_odbc, r_envoie);
        fails++;
    } else if (!(r_odbc < r_envoie)) {
        fprintf(stderr, "FAIL: 'owl search sql' : ordre relatif sqlODBC (rang %d) / "
                "SQLEnvoie (rang %d) rompu — le boost documentation-first a du "
                "reordonner l'interieur des non-documentation, ce qu'il ne doit "
                "PAS faire (critere 4)\n", r_odbc, r_envoie);
        fails++;
    }
    free(sql);

    printf("=== feat/2141 ranking documentation-first (owl search) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (documentation-first applique CLI, invariant intra-categorie preserve)\n");
    return 0;
}
