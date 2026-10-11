/*  — digue anti-doublon SANS-ACCENT dans la liste des valeurs de
 * l'enum `couleur` affichee par `owl explain couleur` (support : la fiche
 * couleur listait 57 valeurs dont 13 variantes SANS-ACCENT redondantes —
 * GrisFonce/GrisFoncé, RougeFonce/RougeFoncé, CouleurDefaut/CouleurDéfaut,
 * iSysteme/iSystème...).
 *
 * Ces alias ASCII sont LOAD-BEARING dans `.values` : ils alimentent la
 * wordlist TextMate `constant.language.owl` (accent-sensible) qui colore les
 * formes sans-accent pendant la frappe. Interdit de les retirer de `.values`.
 * Le fix est un dedup a l'AFFICHAGE uniquement (rendu `owl explain`), qui
 * saute toute valeur dont la forme accent-strippee (casse-insensible) est
 * deja apparue plus tot dans la liste.
 *
 * Ce scanner verrouille le rendu explain :
 *   1. `owl explain couleur --json` : le tableau `values` ne contient plus
 *      deux valeurs partageant la meme forme accent-strippee (casse-insens.),
 *      et son compte est le compte DEDUPE (44).
 *   2. `owl explain couleur` (FR formate) : l'entete annonce "Valeurs (44)"
 *      et plus "Valeurs (57)".
 *   3. `owl explain GrisFonce` : la forme sans-accent resout toujours vers la
 *      constante couleur (non-regression runtime).
 *
 * La preservation du calque coloration (`--vscode-data` conserve GrisFonce)
 * est verifiee separement par la fixture catalog couleur_alias_coloration.
 *
 * Harness boite noire compile a la volee par tests/cli/doc_lang/run.sh
 * (calque enum_dup_scan.c). Invoque le VRAI owl.exe (chemin en argv[1]).
 *
 * Exit code : 0 si le rendu explain est dedupe (44, sans doublon accent-var)
 * et si GrisFonce resout, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

/* Translitere en ASCII les accents latin-1 encodes UTF-8 (prefixe 0xC3), puis
 * met en minuscule. La sortie owl.exe est en UTF-8, donc "GrisFoncé" arrive
 * comme G r i s F o n c 0xC3 0xA9 -> normalise en "grisfonce". */
static void norm_key(const char* src, char* dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0xC3 && src[i + 1]) {
            unsigned char nx = (unsigned char)src[i + 1];
            char base = 0;
            if      (nx >= 0x80 && nx <= 0x85) base = 'a';
            else if (nx == 0x87)               base = 'c';
            else if (nx >= 0x88 && nx <= 0x8B) base = 'e';
            else if (nx >= 0x8C && nx <= 0x8F) base = 'i';
            else if (nx == 0x91)               base = 'n';
            else if ((nx >= 0x92 && nx <= 0x96) || nx == 0x98) base = 'o';
            else if (nx >= 0x99 && nx <= 0x9C) base = 'u';
            else if (nx == 0x9D)               base = 'y';
            else if (nx >= 0xA0 && nx <= 0xA5) base = 'a';
            else if (nx == 0xA7)               base = 'c';
            else if (nx >= 0xA8 && nx <= 0xAB) base = 'e';
            else if (nx >= 0xAC && nx <= 0xAF) base = 'i';
            else if (nx == 0xB1)               base = 'n';
            else if ((nx >= 0xB2 && nx <= 0xB6) || nx == 0xB8) base = 'o';
            else if (nx >= 0xB9 && nx <= 0xBC) base = 'u';
            else if (nx == 0xBD || nx == 0xBF) base = 'y';
            if (base) { dst[o++] = base; i++; continue; }
        }
        dst[o++] = (char)tolower(c);
    }
    dst[o] = '\0';
}

/* Extrait les tokens du tableau JSON `"values": [ ... ]`. Remplit vals[]
 * (chaines allouees) et retourne le nombre. -1 si le tableau est absent. */
#define MAX_VALS 512
static int extract_json_values(const char* json, char vals[][256]) {
    const char* hdr = strstr(json, "\"values\": [");
    if (!hdr) return -1;
    const char* p = hdr + strlen("\"values\": [");
    const char* end = strchr(p, ']');
    if (!end) return -1;
    int n = 0;
    while (p < end && n < MAX_VALS) {
        const char* q = strchr(p, '"');
        if (!q || q >= end) break;
        q++;
        const char* r = strchr(q, '"');
        if (!r || r > end) break;
        size_t len = (size_t)(r - q);
        if (len >= 256) len = 255;
        memcpy(vals[n], q, len);
        vals[n][len] = '\0';
        n++;
        p = r + 1;
    }
    return n;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* --- 1. JSON : dedup accent-variant + compte 44 --- */
    char* js = run_owl(owlBin, "explain couleur --json");
    if (!js || !*js) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain couleur --json'\n");
        free(js);
        return 1;
    }
    static char vals[MAX_VALS][256];
    int nvals = extract_json_values(js, vals);
    if (nvals < 0) {
        fprintf(stderr, "FAIL: tableau \"values\" introuvable dans le JSON de couleur\n");
        free(js);
        return 1;
    }

    /* Detection de doublon accent-strippe (casse-insensible). */
    static char keys[MAX_VALS][256];
    int nkeys = 0;
    for (int i = 0; i < nvals; i++) {
        char key[256];
        norm_key(vals[i], key, sizeof(key));
        for (int j = 0; j < nkeys; j++) {
            if (strcmp(keys[j], key) == 0) {
                fprintf(stderr,
                    "FAIL: JSON couleur : valeur \"%s\" (forme normalisee \"%s\") "
                    "est un doublon accent-variant non deduplique au rendu\n",
                    vals[i], key);
                fails++;
                break;
            }
        }
        snprintf(keys[nkeys++], sizeof(keys[0]), "%s", key);
    }

    if (nvals != 44) {
        fprintf(stderr,
            "FAIL: JSON couleur : %d valeurs listees, attendu 44 (dedupe)\n", nvals);
        fails++;
    }
    free(js);

    /* --- 2. FR formate : entete "Valeurs (44)" --- */
    char* fr = run_owl(owlBin, "explain couleur");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain couleur'\n");
        free(fr);
        return 1;
    }
    if (!strstr(fr, "Valeurs (44)")) {
        fprintf(stderr, "FAIL: 'explain couleur' n'annonce pas \"Valeurs (44)\"\n");
        fails++;
    }
    if (strstr(fr, "Valeurs (57)")) {
        fprintf(stderr, "FAIL: 'explain couleur' annonce encore \"Valeurs (57)\" (non dedupe)\n");
        fails++;
    }
    free(fr);

    /* --- 3. Non-regression runtime : GrisFonce resout vers la constante --- */
    char* gf = run_owl(owlBin, "explain GrisFonce");
    if (!gf || !*gf) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain GrisFonce'\n");
        free(gf);
        return 1;
    }
    char gfnorm[65536];
    norm_key(gf, gfnorm, sizeof(gfnorm));
    if (!strstr(gfnorm, "grisfonce")) {
        fprintf(stderr, "FAIL: 'explain GrisFonce' ne resout pas vers la couleur GrisFoncé\n");
        fails++;
    }
    if (!strstr(gfnorm, "constante")) {
        fprintf(stderr, "FAIL: 'explain GrisFonce' n'identifie pas une constante\n");
        fails++;
    }
    free(gf);

    printf("=== fix/1854 dedup affichage variantes sans-accent de couleur ===\n");
    printf("Valeurs listees (attendu 44) : %d — echecs : %d\n", nvals, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s)) — doublon accent-variant non deduplique "
               "ou GrisFonce ne resout plus.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (couleur dedupe a 44 sans doublon accent-variant, "
           "GrisFonce resout)\n");
    return 0;
}
