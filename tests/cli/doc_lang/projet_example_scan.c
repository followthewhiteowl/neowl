/*  (ST3 de l'umbrella , ) — contenu  :
 * exemple commente du bloc `est un Projet` demontrant les proprietes
 * "avancees" (au-dela de Langues/Description/Auteur/Licence deja illustrees
 * en section 1/3 du guide `24_projet.md`) : la fenetre de demarrage
 * (`FenetreDemarrage`) et l'avertissement de convention de nommage
 * (`ActiveAvertissementConventionDeNommage`).
 *
 * Choix de surface : une declaration `X est un Projet` ne peut PAS vivre
 * dans un scenario `@sujet` classique. Confirme au depot de ce scanner
 * (`./build/owl.exe --check` sur un fichier jetable) :
 *   - un bloc `est un Projet` en top-level d'un fichier bascule ce fichier en
 *     mode "fichier-projet strict" : `owl --check` refuse toute instruction
 *     hors du bloc ("Un fichier-projet ne peut contenir que le bloc Projet"),
 *     donc aucune `PROCEDURE scenario_*` voisine n'est possible ;
 *   - une `PROCEDURE` a l'INTERIEUR du bloc Projet est elle aussi refusee
 *     ("Seul l'événement Fermeture y est autorisé").
 * Aucun angle scenario ne peut donc "tourner proprement" — la decision dev
 * (, "fidelite complete") laisse alors le guide `24_projet.md` comme
 * seul vecteur. Ce scanner verrouille le contenu de ce guide, pas un
 * scenario.
 *
 * ROUGE attendu tant que l'exemple n'est pas ajoute : le guide ne mentionne
 * aujourd'hui aucune des deux proprietes (confirme : seuls Langues,
 * Description, Auteur, Licence sont illustres dans les sections 1 a 3).
 *
 * Boite noire, lecture directe du guide embarque via `owl learn`.
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

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    char* out = run_owl(owlBin, "learn 24_projet");
    if (!out || !*out) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl learn 24_projet'\n");
        free(out);
        return 1;
    }

    if (!strstr(out, "ActiveAvertissementConventionDeNommage")) {
        fprintf(stderr, "FAIL: guide 24_projet : exemple avec "
                "ActiveAvertissementConventionDeNommage absent\n");
        fails++;
    }
    if (!strstr(out, "FenetreDemarrage") &&
        !strstr(out, "Fen\xc3\xaatreD\xc3\xa9marrage") /* FenêtreDémarrage, UTF-8 */) {
        fprintf(stderr, "FAIL: guide 24_projet : exemple avec la propriete "
                "FenetreDemarrage/FenêtreDémarrage absent\n");
        fails++;
    }
    free(out);

    printf("=== feat/2142 exemple commente Projet (24_projet.md) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — exemple pas encore ajoute au guide\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (guide 24_projet illustre FenetreDemarrage + ActiveAvertissementConventionDeNommage)\n");
    return 0;
}
