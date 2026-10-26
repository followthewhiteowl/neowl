/*  — l'aide (hover / --vscode-data) de la prop `..MasqueAffichage` du
 * champ Saisie ne doit plus etre date-only. Symptome (support #380) : la descFR
 * disait « Masque d'affichage d'un champ Date : ... ex. "AAAA-MM-JJ",
 * "JJ/MM/AAAA" », alors que la prop pilote REELLEMENT le format affiche/relu des
 * trois types temporels du champ Saisie — Date, Heure et
 * Duree — et que Num/Monetaire ont leurs propres props
 * (FormatNombre / FormatMonetaire). L'utilisateur n'avait aucun exemple de masque
 * pour Heure / Duree, ni d'orientation vers les props numeriques.
 *
 * Ce harness isole l'entree {"kind":"Saisie","name":"MasqueAffichage"} du dump
 * `owl --vscode-data` (la surface qui alimente le hover VSCode) et verifie que
 * descFR ET descEN :
 *   - citent les trois types temporels (Date, Heure/Time, Duree/Duration) ;
 *   - donnent un exemple de masque Heure (jeton "HH:MI") en plus de la Date ;
 *   - orientent vers FormatNombre / FormatMonetaire (resp. NumberFormat /
 *     CurrencyFormat en EN) pour les champs numerique / monetaire.
 *
 * Mutation : remettre la desc date-only -> "Heure"/"Time", "HH:MI" et
 * "FormatNombre"/"NumberFormat" disparaissent -> ce test rougit. La desc de la
 * COLONNE (deja Date/Heure/Duree via , mais sans orientation
 * FormatNombre) n'est PAS ciblee : on filtre sur "kind":"Saisie".
 *
 * Boite noire, calque widget_style_descen_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh. Exit code 0 si la desc Saisie est complete, 1 sinon.
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
    size_t cap = 2097152, len = 0;
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

static int extract_str(const char* start, const char* end,
                       const char* key, char* out, size_t outsz) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(start, needle);
    if (!p || p >= end) return 0;
    p += strlen(needle);
    size_t j = 0;
    while (p < end && *p) {
        if (*p == '\\' && p + 1 < end) {
            if (j + 2 < outsz) { out[j++] = p[0]; out[j++] = p[1]; }
            p += 2;
            continue;
        }
        if (*p == '"') break;
        if (j + 1 < outsz) out[j++] = *p;
        p++;
    }
    out[j] = '\0';
    return 1;
}

/* Jetons ASCII attendus dans la descFR du champ Saisie (prefixes avant accent
 * pour ne pas dependre de l'encodage des bytes : "Dur" couvre Duree, "Mon" dans
 * "FormatMon" couvre FormatMonetaire). */
static const char* const EXPECT_FR[] = {
    "Date", "Heure", "Dur", "HH:MI", "JJ/MM/AAAA",
    "FormatNombre", "FormatMon", NULL
};
static const char* const EXPECT_EN[] = {
    "Date", "Time", "Duration", "HH:MI", "JJ/MM/AAAA",
    "NumberFormat", "CurrencyFormat", NULL
};

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" --vscode-data 2>NUL\"", owlBin);
    char* out = run_capture(cmdline);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide (--vscode-data non execute) : %s\n",
                cmdline);
        free(out);
        printf("RESULT: RED — --vscode-data sans sortie\n");
        return 1;
    }

    /* Isole l'objet {"kind":"Saisie","name":"MasqueAffichage",...}. */
    const char* NEEDLE = "{\"kind\":\"Saisie\",\"name\":\"MasqueAffichage\"";
    const char* start = strstr(out, NEEDLE);
    int fails = 0;
    if (!start) {
        fprintf(stderr, "FAIL: entree Saisie/MasqueAffichage introuvable dans "
                "--vscode-data (emission cassee ?)\n");
        free(out);
        printf("RESULT: RED — entree Saisie/MasqueAffichage absente\n");
        return 1;
    }
    const char* end = strchr(start, '}');
    if (!end) end = out + strlen(out);

    char descFR[4096] = {0}, descEN[4096] = {0};
    if (!extract_str(start, end, "descFR", descFR, sizeof(descFR)) || !descFR[0]) {
        fprintf(stderr, "FAIL: descFR absente/vide\n");
        fails++;
    }
    if (!extract_str(start, end, "descEN", descEN, sizeof(descEN)) || !descEN[0]) {
        fprintf(stderr, "FAIL: descEN absente/vide\n");
        fails++;
    }

    for (int i = 0; EXPECT_FR[i]; i++) {
        if (!strstr(descFR, EXPECT_FR[i])) {
            fprintf(stderr, "FAIL: descFR Saisie/MasqueAffichage : jeton attendu "
                    "\"%s\" absent (desc encore date-only ?)\n", EXPECT_FR[i]);
            fails++;
        }
    }
    for (int i = 0; EXPECT_EN[i]; i++) {
        if (!strstr(descEN, EXPECT_EN[i])) {
            fprintf(stderr, "FAIL: descEN Saisie/MasqueAffichage : jeton attendu "
                    "\"%s\" absent (desc encore date-only ?)\n", EXPECT_EN[i]);
            fails++;
        }
    }
    free(out);

    printf("=== fix/2164 desc Saisie/MasqueAffichage couvre Date/Heure/Duree + "
           "oriente FormatNombre ===\n");
    printf("Echecs : %d\n", fails);
    if (fails > 0) {
        printf("RESULT: RED (%d) — l'aide de ..MasqueAffichage reste incomplete.\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (desc temporelle complete + orientation numerique)\n");
    return 0;
}
