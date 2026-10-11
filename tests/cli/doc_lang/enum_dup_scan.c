/*  — digue anti-doublon FR+EN dans la liste des valeurs des
 * enumerations registre affichee par `owl explain <enum> --en` (support #272 :
 * la fiche errInfo listait chaque composant deux fois, ex "errSummary"
 * apparaissant a la fois via son alias EN nu et via le pairage FR/EN).
 *
 * Cause racine (cf. ticket  / ) : `.values` d'un enum ne
 * doit porter QUE la forme FR canon ; le rendu `owl explain --en` derive la
 * forme EN du pairage bilingue et l'affiche en "FR / EN". Une regression qui
 * re-ajoute l'alias EN comme entree separee de `.values` fait reapparaitre
 * ce meme alias EN a la fois DANS la paire ET comme ligne nue en trop ->
 * chaque concept apparait deux fois.
 *
 * Version generique : au lieu de cibler 2 enums en dur, ce scanner ENUMERE
 * TOUS les enums registre (via `owl --dump-doc-fiches`, lignes "enum\t<nom>")
 * et echoue si l'un d'eux ré-affiche un doublon. Detection precise : pour
 * chaque enum, on scanne le bloc "Values (N)" en mode EN et on cherche tout
 * token qui apparait A LA FOIS comme partie EN d'une paire "FR / EN" ET seul
 * sur une ligne nue (= alias EN nu redondant, exactement le pattern ).
 *
 * NB : les variantes SANS-ACCENT de l'enum `couleur` (ex "GrisFonce /
 * DarkGray" en plus de "GrisFoncé / DarkGray", sujet distinct ) ne
 * declenchent PAS ce scanner : une partie EN partagee entre deux paires
 * n'est jamais un token nu, donc hors de l'intersection paire-EN ∩ ligne-nue.
 *
 * En plus du scan generique, deux sentinelles de compte explicites sont
 * conservees (regression evidente si ca bouge) : errInfo doit annoncer 16
 * valeurs, boutisme 2.
 *
 * Ce n'est PAS un self-test compile dans owl.exe : harness boite noire
 * compile a la volee par tests/cli/doc_lang/run.sh (calque
 * projet_prop_scan.c / keyword_en_scan.c). On invoque le VRAI owl.exe
 * (chemin en argv[1]).
 *
 * Exit code : 0 si aucun enum registre n'expose de doublon FR/EN et si les
 * deux sentinelles tiennent, 1 sinon.
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

/* Invoque `owl <args>` en silencant stderr. */
static char* run_owl(const char* owlBin, const char* args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>NUL\"", owlBin, args);
    return run_capture(cmdline);
}

/* Extrait le bloc de lignes sous "Values (N)" jusqu'a la ligne vide
 * suivante. Remplit *outCount avec N (le nombre declare dans l'entete).
 * Retourne un buffer alloue (a liberer) contenant le bloc brut (sans
 * l'entete), ou NULL si "Values (" / "Valeurs (" absent. */
static char* extract_values_block(const char* text, int* outCount) {
    const char* hdr = strstr(text, "Valeurs (");
    if (!hdr) hdr = strstr(text, "Values (");
    if (!hdr) return NULL;
    const char* paren = strchr(hdr, '(');
    if (!paren) return NULL;
    *outCount = atoi(paren + 1);
    const char* lineStart = strchr(hdr, '\n');
    if (!lineStart) return NULL;
    lineStart++;
    /* borne = premiere ligne vide (double '\n') */
    const char* end = strstr(lineStart, "\n\n");
    if (!end) end = lineStart + strlen(lineStart);
    size_t len = (size_t)(end - lineStart);
    char* block = (char*)malloc(len + 1);
    memcpy(block, lineStart, len);
    block[len] = '\0';
    return block;
}

/* Copie la ligne courante trimmee dans out (cap octets). Avance *pp sur la
 * ligne suivante. Retourne 1 s'il restait une ligne, 0 en fin de bloc. */
static int next_trimmed_line(const char** pp, char* out, size_t cap) {
    const char* p = *pp;
    if (!*p) return 0;
    const char* eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    size_t copyLen = len < cap - 1 ? len : cap - 1;
    memcpy(out, p, copyLen);
    out[copyLen] = '\0';
    /* trim gauche */
    char* s = out;
    while (*s == ' ' || *s == '\t') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    /* trim droite */
    size_t sl = strlen(out);
    while (sl > 0 && isspace((unsigned char)out[sl - 1])) out[--sl] = '\0';
    *pp = eol ? eol + 1 : p + len;
    return 1;
}

/* Translitere en ASCII les accents latins-1 (UTF-8 sur 2 octets, prefixe
 * 0xC3). Necessaire car les noms d'enum accentues, passes via _popen (flux
 * ANSI), sont manges avant d'atteindre owl.exe ; owl.exe resout de toute
 * facon les symboles en insensible aux accents (invariant n°2), donc la
 * forme ASCII est une cle equivalente et robuste au round-trip console. */
static void de_accent(const char* src, char* dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0xC3 && src[i + 1]) {
            unsigned char n = (unsigned char)src[i + 1];
            char base = 0;
            if      (n >= 0x80 && n <= 0x85) base = 'A';
            else if (n == 0x87)              base = 'C';
            else if (n >= 0x88 && n <= 0x8B) base = 'E';
            else if (n >= 0x8C && n <= 0x8F) base = 'I';
            else if (n == 0x91)              base = 'N';
            else if ((n >= 0x92 && n <= 0x96) || n == 0x98) base = 'O';
            else if (n >= 0x99 && n <= 0x9C) base = 'U';
            else if (n == 0x9D)              base = 'Y';
            else if (n >= 0xA0 && n <= 0xA5) base = 'a';
            else if (n == 0xA7)              base = 'c';
            else if (n >= 0xA8 && n <= 0xAB) base = 'e';
            else if (n >= 0xAC && n <= 0xAF) base = 'i';
            else if (n == 0xB1)              base = 'n';
            else if ((n >= 0xB2 && n <= 0xB6) || n == 0xB8) base = 'o';
            else if (n >= 0xB9 && n <= 0xBC) base = 'u';
            else if (n == 0xBD || n == 0xBF) base = 'y';
            if (base) { dst[o++] = base; i++; continue; }
        }
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

/* Scanne le bloc EN d'un enum. Detecte tout token qui apparait A LA FOIS
 * comme partie EN d'une paire "FR / EN" ET seul sur une ligne nue. Emet
 * chaque doublon sur stderr et retourne le nombre de doublons trouves. */
#define MAX_VALUE_LINES 512
static int scan_en_duplicates(const char* enumName, const char* block) {
    static char pairEN[MAX_VALUE_LINES][256];
    static char lone[MAX_VALUE_LINES][256];
    int nPair = 0, nLone = 0;

    const char* p = block;
    char line[256];
    while (next_trimmed_line(&p, line, sizeof(line))) {
        if (!line[0]) continue;
        char* sep = strstr(line, " / ");
        if (sep) {
            /* partie EN = tout ce qui suit le premier " / " */
            const char* right = sep + 3;
            if (nPair < MAX_VALUE_LINES) {
                snprintf(pairEN[nPair], sizeof(pairEN[0]), "%s", right);
                nPair++;
            }
        } else {
            if (nLone < MAX_VALUE_LINES) {
                snprintf(lone[nLone], sizeof(lone[0]), "%s", line);
                nLone++;
            }
        }
    }

    int dups = 0;
    for (int i = 0; i < nLone; i++) {
        for (int j = 0; j < nPair; j++) {
            if (strcmp(lone[i], pairEN[j]) == 0) {
                fprintf(stderr,
                    "FAIL: %s --en : token \"%s\" present a la fois comme "
                    "partie EN d'une paire \"FR / %s\" ET seul sur une ligne "
                    "(alias EN nu redondant — cf. ADR_0186)\n",
                    enumName, lone[i], lone[i]);
                dups++;
                break;
            }
        }
    }
    return dups;
}

/* Scanne un enum : recupere son bloc EN, cherche les doublons. Si le nom
 * correspond a une sentinelle, verifie aussi le compte declare. Retourne
 * le nombre d'echecs (0 = OK). */
static int scan_enum(const char* owlBin, const char* enumName) {
    int fails = 0;
    char asciiName[256], args[512];
    de_accent(enumName, asciiName, sizeof(asciiName));
    snprintf(args, sizeof(args), "explain %s --en", asciiName);
    char* out = run_owl(owlBin, args);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide pour 'explain %s --en'\n", enumName);
        free(out);
        return 1;
    }
    int n = 0;
    char* block = extract_values_block(out, &n);
    if (!block) {
        fprintf(stderr, "FAIL: bloc Values(...) introuvable pour %s --en\n", enumName);
        free(out);
        return 1;
    }

    fails += scan_en_duplicates(enumName, block);

    /* Sentinelles de compte : regression evidente si le nombre de concepts
     * annonce change. */
    if (strcmp(enumName, "errInfo") == 0 && n != 16) {
        fprintf(stderr, "FAIL: sentinelle errInfo : %d valeurs annoncees, attendu 16\n", n);
        fails++;
    }
    if (strcmp(enumName, "boutisme") == 0 && n != 2) {
        fprintf(stderr, "FAIL: sentinelle boutisme : %d valeurs annoncees, attendu 2\n", n);
        fails++;
    }

    free(block);
    free(out);
    return fails;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    /* Enumeration runtime robuste : `owl --dump-doc-fiches` emet une ligne
     * "enum\t<nom>" par enum registre (meme source que le catalogue doc). */
    char* fiches = run_owl(owlBin, "--dump-doc-fiches");
    if (!fiches || !*fiches) {
        fprintf(stderr, "ERREUR: `owl --dump-doc-fiches` n'a rien renvoye\n");
        free(fiches);
        return 2;
    }

    int fails = 0, scanned = 0, sawErrInfo = 0, sawBoutisme = 0;
    char* p = fiches;
    while (*p) {
        char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char lineBuf[512];
        size_t copyLen = len < sizeof(lineBuf) - 1 ? len : sizeof(lineBuf) - 1;
        memcpy(lineBuf, p, copyLen);
        lineBuf[copyLen] = '\0';
        /* trim \r final */
        size_t bl = strlen(lineBuf);
        while (bl > 0 && (lineBuf[bl - 1] == '\r' || isspace((unsigned char)lineBuf[bl - 1])))
            lineBuf[--bl] = '\0';

        if (strncmp(lineBuf, "enum\t", 5) == 0) {
            const char* name = lineBuf + 5;
            if (*name) {
                fails += scan_enum(owlBin, name);
                scanned++;
                if (strcmp(name, "errInfo") == 0) sawErrInfo = 1;
                if (strcmp(name, "boutisme") == 0) sawBoutisme = 1;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(fiches);

    /* Les sentinelles doivent avoir ete rencontrees : si l'enumeration ne les
     * a pas listees, le scanner est aveugle -> echec. */
    if (!sawErrInfo) {
        fprintf(stderr, "FAIL: sentinelle errInfo absente de l'enumeration des enums\n");
        fails++;
    }
    if (!sawBoutisme) {
        fprintf(stderr, "FAIL: sentinelle boutisme absente de l'enumeration des enums\n");
        fails++;
    }

    printf("=== fix/1777 doublons FR+EN dans les valeurs d'enum (owl explain) ===\n");
    printf("Enums balayes : %d — echecs : %d\n", scanned, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s)) — un enum registre re-affiche un alias "
               "EN nu redondant (ADR_0186) ou une sentinelle a bouge.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (%d enums sans doublon FR/EN, sentinelles errInfo/boutisme OK)\n",
           scanned);
    return 0;
}
