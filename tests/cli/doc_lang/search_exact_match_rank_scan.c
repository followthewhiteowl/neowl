/*  — residuel de la cloture de  (doublon de ,
 * support #308) : `owl search` ne fait pas primer le match EXACT du nom
 * d'un symbole sur une mention incidente en fondation. Mesure coordinator
 * (2026-10-25) : `owl search colonne` (FR) classe le champ `Colonne` en
 * 16e position, noyee sous des fondations qui ne font que mentionner
 * « colonne » en passant ; `owl search column` (EN) le classe en 5e
 * (deja bien mieux) — asymetrie FR/EN qui trahit un biais de scoring selon
 * la forme/tokenisation.
 *
 * Ce scanner verrouille :
 *   1. Panel de NON-REGRESSION (VERT des aujourd'hui, a PRESERVER apres
 *      impl) : un jeu de symboles varies (enum, const, native, type,
 *      propriete) dont le nom exact est deja bien classe (top-3) quand on
 *      tape ce nom. Objectif : prouver que le levier qui va corriger le cas
 *      colonne/column ne degrade pas ce qui fonctionne deja.
 *   2. Le cas a corriger (ROUGE attendu) : `owl search colonne` doit classer
 *      `Colonne [champ]` dans le top-3 — aujourd'hui tres loin derriere des
 *      fondations incidentes.
 *   3. La symetrie FR/EN (ROUGE attendu) : l'ecart entre le rang FR
 *      (`colonne`) et le rang EN (`column`) pour le meme symbole `Colonne`
 *      doit rester faible — aujourd'hui l'ecart est large, signe du biais
 *      de scoring selon la langue de la requete.
 *
 * Boite noire, calque search_doc_first_scan.c. Compile a la
 * volee par tests/cli/doc_lang/run.sh.
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

/* Rang 0-based d'un hit : chaque ligne de resultat `owl search` commence
 * par exactement 2 espaces d'indentation puis le nom de l'entree, suivi
 * d'un espace de padding avant le marqueur `[kind]`. On ancre sur
 * "\n  <name> " (debut de ligne EXACT) plutot que sur une simple
 * sous-chaine `name`, pour ne pas confondre `Colonne` avec un hit dont le
 * nom se termine incidemment par les memes lettres (ex. `TitreColonne`
 * mentionne en plein milieu d'une ligne de fondation). Compter le nombre
 * de `[` rencontres STRICTEMENT AVANT le debut de cette ligne = nombre de
 * hits qui la precedent. */
static int rank_of(const char* out, const char* name) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\n  %s ", name);
    const char* p = strstr(out, needle);
    if (!p) return -1;
    int rank = 0;
    for (const char* q = out; q < p; q++) if (*q == '[') rank++;
    return rank;
}

static int abs_int(int v) { return v < 0 ? -v : v; }

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- 1. Panel de non-regression : symbole exact deja en tete, varie
     * par kind (enum/const/native/type/propriete), doit rester VERT. --- */
    /* Requetes en forme SANS ACCENT (invariant #2 : accent-insensible au
     * runtime) — evite le piege d'encodage cmd.exe/_popen sur Windows avec
     * des arguments accentues, qui corrompt les octets UTF-8 avant meme
     * d'atteindre owl.exe. */
    struct { const char* query; const char* name; } panel[] = {
        { "typeBouton", "typeBouton" },                 /* enum */
        { "teBoutonG", "teBoutonG" },                   /* const */
        { "evtBoutonGauche", "evtBoutonGauche" },       /* const */
        { "dCopieImageFenetre", "dCopieImageFenêtre" }, /* native */
        { "MatriceColonne", "MatriceColonne" },         /* native */
        { "TableColonnesFiltrees", "TableColonnesFiltrées" }, /* native */
        { "xlsNbColonne", "xlsNbColonne" },             /* native */
        { "xlsColonne", "xlsColonne" },                 /* type */
        { "styleZoneBouton", "styleZoneBouton" },       /* type */
        { "FenetreDemarrage", "Projet..FenêtreDémarrage" }, /* propriété */
    };
    size_t panel_n = sizeof(panel) / sizeof(panel[0]);
    for (size_t i = 0; i < panel_n; i++) {
        char args[256];
        snprintf(args, sizeof(args), "search \"%s\"", panel[i].query);
        char* out = run_owl(owlBin, args);
        if (!out || !*out) {
            fprintf(stderr, "FAIL: sortie vide pour 'owl search %s'\n", panel[i].query);
            free(out);
            fails++;
            continue;
        }
        int rank = rank_of(out, panel[i].name);
        if (rank < 0) {
            fprintf(stderr, "FAIL: panel non-regression : '%s' introuvable dans "
                    "'owl search %s' — needle absent, verifier le nom canon\n",
                    panel[i].name, panel[i].query);
            fails++;
        } else if (rank >= 3) {
            fprintf(stderr, "FAIL: panel non-regression : '%s' (rang %d) a regresse "
                    "hors du top-3 pour 'owl search %s'\n",
                    panel[i].name, rank, panel[i].query);
            fails++;
        }
        free(out);
    }

    /* --- 2. Cas a corriger (ROUGE attendu) : owl search colonne (FR) ->
     * Colonne [champ] doit etre dans le top-3. --- */
    char* fr = run_owl(owlBin, "search colonne");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl search colonne'\n");
        free(fr);
        return 1;
    }
    int r_colonne_fr = rank_of(fr, "Colonne");
    if (r_colonne_fr < 0) {
        fprintf(stderr, "FAIL: 'owl search colonne' : hit 'Colonne [champ]' introuvable\n");
        fails++;
    } else if (r_colonne_fr >= 3) {
        fprintf(stderr, "FAIL: 'owl search colonne' : le champ Colonne (rang %d) n'est pas "
                "dans le top-3 — match exact du nom pas encore priorise sur les fondations "
                "incidentes (fix/2306)\n", r_colonne_fr);
        fails++;
    }

    /* --- 3. Symetrie FR/EN (ROUGE attendu) : l'ecart de rang entre la
     * requete FR 'colonne' et la requete EN 'column' pour le meme symbole
     * Colonne doit rester faible (<= 2). --- */
    char* en = run_owl(owlBin, "search column --en");
    if (!en || !*en) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl search column --en'\n");
        free(en);
        free(fr);
        return 1;
    }
    int r_colonne_en = rank_of(en, "Colonne");
    if (r_colonne_en < 0) {
        fprintf(stderr, "FAIL: 'owl search column --en' : hit 'Colonne [widget]' introuvable\n");
        fails++;
    } else if (r_colonne_fr >= 0) {
        int gap = abs_int(r_colonne_fr - r_colonne_en);
        if (gap > 2) {
            fprintf(stderr, "FAIL: asymetrie FR/EN : rang FR('colonne')=%d vs "
                    "rang EN('column')=%d, ecart %d > 2 — le scoring favorise "
                    "anormalement une des deux langues (fix/2306)\n",
                    r_colonne_fr, r_colonne_en, gap);
            fails++;
        }
    }
    free(fr);
    free(en);

    printf("=== fix/2306 match exact prime sur fondation (owl search) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (panel non-regression preserve, match exact priorise, symetrie FR/EN)\n");
    return 0;
}
