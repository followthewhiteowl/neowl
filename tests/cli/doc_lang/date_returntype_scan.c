/*  — digue sur le returnType precis des natives systeme de date
 * (support #293 : `owl explain DateSys/HeureSys/DateHeureSys` annoncait
 * `date|heure|dateheure`, une union ambigue heritee d'un tableau de signature
 * partage `sig_sys[]` / `sig_no_arg[]`). L'utilisateur, ne voyant pas le type,
 * croyait a une chaine -> `DateSys()+HeureSys()` crashe, `NumToString(date)`
 * rend vide.
 *
 * Ce scanner verrouille, pour chaque native concernee, le return_type expose
 * par `owl explain <name> --json` : il doit valoir le type PRECIS attendu, et
 * l'union `date|heure|dateheure` ne doit plus jamais reapparaitre.
 *
 * Table verrouillee :
 *   DateSys, DateDuJour (Today)        -> date
 *   HeureSys (TimeSys), Maintenant (Now) -> heure
 *   DateHeureSys (SysDateTime)         -> dateheure
 *   AnneeEnCours, MoisEnCours, JourEnCours (entiers systeme) -> entier
 *
 * NB : la SEMANTIQUE runtime (objets types) est inchangee — ce scanner ne
 * couvre QUE la metadata de signature affichee dans l'aide.
 *
 * Harness boite noire compile a la volee par tests/cli/doc_lang/run.sh
 * (calque enum_dup_scan.c / projet_prop_scan.c). Invoque le VRAI owl.exe
 * (chemin en argv[1]).
 *
 * Exit code : 0 si tous les return_type attendus tiennent et si aucune union
 * ambigue ne subsiste, 1 sinon.
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

/* Verifie que chaque occurrence de "return_type" dans le JSON vaut la valeur
 * attendue et que l'union ambigue ne subsiste pas. Retourne 0 si OK, 1 sinon. */
static int check_native(const char* owlBin, const char* name, const char* expected) {
    char args[256];
    snprintf(args, sizeof(args), "explain %s --json", name);
    char* out = run_owl(owlBin, args);
    if (!out || !*out) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain %s --json'\n", name);
        free(out);
        return 1;
    }

    int fails = 0;

    /* L'union ambigue ne doit plus jamais apparaitre. */
    if (strstr(out, "date|heure|dateheure")) {
        fprintf(stderr, "FAIL: %s expose encore l'union ambigue \"date|heure|dateheure\"\n", name);
        fails++;
    }

    /* Chaque return_type doit valoir exactement la valeur attendue. */
    char needle[128];
    snprintf(needle, sizeof(needle), "\"return_type\": \"%s\"", expected);
    const char* p = out;
    int nFound = 0, nBad = 0;
    while ((p = strstr(p, "\"return_type\": \"")) != NULL) {
        nFound++;
        if (strncmp(p, needle, strlen(needle)) != 0) {
            /* extrait la valeur reelle pour le message */
            const char* v = p + strlen("\"return_type\": \"");
            const char* q = strchr(v, '"');
            int vlen = q ? (int)(q - v) : 0;
            fprintf(stderr, "FAIL: %s : return_type attendu \"%s\", trouve \"%.*s\"\n",
                    name, expected, vlen, v);
            nBad++;
        }
        p += strlen("\"return_type\": \"");
    }
    if (nFound == 0) {
        fprintf(stderr, "FAIL: %s : aucun return_type dans le JSON\n", name);
        fails++;
    }
    fails += nBad;

    free(out);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    struct { const char* name; const char* type; } cases[] = {
        { "DateSys",      "date" },
        { "DateDuJour",   "date" },
        { "Today",        "date" },
        { "HeureSys",     "heure" },
        { "TimeSys",      "heure" },
        { "Maintenant",   "heure" },
        { "Now",          "heure" },
        { "DateHeureSys", "dateheure" },
        { "SysDateTime",  "dateheure" },
        { "AnneeEnCours", "entier" },
        { "MoisEnCours",  "entier" },
        { "JourEnCours",  "entier" },
    };
    int nCases = (int)(sizeof(cases) / sizeof(cases[0]));

    int fails = 0;
    for (int i = 0; i < nCases; i++) {
        fails += check_native(owlBin, cases[i].name, cases[i].type);
    }

    printf("=== fix/1779 returnType precis des natives systeme de date ===\n");
    printf("Natives balayees : %d — echecs : %d\n", nCases, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d native(s) avec returnType incorrect ou union ambigue)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (returnType precis verrouille pour les 12 natives systeme)\n");
    return 0;
}
