/*  — digue anti-native-interne dans le maillage "Voir aussi" du
 * rendu CLI doc (`owl explain <sym>`). Meme classe de harness que
 * leak_scan.c : boite noire, popen sur le VRAI owl.exe, scan
 * du texte stdout — mais ici on ne traque pas une fuite linguistique, on
 * traque la presence d'une native marquee OWL_NAT_INTERNAL dans la sortie
 * user-facing.
 *
 * Bug historique (support #166) : find_neighbors_native (src/cli_doc.c)
 * oubliait le filtre `d->flags & OWL_NAT_INTERNAL` que ~14 autres sites
 * appliquent deja. Resultat : __MultilingueAssignSource (native interne,
 * categorie Internationalisation) atterrissait dans la ligne "voisins"
 * des 5 natives i18n publiques (Langue / LangueSource / LangueSysteme /
 * LangueSensEcriture / ListeLangues), alors qu'elle est absente de la
 * recherche et des catalogues — l'utilisateur ne pouvait rien en faire.
 *
 * Principe : pour chaque symbole i18n public, on capture `owl explain`
 * et on asserte que le token interne connu (__MultilingueAssignSource)
 * n'apparait nulle part, et plus generalement qu'aucun token prefixe
 * `__` (convention des natives internes) ne fuite dans la sortie.
 *
 * Ce n'est PAS un self-test compile dans owl.exe : harness boite noire
 * independant, compile a la volee par tests/cli/doc_lang/run.sh.
 *
 * Exit code : 0 si aucune native interne detectee, 1 sinon (rouge
 * attendu tant que src/cli_doc.c n'est pas corrige).
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

/* Compte les occurrences d'un token litteral dans le texte. */
static int count_occurrences(const char* text, const char* token) {
    if (!text || !token || !*token) return 0;
    int hits = 0;
    size_t tlen = strlen(token);
    const char* p = text;
    while ((p = strstr(p, token)) != NULL) { hits++; p += tlen; }
    return hits;
}

/* Compte les tokens debutant par "__" (convention native interne). Un
 * token commence a une frontiere non-mot et court tant que les octets
 * sont "de mot" (alnum / '_' / continuation UTF-8). */
static int is_word_byte(unsigned char c) {
    return isalnum(c) || c == '_' || (c & 0x80) != 0;
}

static int count_dunder_tokens(const char* cmd, const char* text) {
    if (!text) return 0;
    int hits = 0;
    const char* p = text;
    while (*p) {
        int at_boundary = (p == text) || !is_word_byte((unsigned char)p[-1]);
        if (at_boundary && p[0] == '_' && p[1] == '_') {
            char tok[128];
            int n = 0;
            while (is_word_byte((unsigned char)p[n]) && n < (int)sizeof(tok) - 1) {
                tok[n] = p[n]; n++;
            }
            tok[n] = '\0';
            fprintf(stderr, "[INTERNAL %d] cmd=\"%s\" token=\"%s\"\n",
                    hits + 1, cmd, tok);
            hits++;
            p += n;
            continue;
        }
        p++;
    }
    return hits;
}

/* Les 5 natives i18n publiques dont la ligne "voisins" listait la native
 * interne __MultilingueAssignSource. */
static const char* SYMBOLS[] = {
    "Langue",
    "LangueSource",
    "LangueSyst\xC3\xA8me",       /* LangueSystème (UTF-8) */
    "LangueSens\xC3\x89""criture", /* LangueSensÉcriture (UTF-8) */
    "ListeLangues",
};
#define SYMBOLS_COUNT ((int)(sizeof(SYMBOLS)/sizeof(SYMBOLS[0])))

/* Token interne connu, cite verbatim par le ticket support #166. */
static const char* KNOWN_INTERNAL = "__MultilingueAssignSource";

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    int knownHits = 0;
    int dunderHits = 0;

    for (int i = 0; i < SYMBOLS_COUNT; i++) {
        char cmdline[1024];
        /* Enveloppe la commande entiere dans une paire de guillemets
         * supplementaire : sous Windows, _popen appelle `cmd /c "<cmd>"`
         * et cmd.exe ne retire QUE la paire la plus externe. Sans ce
         * wrapper, un chemin d'exe entre guillemets (forward-slash ou
         * espace) casse le parsing et la commande echoue silencieusement
         * (capture vide -> faux GREEN). */
        snprintf(cmdline, sizeof(cmdline),
                 "\"\"%s\" explain \"%s\" 2>&1\"", owlBin, SYMBOLS[i]);
        char* out = run_capture(cmdline);
        if (!out) {
            fprintf(stderr, "ERREUR: impossible de lancer: %s\n", cmdline);
            continue;
        }
        int k = count_occurrences(out, KNOWN_INTERNAL);
        if (k > 0)
            fprintf(stderr, "[KNOWN] cmd=\"explain %s\" token=\"%s\" x%d\n",
                    SYMBOLS[i], KNOWN_INTERNAL, k);
        knownHits += k;
        dunderHits += count_dunder_tokens(SYMBOLS[i], out);
        free(out);
    }

    printf("=== fix/1153 digue native-interne dans voisins CLI doc ===\n");
    printf("Symboles i18n echantillonnes : %d\n", SYMBOLS_COUNT);
    printf("Occurrences de %s : %d\n", KNOWN_INTERNAL, knownHits);
    printf("Tokens __ (natives internes) detectes : %d\n\n", dunderHits);

    if (knownHits > 0 || dunderHits > 0) {
        printf("RESULT: RED — une native interne fuite dans la sortie "
               "user-facing (find_neighbors_native doit filtrer "
               "OWL_NAT_INTERNAL).\n");
        return 1;
    }
    /* Garde ensemble vide : le compte etait affiche, jamais
     * verifie. Un echantillon vide rend GREEN sans avoir scanne une sortie. */
    if (SYMBOLS_COUNT == 0) {
        printf("RESULT: RED — CORPUS VIDE : 0 symbole echantillonne. "
               "Un controle qui n'examine rien ne prouve rien.\n");
        return 1;
    }
    printf("RESULT: GREEN (%d symboles scannes, aucune native interne dans le "
           "maillage voisins)\n", SYMBOLS_COUNT);
    return 0;
}
