// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du moteur de traduction FR <-> EN de la surface langage
 *. Execute via `owl --self-test-translate` (cf main.c).
 *
 * Contrat verrouille :
 *   - round-trip identite au canon : translate(translate(F, EN), FR) ==
 *     owl_format(F), et symetrique EN. Couvre mots-cles, natives, types
 *     primitifs, proprietes builtin, blocs/proprietes DSL (fenetre), mix.
 *   - identifiants / chaines / commentaires INCHANGES par la traduction.
 *   - tokens de surface sans canon bilingue (neutres : date/buffer, TAB,
 *     Null...) laisses tels quels -> round-trip preserve.
 *   - formes heritees de fin de ligne (RC, CR) NON neutres : reecrites en
 *     CRLF dans les deux sens.
 *   - le code EN produit COMPILE (--check via temp file).
 *
 * Approche : appels in-process directs a owl_translate / owl_format. Le
 * compile-check passe par un fichier temporaire + handle_check_command_no_
 * vm_lifecycle (pattern cli_ops_test, dup2 stdout pour absorber la sortie).
 *
 * Retourne 0 si tout passe, sinon le nombre d'echecs. */

#include "vm.h"
#include "formatter.h"
#include "owl_registry.h"
#include "fixture_unique.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #define owl_mkdir(p)  _mkdir(p)
  #define owl_rmdir  _rmdir
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #define owl_mkdir(p)  mkdir((p), 0755)
  #define owl_rmdir  rmdir
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

extern int handle_check_command_no_vm_lifecycle(const char* filepath);
extern int handle_translate_command(int argc, char* argv[]);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Helpers
 * ================================================================ */

/* Dossier temporaire propre au processus.
 *
 * A nom constant, deux `--self-test-translate` lances du meme dossier
 * ecrivaient les MEMES fichiers (`t.owl`, `src_lf.owl`, `stdout_cap.owl`...) :
 * l'un tronquait la source que l'autre traduisait, ou effacait la capture que
 * l'autre relisait -> "EN compile" et "capture -o non NULL" rouges sans qu'une
 * ligne du traducteur n'ait bouge. Le dossier est cree a la premiere demande et
 * retire en fin de famille.
 *
 * Point initial : ce dossier nait dans le repertoire courant — la
 * racine du depot quand `make` lance ce self-test — et se remplit de `.owl`.
 * Tous les walkers du depot sautent les entrees pointees, donc une campagne
 * `owl test` concurrente ne les charge plus comme compagnons pour abandonner
 * quand le cas suivant les remplace. Le retrait de fin nettoie, il ne ferme
 * pas la fenetre de course : elle est ouverte pendant l'execution. */
static const char* tr_tmp_dir(void) {
    static char dir[128];
    if (dir[0] == '\0') {
        fixture_unique(dir, sizeof dir, ".owl_translate_tmp_", "");
        owl_mkdir(dir);
    }
    return dir;
}

/* Compose `<dossier temp>/<feuille>` dans `out` (le dossier existe au retour).
 * Les chemins rendus sont donc TOUJOURS des chemins reellement creables : les
 * assertions qui dependent de l'existence du fichier gardent leur sens. */
static const char* tr_tmp_path(char* out, size_t taille, const char* feuille) {
    snprintf(out, taille, "%s/%s", tr_tmp_dir(), feuille);
    return out;
}

/* translate(translate(src, to), back) — chaine allouee (caller free). */
static char* round_trip(const char* src, OwlTranslateLang to,
                        OwlTranslateLang back) {
    char* mid = owl_translate(src, to);
    if (mid == NULL) return NULL;
    char* out = owl_translate(mid, back);
    free(mid);
    return out;
}

/* Verifie l'identite round-trip au canon pour une source ecrite dans
 * `srcLang` : on traduit vers l'autre langue puis on revient, le resultat
 * doit egaler fmt(src). Contrat : translate(translate(F, autre), F_lang)
 * == fmt(F). */
static void check_rt_lang(const char* src, OwlTranslateLang srcLang,
                          const char* label) {
    OwlTranslateLang other = (srcLang == OWL_TRANSLATE_FR)
                             ? OWL_TRANSLATE_EN : OWL_TRANSLATE_FR;
    char* canon = owl_format(src);
    char* rt = round_trip(src, other, srcLang);

    CHECK(canon != NULL, label);
    CHECK(rt != NULL, label);
    if (canon && rt) {
        bool eq = strcmp(canon, rt) == 0;
        CHECK(eq, label);
        if (!eq) {
            fprintf(stderr, "    [%s] RT != fmt\n      fmt : %s\n      rt  : %s\n",
                    label, canon, rt);
        }
    }
    free(canon);
    free(rt);
}

/* Source FR canon : round-trip EN->FR == fmt. */
static void check_round_trip_identity(const char* src, const char* label) {
    check_rt_lang(src, OWL_TRANSLATE_FR, label);
}

/* Verifie qu'un substring est PRESENT dans la traduction vers `to`. */
static void check_translate_contains(const char* src, OwlTranslateLang to,
                                     const char* needle, const char* label) {
    char* out = owl_translate(src, to);
    CHECK(out != NULL, label);
    if (out) {
        bool has = strstr(out, needle) != NULL;
        CHECK(has, label);
        if (!has) fprintf(stderr, "    [%s] '%s' absent de:\n      %s\n", label, needle, out);
    }
    free(out);
}

/* Verifie qu'un substring est ABSENT de la traduction vers `to`. */
static void check_translate_absent(const char* src, OwlTranslateLang to,
                                   const char* needle, const char* label) {
    char* out = owl_translate(src, to);
    CHECK(out != NULL, label);
    if (out) {
        bool has = strstr(out, needle) != NULL;
        CHECK(!has, label);
        if (has) fprintf(stderr, "    [%s] '%s' present (devait etre absent) dans:\n      %s\n",
                         label, needle, out);
    }
    free(out);
}

/* Compile-check du code EN produit : ecrit dans un temp, lance --check,
 * absorbe stdout. Retourne true si exit 0. */
static bool en_compiles(const char* src, const char* label) {
    /* Sous-dossier dedie : isole le temp du prescan de contexte (sinon les
     * .owl voisins du cwd de test polluent le compile-check : faux shadow). */
    char TMP[192], TMP_LOG[192];
    tr_tmp_path(TMP, sizeof TMP, "t.owl");
    tr_tmp_path(TMP_LOG, sizeof TMP_LOG, "t.log");

    char* en = owl_translate(src, OWL_TRANSLATE_EN);
    if (en == NULL) { CHECK(false, label); return false; }

    FILE* f = fopen(TMP, "wb");
    if (f == NULL) { free(en); CHECK(false, label); return false; }
    fputs(en, f);
    fclose(f);

    /* Absorbe stdout pendant le check (le check imprime un rapport JSON/texte). */
    fflush(stdout);
    int saved = owl_dup(owl_fileno(stdout));
    int devnull = owl_open(TMP_LOG,
                           O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (devnull >= 0) { owl_dup2(devnull, owl_fileno(stdout)); owl_close(devnull); }

    int rc = handle_check_command_no_vm_lifecycle(TMP);

    fflush(stdout);
    if (saved >= 0) { owl_dup2(saved, owl_fileno(stdout)); owl_close(saved); }

    bool ok = (rc == 0);
    /* Le JSON du check est utile quand le compile-check echoue : sans lui, le
     * rapport ne donne que le rc et la source, pas le diagnostic. */
    if (!ok) {
        char buf[4096]; buf[0] = '\0';
        FILE* lg = fopen(TMP_LOG, "rb");
        if (lg) { size_t n = fread(buf, 1, sizeof(buf) - 1, lg); buf[n] = '\0'; fclose(lg); }
        fprintf(stderr, "    [%s] diagnostics du check : %s\n", label, buf);
    }
    owl_unlink(TMP_LOG);
    owl_unlink(TMP);

    CHECK(ok, label);
    if (!ok) fprintf(stderr, "    [%s] EN ne compile pas (rc=%d):\n%s\n", label, rc, en);
    free(en);
    return ok;
}

/* Compile-check direct d'une source OWL (sans traduction). Ecrit dans un
 * temp, lance --check, absorbe stdout. Retourne true si exit 0.  :
 * sert a prouver que le compilateur reste gender-agnostic (accepte un ET une
 * devant le meme type, zero enforcement de genre). */
static bool owl_compiles(const char* src, const char* label) {
    char TMP[192], TMP_LOG[192];
    tr_tmp_path(TMP, sizeof TMP, "g.owl");
    tr_tmp_path(TMP_LOG, sizeof TMP_LOG, "g.log");

    FILE* f = fopen(TMP, "wb");
    if (f == NULL) { CHECK(false, label); return false; }
    fputs(src, f);
    fclose(f);

    fflush(stdout);
    int saved = owl_dup(owl_fileno(stdout));
    int devnull = owl_open(TMP_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (devnull >= 0) { owl_dup2(devnull, owl_fileno(stdout)); owl_close(devnull); }

    int rc = handle_check_command_no_vm_lifecycle(TMP);

    fflush(stdout);
    if (saved >= 0) { owl_dup2(saved, owl_fileno(stdout)); owl_close(saved); }
    owl_unlink(TMP_LOG);
    owl_unlink(TMP);

    bool ok = (rc == 0);
    CHECK(ok, label);
    if (!ok) fprintf(stderr, "    [%s] ne compile pas (rc=%d):\n%s\n", label, rc, src);
    return ok;
}

/* ================================================================
 *  : parite octets stdout / -o + preservation EOL
 *
 * `handle_translate_command` emet stdout via fputs() sans binariser fd 1.
 * Sur Windows, stdout par defaut est en MODE TEXTE : chaque '\n' emis par
 * fputs est retraduit en "\r\n" par le CRT. Le formatter etant EOL-
 * preservant, une source deja CRLF traverse verbatim -> "\r\n" en memoire
 * -> "\r\r\n" sur stdout (bug support #201). Une source LF, elle, se voit
 * silencieusement transformee en CRLF sur stdout (perte de l'EOL source).
 *
 * Le chemin `-o <fichier>` (fopen "wb") est sain et sert d'oracle : sur la
 * meme source, les octets stdout doivent lui etre STRICTEMENT identiques.
 *
 * PIEGE : `_dup2` recopie le mode binaire du FICHIER DE CAPTURE vers fd 1.
 * Si le fichier de capture est ouvert avec O_BINARY, fd 1 devient binaire
 * et le bug ne se manifeste plus -> faux vert. Les captures stdout de cette
 * section sont donc ouvertes SANS O_BINARY (mode texte), pour reproduire
 * fidelement `owl translate ... > out.owl` depuis un shell Windows.
 * ================================================================ */

/* Ecrit une fixture .owl en LF explicite (octets bruts, "wb"). */
static bool write_fixture_lf(const char* path, const char* const* lines, int n) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    for (int i = 0; i < n; i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

/* Ecrit une fixture .owl en CRLF explicite (octets bruts a la main, "wb"
 * + '\r' puis '\n' ecrits separement — aucune conversion implicite). */
static bool write_fixture_crlf(const char* path, const char* const* lines, int n) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    for (int i = 0; i < n; i++) {
        fputs(lines[i], f);
        fputc('\r', f);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

/* Lit un fichier en octets bruts (mode binaire), retourne le buffer alloue
 * (caller free) et sa longueur exacte via *outLen. NULL si echec. */
static char* read_bytes(const char* path, long* outLen) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (outLen) *outLen = (long)got;
    return buf;
}

/* Lance `owl translate --to en <srcPath>` en capturant les OCTETS REELS
 * ecrits sur stdout (fichier de capture ouvert SANS O_BINARY, cf. piege
 * ci-dessus). Retourne le buffer (caller free), longueur via *outLen. */
static char* run_translate_stdout(const char* srcPath, long* outLen) {
    char CAP[192];
    tr_tmp_path(CAP, sizeof CAP, "stdout_cap.owl");
    /* Volontairement SANS O_BINARY : _dup2 recopierait un mode binaire sur
     * fd 1 et masquerait le bug (cf. commentaire de section). */
    int fd = owl_open(CAP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;

    fflush(stdout);
    int saved = owl_dup(owl_fileno(stdout));
    owl_dup2(fd, owl_fileno(stdout));
    owl_close(fd);

    char* argv[] = { "owl", "translate", "--to", "en", (char*)srcPath };
    handle_translate_command(5, argv);

    fflush(stdout);
    if (saved >= 0) { owl_dup2(saved, owl_fileno(stdout)); owl_close(saved); }

    char* out = read_bytes(CAP, outLen);
    owl_unlink(CAP);
    return out;
}

/* Lance `owl translate --to en <srcPath> -o <out>` (chemin oracle, sain).
 * Retourne les octets ecrits (caller free), longueur via *outLen. */
static char* run_translate_dashO(const char* srcPath, long* outLen) {
    char OUT[192];
    tr_tmp_path(OUT, sizeof OUT, "dasho_cap.owl");
    char* argv[] = { "owl", "translate", "--to", "en", (char*)srcPath,
                     "-o", (char*)OUT };
    handle_translate_command(7, argv);
    char* out = read_bytes(OUT, outLen);
    owl_unlink(OUT);
    return out;
}

/* Compte les occurrences d'une sous-sequence de 2 octets dans un buffer
 * de longueur `len` (pas de dependance a strstr, qui s'arreterait sur un
 * NUL potentiel dans un buffer binaire). */
static int count_seq2(const char* buf, long len, char a, char b) {
    int n = 0;
    for (long i = 0; i + 1 < len; i++) {
        if (buf[i] == a && buf[i + 1] == b) n++;
    }
    return n;
}

static int count_byte(const char* buf, long len, char c) {
    int n = 0;
    for (long i = 0; i < len; i++) if (buf[i] == c) n++;
    return n;
}

/* Fixture representative (~22 lignes, contenu varie : commentaires,
 * PROCEDURE/FIN, declarations `est un(e)`, SI/FIN, boucle POUR, chaines
 * litterales) — pas un cas jouet a 2 lignes. */
static const char* FIXTURE_LINES[] = {
    "// Salutation pedagogique",
    "PROCEDURE Salut(nom est une cha\xc3\xaene)",
    "\tSI nom = \"\" ALORS",
    "\t\tRENVOYER \"Bonjour inconnu\"",
    "\tFIN",
    "\tmsg est une cha\xc3\xaene = \"Bonjour \" + nom",
    "\tRENVOYER msg",
    "FIN",
    "",
    "// Compteur simple",
    "PROCEDURE Compte(n est un entier)",
    "\ttotal est un entier = 0",
    "\tPOUR i = 1 \xc3\x80 n",
    "\t\ttotal = total + i",
    "\tFIN",
    "\tRENVOYER total",
    "FIN",
    "",
    "s est une cha\xc3\xaene = Salut(\"Owl\")",
    "Trace(s)",
    "c est un entier = Compte(5)",
    "Trace(c)",
};
static const int FIXTURE_N =
    (int)(sizeof(FIXTURE_LINES) / sizeof(FIXTURE_LINES[0]));

/* Cas 1+2+3+4 : source CRLF, stdout sans \r\r\n, parite avec -o, compte de
 * lignes conserve. */
static void test_stdout_eol_crlf(void) {
    char SRC[192];
    tr_tmp_path(SRC, sizeof SRC, "src_crlf.owl");
    CHECK(write_fixture_crlf(SRC, FIXTURE_LINES, FIXTURE_N),
          "fixture CRLF ecrite");

    long lenStdout = 0, lenOracle = 0;
    char* stdoutBuf = run_translate_stdout(SRC, &lenStdout);
    char* oracleBuf = run_translate_dashO(SRC, &lenOracle);

    CHECK(stdoutBuf != NULL, "CRLF: capture stdout non NULL");
    CHECK(oracleBuf != NULL, "CRLF: capture -o non NULL");

    if (stdoutBuf) {
        int nDoubled = count_seq2(stdoutBuf, lenStdout, '\r', '\r');
        CHECK(nDoubled == 0, "CRLF: aucune sequence \\r\\r\\n sur stdout");
        if (nDoubled != 0) {
            fprintf(stderr,
                "    [CRLF] %d occurrence(s) de \\r\\r trouvees sur stdout "
                "(len=%ld)\n", nDoubled, lenStdout);
        }
    }

    if (stdoutBuf && oracleBuf) {
        bool sameLen = (lenStdout == lenOracle);
        CHECK(sameLen, "CRLF: stdout et -o ont la meme longueur");
        if (!sameLen) {
            fprintf(stderr,
                "    [CRLF] longueur stdout=%ld != -o=%ld\n",
                lenStdout, lenOracle);
        }
        bool sameBytes = sameLen &&
            memcmp(stdoutBuf, oracleBuf, (size_t)lenStdout) == 0;
        CHECK(sameBytes, "CRLF: stdout octet-pour-octet identique a -o");
        if (!sameBytes) {
            long firstDiff = -1;
            long minLen = sameLen ? lenStdout
                                   : (lenStdout < lenOracle ? lenStdout : lenOracle);
            for (long i = 0; i < minLen; i++) {
                if (stdoutBuf[i] != oracleBuf[i]) { firstDiff = i; break; }
            }
            fprintf(stderr,
                "    [CRLF] divergence stdout/-o au offset %ld\n", firstDiff);
        }
    }

    if (stdoutBuf) {
        int nLines = count_byte(stdoutBuf, lenStdout, '\n');
        CHECK(nLines == FIXTURE_N,
              "CRLF: nombre de lignes conserve (source N -> stdout N)");
        if (nLines != FIXTURE_N) {
            fprintf(stderr,
                "    [CRLF] lignes source=%d, lignes stdout=%d\n",
                FIXTURE_N, nLines);
        }
    }

    free(stdoutBuf);
    free(oracleBuf);
    owl_unlink(SRC);
}

/* Cas 1+2+3+4 (variante LF) : source LF, stdout sans \r du tout, parite
 * avec -o, compte de lignes conserve. */
static void test_stdout_eol_lf(void) {
    char SRC[192];
    tr_tmp_path(SRC, sizeof SRC, "src_lf.owl");
    CHECK(write_fixture_lf(SRC, FIXTURE_LINES, FIXTURE_N),
          "fixture LF ecrite");

    long lenStdout = 0, lenOracle = 0;
    char* stdoutBuf = run_translate_stdout(SRC, &lenStdout);
    char* oracleBuf = run_translate_dashO(SRC, &lenOracle);

    CHECK(stdoutBuf != NULL, "LF: capture stdout non NULL");
    CHECK(oracleBuf != NULL, "LF: capture -o non NULL");

    if (stdoutBuf) {
        int nCR = count_byte(stdoutBuf, lenStdout, '\r');
        CHECK(nCR == 0, "LF: aucun \\r sur stdout (EOL source preserve)");
        if (nCR != 0) {
            fprintf(stderr,
                "    [LF] %d octet(s) \\r trouves sur stdout (len=%ld)\n",
                nCR, lenStdout);
        }
    }

    if (stdoutBuf && oracleBuf) {
        bool sameLen = (lenStdout == lenOracle);
        CHECK(sameLen, "LF: stdout et -o ont la meme longueur");
        if (!sameLen) {
            fprintf(stderr,
                "    [LF] longueur stdout=%ld != -o=%ld\n",
                lenStdout, lenOracle);
        }
        bool sameBytes = sameLen &&
            memcmp(stdoutBuf, oracleBuf, (size_t)lenStdout) == 0;
        CHECK(sameBytes, "LF: stdout octet-pour-octet identique a -o");
        if (!sameBytes) {
            long firstDiff = -1;
            long minLen = sameLen ? lenStdout
                                   : (lenStdout < lenOracle ? lenStdout : lenOracle);
            for (long i = 0; i < minLen; i++) {
                if (stdoutBuf[i] != oracleBuf[i]) { firstDiff = i; break; }
            }
            fprintf(stderr,
                "    [LF] divergence stdout/-o au offset %ld\n", firstDiff);
        }
    }

    if (stdoutBuf) {
        int nLines = count_byte(stdoutBuf, lenStdout, '\n');
        CHECK(nLines == FIXTURE_N,
              "LF: nombre de lignes conserve (source N -> stdout N)");
        if (nLines != FIXTURE_N) {
            fprintf(stderr,
                "    [LF] lignes source=%d, lignes stdout=%d\n",
                FIXTURE_N, nLines);
        }
    }

    free(stdoutBuf);
    free(oracleBuf);
    owl_unlink(SRC);
}

/* ================================================================
 * Corpus
 * ================================================================ */

/*  garde-fou : le COMPILATEUR reste gender-agnostic. `un` et `une`
 * devant le MEME type compilent tous deux, sans enforcement. Le genre est une
 * donnee passive lue uniquement par translate, jamais par le compilo. */
static void test_compiler_gender_agnostic(void) {
    /* entier est masculin, mais `une entier` doit compiler tout autant. */
    owl_compiles("n est un entier\n",  "un entier compile");
    owl_compiles("n est une entier\n", "une entier compile (gender-agnostic)");
    /* chaine feminin + son contraire. */
    owl_compiles("s est une cha\xc3\xaene\n", "une chaine compile");
    owl_compiles("s est un cha\xc3\xaene\n",  "un chaine compile (gender-agnostic)");
    /* reel masculin + contraire ; date feminin + contraire. */
    owl_compiles("r est un r\xc3\xa9""el\n",  "un reel compile");
    owl_compiles("r est une r\xc3\xa9""el\n", "une reel compile (gender-agnostic)");
    owl_compiles("d est une date\n", "une date compile");
    owl_compiles("d est un date\n",  "un date compile (gender-agnostic)");
}

/* Mots-cles de controle + declaration : SI/IF, POUR/FOR, FIN/END... */
static void test_keywords_control(void) {
    static const char* FR =
        "n est un entier = 3\n"
        "SI n > 0 ALORS\n"
        "\tTrace(\"positif\")\n"
        "SINON\n"
        "\tTrace(\"negatif\")\n"
        "FIN\n";
    check_round_trip_identity(FR, "kw control RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "IF ", "kw SI->IF");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "THEN", "kw ALORS->THEN");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "ELSE", "kw SINON->ELSE");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "END", "kw FIN->END");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "SI ", "kw FR efface");
    en_compiles(FR, "kw control EN compile");
}

/* Boucle POUR ... DE ... A, logiques ET/OU. */
static void test_keywords_loop_logic(void) {
    static const char* FR =
        "POUR i = 1 A 10\n"
        "\tSI i > 2 ET i < 8 ALORS\n"
        "\t\tTrace(i)\n"
        "\tFIN\n"
        "FIN\n";
    check_round_trip_identity(FR, "kw loop/logic RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "FOR ", "kw POUR->FOR");
    check_translate_contains(FR, OWL_TRANSLATE_EN, " AND ", "kw ET->AND");
    en_compiles(FR, "kw loop/logic EN compile");
}

/* Types primitifs : chaine/string, entier/integer, booleen/boolean. */
static void test_types_primitives(void) {
    static const char* FR =
        "s est une cha\xc3\xaene\n"
        "n est un entier\n"
        "b est un bool\xc3\xa9""en\n";
    check_round_trip_identity(FR, "types prim RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "string", "type chaine->string");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "integer", "type entier->integer");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "boolean", "type booleen->boolean");
    en_compiles(FR, "types prim EN compile");

    /*  (support #202 follow-up) — TROU DE COUVERTURE : types primitifs
     * en ELEMENT de collection. Canon = PLURIEL en FR (`tableau de entiers`),
     * SINGULIER en EN (`array of
     * integer`). Le round-trip EN->FR perdait le pluriel (l'info se perd via
     * l'EN) ; le fix le restaure ET fmt canonicalise le singulier -> pluriel. */
    static const char* COLL =
        "te est un tableau d'entiers\n"
        "ts est un tableau de cha\xc3\xaenes\n"
        "tr est un tableau de r\xc3\xa9""els\n"
        "tb est un tableau de bool\xc3\xa9""ens\n";
    check_round_trip_identity(COLL, "1296 element de collection RT (pluriel canon)");
    check_translate_contains(COLL, OWL_TRANSLATE_EN, "array of integer",
                             "1296 FR->EN element au singulier (canon EN)");
    en_compiles(COLL, "1296 collection EN compile");

    /* pile de / file de : l'element se pluralise pareil. On le verifie par la
     * canonicalisation fmt (FR->FR) pour eviter le bug de GENRE d'article
     * (`une pile` -> `a pile` non retabli EN->FR), distinct du nombre et hors
     * scope 1296 (follow-up). */
    check_translate_contains("pe est une pile de entier\n", OWL_TRANSLATE_FR,
                             "pile de entiers", "1296 pile: element pluralise (fmt FR)");
    check_translate_contains("fe est une file de entier\n", OWL_TRANSLATE_FR,
                             "file de entiers", "1296 file: element pluralise (fmt FR)");

    /* Canonicalisation fmt : un singulier non-canon `tableau de entier` doit
     * (a) etre pluralise par fmt/translate FR, (b) round-tripper vers le
     * pluriel (contrat translate o translate == owl_format valide des DEUX
     * cotes, y compris entree singuliere). */
    static const char* SG = "u est un tableau de entier\n";
    check_translate_contains(SG, OWL_TRANSLATE_FR, "tableau de entiers",
                             "1296 fmt/translate FR pluralise le singulier");
    /* Round-trip au canon elide `d'entiers` (forme EN->FR devant voyelle). Le
     * connecteur `de` d'une source FR n'est pas elide par le formatter (path
     * gate sur le connecteur SOURCE `of`) et l'elision n'est round-trippable
     * que sur la forme deja elidee : d'ou la source dediee ici. */
    check_round_trip_identity("u est un tableau d'entiers\n",
                              "1296 collection elidee -> round-trip canon");
}

/*  — type composE "tableau associatif" <-> "associative array" :
 * l'adjectif est reordonne selon la grammaire cible (FR nom-adjectif, EN
 * adjectif-nom), article compris (support #202). */
static void test_composite_array_word_order(void) {
    /*  — element au PLURIEL en FR (canon : `tableau associatif de
     * chaînes`/`d'entiers`). Singulier
     * en EN (`associative array of integer`). */
    static const char* FR = "dico est un tableau associatif de entiers\n";
    static const char* EN = "dico is an associative array of integer\n";
    /* FR -> EN : adjectif-nom + article phonetique (an), element au singulier EN. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "an associative array of integer",
                             "1284 FR->EN ordre+article");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "array associative",
                           "1284 FR->EN pas d'ordre inverse");
    /* EN -> FR : nom-adjectif + article genre (un), element au PLURIEL FR. */
    check_translate_contains(EN, OWL_TRANSLATE_FR, "un tableau associatif de entiers",
                             "1284 EN->FR ordre+article");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "associatif tableau",
                           "1284 EN->FR pas d'ordre inverse");
    /* Stabilite : round-trip FR->EN->FR redonne la forme canonique. */
    check_round_trip_identity(FR, "1284 round-trip FR");
}

/* Booleens Vrai/True/Faux/False ; Null neutre. */
static void test_booleans(void) {
    static const char* FR =
        "b est un bool\xc3\xa9""en = Vrai\n"
        "c est un bool\xc3\xa9""en = Faux\n";
    check_round_trip_identity(FR, "booleans RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "True", "Vrai->True");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "False", "Faux->False");
    en_compiles(FR, "booleans EN compile");
}

/* Natives : registry bilingue (nameFR <-> nameEN). */
static void test_natives(void) {
    /* Taille (FR) <-> Length (EN). */
    static const char* FR =
        "s est une cha\xc3\xaene = \"abc\"\n"
        "n est un entier = Taille(s)\n";
    check_round_trip_identity(FR, "natives RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Length", "Taille->Length");
    en_compiles(FR, "natives EN compile");
}

/* Pass-through : identifiants user, chaines, commentaires INCHANGES. */
static void test_passthrough(void) {
    static const char* FR =
        "// un commentaire avec des mots si pour fin\n"
        "monIdentifiant est une cha\xc3\xaene = \"si pour fin alors\"\n";
    char* en = owl_translate(FR, OWL_TRANSLATE_EN);
    CHECK(en != NULL, "passthrough translate ok");
    if (en) {
        /* L'identifiant user n'est pas traduit. */
        CHECK(strstr(en, "monIdentifiant") != NULL, "identifiant user intact");
        /* La chaine litterale n'est pas traduite (contenu, pas langage). */
        CHECK(strstr(en, "\"si pour fin alors\"") != NULL, "chaine litterale intacte");
        /* Le commentaire n'est pas traduit. */
        CHECK(strstr(en, "// un commentaire avec des mots si pour fin") != NULL,
              "commentaire intact");
        free(en);
    }
    check_round_trip_identity(FR, "passthrough RT");
}

/* Tokens neutres : pas de canon bilingue oppose -> inchanges. date, buffer,
 * TAB, Null. Round-trip preserve. */
static void test_neutral_tokens(void) {
    static const char* FR =
        "d est une date\n"
        "buf est un buffer\n"
        "s est une cha\xc3\xaene = \"a\" + TAB\n"
        "p est un entier = Null\n";
    check_round_trip_identity(FR, "neutral tokens RT");
    /* date/buffer/TAB/Null restent identiques quelle que soit la cible. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "date", "date neutre");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "buffer", "buffer neutre");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Null", "Null neutre");
}

/*  — visibilite de classe : PRIVÉ/PRIVATE et PROTÉGÉ/PROTECTED sont
 * une PAIRE bilingue, pas des formes neutres. Sans cette bascule un fichier
 * traduit en EN gardait un mot-cle FR accentue au milieu du code anglais, et
 * le formateur reecrivait la forme FR par-dessus la forme EN ecrite par
 * l'utilisateur. Les deux graphies EN sont acceptees par le scanner depuis
 * l'origine (src/scanner.c) : rien de neuf n'est nomme ici, on branche des
 * formes deja declarees. */
static void test_visibility_bilingual(void) {
    static const char* FR =
        "cCompte est une Classe\n"
        "\tPUBLIC\n"
        "\t\tnPub est un entier\n"
        "\tPRIV\xc3\x89\n"
        "\t\tnSolde est un entier\n"
        "\tPROT\xc3\x89G\xc3\x89\n"
        "\t\tnAutre est un entier\n"
        "FIN\n";
    /* FR -> EN : les marqueurs de bloc basculent. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PRIVATE",
                             "1409 PRIVÉ -> PRIVATE");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PROTECTED",
                             "1409 PROTÉGÉ -> PROTECTED");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PUBLIC",
                             "1409 PUBLIC identique en EN");
    /* Volet negatif : la forme FR doit DISPARAITRE, pas seulement coexister. */
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PRIV\xc3\x89",
                           "1409 plus de PRIVÉ en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PROT\xc3\x89G\xc3\x89",
                           "1409 plus de PROTÉGÉ en EN");

    static const char* EN =
        "cCompte is a Class\n"
        "\tPUBLIC\n"
        "\t\tnPub is an integer\n"
        "\tPRIVATE\n"
        "\t\tnSolde is an integer\n"
        "\tPROTECTED\n"
        "\t\tnAutre is an integer\n"
        "END\n";
    /* EN -> FR : bascule reciproque (marqueur de bloc = masculin accentue). */
    check_translate_contains(EN, OWL_TRANSLATE_FR, "PRIV\xc3\x89",
                             "1409 PRIVATE -> PRIVÉ");
    check_translate_contains(EN, OWL_TRANSLATE_FR, "PROT\xc3\x89G\xc3\x89",
                             "1409 PROTECTED -> PROTÉGÉ");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "PRIVATE",
                           "1409 plus de PRIVATE en FR");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "PROTECTED",
                           "1409 plus de PROTECTED en FR");

    check_round_trip_identity(FR, "1409 visibilite RT");
    en_compiles(FR, "1409 classe EN compile");
    owl_compiles(EN, "1409 classe ecrite en EN compile");
}

/*  — modificateur de visibilite INLINE (« PROCÉDURE PRIVÉE foo() »).
 * L'accord feminin est une regle FR : la cible EN rend la forme invariable
 * (PRIVATE / PROTECTED / PUBLIC), le retour FR restaure le feminin. */
static void test_visibility_inline_bilingual(void) {
    static const char* FR =
        "cB est une Classe\n"
        "\tPROCEDURE PRIV\xc3\x89""E Calc()\n"
        "\t\tTrace(1)\n"
        "\tFIN\n"
        "\tPROCEDURE PROT\xc3\x89G\xc3\x89""E Aide()\n"
        "\t\tTrace(2)\n"
        "\tFIN\n"
        "\tPROCEDURE PUBLIQUE Expose()\n"
        "\t\tTrace(3)\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PROCEDURE PRIVATE Calc",
                             "1409 PROCÉDURE PRIVÉE -> PROCEDURE PRIVATE");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PROCEDURE PROTECTED Aide",
                             "1409 PROCÉDURE PROTÉGÉE -> PROCEDURE PROTECTED");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PROCEDURE PUBLIC Expose",
                             "1409 PROCÉDURE PUBLIQUE -> PROCEDURE PUBLIC");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PRIV\xc3\x89""E",
                           "1409 plus de PRIVÉE inline en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PROT\xc3\x89G\xc3\x89""E",
                           "1409 plus de PROTÉGÉE inline en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PUBLIQUE",
                           "1409 plus de PUBLIQUE inline en EN");

    static const char* EN =
        "cB is a Class\n"
        "\tPROCEDURE PRIVATE Calc()\n"
        "\t\tTrace(1)\n"
        "\tEND\n"
        "FIN\n";
    check_translate_contains(EN, OWL_TRANSLATE_FR, "PROCEDURE PRIV\xc3\x89""E Calc",
                             "1409 PROCEDURE PRIVATE -> PROCÉDURE PRIVÉE");

    check_round_trip_identity(FR, "1409 visibilite inline RT");
    en_compiles(FR, "1409 methodes EN compilent");
}

/*  — constantes nommees en position d'expression. Le registre porte
 * deja la paire FR/EN (name / name_en) pour l'essentiel du catalogue ; la
 * traduction ne la consommait pas et laissait la forme FR au milieu du code
 * EN. Echantillon : la famille de recherche dans les chaines (l'enumeration
 * optionsRechercheChaîne au complet) plus un temoin d'une autre famille. */
static void test_constants_bilingual(void) {
    static const char* FR =
        "n est un entier = Position(\"abcabc\", \"abc\", 1, DepuisFin)\n"
        "m est un entier = Position(\"abcabc\", \"abc\", 1, DepuisD\xc3\xa9""but)\n"
        "o est un entier = Position(\"abcabc\", \"abc\", 1, SansCasse)\n"
        "p est un entier = Position(\"abcabc\", \"abc\", 1, MotComplet)\n"
        "s est une cha\xc3\xaene = SansEspace(\"  a  \", sscDroite)\n";
    check_translate_contains(FR, OWL_TRANSLATE_EN, "FromEnd",
                             "1409 DepuisFin -> FromEnd");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "FromBeginning",
                             "1409 DepuisDébut -> FromBeginning");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "IgnoreCase",
                             "1409 SansCasse -> IgnoreCase");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "WholeWord",
                             "1409 MotComplet -> WholeWord");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "sscRight",
                             "1409 sscDroite -> sscRight");
    /* Volet negatif : plus une seule forme FR dans la sortie EN. */
    check_translate_absent(FR, OWL_TRANSLATE_EN, "DepuisFin",
                           "1409 plus de DepuisFin en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "DepuisD\xc3\xa9""but",
                           "1409 plus de DepuisDébut en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "SansCasse",
                           "1409 plus de SansCasse en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "MotComplet",
                           "1409 plus de MotComplet en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "sscDroite",
                           "1409 plus de sscDroite en EN");

    static const char* EN =
        "n is an integer = Position(\"abcabc\", \"abc\", 1, FromEnd)\n"
        "o is an integer = Position(\"abcabc\", \"abc\", 1, IgnoreCase)\n"
        "s is a string = NoSpace(\"  a  \", sscRight)\n";
    check_translate_contains(EN, OWL_TRANSLATE_FR, "DepuisFin",
                             "1409 FromEnd -> DepuisFin");
    check_translate_contains(EN, OWL_TRANSLATE_FR, "SansCasse",
                             "1409 IgnoreCase -> SansCasse");
    check_translate_contains(EN, OWL_TRANSLATE_FR, "sscDroite",
                             "1409 sscRight -> sscDroite");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "FromEnd",
                           "1409 plus de FromEnd en FR");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "IgnoreCase",
                           "1409 plus de IgnoreCase en FR");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "sscRight",
                           "1409 plus de sscRight en FR");

    check_round_trip_identity(FR, "1409 constantes RT");
    en_compiles(FR, "1409 constantes EN compilent");
    owl_compiles(EN, "1409 constantes ecrites en EN compilent");
}

/*  — RC et CR ne sont PAS des tokens neutres : ce sont des formes
 * heritees (CAT_EOL_LEGACY, cf. ) que la traduction reecrit en CRLF,
 * dans les deux sens. Le verrou porte sur la reecriture elle-meme : la forme
 * heritee doit disparaitre de la sortie, pas seulement CRLF y apparaitre. */
static void test_eol_legacy_rewritten(void) {
    static const char* FR_RC =
        "s est une cha\xc3\xaene = \"a\" + RC + \"b\"\n";
    check_translate_contains(FR_RC, OWL_TRANSLATE_EN, "CRLF", "RC -> CRLF (EN)");
    check_translate_absent  (FR_RC, OWL_TRANSLATE_EN, "RC",   "RC efface (EN)");
    check_translate_contains(FR_RC, OWL_TRANSLATE_FR, "CRLF", "RC -> CRLF (FR)");
    check_translate_absent  (FR_RC, OWL_TRANSLATE_FR, "RC",   "RC efface (FR)");

    static const char* FR_CR =
        "s est une cha\xc3\xaene = \"a\" + CR + \"b\"\n";
    check_translate_contains(FR_CR, OWL_TRANSLATE_EN, "CRLF", "CR -> CRLF (EN)");
    check_translate_contains(FR_CR, OWL_TRANSLATE_FR, "CRLF", "CR -> CRLF (FR)");
    /* `CR` seul ne subsiste pas : la seule occurrence de la lettre C suivie
     * de R dans la sortie est celle du CRLF canon. */
    check_translate_absent  (FR_CR, OWL_TRANSLATE_EN, "+ CR +", "CR efface (EN)");
    check_translate_absent  (FR_CR, OWL_TRANSLATE_FR, "+ CR +", "CR efface (FR)");

    en_compiles(FR_RC, "RC reecrit EN compile");
    en_compiles(FR_CR, "CR reecrit EN compile");
}

/* Fenetre DSL : bloc + proprietes bilingues. */
static void test_dsl_window(void) {
    static const char* FR =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"Bonjour\"\n"
        "\tFIN\n"
        "FIN\n";
    check_round_trip_identity(FR, "dsl window RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Window", "Fenetre->Window");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Title", "Titre->Title (DSL prop)");
    en_compiles(FR, "dsl window EN compile");
}

/* Round-trip natives a homonyme keyword (, bug 2).
 * `Supprime` a pour nameEN `Delete`, qui est aussi un mot-cle OOP. La
 * direction EN->FR doit re-resoudre `Delete` -> `Supprime` (bidirectionnel
 * via le registry). Avant le fix, `Delete` matchait le keyword NEUTRE et
 * restait `Delete` en FR (round-trip casse). */
static void test_native_delete_roundtrip(void) {
    static const char* FR =
        "PROCEDURE P(t est un tableau d'entiers)\n"
        "\tt = Supprime(t, 1)\n"
        "FIN\n";
    /* FR -> EN : Supprime -> Delete. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Delete(", "Supprime->Delete");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "Supprime", "Supprime absent en EN");
    /* EN -> FR : Delete -> Supprime (le bug : restait Delete). */
    static const char* EN =
        "PROCEDURE P(t is an array of integer)\n"
        "\tt = Delete(t, 1)\n"
        "END\n";
    check_translate_contains(EN, OWL_TRANSLATE_FR, "Supprime(", "Delete->Supprime");
    check_translate_absent(EN, OWL_TRANSLATE_FR, "Delete", "Delete absent en FR");
    /* Identite round-trip au canon dans les deux sens. */
    check_round_trip_identity(FR, "Supprime RT");
    check_rt_lang(EN, OWL_TRANSLATE_EN, "Delete RT");
}

/* Fenetre widget representative (calque FEN_Snake, ).
 *
 * Couvre les 4 bugs corriges :
 *   1. homonymie KIND/PROPRIETE : `Libellé` widget (nameEN Label) contient une
 *      propriete caption `Libellé` (nameEN Caption). La propriete doit emettre
 *      Caption, pas Label.
 *   3. bloc PROPRIETES -> PROPERTIES.
 *   4. article traduit : `une Fenêtre` -> `a Window` (genre F
 *      recupere via OwlTypeDef.genre cote EN->FR : `a Window` -> `une Fenêtre`).
 *
 * Plus : Champ Libellé/Saisie/Bouton, Timer, natives Ajoute/Taille/
 * TableauInsère/SupprimeTout, POUR...À, SI...ALORS...SINON, RENVOYER.
 *
 * NOTE compile EN : ce corpus n'inclut PAS `Supprime(...)` comme RHS ni un
 * type de parametre d'evenement en EN, deux constructions que le COMPILATEUR
 * EN refuse encore (homonyme natif `Delete` vs mot-cle OOP `delete` cote
 * parseur ; validation de signature d'evenement qui n'accepte pas les noms
 * de type EN). Ces deux points sont des limites COMPILATEUR hors perimetre du
 * moteur translate (le translate produit la forme canon EN correcte ; cf.
 * round-trip Delete/Supprime ci-dessus, stable au niveau texte). */
static void test_dsl_widget_window(void) {
    static const char* FR =
        "FEN_Jeu est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre   = \"Demo\"\n"
        "\t\tLargeur = 400\n"
        "\tFIN\n"
        "\n"
        "\tLIB_Score est un Champ Libell\xc3\xa9\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibell\xc3\xa9 = \"Score : 0\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\ttmr est un Timer\n"
        "\tcases est un tableau d'entiers\n"
        "\n"
        "\tPROCEDURE Maj(n est un entier)\n"
        "\t\tAjoute(cases, n)\n"
        "\t\tcases = TableauIns\xc3\xa8re(cases, 1, n)\n"
        "\t\tnb est un entier = Taille(cases)\n"
        "\t\tPOUR i = 1 \xc3\x80 nb\n"
        "\t\t\tSI cases[i] > 0 ALORS\n"
        "\t\t\t\tRENVOYER i\n"
        "\t\t\tSINON\n"
        "\t\t\t\tAjoute(cases, i)\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\t\tSupprimeTout(cases)\n"
        "\t\tRENVOYER 0\n"
        "\tFIN\n"
        "FIN\n";

    /* Bug 1 : la propriete caption -> Caption (pas Label, nameEN du kind). */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Caption =", "caption prop -> Caption");
    /* Le KIND, lui, reste bien Label ;  : le prefixe widget passe a
     * Control en EN (plus de franglais `Champ Label`). */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Control Label", "prefix+kind Libelle -> Control Label");
    /* Bug 3 : bloc PROPRIETES -> PROPERTIES. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "PROPERTIES", "PROPRIETES -> PROPERTIES");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "PROPRIETES", "plus de PROPRIETES en EN");
    /*  : article traduit. `une Fenêtre` -> `a Window` (W consonne). */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "is a Window", "Fenetre -> a Window (article)");
    /* Natives + controle traduits. */
    check_translate_contains(FR, OWL_TRANSLATE_EN, "Add(", "Ajoute -> Add");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "ArrayInsert(", "TableauInsere -> ArrayInsert");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "DeleteAll(", "SupprimeTout -> DeleteAll");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "RESULT", "RENVOYER -> RESULT");

    /* Round-trip identite au canon : translate(translate(FR, EN), FR) == fmt(FR). */
    check_round_trip_identity(FR, "widget window RT");

    /* Le code EN produit compile (--check clean). */
    en_compiles(FR, "widget window EN compile");
}

/* Mix complet : procedure + controle + types + natives. */
static void test_mix(void) {
    /* Params nommes hors homonymie native (Gauche/Droite SONT des natives :
     * un param homonyme se fait canoniser par le formatter cote `is` — bug
     * formatter pre-existant, hors scope ). Identite round-trip
     * verifiee sur du code dont les identifiants user sont propres. */
    static const char* FR =
        "PROCEDURE Additionne(premier est un entier, second est un entier)\n"
        "\tres est un entier = premier + second\n"
        "\tSI res > 0 ALORS\n"
        "\t\tRENVOYER res\n"
        "\tFIN\n"
        "\tRENVOYER 0\n"
        "FIN\n";
    check_round_trip_identity(FR, "mix RT");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "integer", "mix type");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "IF ", "mix control");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "RESULT", "mix RENVOYER->RESULT");
    en_compiles(FR, "mix EN compile");
}

/* Corpus EN canon : symetrie. Part d'EN, RT doit == fmt(EN). */
static void test_en_corpus_symmetry(void) {
    static const char* EN =
        "IF x > 0 THEN\n"
        "\tTrace(\"pos\")\n"
        "ELSE\n"
        "\tTrace(\"neg\")\n"
        "END\n";
    check_rt_lang(EN, OWL_TRANSLATE_EN, "EN corpus RT");
    /* EN -> FR doit produire du FR. */
    check_translate_contains(EN, OWL_TRANSLATE_FR, "SI ", "EN IF->SI");
    check_translate_contains(EN, OWL_TRANSLATE_FR, "ALORS", "EN THEN->ALORS");
    check_translate_contains(EN, OWL_TRANSLATE_FR, "FIN", "EN END->FIN");
}

/*  : article de declaration traduit (un/une <-> a/an) via le genre
 * du type. FR->EN : phonetique du nom EN (un entier -> an integer, une Fenêtre
 * -> a Window). EN->FR : genre du type (a Window -> une Fenêtre, an integer ->
 * un entier). Round-trip identite dans les deux sens, masculins ET feminins. */
static void test_article_genre(void) {
    /* --- FR -> EN : article phonetique --- */
    /* Masculin entier : un -> an (integer commence par voyelle). */
    check_translate_contains("n est un entier\n", OWL_TRANSLATE_EN,
                             "is an integer", "un entier -> an integer");
    /* Feminin chaine : une -> a (string consonne). */
    check_translate_contains("s est une cha\xc3\xaene\n", OWL_TRANSLATE_EN,
                             "is a string", "une chaine -> a string");
    /* Feminin Fenetre : une -> a (Window consonne). */
    check_translate_contains("F est une Fen\xc3\xaatre\n", OWL_TRANSLATE_EN,
                             "is a Window", "une Fenetre -> a Window");
    /* Masculin Bouton : un -> a (Button consonne). */
    check_translate_contains("B est un Bouton\n", OWL_TRANSLATE_EN,
                             "is a Button", "un Bouton -> a Button");

    /* --- EN -> FR : article via genre --- */
    /* Masculin entier : an integer -> un entier. */
    check_translate_contains("n is an integer\n", OWL_TRANSLATE_FR,
                             "est un entier", "an integer -> un entier");
    /* Feminin string : a string -> une chaine. */
    check_translate_contains("s is a string\n", OWL_TRANSLATE_FR,
                             "est une cha\xc3\xaene", "a string -> une chaine");
    /* Feminin Window : a Window -> une Fenetre (genre F recupere). */
    check_translate_contains("F is a Window\n", OWL_TRANSLATE_FR,
                             "est une Fen\xc3\xaatre", "a Window -> une Fenetre");
    /* Masculin Button : a Button -> un Bouton (genre M). */
    check_translate_contains("B is a Button\n", OWL_TRANSLATE_FR,
                             "est un Bouton", "a Button -> un Bouton");

    /* --- Round-trip identite FR -> EN -> FR (masculins + feminins) --- */
    check_round_trip_identity("n est un entier\n",       "RT un entier");
    check_round_trip_identity("s est une cha\xc3\xaene\n", "RT une chaine");
    check_round_trip_identity("F est une Fen\xc3\xaatre\n", "RT une Fenetre");
    check_round_trip_identity("B est un Bouton\n",        "RT un Bouton");
    check_round_trip_identity("t est un Timer\n",         "RT un Timer");
    check_round_trip_identity("img est une Image\n",      "RT une Image");

    /* --- Round-trip identite EN -> FR -> EN --- */
    check_rt_lang("n is an integer\n",  OWL_TRANSLATE_EN, "RT an integer");
    check_rt_lang("s is a string\n",    OWL_TRANSLATE_EN, "RT a string");
    check_rt_lang("F is a Window\n",    OWL_TRANSLATE_EN, "RT a Window");
    check_rt_lang("B is a Button\n",    OWL_TRANSLATE_EN, "RT a Button");

    /* Le fallback « type sans genre (NONE) -> article ecrit preserve » n'a plus
     * d'instance testable :  a pose le genre a la source sur les 8
     * derniers types qui en manquaient, et la mesure des 81 types du registre
     * rend 0 article degenere (`est a`). Le contrat vit toujours dans
     * formatter.c (cf. la note au point de lecture du genre) ; le verrou qui
     * l'exercait via UUID a ete retire plutot que deplace, faute d'instance —
     * y remettre un nom serait fabriquer le cas qu'on pretend observer. */

    /* --- Le code EN produit compile. --- */
    en_compiles("n est un entier\n",       "EN compile un entier");
    en_compiles("s est une cha\xc3\xaene\n", "EN compile une chaine");
}

/*  : prefixe widget `Champ` <-> `Control`. Le mot `Champ` qui
 * precede un kind reconnu (en position `<copule> <article>? Champ <kind>`)
 * est un prefixe widget — il doit suivre la langue cible (EN -> Control,
 * FR -> Champ). `Field` est le synonyme EN tolere, rabattu vers Champ en FR.
 *
 * Contrainte critique : `champ` est aussi un identifiant variable valide et
 * un type nu. La reecriture est STRICTEMENT contextuelle (kind reconnu qui
 * suit) — jamais un pairing keyword global. Les cas type-nu / variable ne
 * doivent PAS etre rabattus sur Control. */
static void test_dsl_widget_prefix_control(void) {
    /* --- FR -> EN : Champ <Kind> -> Control <KindEN> sur les 11 kinds --- */
    check_translate_contains("X est un Champ Libell\xc3\xa9\n", OWL_TRANSLATE_EN,
                             "is a Control Label", "Champ Libelle -> Control Label");
    check_translate_contains("X est un Champ Bouton\n", OWL_TRANSLATE_EN,
                             "is a Control Button", "Champ Bouton -> Control Button");
    check_translate_contains("X est un Champ Table\n", OWL_TRANSLATE_EN,
                             "is a Control Table", "Champ Table -> Control Table");
    check_translate_contains("X est un Champ Conteneur\n", OWL_TRANSLATE_EN,
                             "is a Control Container", "Champ Conteneur -> Control Container");
    check_translate_contains("X est un Champ Image\n\t\tPROPRIETES\n\t\tFIN\n",
                             OWL_TRANSLATE_EN,
                             "Control Image", "Champ Image -> Control Image");
    check_translate_contains("X est un Champ Liste\n\t\tPROPRIETES\n\t\tFIN\n",
                             OWL_TRANSLATE_EN,
                             "Control List", "Champ Liste -> Control List");
    /* Combo : canon EN du registry est ComboBox (name_en), pas Combo. */
    check_translate_contains("X est un Champ Combo\n", OWL_TRANSLATE_EN,
                             "is a Control ComboBox", "Champ Combo -> Control ComboBox");
    check_translate_contains("X est un Champ S\xc3\xa9lecteur\n", OWL_TRANSLATE_EN,
                             "is a Control RadioGroup", "Champ Selecteur -> Control RadioGroup");
    check_translate_contains("X est un Champ Interrupteur\n", OWL_TRANSLATE_EN,
                             "is a Control CheckBox", "Champ Interrupteur -> Control CheckBox");
    check_translate_contains("X est un Champ ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e\n",
                             OWL_TRANSLATE_EN,
                             "is a Control Looper", "Champ ZoneRepetee -> Control Looper");
    /* `Champ de Saisie` : le `de` intercale entre prefixe et kind est traduit
     * (`of`) ; le prefixe doit quand meme passer a Control. */
    check_translate_contains("X est un Champ de Saisie\n", OWL_TRANSLATE_EN,
                             "is a Control of Input", "Champ de Saisie -> Control of Input");
    /* Plus aucun `Champ` cote EN sur ces declarations. */
    check_translate_absent("X est un Champ Bouton\n", OWL_TRANSLATE_EN,
                           "Champ", "prefixe Champ efface en EN");

    /* --- EN -> FR : Control <KindEN> -> Champ <Kind> --- */
    check_translate_contains("X is a Control Label\n", OWL_TRANSLATE_FR,
                             "est un Champ Libell\xc3\xa9", "Control Label -> Champ Libelle");
    check_translate_contains("X is a Control Button\n", OWL_TRANSLATE_FR,
                             "est un Champ Bouton", "Control Button -> Champ Bouton");
    check_translate_absent("X is a Control Button\n", OWL_TRANSLATE_FR,
                           "Control", "prefixe Control efface en FR");
    /* `Field` synonyme EN tolere -> rabattu vers Champ en FR. */
    check_translate_contains("X is a Field Button\n", OWL_TRANSLATE_FR,
                             "est un Champ Bouton", "Field Button -> Champ Bouton");
    /* Combo reverse : ComboBox (canon EN) -> Combo (canon FR).
     * `absent ComboBox` verrouille contre le faux positif substring
     * (`Champ Combo` est un substring de `Champ ComboBox`). */
    check_translate_contains("X is a Control ComboBox\n", OWL_TRANSLATE_FR,
                             "est un Champ Combo", "Control ComboBox -> Champ Combo");
    check_translate_absent("X is a Control ComboBox\n", OWL_TRANSLATE_FR,
                           "ComboBox", "ComboBox rabattu vers Combo en FR");

    /* --- Round-trip identite : Champ -> Control -> Champ et inverse --- */
    check_round_trip_identity("X est un Champ Bouton\n", "RT Champ Bouton");
    check_round_trip_identity("X est un Champ Libell\xc3\xa9\n", "RT Champ Libelle");
    check_round_trip_identity("X est un Champ Combo\n", "RT Champ Combo");
    check_rt_lang("X is a Control Button\n", OWL_TRANSLATE_EN, "RT Control Button");
    check_rt_lang("X is a Control ComboBox\n", OWL_TRANSLATE_EN, "RT Control ComboBox");

    /* --- Non-regression CRITIQUE : `champ` variable / type nu NON reecrit ---
     * Pas de kind reconnu qui suit -> ce n'est pas un prefixe widget. */
    /* variable `champ` (LHS d'une declaration de chaine). */
    check_translate_absent("champ est une cha\xc3\xaene = \"x\"\n", OWL_TRANSLATE_EN,
                           "Control", "champ variable NON reecrit en Control");
    check_translate_contains("champ est une cha\xc3\xaene = \"x\"\n", OWL_TRANSLATE_EN,
                             "champ is a string", "champ variable preserve");
    /* type nu en parametre : `c est un Champ` (aucun kind derriere).
     *
     *  — aiguille resserree. `Champ` est une entree du registre de
     * types, nameEN = "Control", et `PROCEDURE F(c is a Control)` compile
     * (mesure : `--check` rc=0). La bascule du TYPE est donc legitime ; c'est
     * la bascule du PREFIXE widget que  voulait interdire ici, et les
     * deux rendent le meme texte dans cette position (aucun kind derriere ->
     * la machinerie prefixe, engagee ou non, n'emet rien de plus que
     * `Control`). Un `absent "Control"` ne peut donc plus distinguer les deux :
     * il ne mordait que du legitime. Ce cas garde desormais ce qu'il peut
     * reellement prouver — le type nu bascule et revient au `cmp`. La
     * contextualite stricte du prefixe reste portee (et mord) sur l'assertion
     * voisine `champ variable NON reecrit en Control`. */
    check_translate_contains("PROCEDURE F(c est un Champ)\n\tTrace(\"ok\")\nFIN\n",
                             OWL_TRANSLATE_EN,
                             "is a Control)", "type Champ nu -> Control (type, pas prefixe)");
    check_round_trip_identity("PROCEDURE F(c est un Champ)\n\tTrace(\"ok\")\nFIN\n",
                              "RT type Champ nu");
}

/*  — balayage COMPLET des sortes de controle (CONTROL_KINDS) en
 * traduction. Quatre sortes avaient leur paire FR/EN dans la table sans que la
 * traduction ne l'emette : Onglet/Tab, Volet/Pane, Jauge/ProgressBar,
 * CodeBarres/BarCodes. Symptome le plus visible sur Volet, sous-bloc sans
 * prefixe `Champ` : la sorte restait FR pendant que l'article basculait, donc
 * `est un Volet` -> `is a Volet` -> `est a Volet` (round-trip rompu, franglais
 * que l'utilisateur n'a pas ecrit).
 *
 * Cas particulier verrouille ici : `Tab` est aussi la constante TAB. La sorte
 * ne doit gagner qu'en POSITION DE GENRE (`Champ Tab`) ; partout ailleurs
 * `TAB` reste la constante, dans les deux sens de traduction. */
static void test_fix1418_control_kinds_sweep(void) {
    /* --- Onglet <-> Tab (prefixe `Champ`) --- */
    check_translate_contains("X est un Champ Onglet\n", OWL_TRANSLATE_EN,
                             "is a Control Tab", "Champ Onglet -> Control Tab");
    check_translate_absent("X est un Champ Onglet\n", OWL_TRANSLATE_EN,
                           "Onglet", "Onglet efface en EN");
    check_translate_contains("X is a Control Tab\n", OWL_TRANSLATE_FR,
                             "est un Champ Onglet", "Control Tab -> Champ Onglet");
    check_translate_absent("X is a Control Tab\n", OWL_TRANSLATE_FR,
                           "Tab", "Tab rabattu vers Onglet en FR");

    /* --- Jauge <-> ProgressBar --- */
    check_translate_contains("X est un Champ Jauge\n", OWL_TRANSLATE_EN,
                             "is a Control ProgressBar",
                             "Champ Jauge -> Control ProgressBar");
    check_translate_absent("X est un Champ Jauge\n", OWL_TRANSLATE_EN,
                           "Jauge", "Jauge efface en EN");
    check_translate_contains("X is a Control ProgressBar\n", OWL_TRANSLATE_FR,
                             "est un Champ Jauge",
                             "Control ProgressBar -> Champ Jauge");
    check_translate_absent("X is a Control ProgressBar\n", OWL_TRANSLATE_FR,
                           "ProgressBar", "ProgressBar rabattu vers Jauge en FR");

    /* --- CodeBarres <-> BarCodes. Sorte homonyme d'un type compose : le bloc
     *     PROPRIETES confirme l'intention controle (comme Image / Liste). --- */
    check_translate_contains("X est un Champ CodeBarres\n\t\tPROPRIETES\n\t\tFIN\n",
                             OWL_TRANSLATE_EN, "Control BarCodes",
                             "Champ CodeBarres -> Control BarCodes");
    check_translate_absent("X est un Champ CodeBarres\n\t\tPROPRIETES\n\t\tFIN\n",
                           OWL_TRANSLATE_EN, "CodeBarres",
                           "CodeBarres efface en EN");
    check_translate_contains("X is a Control BarCodes\n\t\tPROPERTIES\n\t\tEND\n",
                             OWL_TRANSLATE_FR, "Champ CodeBarres",
                             "Control BarCodes -> Champ CodeBarres");
    check_translate_absent("X is a Control BarCodes\n\t\tPROPERTIES\n\t\tEND\n",
                           OWL_TRANSLATE_FR, "BarCodes",
                           "BarCodes rabattu vers CodeBarres en FR");

    /* --- Volet <-> Pane. Sous-bloc de role SANS prefixe `Champ` : l'article
     *     est directement accole a la sorte, c'est le cas qui rendait `est a`. */
    check_translate_contains("VOL_A est un Volet\n", OWL_TRANSLATE_EN,
                             "is a Pane", "Volet -> Pane");
    check_translate_absent("VOL_A est un Volet\n", OWL_TRANSLATE_EN,
                           "Volet", "Volet efface en EN");
    check_translate_contains("VOL_A is a Pane\n", OWL_TRANSLATE_FR,
                             "est un Volet", "Pane -> Volet");
    check_translate_absent("VOL_A is a Pane\n", OWL_TRANSLATE_FR,
                           "Pane", "Pane rabattu vers Volet en FR");

    /* --- Aucune forme MIXTE produite : c'est le vrai defaut constate. --- */
    check_translate_absent("VOL_A est un Volet\n", OWL_TRANSLATE_EN,
                           "is un", "pas de `is un` (mixte)");
    check_translate_absent("VOL_A is a Pane\n", OWL_TRANSLATE_FR,
                           "est a ", "pas de `est a` (mixte)");
    check_translate_absent("X est un Champ Onglet\n", OWL_TRANSLATE_EN,
                           "is un", "pas de `is un` sur Onglet");

    /* --- Round-trip identite sur les quatre sortes, dans les deux sens. --- */
    check_round_trip_identity("X est un Champ Onglet\n", "RT Champ Onglet");
    check_round_trip_identity("X est un Champ Jauge\n", "RT Champ Jauge");
    check_round_trip_identity("VOL_A est un Volet\n", "RT Volet");
    check_rt_lang("X is a Control Tab\n", OWL_TRANSLATE_EN, "RT Control Tab");
    check_rt_lang("X is a Control ProgressBar\n", OWL_TRANSLATE_EN,
                  "RT Control ProgressBar");
    check_rt_lang("VOL_A is a Pane\n", OWL_TRANSLATE_EN, "RT Pane");

    /* --- Fenetre complete Onglet + Volet : le cas rapporte, round-trip
     *     octet-a-octet, et les deux langues compilent. --- */
    static const char* FEN_FR =
        "FEN_A est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"T\"\n"
        "\tFIN\n"
        "\tONG_A est un Champ Onglet\n"
        "\t\tPROPRIETES\n"
        "\t\t\tVisible = Vrai\n"
        "\t\tFIN\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibell\xc3\xa9 = \"Un\"\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(FEN_FR, OWL_TRANSLATE_EN, "is a Control Tab",
                             "fenetre : Champ Onglet -> Control Tab");
    check_translate_contains(FEN_FR, OWL_TRANSLATE_EN, "is a Pane",
                             "fenetre : Volet -> Pane");
    check_round_trip_identity(FEN_FR, "RT fenetre Onglet + Volet");
    /* Verrou  — QUATRE compile-checks de la MEME fenetre dans le meme
     * process, dans les DEUX ordres (FR puis EN, puis EN puis FR). Les
     * registres de declaration Fenetre/Etat vivent dans la VM et ce harness ne
     * refait pas de vm_init entre deux checks : tant qu'un check ne repart pas
     * d'un registre vide, le second retrouve `FEN_A` deja enregistree et rend
     * un faux E_SEM_WINDOW_DUPLICATE (rc=65) suivi d'une cascade "bloc ferme
     * trop tot". Ce bloc rougit si l'etat refuit. */
    owl_compiles(FEN_FR, "fenetre FR compile");
    en_compiles(FEN_FR, "fenetre EN compile");
    en_compiles(FEN_FR, "fenetre EN compile (2e check du meme process)");
    owl_compiles(FEN_FR, "fenetre FR compile (apres l'EN, ordre inverse)");

    /* --- Non-regression CRITIQUE : la constante TAB n'est PAS une sorte hors
     *     position de genre. `Tab` ne gagne qu'apres `Champ`/`Control`. --- */
    check_translate_contains("s est une cha\xc3\xaene = \"a\" + TAB\n",
                             OWL_TRANSLATE_EN, "TAB",
                             "constante TAB preservee FR->EN");
    check_translate_absent("s est une cha\xc3\xaene = \"a\" + TAB\n",
                           OWL_TRANSLATE_EN, "Onglet",
                           "constante TAB NON prise pour la sorte Onglet");
    check_translate_contains("s is a string = \"a\" + TAB\n",
                             OWL_TRANSLATE_FR, "TAB",
                             "constante TAB preservee EN->FR");
    check_round_trip_identity("s est une cha\xc3\xaene = \"a\" + TAB\n",
                              "RT constante TAB");
}

/*  — DSL des types composes (themes/styles) : translate TYPE-AWARE.
 *
 * Discriminant central : la MEME cle FR `Libellé` a une forme EN DIFFERENTE
 * selon le type compose englobant :
 *   - styleLibellé.Libellé -> Label
 *   - styleBouton.Libellé  -> Caption
 * Donc le translate doit connaitre le type compose courant pour resoudre le
 * bon name_en de champ. Sans ca, l'EN produit ne compile pas (`Champ 'Caption'
 * inconnu sur type styleLibellé`).
 *
 * NB needles : `Label` est un substring de `styleLabel`, donc on cible la
 * ligne de CHAMP via le tab de tete (`\tLabel`) pour ne pas passer vert
 * trivialement sur le nom de type. La discrimination forte du champ Libellé
 * repose sur `absent Caption` + `en_compiles`. */
static void test_dsl_theme_style(void) {
    /* Case 1 : styleLibellé -> styleLabel ; champ Libellé -> Label (PAS
     * Caption) ; l'EN compile. */
    static const char* FR1 =
        "STY est un styleLibell\xc3\xa9 = {\n"
        "\tLibell\xc3\xa9 = { Couleur = RVB(0, 0, 0) }\n"
        "}\n";
    check_translate_contains(FR1, OWL_TRANSLATE_EN, "styleLabel",
                             "styleLibelle -> styleLabel");
    check_translate_contains(FR1, OWL_TRANSLATE_EN, "\tLabel",
                             "champ Libelle (styleLibelle) -> Label");
    check_translate_absent(FR1, OWL_TRANSLATE_EN, "Caption",
                           "PAS de Caption sous styleLibelle");
    en_compiles(FR1, "styleLibelle EN compile");

    /* Case 2 : styleBouton -> styleButton ; champ Libellé -> Caption
     * (discriminant : meme cle FR, forme EN differente selon le type). */
    static const char* FR2 =
        "STY est un styleBouton = {\n"
        "\tLibell\xc3\xa9 = { Couleur = RVB(0, 0, 0) }\n"
        "}\n";
    check_translate_contains(FR2, OWL_TRANSLATE_EN, "styleButton",
                             "styleBouton -> styleButton");
    check_translate_contains(FR2, OWL_TRANSLATE_EN, "Caption",
                             "champ Libelle (styleBouton) -> Caption");
    en_compiles(FR2, "styleBouton EN compile");

    /* Case 3 : styleConteneur -> styleContainer ; CadreExtérieur ->
     * OuterFrame ; CouleurFond -> BackgroundColor. */
    static const char* FR3 =
        "STY est un styleConteneur = {\n"
        "\tCadreExt\xc3\xa9rieur = { CouleurFond = RVB(1, 2, 3) }\n"
        "}\n";
    check_translate_contains(FR3, OWL_TRANSLATE_EN, "styleContainer",
                             "styleConteneur -> styleContainer");
    check_translate_contains(FR3, OWL_TRANSLATE_EN, "OuterFrame",
                             "CadreExterieur -> OuterFrame");
    check_translate_contains(FR3, OWL_TRANSLATE_EN, "BackgroundColor",
                             "CouleurFond -> BackgroundColor");
    en_compiles(FR3, "styleConteneur EN compile");

    /* Case 4 : bloc Thème -> Theme (+ style imbrique traduit). */
    static const char* FR4 =
        "MonTh\xc3\xa8me est un Th\xc3\xa8me\n"
        "\tfond est un styleConteneur = {\n"
        "\t\tCadreExt\xc3\xa9rieur = { CouleurFond = RVB(1, 2, 3) }\n"
        "\t}\n"
        "FIN\n";
    check_translate_contains(FR4, OWL_TRANSLATE_EN, "is a Theme",
                             "bloc Thème -> Theme");
    check_translate_contains(FR4, OWL_TRANSLATE_EN, "styleContainer",
                             "Theme: styleConteneur -> styleContainer");
    /* `un Thème` (keyword de type FR) doit disparaitre ; on ne teste PAS le
     * bare `Thème` car l'identifiant user `MonThème` le contient legitimement. */
    check_translate_absent(FR4, OWL_TRANSLATE_EN, "un Th\xc3\xa8me",
                           "keyword Thème traduit (plus de `un Thème`)");

    /* Case 5 : valeur d'enum DSL dégradéRadial -> gradientRadial ; champ
     * Dégradé -> Gradient. */
    static const char* FR5 =
        "STY est un styleConteneur = {\n"
        "\tCadreExt\xc3\xa9rieur = {\n"
        "\t\tD\xc3\xa9grad\xc3\xa9 = { Type = d\xc3\xa9grad\xc3\xa9Radial }\n"
        "\t}\n"
        "}\n";
    check_translate_contains(FR5, OWL_TRANSLATE_EN, "Gradient",
                             "champ Dégradé -> Gradient");
    check_translate_contains(FR5, OWL_TRANSLATE_EN, "gradientRadial",
                             "valeur dégradéRadial -> gradientRadial");
    en_compiles(FR5, "enum DSL EN compile");

    /* Case 6 : round-trip FR->EN->FR == fmt(FR) (idempotence bilingue). */
    check_round_trip_identity(FR1, "theme style RT (libelle)");
    check_round_trip_identity(FR2, "theme style RT (bouton)");
    check_round_trip_identity(FR3, "theme style RT (conteneur)");
    check_round_trip_identity(FR5, "theme style RT (enum)");

    /* Case 7 : témoin non-régression (surface déjà correcte). */
    check_translate_contains(FR3, OWL_TRANSLATE_EN, "is a", "copule est un -> is a");
    check_translate_contains(FR3, OWL_TRANSLATE_EN, "RGB(", "native RVB -> RGB");
}

/*  — DSL des types composes : blocs `Etats` (palettes d'etat de style).
 *
 * Suite de  : le mot-cle `Etats`->`States` passait deja, mais
 * l'INTERIEUR du bloc restait FR (franglais). Deux niveaux a traduire :
 *   1. les CLES d'etat (Survol/Focus/Appuyé/Grisé/LectureSeule) -> forme EN
 *      PascalCase (Hover/Focus/Pressed/Grayed/ReadOnly). Ces cles ne sont pas
 *      des champs registry : elles resolvent contre la palette du type
 *      collection courant (styleXxxEtats).
 *   2. les CHAMPS internes d'un sous-bloc d'etat (Couleur/CouleurFond/Cadre...)
 *      -> resolus contre le baseType PLAT de la palette (styleXxxBase), comme
 *      tout autre compose. */
static void test_dsl_theme_style_etats(void) {
    /* Case A : palette zone de saisie (survol/focus/grisé/lectureSeule). */
    /* Article `une` = genre canonique de styleZoneSaisie (« zone » feminin) ;
     * requis pour le round-trip (translate normalise l'article au genre du
     * type, fmt preserve l'ecrit -> l'entree doit deja etre canonique). */
    static const char* FRA =
        "STY est une styleZoneSaisie = {\n"
        "\tEtats = {\n"
        "\t\tSurvol = { CouleurFond = RVB(1, 2, 3) }\n"
        "\t\tFocus = { Cadre = { Couleur = RVB(4, 5, 6), Epaisseur = 2 } }\n"
        "\t\tGris\xc3\xa9 = { Couleur = RVB(7, 8, 9) }\n"
        "\t\tLectureSeule = { CouleurFond = RVB(10, 11, 12) }\n"
        "\t}\n"
        "}\n";
    /* Cles d'etat -> forme EN PascalCase. */
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "Hover",
                             "cle etat Survol -> Hover");
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "Grayed",
                             "cle etat Grisé -> Grayed");
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "ReadOnly",
                             "cle etat LectureSeule -> ReadOnly");
    /* Champs internes resolus contre le baseType (styleZoneSaisieBase). */
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "BackgroundColor",
                             "champ interne CouleurFond -> BackgroundColor");
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "Border",
                             "sous-champ Cadre (etat Focus) -> Border");
    check_translate_contains(FRA, OWL_TRANSLATE_EN, "Thickness",
                             "sous-champ Epaisseur (Cadre etat) -> Thickness");
    /* Zero franglais : aucune forme FR ne subsiste dans le bloc Etats.
     * "Couleur" couvre a la fois Couleur ET CouleurFond (substring). */
    check_translate_absent(FRA, OWL_TRANSLATE_EN, "Survol",
                           "plus de cle FR Survol");
    check_translate_absent(FRA, OWL_TRANSLATE_EN, "Gris\xc3\xa9",
                           "plus de cle FR Grisé");
    check_translate_absent(FRA, OWL_TRANSLATE_EN, "LectureSeule",
                           "plus de cle FR LectureSeule");
    check_translate_absent(FRA, OWL_TRANSLATE_EN, "Couleur",
                           "plus de champ FR Couleur/CouleurFond dans Etats");
    en_compiles(FRA, "Etats zone saisie EN compile");
    check_round_trip_identity(FRA, "Etats zone saisie RT");

    /* Case B : palette cadre exterieur (état Appuyé present ; sous-champ Ombre
     * du baseType). Discriminant Appuyé -> Pressed (absent de la palette
     * saisie). */
    static const char* FRB =
        "STY est un styleCadreExt\xc3\xa9rieur = {\n"
        "\tEtats = {\n"
        "\t\tAppuy\xc3\xa9 = { CouleurFond = RVB(1, 2, 3) }\n"
        "\t\tSurvol = { Ombre = { Couleur = RVB(4, 5, 6) } }\n"
        "\t}\n"
        "}\n";
    check_translate_contains(FRB, OWL_TRANSLATE_EN, "Pressed",
                             "cle etat Appuyé -> Pressed");
    check_translate_contains(FRB, OWL_TRANSLATE_EN, "Shadow",
                             "sous-champ Ombre (etat Survol) -> Shadow");
    check_translate_absent(FRB, OWL_TRANSLATE_EN, "Appuy\xc3\xa9",
                           "plus de cle FR Appuyé");
    check_translate_absent(FRB, OWL_TRANSLATE_EN, "Ombre",
                           "plus de champ FR Ombre dans Etats");
    en_compiles(FRB, "Etats cadre exterieur EN compile");
    check_round_trip_identity(FRB, "Etats cadre exterieur RT");
}

/*  — cascade `Style..X..Y` ecrite EN LIGNE dans un bloc PROPRIETES.
 *
 * Gap corrige : la resolution type-aware est portee par la pile
 * indexee sur les accolades ; une cascade en ligne n'en a pas, donc aucun
 * segment ne resolvait et la construction entiere restait dans sa langue
 * source, dans le meme bloc ou `Libellé` devenait bien `Caption`.
 *
 * ORACLE : la forme bloc, qui elle traduit. Chaque fixture en ligne est
 * confrontee a son equivalent en accolades, ecrit avec UN SEUL champ par
 * niveau — les identifiants suivis d'un `=` de sa traduction donnent alors,
 * dans l'ordre, les formes cibles attendues du chemin. Aucun attendu n'est
 * ecrit a la main : si le registre bouge, l'oracle bouge avec lui. */

/* Identifiants suivis d'un `=` (hors `==`) dans `out`, dans l'ordre. Sur une
 * forme bloc a un champ par niveau, c'est exactement le chemin — precede du
 * nom de type de la declaration, que le caller saute. */
static int extract_assigned_names(const char* out, char segs[][64], int max) {
    int n = 0;
    const char* p = out;
    while (*p != '\0' && n < max) {
        unsigned char c = (unsigned char)*p;
        if (!(isalpha(c) || c >= 0x80 || c == '_')) { p++; continue; }
        const char* w = p;
        while (*p != '\0') {
            unsigned char d = (unsigned char)*p;
            if (!(isalnum(d) || d >= 0x80 || d == '_')) break;
            p++;
        }
        int len = (int)(p - w);
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '=' && q[1] != '=' && len > 0 && len < 64) {
            memcpy(segs[n], w, (size_t)len);
            segs[n][len] = '\0';
            n++;
        }
    }
    return n;
}

/* Recherche de sous-chaine insensible a la casse. La convention de graphie
 * differe entre les deux formes pour le seul segment d'etat (cle de bloc
 * `Hover` en PascalCase, subscript `[hover]` en minuscule) : c'est un choix de
 * la DSL, pas une divergence de traduction — on confronte donc la LANGUE. */
static bool contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (const char* p = hay; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nl && p[i] != '\0'
               && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
    }
    return false;
}

/* Round-trip cible : apres aller-retour, `needle` doit se retrouver a
 * l'identique. Verrou de round-trip porte sur une construction precise quand la
 * fixture entiere n'est pas RT-stable pour une raison etrangere au correctif. */
static void check_round_trip_contains(const char* src, OwlTranslateLang to,
                                      const char* needle, const char* label) {
    OwlTranslateLang back = (to == OWL_TRANSLATE_EN)
                            ? OWL_TRANSLATE_FR : OWL_TRANSLATE_EN;
    char* rt = round_trip(src, to, back);
    CHECK(rt != NULL, label);
    if (rt != NULL) {
        bool has = strstr(rt, needle) != NULL;
        CHECK(has, label);
        if (!has)
            fprintf(stderr, "    [%s] '%s' perdu au round-trip :\n%s\n",
                    label, needle, rt);
    }
    free(rt);
}

/* Confronte une cascade en ligne a son equivalent en accolades : tout segment
 * cible produit par la forme bloc doit se retrouver dans la forme en ligne. */
static void check_inline_matches_block(const char* inlineSrc, const char* blockSrc,
                                       OwlTranslateLang to, const char* label) {
    char* blockOut = owl_translate(blockSrc, to);
    char* inlineOut = owl_translate(inlineSrc, to);
    CHECK(blockOut != NULL && inlineOut != NULL, label);
    if (blockOut != NULL && inlineOut != NULL) {
        char segs[16][64];
        int n = extract_assigned_names(blockOut, segs, 16);
        /* segs[0] = nom du type de la declaration `STY is a <type> = {` ;
         * le chemin commence au segment suivant. */
        CHECK(n >= 2, label);
        for (int i = 1; i < n; i++) {
            bool has = contains_ci(inlineOut, segs[i]);
            CHECK(has, label);
            if (!has)
                fprintf(stderr, "    [%s] segment '%s' (rendu par la forme bloc) "
                                "absent de la forme en ligne :\n      %s\n",
                        label, segs[i], inlineOut);
        }
    }
    free(blockOut);
    free(inlineOut);
}

static void test_fix1415_cascade_style_inline(void) {
    /* --- Champ Onglet : chemin a 4 segments, dont un subscript d'etat. --- */
    static const char* ONG_INLINE =
        "FEN_Ong est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"t\"\n"
        "\tFIN\n"
        "\n"
        "\tONG_X est un Champ Onglet\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..VoletActif..Etats[survol]..CouleurFond = RVB(1, 2, 3)\n"
        "\t\tFIN\n"
        "\n"
        /* Un Champ Onglet doit declarer au moins un Volet pour compiler. */
        "\t\tVOL_A est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibell\xc3\xa9 = \"a\"\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    static const char* ONG_BLOCK =
        "STY est un styleOnglet = {\n"
        "\tVoletActif = { Etats = { Survol = { CouleurFond = RVB(1, 2, 3) } } }\n"
        "}\n";
    check_inline_matches_block(ONG_INLINE, ONG_BLOCK, OWL_TRANSLATE_EN,
                               "cascade en ligne onglet == forme bloc");
    /* Zero forme source survivante : le defaut d'origine laissait les QUATRE
     * segments en francais. */
    check_translate_absent(ONG_INLINE, OWL_TRANSLATE_EN, "VoletActif",
                           "plus de segment FR VoletActif en ligne");
    check_translate_absent(ONG_INLINE, OWL_TRANSLATE_EN, "Etats[",
                           "plus de segment FR Etats[ en ligne");
    check_translate_absent(ONG_INLINE, OWL_TRANSLATE_EN, "survol",
                           "plus d'etat FR survol en ligne");
    check_translate_absent(ONG_INLINE, OWL_TRANSLATE_EN, "CouleurFond",
                           "plus de segment FR CouleurFond en ligne");
    /* Aucune forme MIXTE produite, dans un sens comme dans l'autre. */
    check_translate_absent(ONG_INLINE, OWL_TRANSLATE_EN, "Etats[hover]",
                           "pas de forme mixte Etats[hover]");
    en_compiles(ONG_INLINE, "cascade en ligne onglet EN compile");
    /* Round-trip porte sur la LIGNE de cascade et non sur le fichier : la
     * fixture doit declarer un Volet pour compiler, et la sorte `Volet` n'a pas
     * (encore) de bascule bilingue en translate — defaut ETRANGER a la cascade,
     * remonte tel quel. Les fixtures bouton / saisie ci-dessous verrouillent le
     * round-trip du fichier entier. */
    check_round_trip_contains(ONG_INLINE, OWL_TRANSLATE_EN,
                              "Style..VoletActif..Etats[survol]..CouleurFond",
                              "cascade en ligne onglet RT (ligne de cascade)");

    /* --- Sens inverse EN -> FR sur la meme cascade. --- */
    static const char* ONG_INLINE_EN =
        "FEN_Ong is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"t\"\n"
        "\tEND\n"
        "\n"
        "\tONG_X is a Control Onglet\n"
        "\t\tPROPERTIES\n"
        "\t\t\tStyle..ActivePane..States[hover]..BackgroundColor = RGB(1, 2, 3)\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    static const char* ONG_BLOCK_EN =
        "STY is a styleTab = {\n"
        "\tActivePane = { States = { Hover = { BackgroundColor = RGB(1, 2, 3) } } }\n"
        "}\n";
    check_inline_matches_block(ONG_INLINE_EN, ONG_BLOCK_EN, OWL_TRANSLATE_FR,
                               "cascade en ligne onglet EN->FR == forme bloc");
    check_translate_absent(ONG_INLINE_EN, OWL_TRANSLATE_FR, "ActivePane",
                           "plus de segment EN ActivePane en FR");
    check_translate_absent(ONG_INLINE_EN, OWL_TRANSLATE_FR, "States[survol]",
                           "pas de forme mixte States[survol]");
    check_translate_absent(ONG_INLINE_EN, OWL_TRANSLATE_FR, "BackgroundColor",
                           "plus de segment EN BackgroundColor en FR");

    /* --- Discriminant de TYPE : le meme segment source `Libellé` rend
     *     `Caption` sous un Bouton et `Label` sous une Saisie. Une resolution
     *     a plat (sans porter le type le long du chemin) ne peut pas rendre
     *     les deux — ce couple verrouille la resolution type-aware. --- */
    static const char* BTN_INLINE =
        "FEN_B est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"t\"\n"
        "\tFIN\n"
        "\n"
        "\tBTN_X est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..Libell\xc3\xa9..Etats[appuy\xc3\xa9]..Couleur = RVB(1, 2, 3)\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    static const char* BTN_BLOCK =
        "STY est un styleBouton = {\n"
        "\tLibell\xc3\xa9 = { Etats = { Appuy\xc3\xa9 = { Couleur = RVB(1, 2, 3) } } }\n"
        "}\n";
    check_inline_matches_block(BTN_INLINE, BTN_BLOCK, OWL_TRANSLATE_EN,
                               "cascade en ligne bouton == forme bloc");
    check_translate_contains(BTN_INLINE, OWL_TRANSLATE_EN, "Style..Caption..",
                             "Libellé sous styleBouton -> Caption");
    en_compiles(BTN_INLINE, "cascade en ligne bouton EN compile");
    check_round_trip_identity(BTN_INLINE, "cascade en ligne bouton RT");

    static const char* SAI_INLINE =
        "FEN_S est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"t\"\n"
        "\tFIN\n"
        "\n"
        "\tSAI_X est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..Libell\xc3\xa9..Couleur = RVB(1, 2, 3)\n"
        "\t\t\tStyle..ZoneSaisie..Etats[survol]..CouleurFond = RVB(4, 5, 6)\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    static const char* SAI_BLOCK =
        "STY est une styleSaisie = {\n"
        "\tZoneSaisie = { Etats = { Survol = { CouleurFond = RVB(4, 5, 6) } } }\n"
        "}\n";
    check_inline_matches_block(SAI_INLINE, SAI_BLOCK, OWL_TRANSLATE_EN,
                               "cascade en ligne saisie == forme bloc");
    check_translate_contains(SAI_INLINE, OWL_TRANSLATE_EN, "Style..Label..",
                             "Libellé sous styleSaisie -> Label (pas Caption)");
    check_translate_absent(SAI_INLINE, OWL_TRANSLATE_EN, "Caption",
                           "pas de Caption sous styleSaisie");
    en_compiles(SAI_INLINE, "cascade en ligne saisie EN compile");
    check_round_trip_identity(SAI_INLINE, "cascade en ligne saisie RT");

    /* --- Balayage de la famille : les palettes d'etat du menu principal et de
     *     la languette d'onglet manquaient au formatter, ce qui aveuglait les
     *     DEUX formes (le sous-bloc d'etat restait source en forme bloc). --- */
    static const char* MNU_BLOCK =
        "STY est un styleMenuPrincipal = {\n"
        "\tBarre = { Etats = { Survol = { CouleurFond = RVB(1, 2, 3) } } }\n"
        "}\n";
    check_translate_contains(MNU_BLOCK, OWL_TRANSLATE_EN, "Hover",
                             "palette menu barre : Survol -> Hover");
    check_translate_contains(MNU_BLOCK, OWL_TRANSLATE_EN, "BackgroundColor",
                             "palette menu barre : CouleurFond -> BackgroundColor");
    check_round_trip_identity(MNU_BLOCK, "palette menu barre RT");

    static const char* MNU_OPT_BLOCK =
        "STY est un styleMenuPrincipal = {\n"
        "\tOptionMenu = { Etats = { Survol = { Couleur = RVB(1, 2, 3) } } }\n"
        "}\n";
    check_translate_contains(MNU_OPT_BLOCK, OWL_TRANSLATE_EN, "Hover",
                             "palette menu option : Survol -> Hover");
    check_round_trip_identity(MNU_OPT_BLOCK, "palette menu option RT");

    /* Etat `appuyé` du subscript : absent de la palette du formatter avant ce
     * correctif (seule la cle de BLOC `Appuyé` etait connue). */
    check_translate_contains(BTN_INLINE, OWL_TRANSLATE_EN, "[pressed]",
                             "subscript appuyé -> pressed");

    /* ---  — cascade `Style..X..Y` dans le bloc PROPRIETES d'un menu.
     *     La racine se lit sur la prop `Style` de la fiche de catalogue de la
     *     sorte declaree, comme pour un champ : sans fiche, aucun segment ne
     *     resout et le seul element qui bascule est l'index d'etat, ce qui
     *     produit la forme MIXTE `Etats[hover]`. --- */
    static const char* MNU_INLINE =
        "FEN_M est une Fen\xc3\xaatre\n"
        "\tPROPRIETES\n"
        "\t\tTitre         = \"t\"\n"
        "\t\tMenuPrincipal = MNU_X\n"
        "\tFIN\n"
        "\n"
        "\tMNU_X est un MenuPrincipal\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..Barre..Etats[survol]..CouleurFond = RVB(1, 2, 3)\n"
        "\t\t\tStyle..SousMenu..Couleur"
        "S\xc3\xa9parateur       = RVB(4, 5, 6)\n"
        "\t\tFIN\n"
        "\n"
        "\t\tSM_F est un SousMenu\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibell\xc3\xa9 = \"F\"\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    static const char* MNU_INLINE_BLOCK =
        "STY est un styleMenuPrincipal = {\n"
        "\tBarre = { Etats = { Survol = { CouleurFond = RVB(1, 2, 3) } } }\n"
        "\tSousMenu = { Couleur" "S\xc3\xa9parateur = RVB(4, 5, 6) }\n"
        "}\n";
    check_inline_matches_block(MNU_INLINE, MNU_INLINE_BLOCK, OWL_TRANSLATE_EN,
                               "cascade en ligne menu == forme bloc");
    check_translate_contains(MNU_INLINE, OWL_TRANSLATE_EN,
                             "Style..Bar..States[hover]..BackgroundColor",
                             "cascade menu barre FR->EN resolue de bout en bout");
    check_translate_contains(MNU_INLINE, OWL_TRANSLATE_EN,
                             "Style..Submenu..SeparatorColor",
                             "cascade menu sous-menu FR->EN resolue");
    /* Aucune forme MIXTE : c'est exactement ce que rendait le defaut (seul
     * l'index d'etat basculait, les segments restaient en francais). */
    check_translate_absent(MNU_INLINE, OWL_TRANSLATE_EN, "Etats[hover]",
                           "pas de forme mixte Etats[hover] sous un menu");
    check_translate_absent(MNU_INLINE, OWL_TRANSLATE_EN, "Barre",
                           "plus de segment FR Barre sous un menu");
    check_translate_absent(MNU_INLINE, OWL_TRANSLATE_EN, "CouleurFond",
                           "plus de segment FR CouleurFond sous un menu");
    en_compiles(MNU_INLINE, "cascade en ligne menu EN compile");
    check_round_trip_identity(MNU_INLINE, "cascade en ligne menu RT");

    /* --- Sens inverse EN -> FR sur la meme cascade, menu contextuel. --- */
    static const char* MNU_CTX_EN =
        "FEN_C is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle       = \"t\"\n"
        "\t\tContextMenu = MNU_C\n"
        "\tEND\n"
        "\n"
        "\tMNU_C is a ContextMenu\n"
        "\t\tPROPERTIES\n"
        "\t\t\tStyle..MenuOption..States[hover]..BackgroundColor = RGB(1, 2, 3)\n"
        "\t\tEND\n"
        "\n"
        "\t\tOPT_C is a MenuOption\n"
        "\t\t\tPROPERTIES\n"
        "\t\t\t\tCaption = \"C\"\n"
        "\t\t\tEND\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    check_translate_contains(MNU_CTX_EN, OWL_TRANSLATE_FR,
                             "Style..OptionMenu..Etats[survol]..CouleurFond",
                             "cascade menu contextuel EN->FR resolue de bout en bout");
    check_translate_absent(MNU_CTX_EN, OWL_TRANSLATE_FR, "States[survol]",
                           "pas de forme mixte States[survol] sous un menu");
    check_translate_absent(MNU_CTX_EN, OWL_TRANSLATE_FR, "BackgroundColor",
                           "plus de segment EN BackgroundColor sous un menu");
    /* L'article suit le genre de la sorte dans les deux sens : `is a
     * ContextMenu` <-> `est un MenuContextuel`, jamais `est a` ni `is un`. */
    check_translate_contains(MNU_CTX_EN, OWL_TRANSLATE_FR, "est un MenuContextuel",
                             "article FR du MenuContextuel");
    check_translate_contains(MNU_INLINE, OWL_TRANSLATE_EN, "is a MainMenu",
                             "article EN du MenuPrincipal");
    check_rt_lang(MNU_CTX_EN, OWL_TRANSLATE_EN,
                  "cascade en ligne menu contextuel RT");

    /* --- Hors bloc PROPRIETES, la sorte du controle est inconnue du
     *     formatter : la cascade reste verbatim plutot que d'etre resolue au
     *     hasard contre un type arbitraire (le couple Caption/Label ci-dessus
     *     montre ce que couterait une resolution a plat). --- */
    static const char* HORS_BLOC =
        "LIB_X..Style..Libell\xc3\xa9..Couleur = RVB(1, 2, 3)\n";
    check_translate_contains(HORS_BLOC, OWL_TRANSLATE_EN, "Style..Libell\xc3\xa9..",
                             "cascade hors PROPRIETES : verbatim, pas de devinette");
}

/*  — methodes fluent (apres `.`) + borne de plage À/TO en translate.
 *
 * Gap corrige : le chemin methode-apres-point canonisait la casse/accents mais
 * ne basculait pas vers la forme partenaire de la langue cible ; la borne de
 * plage `À`/`TO` restait figee (bare `a` -> `À` quelle que soit la cible).
 * On s'appuie sur le pairing bilingue du registry natives (Ajoute<->Add) : une
 * methode fluent qui EST une native bilingue bascule ; une methode user sans
 * partenaire natif reste intacte (pas de sur-traduction). */
static void test_fluent_methods_and_range(void) {
    /* --- Methode fluent bilingue : FR -> EN (tab.Ajoute -> tab.Add) --- */
    static const char* FR_M =
        "PROCEDURE P()\n"
        "\ttab est un tableau d'entiers\n"
        "\ttab.Ajoute(3)\n"
        "FIN\n";
    check_translate_contains(FR_M, OWL_TRANSLATE_EN, ".Add(", "fluent Ajoute -> Add");
    check_translate_absent(FR_M, OWL_TRANSLATE_EN, "Ajoute", "plus de Ajoute en EN");
    en_compiles(FR_M, "fluent method EN compile");
    check_round_trip_identity(FR_M, "fluent method RT");

    /* --- Methode fluent bilingue : EN -> FR (symetrie tab.Add -> tab.Ajoute) --- */
    static const char* EN_M =
        "PROCEDURE P()\n"
        "\ttab is an array of integer\n"
        "\ttab.Add(3)\n"
        "END\n";
    check_translate_contains(EN_M, OWL_TRANSLATE_FR, ".Ajoute(", "fluent Add -> Ajoute");
    check_translate_absent(EN_M, OWL_TRANSLATE_FR, "Add", "plus de Add en FR");

    /* --- Borne de plage : POUR i = 1 À 64 -> FOR i = 1 TO 64 --- */
    static const char* FR_R =
        "POUR i = 1 \xc3\x80 64\n"
        "\tTrace(i)\n"
        "FIN\n";
    check_translate_contains(FR_R, OWL_TRANSLATE_EN, "FOR ", "POUR -> FOR");
    check_translate_contains(FR_R, OWL_TRANSLATE_EN, " TO ", "borne À -> TO");
    check_translate_absent(FR_R, OWL_TRANSLATE_EN, "\xc3\x80", "plus de À en EN");
    en_compiles(FR_R, "range EN compile");
    check_round_trip_identity(FR_R, "range RT");

    /* --- Borne de plage : EN -> FR (FOR i = 1 TO 64 -> POUR i = 1 À 64) --- */
    static const char* EN_R =
        "FOR i = 1 TO 64\n"
        "\tTrace(i)\n"
        "END\n";
    check_translate_contains(EN_R, OWL_TRANSLATE_FR, "\xc3\x80", "borne TO -> À");
    check_translate_absent(EN_R, OWL_TRANSLATE_FR, " TO ", "plus de TO en FR");

    /* --- Non-regression : methode user (pas une native bilingue) INCHANGEE --- */
    static const char* FR_U =
        "PROCEDURE P(o est un cObjet)\n"
        "\to.MaMethode(3)\n"
        "FIN\n";
    check_translate_contains(FR_U, OWL_TRANSLATE_EN, "MaMethode", "methode user intacte EN");
    static const char* EN_U =
        "PROCEDURE P(o is a cObjet)\n"
        "\to.MaMethode(3)\n"
        "END\n";
    check_translate_contains(EN_U, OWL_TRANSLATE_FR, "MaMethode", "methode user intacte FR");
}

/*  : PAS est surcharge (meme token que STEP et NOT). En negation
 * (PAS <expr>), canon EN = NOT ; en pas de boucle (POUR ... À <fin> PAS <n>),
 * canon EN = STEP. La cible FR ramene les deux a PAS. */
static void test_negation_vs_step(void) {
    /* --- Negation simple : SI PAS x ALORS -> IF NOT x THEN --- */
    static const char* FR_NEG =
        "b est un bool\xc3\xa9""en = Vrai\n"
        "SI PAS b ALORS\n"
        "\tTrace(\"non\")\n"
        "FIN\n";
    check_translate_contains(FR_NEG, OWL_TRANSLATE_EN, "NOT b", "negation PAS -> NOT");
    check_translate_absent(FR_NEG, OWL_TRANSLATE_EN, "STEP", "negation : pas de STEP parasite");
    en_compiles(FR_NEG, "negation EN compile");
    check_round_trip_identity(FR_NEG, "negation RT");

    /* --- Negations chainees : SI PAS p OU PAS q ALORS -> IF NOT p OR NOT q THEN
     * (p/q et non a/b : `a`/`to` sont le token reserve de borne de plage). --- */
    static const char* FR_CHAIN =
        "p est un bool\xc3\xa9""en = Vrai\n"
        "q est un bool\xc3\xa9""en = Faux\n"
        "SI PAS p OU PAS q ALORS\n"
        "\tTrace(\"x\")\n"
        "FIN\n";
    check_translate_contains(FR_CHAIN, OWL_TRANSLATE_EN, "NOT p", "negation chainee NOT p");
    check_translate_contains(FR_CHAIN, OWL_TRANSLATE_EN, "NOT q", "negation chainee NOT q");
    check_translate_absent(FR_CHAIN, OWL_TRANSLATE_EN, "STEP", "chainee : pas de STEP");
    en_compiles(FR_CHAIN, "negation chainee EN compile");
    check_round_trip_identity(FR_CHAIN, "negation chainee RT");

    /* --- Pas de boucle : POUR i = 1 À 10 PAS 2 -> ... STEP 2 (inchange) --- */
    static const char* FR_STEP =
        "POUR i = 1 \xc3\x80 10 PAS 2\n"
        "\tTrace(i)\n"
        "FIN\n";
    check_translate_contains(FR_STEP, OWL_TRANSLATE_EN, "STEP 2", "pas de boucle -> STEP");
    check_translate_absent(FR_STEP, OWL_TRANSLATE_EN, "NOT", "pas de boucle : pas de NOT");
    en_compiles(FR_STEP, "pas de boucle EN compile");
    check_round_trip_identity(FR_STEP, "pas de boucle RT");

    /* ---  : negation composee PAS DANS -> NOT IN (jamais STEP IN) --- */
    static const char* FR_PASDANS =
        "SI 3 PAS DANS (1, 2, 4) ALORS\n"
        "\tTrace(\"absent\")\n"
        "FIN\n";
    check_translate_contains(FR_PASDANS, OWL_TRANSLATE_EN, "NOT IN", "PAS DANS -> NOT IN");
    check_translate_absent(FR_PASDANS, OWL_TRANSLATE_EN, "STEP", "PAS DANS : jamais STEP");
    en_compiles(FR_PASDANS, "PAS DANS EN compile");
    check_round_trip_identity(FR_PASDANS, "PAS DANS RT");

    /* --- Symetrie EN -> FR : negation IF NOT x THEN -> SI PAS x ALORS --- */
    static const char* EN_NEG =
        "b is a boolean = True\n"
        "IF NOT b THEN\n"
        "\tTrace(\"non\")\n"
        "END\n";
    check_translate_contains(EN_NEG, OWL_TRANSLATE_FR, "PAS b", "negation NOT -> PAS");
    check_translate_absent(EN_NEG, OWL_TRANSLATE_FR, "NOT", "plus de NOT en FR");
    check_translate_absent(EN_NEG, OWL_TRANSLATE_FR, "STEP", "negation EN->FR : pas de STEP");

    /* --- Symetrie EN -> FR : pas de boucle STEP -> PAS --- */
    static const char* EN_STEP =
        "FOR i = 1 TO 10 STEP 2\n"
        "\tTrace(i)\n"
        "END\n";
    check_translate_contains(EN_STEP, OWL_TRANSLATE_FR, "PAS 2", "STEP -> PAS");
    check_translate_absent(EN_STEP, OWL_TRANSLATE_FR, "STEP", "plus de STEP en FR");
}

/*  : les noms d'events (position `PROCEDURE <event>()`) suivent la
 * langue cible. surClic <-> onClick, FinInitialisation <-> EndInitialization,
 * surBoutonGauche <-> onLeftButtonDown. Une procedure USER homonyme d'aucun
 * event reste intacte dans les deux sens (pas de sur-traduction). */
static void test_event_names(void) {
    /* --- Event widget : FR -> EN (surClic -> onClick) --- */
    static const char* FR_CLIC =
        "PROCEDURE surClic()\n"
        "\tTrace(\"clic\")\n"
        "FIN\n";
    check_translate_contains(FR_CLIC, OWL_TRANSLATE_EN, "PROCEDURE onClick(",
                             "event surClic -> onClick");
    check_translate_absent(FR_CLIC, OWL_TRANSLATE_EN, "surClic",
                           "plus de surClic en EN");
    en_compiles(FR_CLIC, "event onClick EN compile");
    check_round_trip_identity(FR_CLIC, "event surClic RT");

    /* --- Event widget : EN -> FR (onClick -> surClic) --- */
    static const char* EN_CLIC =
        "PROCEDURE onClick()\n"
        "\tTrace(\"clic\")\n"
        "END\n";
    check_translate_contains(EN_CLIC, OWL_TRANSLATE_FR, "PROCEDURE surClic(",
                             "event onClick -> surClic");
    check_translate_absent(EN_CLIC, OWL_TRANSLATE_FR, "onClick",
                           "plus de onClick en FR");

    /* --- Lifecycle fenetre : FinInitialisation <-> EndInitialization --- */
    static const char* FR_INIT =
        "PROCEDURE FinInitialisation()\n"
        "\tTrace(\"init\")\n"
        "FIN\n";
    check_translate_contains(FR_INIT, OWL_TRANSLATE_EN, "PROCEDURE EndInitialization(",
                             "event FinInitialisation -> EndInitialization");
    check_translate_absent(FR_INIT, OWL_TRANSLATE_EN, "FinInitialisation",
                           "plus de FinInitialisation en EN");
    check_round_trip_identity(FR_INIT, "event FinInitialisation RT");

    static const char* EN_INIT =
        "PROCEDURE EndInitialization()\n"
        "\tTrace(\"init\")\n"
        "END\n";
    check_translate_contains(EN_INIT, OWL_TRANSLATE_FR, "PROCEDURE FinInitialisation(",
                             "event EndInitialization -> FinInitialisation");

    /* --- Event souris a nom compose : surBoutonGauche <-> onLeftButtonDown --- */
    static const char* FR_BTN =
        "PROCEDURE surBoutonGauche()\n"
        "\tTrace(\"btn\")\n"
        "FIN\n";
    check_translate_contains(FR_BTN, OWL_TRANSLATE_EN, "PROCEDURE onLeftButtonDown(",
                             "event surBoutonGauche -> onLeftButtonDown");
    check_translate_absent(FR_BTN, OWL_TRANSLATE_EN, "surBoutonGauche",
                           "plus de surBoutonGauche en EN");
    check_round_trip_identity(FR_BTN, "event surBoutonGauche RT");

    /* --- Non-regression : procedure USER (pas un event) INCHANGEE --- */
    static const char* FR_USER =
        "PROCEDURE CalculeScore()\n"
        "\tTrace(\"score\")\n"
        "FIN\n";
    check_translate_contains(FR_USER, OWL_TRANSLATE_EN, "PROCEDURE CalculeScore(",
                             "procedure user intacte EN");
    static const char* EN_USER =
        "PROCEDURE CalculeScore()\n"
        "\tTrace(\"score\")\n"
        "END\n";
    check_translate_contains(EN_USER, OWL_TRANSLATE_FR, "PROCEDURE CalculeScore(",
                             "procedure user intacte FR");
}

/* ================================================================
 *  (support #219) — audit systematique equivalence FR/EN :
 * trous bilingues au niveau declaration/DSL (Classe/Structure/
 * Combinaison/Énumération/MenuPrincipal/Etat).
 * ================================================================ */

/* F1+F3 : translate --to en n'applique pas des formes EN pourtant
 * reconnues par le compilateur (verifie : Report/Body/DocumentHeader/
 * PageHeader/PageFooter/DocumentFooter/Enumeration compilent tous deja).
 * Aujourd'hui translate laisse Etat/Corps/EnTeteDocument/HautDePage/
 * BasDePage/FinDeDocument/Énumération inchanges en EN — trou de table
 * translate (fmt_keywords[]), pas un chantier canon. */
static void test_feat1303_f1_f3_dsl_block_keywords_en(void) {
    /* Énumération -> Enumeration. */
    static const char* ENUM_FR =
        "x est une \xc3\x89num\xc3\xa9ration\n"
        "\tAlpha = 1\n"
        "\tBeta  = 2\n"
        "FIN\n";
    check_translate_contains(ENUM_FR, OWL_TRANSLATE_EN, "Enumeration",
                             "1303 F1/F3 \xc3\x89num\xc3\xa9ration -> Enumeration");
    check_translate_absent(ENUM_FR, OWL_TRANSLATE_EN, "\xc3\x89num\xc3\xa9ration",
                           "1303 F1/F3 plus d'\xc3\x89num\xc3\xa9ration en EN");

    /* Bloc Etat + sous-blocs : Report/Body/DocumentHeader/PageHeader/
     * PageFooter/DocumentFooter. */
    /*  : les noms de bloc suivent la convention deja portee par
     * l'entree `corps` (initiale minuscule). Un nom d'identifiant appartient a
     * l'utilisateur et traverse le formateur intact : nommer un bloc
     * exactement comme le mot-cle sonde ferait prendre le NOM pour le TYPE aux
     * check_translate_absent ci-dessous. */
    static const char* ETAT_FR =
        "R est un Etat\n"
        "\tenTeteDocument est un EnTeteDocument\n"
        "\tFIN\n"
        "\thautDePage est un HautDePage\n"
        "\tFIN\n"
        "\tcorps est un Corps\n"
        "\tFIN\n"
        "\tbasDePage est un BasDePage\n"
        "\tFIN\n"
        "\tfinDeDocument est un FinDeDocument\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "Report",
                             "1303 F1 Etat -> Report");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "Etat",
                           "1303 F1 plus de Etat en EN");
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "Body",
                             "1303 F1 Corps -> Body");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "Corps",
                           "1303 F1 plus de Corps en EN");
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "DocumentHeader",
                             "1303 F1 EnTeteDocument -> DocumentHeader");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "EnTeteDocument",
                           "1303 F1 plus de EnTeteDocument en EN");
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "PageHeader",
                             "1303 F1 HautDePage -> PageHeader");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "HautDePage",
                           "1303 F1 plus de HautDePage en EN");
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "PageFooter",
                             "1303 F1 BasDePage -> PageFooter");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "BasDePage",
                           "1303 F1 plus de BasDePage en EN");
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "DocumentFooter",
                             "1303 F1 FinDeDocument -> DocumentFooter");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "FinDeDocument",
                           "1303 F1 plus de FinDeDocument en EN");

    /* Temoin (coordinator) : les formes EN cibles compilent DEJA — pas un
     * chantier canon a inventer, juste la table translate a completer. */
    static const char* ETAT_EN =
        "R is a Report\n"
        "\tDocumentHeader is a DocumentHeader\n"
        "\tEND\n"
        "\tPageHeader is a PageHeader\n"
        "\tEND\n"
        "\tbody is a Body\n"
        "\tEND\n"
        "\tPageFooter is a PageFooter\n"
        "\tEND\n"
        "\tDocumentFooter is a DocumentFooter\n"
        "\tEND\n"
        "END\n";
    owl_compiles(ETAT_EN, "1303 F1 formes EN Report/Body/... compilent deja");
    /* Verrou , volet Etat : vm.etats fuit de la meme facon que
     * vm.windows. Deuxieme check de la meme source dans le meme process. */
    owl_compiles(ETAT_EN, "1303 F1 formes EN Report (2e check du meme process)");
}

/* F2 : article un/une jamais restaure EN->FR pour les mots-cles de
 * declaration/DSL.  (genre) ne couvre que primitifs + widgets de
 * base -> round-trip casse sur classes/structs/enums/combinaisons/menus/
 * etats. Aujourd'hui `is a Class` revient `est a Classe` (article `a` non
 * traduit) au lieu de `une Classe`. */
static void test_feat1303_f2_article_declarations_dsl(void) {
    check_round_trip_identity("x est une Classe\nFIN\n",
                              "1303 F2 RT une Classe");
    check_round_trip_identity("x est une Structure\nFIN\n",
                              "1303 F2 RT une Structure");
    check_round_trip_identity(
        "x est une Combinaison\n\tAlpha\n\tBeta\nFIN\n",
        "1303 F2 RT une Combinaison");
    check_round_trip_identity(
        "x est une \xc3\x89num\xc3\xa9ration\n\tAlpha = 1\n\tBeta = 2\nFIN\n",
        "1303 F2 RT une \xc3\x89num\xc3\xa9ration");
    check_round_trip_identity("MNU est un MenuPrincipal\nFIN\n",
                              "1303 F2 RT un MenuPrincipal");
    check_round_trip_identity(
        "R est un Etat\n\tc est un Corps\n\tFIN\nFIN\n",
        "1303 F2 RT un Etat");
}

/* F5 : MargeHaut/RetraitHaut (et Bas/Gauche/Droite<->Droit) collisionnent
 * sur le meme nameEN TopMargin/BottomMargin/LeftMargin/RightMargin
 * (src/ui_catalog.c). Round-trip d'un bloc PROPRIETES d'Etat avec les
 * marges Marge* doit rester stable — PAS de mutation silencieuse vers
 * Retrait*. Ce test ne tranche PAS le canon Marge/Retrait, decision
 * coordinator (alpha break net : un seul canon FR, l'autre supprime, ou
 * collision EN a lever). */
static void test_feat1303_f5_etat_marge_retrait_stability(void) {
    /* F5: canon Marge/Retrait a trancher par coordinator */
    static const char* FR =
        "R est un Etat\n"
        "\tPROPRIETES\n"
        "\t\tMargeHaut   = 10\n"
        "\t\tMargeBas    = 20\n"
        "\t\tMargeGauche = 30\n"
        "\t\tMargeDroite = 40\n"
        "\tFIN\n"
        "\tc est un Corps\n"
        "\tFIN\n"
        "FIN\n";
    check_round_trip_identity(FR,
        "1303 F5 RT marges Etat (pas de mutation vers Retrait*)");
}

/* F4 (audit ) : INVESTIGUÉ — PAS un bug. Le repro initial (round-trip
 * `.actif`->`.Actif`) venait d'un fichier traduit HORS de son projet : sans le
 * prescan projet, l'API context-free ne connait pas la casse canon des membres
 * d'enum et pascalise par defaut. Le vrai `owl translate` (CLI, avec prescan)
 * preserve `.actif` sur fichier autonome ET in-project. Aucun test F4 conserve
 * (les helpers context-free de ce fichier ne peuvent pas exercer le prescan ;
 * un verrou honnete devrait passer par le chemin fichier). Nuance mineure hors
 * scope : le handler MCP owl_translate (chaine sans fichier) pascalise encore —
 * candidat ticket separe si besoin. */

/* ================================================================
 *  — canon FR accentue de la famille Etat : État /
 * EnTêteDocument / Itération.
 *
 * Ce que ces verrous protegent : la forme EN de sortie (Report /
 * DocumentHeader / Iteration) ET la forme FR de sortie, qui doit desormais
 * porter l'accent dans les DEUX sens. Avant , `Report -> fr`
 * rendait `Etat` et `DocumentHeader -> fr` rendait `EnTeteDocument` ;
 * `Iteration` n'etait ni canonise ni traduit (absent de fmt_keywords[]) et
 * son article restait fige a `an` au retour EN->FR.
 *
 * La graphie ASCII reste acceptee EN ENTREE (invariant 2) : c'est elle qui
 * absorbe la migration du code existant.
 * ================================================================ */
static void test_fix1349_canon_accentue_famille_etat(void) {
    /* --- Etat / État <-> Report --- */
    static const char* ETAT_FR =
        "R est un \xc3\x89tat\n"
        "\tc est un Corps\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(ETAT_FR, OWL_TRANSLATE_EN, "Report",
                             "1349 \xc3\x89tat -> Report");
    check_translate_absent(ETAT_FR, OWL_TRANSLATE_EN, "\xc3\x89tat",
                           "1349 plus d'\xc3\x89tat en EN");
    static const char* ETAT_EN =
        "R is a Report\n"
        "\tc is a Body\n"
        "\tEND\n"
        "END\n";
    check_translate_contains(ETAT_EN, OWL_TRANSLATE_FR, "\xc3\x89tat",
                             "1349 Report -> \xc3\x89tat (accentue)");
    check_translate_absent(ETAT_EN, OWL_TRANSLATE_FR, "un Etat",
                           "1349 Report -> fr ne rend plus 'Etat' sans accent");
    check_round_trip_identity(ETAT_FR, "1349 RT \xc3\x89tat");
    /* Graphie ASCII toleree en entree : meme sortie EN. */
    check_translate_contains("R est un Etat\n\tc est un Corps\n\tFIN\nFIN\n",
                             OWL_TRANSLATE_EN, "Report",
                             "1349 Etat (ASCII, entree toleree) -> Report");

    /* --- EnTeteDocument / EnTêteDocument <-> DocumentHeader --- */
    /* Nom d'Etat distinct de celui des autres cas : owl_compiles enchaine des
     * --check dans le MEME process (pas de cycle VM), et deux Etats homonymes
     * y collisionnent. */
    static const char* ENTETE_FR =
        "R1349 est un \xc3\x89tat\n"
        "\tintro est un EnT\xc3\xaateDocument\n"
        "\tFIN\n"
        "\tc est un Corps\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(ENTETE_FR, OWL_TRANSLATE_EN, "DocumentHeader",
                             "1349 EnT\xc3\xaateDocument -> DocumentHeader");
    check_translate_absent(ENTETE_FR, OWL_TRANSLATE_EN, "EnT\xc3\xaateDocument",
                           "1349 plus d'EnT\xc3\xaateDocument en EN");
    static const char* ENTETE_EN =
        "R1349 is a Report\n"
        "\tintro is a DocumentHeader\n"
        "\tEND\n"
        "\tc is a Body\n"
        "\tEND\n"
        "END\n";
    check_translate_contains(ENTETE_EN, OWL_TRANSLATE_FR, "EnT\xc3\xaateDocument",
                             "1349 DocumentHeader -> EnT\xc3\xaateDocument (accentue)");
    check_translate_absent(ENTETE_EN, OWL_TRANSLATE_FR, "un EnTeteDocument",
                           "1349 DocumentHeader -> fr ne rend plus 'EnTeteDocument'");
    check_round_trip_identity(ENTETE_FR, "1349 RT EnT\xc3\xaateDocument");
    check_translate_contains(
        "R est un Etat\n\tintro est un EnTeteDocument\n\tFIN\n"
        "\tc est un Corps\n\tFIN\nFIN\n",
        OWL_TRANSLATE_EN, "DocumentHeader",
        "1349 EnTeteDocument (ASCII, entree toleree) -> DocumentHeader");

    /* --- Iteration / Itération : forme EN identique au FR sans accent.
     * Le bloc est feminin (« une itération ») : l'article doit revenir a
     * `une` au retour EN->FR, pas rester fige sur `an`. */
    static const char* ITER_FR =
        "R est un \xc3\x89tat\n"
        "\tsep est une It\xc3\xa9ration\n"
        "\tFIN\n"
        "\tc est un Corps\n"
        "\tFIN\n"
        "FIN\n";
    check_translate_contains(ITER_FR, OWL_TRANSLATE_EN, "is an Iteration",
                             "1349 It\xc3\xa9ration -> an Iteration");
    check_translate_absent(ITER_FR, OWL_TRANSLATE_EN, "It\xc3\xa9ration",
                           "1349 plus d'It\xc3\xa9ration en EN");
    static const char* ITER_EN =
        "R is a Report\n"
        "\tsep is an Iteration\n"
        "\tEND\n"
        "\tc is a Body\n"
        "\tEND\n"
        "END\n";
    check_translate_contains(ITER_EN, OWL_TRANSLATE_FR, "une It\xc3\xa9ration",
                             "1349 an Iteration -> une It\xc3\xa9ration (article + accent)");
    check_translate_absent(ITER_EN, OWL_TRANSLATE_FR, "an Iteration",
                           "1349 an Iteration -> fr ne laisse plus l'article EN");
    check_round_trip_identity(ITER_FR, "1349 RT It\xc3\xa9ration");
    check_translate_contains(
        "R est un Etat\n\tsep est une Iteration\n\tFIN\n"
        "\tc est un Corps\n\tFIN\nFIN\n",
        OWL_TRANSLATE_EN, "is an Iteration",
        "1349 Iteration (ASCII, entree toleree) -> an Iteration");

    /* Les formes EN produites compilent (temoin : le canon FR accentue ne
     * casse pas la sortie EN acceptee par le compilo). */
    owl_compiles(ENTETE_EN, "1349 formes EN Report/DocumentHeader compilent");
}

/*  — TOUT le registre de types composes bascule en traduction.
 *
 * Verrou MECANIQUE : la boucle itere owl_registry_type_at(), la source meme que
 * lit le formateur. Un type ajoute au registre APRES ce ticket entre donc dans
 * le controle sans edit ici — c'est ce qui ferme la classe au lieu de corriger
 * ses instances. Toute paire FR/EN non emise, et tout genre oublie, rougit.
 *
 * Mesure d'ouverture : 81 types, 68 paires FR/EN distinctes, 0 bascule ;
 * 8 types sans genre -> l'article EN->FR degenerait en `est a`.
 *
 * Quatre controles par type :
 *   a) genre renseigne — sans lui l'article EN->FR reste `a` et la phrase est
 *      mixte (`est a Volet`, la demi-mesure de ).
 *   b) FR->EN : la forme EN est emise ET la forme FR effacee (si distincte).
 *   c) EN->FR : la forme FR revient, et l'article avec (aucun `est a `).
 *   d) round-trip identite au canon dans les DEUX sens.
 *
 * Les types a nom FR identique au nom EN (Style, Timer, Image, MockNative…) ne
 * subissent pas (b) cote effacement : il n'y a rien a effacer. */
static void test_fix1430_registry_types_sweep(void) {
    int nt = owl_registry_types_count();
    CHECK(nt > 0, "registre de types non vide");

    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (t == NULL || t->canonical == NULL) continue;
        const char* fr = t->canonical;
        const char* en = t->nameEN;

        char label[192];
        snprintf(label, sizeof label, "type registre %s", fr);

        /* (a) genre du type : porte par le registre, consomme par l'article. */
        CHECK(t->genre != OWL_GENRE_NONE, label);

        /* Declaration FR minimale, article pris du genre du registre. */
        char src[256];
        snprintf(src, sizeof src, "v est %s %s\n",
                 (t->genre == OWL_GENRE_F) ? "une" : "un", fr);

        /* (b) FR -> EN. */
        if (en != NULL) {
            check_translate_contains(src, OWL_TRANSLATE_EN, en, label);
            /* Effacement de la forme FR — sauf si le nom FR est un SOUS-MOT du
             * nom EN (xmlAttribut ⊂ xmlAttribute, styleCombo ⊂ styleCombobox) :
             * le needle serait alors trivialement present et l'assertion
             * mensongere. Sur ces deux types, la discrimination repose sur le
             * `contains` de la forme EN et sur le round-trip identite. */
            if (strcmp(en, fr) != 0 && strstr(en, fr) == NULL)
                check_translate_absent(src, OWL_TRANSLATE_EN, fr, label);
        }
        check_translate_absent(src, OWL_TRANSLATE_EN, "is un", label);

        /* (d) round-trip FR. */
        check_round_trip_identity(src, label);

        /* (c) EN -> FR, depuis l'EN reellement produit (pas un EN devine :
         *     l'article a/an est choisi par le formateur). */
        char* enSrc = owl_translate(src, OWL_TRANSLATE_EN);
        CHECK(enSrc != NULL, label);
        if (enSrc != NULL) {
            check_translate_contains(enSrc, OWL_TRANSLATE_FR, fr, label);
            check_translate_absent(enSrc, OWL_TRANSLATE_FR, "est a ", label);
            check_rt_lang(enSrc, OWL_TRANSLATE_EN, label);
            free(enSrc);
        }
    }
}

/*  — les trois cas nommes au ticket, avec compile-check des deux
 * langues. Le troisieme (`CodeBarres` -> `BarCodes`) est le plus parlant : la
 * paire existait au registre et la forme EN compilait deja. */
static void test_fix1430_declarations_nommees(void) {
    static const char* FR_HTTP = "r est une httpRequ\xc3\xaate\n";
    check_translate_contains(FR_HTTP, OWL_TRANSLATE_EN, "r is a httpRequest",
                             "httpRequête -> httpRequest");
    check_translate_absent(FR_HTTP, OWL_TRANSLATE_EN, "httpRequ\xc3\xaate",
                           "httpRequête efface en EN");
    check_translate_contains("r is a httpRequest\n", OWL_TRANSLATE_FR,
                             "r est une httpRequ\xc3\xaate",
                             "httpRequest -> httpRequête");
    /* Le double type-vs-fonction (invariant 5) : en EN->FR le nom EN du TYPE ne
     * doit pas etre capte par la NATIVE homonyme. Defaut mesure a l'ouverture :
     * `is a httpRequest` rendait `est une HTTPRequête`, la fonction. */
    check_translate_absent("r is a httpRequest\n", OWL_TRANSLATE_FR,
                           "HTTPRequ\xc3\xaate",
                           "type httpRequête non capte par la native HTTPRequête");
    check_round_trip_identity(FR_HTTP, "RT httpRequête");
    en_compiles(FR_HTTP, "httpRequest compile");

    static const char* FR_CB = "cb est un CodeBarres\n";
    check_translate_contains(FR_CB, OWL_TRANSLATE_EN, "cb is a BarCodes",
                             "CodeBarres -> BarCodes");
    check_translate_absent(FR_CB, OWL_TRANSLATE_EN, "CodeBarres",
                           "CodeBarres efface en EN");
    check_translate_contains("cb is a BarCodes\n", OWL_TRANSLATE_FR,
                             "cb est un CodeBarres", "BarCodes -> CodeBarres");
    check_round_trip_identity(FR_CB, "RT CodeBarres");
    en_compiles(FR_CB, "BarCodes compile");

    static const char* FR_ML = "s est une cha\xc3\xaeneMultilingue\n";
    check_translate_contains(FR_ML, OWL_TRANSLATE_EN, "s is a multilingualString",
                             "chaîneMultilingue -> multilingualString");
    check_translate_absent(FR_ML, OWL_TRANSLATE_EN, "cha\xc3\xaeneMultilingue",
                           "chaîneMultilingue efface en EN");
    check_translate_contains("s is a multilingualString\n", OWL_TRANSLATE_FR,
                             "s est une cha\xc3\xaeneMultilingue",
                             "multilingualString -> chaîneMultilingue");
    check_round_trip_identity(FR_ML, "RT chaîneMultilingue");
    en_compiles(FR_ML, "multilingualString compile");

    /* Aucune forme mixte dans un sens comme dans l'autre. */
    check_translate_absent(FR_CB, OWL_TRANSLATE_EN, "is un",
                           "pas de `is un` sur CodeBarres");
    check_translate_absent("cb is a BarCodes\n", OWL_TRANSLATE_FR, "est a ",
                           "pas de `est a` sur BarCodes");
}

/*  — non-regression du double type-vs-fonction (invariant 5). La
 * resolution par le registre de types est POSITIONNELLE : elle ne gagne qu'apres
 * un connecteur de type (est/un/une/is/a/an) et hors position d'appel. Un nom de
 * type homonyme d'une native (`Couleur`, `Image`, `Champ`) doit rester la native
 * partout ailleurs — c'est exactement le piege d'homonyme que  a paye
 * avec la constante TAB, une echelle en dessous. */
static void test_fix1430_homonymes_natives(void) {
    /* Le double le plus net du registre : type `httpRequête` (EN httpRequest)
     * et native `HTTPRequête` (EN HTTPRequest). Les deux normalisent vers
     * `httprequest`. Dans la MEME ligne, la position de type doit rendre le
     * TYPE et la position d'appel la NATIVE. */
    static const char* FR_CALL =
        "r est une httpRequ\xc3\xaate\n"
        "j est un JSON = HTTPRequ\xc3\xaate(\"http://x\")\n";
    check_translate_contains(FR_CALL, OWL_TRANSLATE_EN, "r is a httpRequest",
                             "position de type -> le TYPE");
    check_translate_contains(FR_CALL, OWL_TRANSLATE_EN, "HTTPRequest(\"http://x\")",
                             "position d'appel -> la NATIVE");
    check_round_trip_identity(FR_CALL, "RT type httpRequête + appel HTTPRequête");
    en_compiles(FR_CALL, "type + appel homonymes compilent en EN");

    /* Le prefixe widget garde sa reecriture contextuelle : `Champ` est un type
     * du registre (nameEN Control), et la sorte qui suit reste resolue par
     * CONTROL_KINDS. Les deux chemins doivent cohabiter. */
    check_translate_contains("BTN_A est un Champ Bouton\n", OWL_TRANSLATE_EN,
                             "is a Control Button", "Champ Bouton -> Control Button");
    check_round_trip_identity("BTN_A est un Champ Bouton\n", "RT Champ Bouton");

    /* Un identifiant utilisateur homonyme d'un type, hors position de type,
     * n'est pas touche (il n'appartient pas au canon). */
    check_translate_contains("police est un entier = 3\n", OWL_TRANSLATE_EN,
                             "police is an integer", "identifiant police intact");
    check_round_trip_identity("police est un entier = 3\n", "RT identifiant police");
}

/* ================================================================
 *  — declaration multiple : `x, y sont des entiers`
 *
 * Forme EN canon : `u, v, w are integers` — PAS d'article, type au PLURIEL.
 * Elle n'est pas inventee ici : c'est la forme deja portee par
 * tests/regression/test_declaration_multi.owl et par la table de mots-cles du
 * scanner (`integers`, `reals`, `strings`, `booleans` y sont enregistres au
 * meme titre que leur singulier).
 *
 * L'article `des` n'a pas d'equivalent anglais : il tombe vers EN et se
 * reconstruit vers FR depuis la copule `sont`, qui, elle, traverse toujours.
 * C'est la copule qui porte l'information de pluriel, pas le `s` du type —
 * indispensable pour `monétaire`, `numérique`, `tableau` et les types
 * utilisateur, dont le pluriel EN n'existe pas dans le langage.
 * ================================================================ */

/* RT strict : translate(translate(src, autre), src_lang) == src A L'OCTET
 * PRES. Plus dur que check_rt_lang (qui compare a fmt(src)) : c'est le critere
 * du ticket — l'aller-retour doit rendre le fichier d'origine. */
static void check_rt_bytes(const char* src, OwlTranslateLang srcLang,
                           const char* label) {
    OwlTranslateLang other = (srcLang == OWL_TRANSLATE_FR)
                             ? OWL_TRANSLATE_EN : OWL_TRANSLATE_FR;
    char* rt = round_trip(src, other, srcLang);
    CHECK(rt != NULL, label);
    if (rt) {
        bool eq = strcmp(src, rt) == 0;
        CHECK(eq, label);
        if (!eq) fprintf(stderr, "    [%s] RT != src\n      src : %s\n      rt  : %s\n",
                         label, src, rt);
    }
    free(rt);
}

/* fmt(src) == attendu, octet a octet (canonicalisation hors traduction). */
static void check_fmt_equals(const char* src, const char* expected,
                             const char* label) {
    char* got = owl_format(src);
    CHECK(got != NULL, label);
    if (got) {
        bool eq = strcmp(got, expected) == 0;
        CHECK(eq, label);
        if (!eq) fprintf(stderr, "    [%s] fmt inattendu\n      attendu : %s\n      obtenu  : %s\n",
                         label, expected, got);
    }
    free(got);
}

static void test_fix1536_declaration_multiple(void) {
    /* --- Le cas rapporte : pluriel + article, avec initialiseur. --- */
    static const char* FR_ENT = "x, y sont des entiers = 3\n";
    check_translate_contains(FR_ENT, OWL_TRANSLATE_EN, "x, y are integers = 3",
                             "FR->EN entiers : article tombe, type au pluriel");
    check_translate_absent(FR_ENT, OWL_TRANSLATE_EN, "des",
                           "aucun article francais ne survit en EN");
    check_rt_bytes(FR_ENT, OWL_TRANSLATE_FR, "RT octets entiers + init");
    en_compiles(FR_ENT, "EN entiers compile");

    /* --- Sans initialiseur. --- */
    static const char* FR_CHN = "s1, s2, s3 sont des cha\xc3\xaenes\n";
    check_translate_contains(FR_CHN, OWL_TRANSLATE_EN, "s1, s2, s3 are strings",
                             "FR->EN chaînes sans init");
    check_rt_bytes(FR_CHN, OWL_TRANSLATE_FR, "RT octets chaînes sans init");
    en_compiles(FR_CHN, "EN chaînes compile");

    /* --- Les deux autres primitifs dont le pluriel EN existe. --- */
    static const char* FR_REE = "r1, r2 sont des r\xc3\xa9""els = 1.5\n";
    check_translate_contains(FR_REE, OWL_TRANSLATE_EN, "r1, r2 are reals = 1.5",
                             "FR->EN réels");
    check_rt_bytes(FR_REE, OWL_TRANSLATE_FR, "RT octets réels");
    en_compiles(FR_REE, "EN réels compile");

    static const char* FR_BOO = "b1, b2 sont des bool\xc3\xa9""ens = Vrai\n";
    check_translate_contains(FR_BOO, OWL_TRANSLATE_EN, "b1, b2 are booleans",
                             "FR->EN booléens");
    check_rt_bytes(FR_BOO, OWL_TRANSLATE_FR, "RT octets booléens");
    en_compiles(FR_BOO, "EN booléens compile");

    /* --- Primitifs SANS pluriel anglais dans le langage (`currencies` et
     * `numerics` ne sont pas des mots du langage) : l'EN reste au singulier,
     * et le retour FR retrouve quand meme le pluriel par la copule. C'est ce
     * qui interdit de faire porter le pluriel au seul `s` du type. --- */
    static const char* FR_MON = "m1, m2 sont des mon\xc3\xa9taires = 1.5\n";
    check_translate_contains(FR_MON, OWL_TRANSLATE_EN, "m1, m2 are currency = 1.5",
                             "FR->EN monétaires (pas de pluriel EN)");
    check_rt_bytes(FR_MON, OWL_TRANSLATE_FR, "RT octets monétaires");
    en_compiles(FR_MON, "EN monétaires compile");

    static const char* FR_NUM = "n1, n2 sont des num\xc3\xa9riques = 2\n";
    check_translate_contains(FR_NUM, OWL_TRANSLATE_EN, "n1, n2 are numeric = 2",
                             "FR->EN numériques (pas de pluriel EN)");
    check_rt_bytes(FR_NUM, OWL_TRANSLATE_FR, "RT octets numériques");
    en_compiles(FR_NUM, "EN numériques compile");

    /* --- Type COMPOSE : le pluriel porte sur le contenant (`tableaux`), le
     * type d'element garde la regle de  (pluriel) ; le connecteur
     * s'elide devant voyelle a la reconstruction EN->FR (`d'entiers`). --- */
    static const char* FR_TAB = "t1, t2 sont des tableaux d'entiers\n";
    check_translate_contains(FR_TAB, OWL_TRANSLATE_EN, "t1, t2 are array of integer",
                             "FR->EN tableaux de entiers");
    check_rt_bytes(FR_TAB, OWL_TRANSLATE_FR, "RT octets tableaux de entiers");
    en_compiles(FR_TAB, "EN tableau compile");

    /* --- Type UTILISATEUR : aucun pluriel (la structure s'appelle Point),
     * seul l'article bouge. --- */
    static const char* FR_USR =
        "Point est une Structure\n"
        "\tz est un entier\n"
        "FIN\n"
        "p, q sont des Point\n";
    check_translate_contains(FR_USR, OWL_TRANSLATE_EN, "p, q are Point",
                             "FR->EN type utilisateur : article tombe seul");
    check_rt_bytes(FR_USR, OWL_TRANSLATE_FR, "RT octets type utilisateur");
    en_compiles(FR_USR, "EN type utilisateur compile");

    /* --- Symetrie : source EN canon, aller-retour EN->FR->EN. --- */
    static const char* EN_INT = "u, v, w are integers\n";
    check_translate_contains(EN_INT, OWL_TRANSLATE_FR, "u, v, w sont des entiers",
                             "EN->FR : article reconstruit, type au pluriel");
    check_rt_bytes(EN_INT, OWL_TRANSLATE_EN, "RT octets source EN");

    static const char* EN_CUR = "m1, m2 are currency = 1.5\n";
    check_translate_contains(EN_CUR, OWL_TRANSLATE_FR, "m1, m2 sont des mon\xc3\xa9taires",
                             "EN->FR monétaires depuis le singulier EN");
    check_rt_bytes(EN_CUR, OWL_TRANSLATE_EN, "RT octets source EN monétaire");

    /* --- Formes TOLEREES par le compilateur, ramenees au canon par `owl fmt`.
     * Sans cette collecte, l'aller-retour ne pourrait pas etre une identite :
     * deux graphies FR se projettent sur la meme phrase anglaise. --- */
    check_fmt_equals("x, y sont des entier = 3\n", "x, y sont des entiers = 3\n",
                     "fmt : singulier tolere -> pluriel canon");
    check_fmt_equals("x, y sont entiers = 3\n", "x, y sont des entiers = 3\n",
                     "fmt : article absent -> article canon");
    check_fmt_equals("u, v are integer\n", "u, v are integers\n",
                     "fmt : singulier EN tolere -> pluriel canon EN");
    check_round_trip_identity("x, y sont des entier = 3\n",
                              "RT au canon depuis le singulier tolere");
    check_round_trip_identity("x, y sont entiers = 3\n",
                              "RT au canon depuis l'article absent");

    /* --- Non-regression : la declaration SIMPLE ne bouge pas (l'article
     * singulier reste traduit, pas efface). --- */
    check_translate_contains("solo est un entier = 10\n", OWL_TRANSLATE_EN,
                             "solo is an integer = 10", "décl simple intacte");
    check_rt_bytes("solo est un entier = 10\n", OWL_TRANSLATE_FR,
                   "RT octets décl simple");
}

/*  — proprietes double-point des TYPES COMPOSES du registre
 * (OptionsEntraînement/TrainingOptions, IAModèle/AIModel).  couvrait
 * les props widget/natives (..Valeur<->..Value) mais pas les props d'instance
 * des types composes, qui vivent dans OwlTypeDef.fields. La traduction laissait
 * `opts..Époques` en FR au milieu d'un code EN. Contrat : bascule bilingue des
 * DEUX cotes, round-trip identite, methode + prop sur le meme receveur, et
 * non-regression des props deja couvertes (..Valeur). */
static void test_fix1648_composed_type_props(void) {
    /* --- Les 5 props d'OptionsEntraînement, FR -> EN. --- */
    static const char* FR =
        "opts est un OptionsEntra\xc3\xaenement\n"
        "opts..\xc3\x89poques           = 30\n"
        "opts..TailleLot         = 64\n"
        "opts..TauxApprentissage = 0.5\n"
        "opts..Optimiseur        = optimSGD\n"
        "opts..Perte             = perteEntropieCrois\xc3\xa9""e\n";
    check_translate_contains(FR, OWL_TRANSLATE_EN, "..Epochs",
                             "1648 Époques -> Epochs");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "..BatchSize",
                             "1648 TailleLot -> BatchSize");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "..LearningRate",
                             "1648 TauxApprentissage -> LearningRate");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "..Optimizer",
                             "1648 Optimiseur -> Optimizer");
    check_translate_contains(FR, OWL_TRANSLATE_EN, "..Loss",
                             "1648 Perte -> Loss");
    /* Volet negatif : aucune forme FR de prop ne survit en EN. */
    check_translate_absent(FR, OWL_TRANSLATE_EN, "\xc3\x89poques",
                           "1648 plus de Époques en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "..TailleLot",
                           "1648 plus de TailleLot en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "..TauxApprentissage",
                           "1648 plus de TauxApprentissage en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "..Optimiseur",
                           "1648 plus de Optimiseur en EN");
    check_translate_absent(FR, OWL_TRANSLATE_EN, "..Perte",
                           "1648 plus de Perte en EN");
    en_compiles(FR, "1648 OptionsEntraînement EN compile");
    check_round_trip_identity(FR, "1648 OptionsEntraînement RT");

    /* --- Symetrie EN -> FR sur la forme EN reellement produite. --- */
    char* enSrc = owl_translate(FR, OWL_TRANSLATE_EN);
    CHECK(enSrc != NULL, "1648 EN source non NULL");
    if (enSrc != NULL) {
        check_translate_contains(enSrc, OWL_TRANSLATE_FR, "..\xc3\x89poques",
                                 "1648 Epochs -> Époques");
        check_translate_contains(enSrc, OWL_TRANSLATE_FR, "..TailleLot",
                                 "1648 BatchSize -> TailleLot");
        check_translate_contains(enSrc, OWL_TRANSLATE_FR, "..TauxApprentissage",
                                 "1648 LearningRate -> TauxApprentissage");
        check_translate_contains(enSrc, OWL_TRANSLATE_FR, "..Optimiseur",
                                 "1648 Optimizer -> Optimiseur");
        check_translate_contains(enSrc, OWL_TRANSLATE_FR, "..Perte",
                                 "1648 Loss -> Perte");
        check_translate_absent(enSrc, OWL_TRANSLATE_FR, "..Epochs",
                               "1648 plus de Epochs en FR");
        check_translate_absent(enSrc, OWL_TRANSLATE_FR, "..LearningRate",
                               "1648 plus de LearningRate en FR");
        free(enSrc);
    }

    /* --- Cas mixte : methode fluente single-dot + prop composee double-point
     * coexistent dans le meme fichier, toutes deux traduites. IAModèle porte
     * des methodes (single-dot) mais pas de bloc `fields` (cf. stdlib_iamodele :
     * declarer des fields y casserait la validation des methodes) ; le mixte est
     * donc porte par une methode fluente de tableau + les props d'options. --- */
    static const char* FR_MIX =
        "m est un IAMod\xc3\xa8le\n"
        "m.AjouteCoucheDense(4, 8)\n"
        "opts est un OptionsEntra\xc3\xaenement\n"
        "opts..\xc3\x89poques = 10\n";
    check_translate_contains(FR_MIX, OWL_TRANSLATE_EN, ".AddDenseLayer(",
                             "1648 mixte : methode AjouteCoucheDense -> AddDenseLayer");
    check_translate_contains(FR_MIX, OWL_TRANSLATE_EN, "..Epochs",
                             "1648 mixte : prop Époques -> Epochs");
    check_translate_absent(FR_MIX, OWL_TRANSLATE_EN, "..\xc3\x89poques",
                           "1648 mixte : plus de Époques en EN");
    en_compiles(FR_MIX, "1648 mixte EN compile");
    check_round_trip_identity(FR_MIX, "1648 mixte RT");

    /* --- Non-regression  : prop builtin ..Valeur<->..Value. --- */
    static const char* FR_VAL =
        "c est un Chrono\n"
        "x est un entier = c..Valeur\n";
    check_translate_contains(FR_VAL, OWL_TRANSLATE_EN, "..Value",
                             "1648 non-reg : Valeur -> Value");
    check_translate_contains("c is a Chrono\nx is an integer = c..Value\n",
                             OWL_TRANSLATE_FR, "..Valeur",
                             "1648 non-reg : Value -> Valeur");
}

/* Invalide : owl_translate(NONE) doit retourner NULL (garde-fou). */
static void test_invalid_target_null(void) {
    char* out = owl_translate("SI x ALORS\nFIN\n", OWL_TRANSLATE_NONE);
    CHECK(out == NULL, "translate NONE -> NULL");
    free(out);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int translate_self_test_run(void) {
    fprintf(stderr, "Running translate self-tests...\n");
    test_keywords_control();
    test_keywords_loop_logic();
    test_types_primitives();
    test_composite_array_word_order();
    test_booleans();
    test_natives();
    test_passthrough();
    test_neutral_tokens();
    test_visibility_bilingual();
    test_visibility_inline_bilingual();
    test_constants_bilingual();
    test_eol_legacy_rewritten();
    test_dsl_window();
    test_native_delete_roundtrip();
    test_dsl_widget_window();
    test_mix();
    test_en_corpus_symmetry();
    test_article_genre();
    test_compiler_gender_agnostic();
    test_dsl_widget_prefix_control();
    test_fix1418_control_kinds_sweep();
    test_fix1430_registry_types_sweep();
    test_fix1430_declarations_nommees();
    test_fix1430_homonymes_natives();
    test_dsl_theme_style();
    test_dsl_theme_style_etats();
    test_fix1415_cascade_style_inline();
    test_fluent_methods_and_range();
    test_negation_vs_step();
    test_event_names();
    test_feat1303_f1_f3_dsl_block_keywords_en();
    test_feat1303_f2_article_declarations_dsl();
    test_feat1303_f5_etat_marge_retrait_stability();
    test_fix1349_canon_accentue_famille_etat();
    test_fix1536_declaration_multiple();
    test_fix1648_composed_type_props();
    test_invalid_target_null();
    test_stdout_eol_crlf();
    test_stdout_eol_lf();

    /* Nettoyage de fin de famille : le dossier temp porte le PID, donc aucune
     * execution suivante ne le reutilisera — sans ce retrait, chacune laisserait
     * un dossier de plus derriere elle. */
    owl_rmdir(tr_tmp_dir());

    if (g_fail > 0) {
        fprintf(stderr, "  %d/%d tests FAILED\n", g_fail, g_pass + g_fail);
    } else {
        fprintf(stderr, "  All %d tests PASSED\n", g_pass);
    }
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE : 0 cas execute. Un controle qui "
                        "n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
