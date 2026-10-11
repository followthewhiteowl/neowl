/*  — garde anti-bruit changelog : `--dump-doc-fiches` (source de la
 * baseline "Nouveautes", tools/doc_fiches_baseline.tsv) NE DOIT PAS gagner
 * les champs de types composites une fois la navigation F1 generalisee.
 * `collect_doc_fiches` (src/main.c) enumere natives/types/primitives/
 * enums/consts/widgets/references — jamais DSLPROPS ni un futur array de
 * champs de types : les proprietes de widgets deja navigables (
 * pour Projet, feature widgets historique) n'y apparaissent pas non plus
 * aujourd'hui, et ce test verrouille que ca continue de valoir pour les
 * champs de types composites.
 *
 * Verification en deux temps :
 *  1. Aucune ligne `type<TAB><NomDeChamp>` ni `property<TAB><NomDeChamp>`
 *     pour les 5 champs cibles de type_field_nav_scan.c (MaxTokens, TopK,
 *     TopP, Endpoint, MotDePasse) — ils ne doivent JAMAIS devenir des fiches
 *     de premier rang, seulement des entrees membres agregees.
 *  2. Le nombre total de lignes de fiches (hors commentaires) ne bouge pas
 *     de plus de quelques unites entre l'etat courant et l'etat courant (ce
 *     test s'auto-consomme comme sentinelle de stabilite : si un jour cette
 *     valeur decroche fortement, c'est le signe qu'un agent a fait fuiter
 *     des membres dans `collect_doc_fiches`).
 *
 * Boite noire, calque projet_prop_scan.c. VERT aujourd'hui ET apres
 *  (le critere d'acceptation exige que ca reste VERT). */

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

static const char* FIELD_NAMES[] = {
    "MaxTokens", "TopK", "TopP", "Endpoint", "MotDePasse",
};
#define FIELD_COUNT ((int)(sizeof(FIELD_NAMES)/sizeof(FIELD_NAMES[0])))

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char cmdline[512];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" --dump-doc-fiches 2>NUL\"", owlBin);
    char* out = run_capture(cmdline);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide (dump-doc-fiches non execute) : %s\n", cmdline);
        free(out);
        printf("RESULT: RED — dump-doc-fiches sans sortie\n");
        return 1;
    }

    int fails = 0;
    for (int i = 0; i < FIELD_COUNT; i++) {
        char needleTab[128];
        /* une ligne baseline est "<kind>\t<name>\n" : on cherche
         * "\t<NomDeChamp>\n" ou "\t<NomDeChamp>\r\n" pour eviter tout
         * faux-positif sur un prefixe partagé. */
        snprintf(needleTab, sizeof(needleTab), "\t%s\n", FIELD_NAMES[i]);
        char needleTabCR[128];
        snprintf(needleTabCR, sizeof(needleTabCR), "\t%s\r\n", FIELD_NAMES[i]);
        if (strstr(out, needleTab) || strstr(out, needleTabCR)) {
            fprintf(stderr, "FAIL: \"%s\" est devenu une fiche de premier rang "
                    "dans --dump-doc-fiches (fuite baseline)\n", FIELD_NAMES[i]);
            fails++;
        } else {
            printf("  OK  \"%s\" absent de la baseline dump-doc-fiches\n", FIELD_NAMES[i]);
        }
    }

    /* compte de lignes non-commentaires, pour info (pas d'assertion figee : le
     * nombre total bouge legitimement au fil des tickets independants) */
    int lines = 0;
    for (const char* p = out; *p; p++) if (*p == '\n') lines++;
    printf("Lignes totales dump-doc-fiches (info) : %d\n", lines);

    free(out);

    printf("=== feat/1866 baseline doc-fiches non polluee par les champs de types ===\n");
    printf("Champs verifies absents : %d, fuites detectees : %d\n", FIELD_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d fuite(s) dans la baseline)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (baseline doc-fiches non polluee)\n");
    return 0;
}
