// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test signature.
 *
 * Porte la categorie `signature` de run_compiler_tests.sh en in-process.
 * Pour chaque tests/compiler/signature/<name>.owl, lance le meme chemin que le
 * script (`owl --check <src>` qui scanne le dossier signature/ comme projet)
 * et compare la sortie a <name>.expected.json.
 *
 * Particularite vs compile_error_test : capture stdout ET stderr (le script
 * fait `2>&1`).
 *
 *  — la fixture est desormais copiee SEULE dans un dossier de travail
 * avant le controle, comme compile_error_test. Le controle d'un fichier couvre
 * ses voisins ( : script = fichier + voisins) : checkees sur place, les
 * fixtures de ce corpus se contaminaient mutuellement — `arity_correct.owl`
 * (valide) voyait l'erreur d'arite de `arity_too_few_args.owl`. Chaque cas doit
 * mesurer SON fichier, pas le corpus.
 *
 * Isolation VM par fixture (vm_init + compiler_reset_poo_state + vm_free) pour
 * reproduire le process-frais-par-fixture du script et eviter la fuite d'etat
 * compile-time. Contenu source casse + JSON restent en memoire C : on ne sort
 * que compteurs + noms de fixtures (securite contexte).
 *
 * Extension  (patron de non-vacuite, pose par ) : le harnais
 * doit PROUVER qu'il a examine quelque chose.
 *   - Tout `<stem>.expected.json` DOIT avoir son entree `<stem>.owl`. La
 *     boucle n'iterait que les `.owl` : un snapshot orphelin n'etait jamais
 *     apparie, donc jamais compare, et rien ne le disait.
 *   - Corpus vide ou dossier absent = ECHEC, plus un skip vert.
 *   - Le bilan affiche le nombre de cas reellement executes.
 *
 * Branche sur `owl --self-test-signature` (cf src/main.c), gate make all.
 * Retour : 0 si tout passe, sinon le nombre d'echecs. */

#include "vm.h"         /* vm_init / vm_free */
#include "compiler.h"   /* compiler_reset_poo_state */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <time.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #include <process.h>   /* _getpid */
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_open   _open
  #define owl_unlink _unlink
  #define owl_getpid _getpid
  #define OWL_MKDIR(p) _mkdir(p)
  #define OWL_RMDIR(p) _rmdir(p)
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_open   open
  #define owl_unlink unlink
  #define owl_getpid getpid
  #define OWL_MKDIR(p) mkdir((p), 0755)
  #define OWL_RMDIR(p) rmdir(p)
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

extern int handle_check_command_no_vm_lifecycle(const char* filepath);

#define SIG_DIR "tests/compiler/signature"

static int  g_pass = 0;
static int  g_fail = 0;
static int  g_cases = 0;
static char g_actual[64 * 1024];
static char g_expected[64 * 1024];

/* Dossier de travail : `build/.sig_selftest_<pid>_<ts>/`, une seule fixture a
 * la fois dedans. Prefixe pointe -> invisible des walkers du depot (meme raison
 * que compile_error_test : sinon les fixtures cassees sont decouvertes comme
 * compagnons d'une campagne `owl test`). */
static char g_workdir[1024];

/* Lit un fichier entier (binaire-safe). *outLen = octets lus. NULL si absent. */
static char* slurp_bytes(const char* path, size_t* outLen) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (outLen) *outLen = n;
    return buf;
}

static void strip_cr(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) if (*r != '\r') *w++ = *r;
    *w = '\0';
}

static void strip_trailing_nl(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Vide le dossier de travail (fichiers reguliers uniquement — il n'y a jamais
 * de sous-dossier). */
static void cleanup_workdir(void) {
    DIR* d = opendir(g_workdir);
    if (!d) return;
    char p[1024];
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;
        snprintf(p, sizeof p, "%s/%s", g_workdir, n);
        owl_unlink(p);
    }
    closedir(d);
}

/* Copie srcPath -> g_workdir/basename (binaire-safe : une fixture peut contenir
 * des octets nuls). */
static bool copy_into_workdir(const char* srcPath, const char* basename) {
    size_t len = 0;
    char* data = slurp_bytes(srcPath, &len);
    if (!data) return false;
    char dst[1024];
    snprintf(dst, sizeof dst, "%s/%s", g_workdir, basename);
    FILE* f = fopen(dst, "wb");
    if (!f) { free(data); return false; }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return true;
}

/* Lance handle_check sur le chemin donne, capture stdout+stderr (comme le
 * `2>&1` du script) dans g_actual. VM fraiche + reset POO par fixture. */
static void run_check_capture(const char* realPath) {
    g_actual[0] = '\0';
    char captPath[1024];
    snprintf(captPath, sizeof captPath, "%s/_sig_capture.out", g_workdir);

    vm_init();
    compiler_reset_poo_state();

    fflush(stdout); fflush(stderr);
    int savedOut = owl_dup(owl_fileno(stdout));
    int savedErr = owl_dup(owl_fileno(stderr));
    int fd = owl_open(captPath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (savedOut < 0 || savedErr < 0 || fd < 0) {
        if (savedOut >= 0) owl_close(savedOut);
        if (savedErr >= 0) owl_close(savedErr);
        if (fd >= 0) owl_close(fd);
        vm_free();
        return;
    }
    owl_dup2(fd, owl_fileno(stdout));
    owl_dup2(fd, owl_fileno(stderr));
    owl_close(fd);

    handle_check_command_no_vm_lifecycle(realPath);
    fflush(stdout); fflush(stderr);

    owl_dup2(savedOut, owl_fileno(stdout)); owl_close(savedOut);
    owl_dup2(savedErr, owl_fileno(stderr)); owl_close(savedErr);

    vm_free();

    size_t len = 0;
    char* cap = slurp_bytes(captPath, &len);
    if (cap) {
        size_t copy = len < sizeof(g_actual) - 1 ? len : sizeof(g_actual) - 1;
        memcpy(g_actual, cap, copy);
        g_actual[copy] = '\0';
        free(cap);
    }
    owl_unlink(captPath);
}

/* Compare g_actual a expectPath (apres normalisation). N'imprime que le nom
 * en cas d'echec — jamais le contenu (securite contexte). */
static void compare_expected(const char* label, const char* expectPath) {
    size_t len = 0;
    char* exp = slurp_bytes(expectPath, &len);
    if (!exp) {
        g_fail++;
        fprintf(stderr, "  MISS %s (pas de .expected.json)\n", label);
        return;
    }
    size_t copy = len < sizeof(g_expected) - 1 ? len : sizeof(g_expected) - 1;
    memcpy(g_expected, exp, copy);
    g_expected[copy] = '\0';
    free(exp);

    strip_cr(g_actual);    strip_trailing_nl(g_actual);
    strip_cr(g_expected);  strip_trailing_nl(g_expected);

    if (strcmp(g_actual, g_expected) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL signature/%s (sortie --check != snapshot)\n", label);
    }
}

static void process_one(const char* owlName) {
    size_t len = strlen(owlName);  /* finit par ".owl" */
    char srcPath[1024], workPath[1024], expPath[1024];
    snprintf(srcPath,  sizeof srcPath,  "%s/%s", SIG_DIR, owlName);
    snprintf(workPath, sizeof workPath, "%s/%s", g_workdir, owlName);
    snprintf(expPath,  sizeof expPath,  "%s/%.*s.expected.json",
             SIG_DIR, (int)(len - 4), owlName);

    cleanup_workdir();
    if (!copy_into_workdir(srcPath, owlName)) {
        /* Echec de copie = cas NON examine : un echec, jamais un silence. */
        g_fail++;
        fprintf(stderr, "  MISS signature/%s (copie dans le dossier de travail "
                        "impossible — cas non examine)\n", owlName);
        return;
    }
    run_check_capture(workPath);
    compare_expected(owlName, expPath);
    cleanup_workdir();
}

/* Un `<stem>.expected.json` sans `<stem>.owl` n'est jamais apparie, donc
 * jamais compare : il ment sur la couverture. */
static void check_orphan_expected(const char* name) {
    size_t stem = strlen(name) - strlen(".expected.json");
    char owlPath[1024];
    snprintf(owlPath, sizeof owlPath, "%s/%.*s.owl", SIG_DIR, (int)stem, name);
    FILE* probe = fopen(owlPath, "rb");
    if (probe) { fclose(probe); return; }
    g_fail++;
    fprintf(stderr, "  ORPHELIN %s/%s : snapshot sans entree — aucun %.*s.owl "
                    "a checker, cette fixture ne teste rien (ecrire l'entree "
                    "ou supprimer le snapshot)\n",
            SIG_DIR, name, (int)stem, name);
}

static bool has_suffix(const char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

int signature_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    g_cases = 0;

    /* Dossier de travail unique par process (le depot heberge plusieurs
     * instances en parallele : un chemin fixe se ferait tronquer sous les
     * pieds). Dans build/ et non %TMP% : ce dernier est scanne en temps reel
     * par l'antivirus (facteur ~20x sur les I/O fixture-par-fixture). */
    snprintf(g_workdir, sizeof g_workdir, "build/.sig_selftest_%d_%ld",
             (int)owl_getpid(), (long)time(NULL));
    cleanup_workdir();
    OWL_RMDIR(g_workdir);
    if (OWL_MKDIR(g_workdir) != 0) {
        fprintf(stderr, "signature: dossier de travail impossible a creer (%s)\n",
                g_workdir);
        return 1;
    }

    DIR* d = opendir(SIG_DIR);
    if (!d) {
        /* Dossier absent = ECHEC, plus un skip vert. Le harnais est
         * branche sur `make all` depuis la racine du depot. */
        fprintf(stderr, "signature: CORPUS INTROUVABLE — %s absent "
                        "(cwd = racine du depot attendue). 0 cas examine.\n", SIG_DIR);
        OWL_RMDIR(g_workdir);
        return 1;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        size_t len = strlen(n);
        /* Snapshot : pas une entree, mais il doit en avoir une. */
        if (has_suffix(n, ".expected.json")) { check_orphan_expected(n); continue; }
        if (len < 5 || strcmp(n + len - 4, ".owl") != 0) continue;
        /* defensif : skip d'eventuels *.expected.owl (signature n'en a pas). */
        if (has_suffix(n, ".expected.owl")) continue;
        g_cases++;
        process_one(n);
    }
    closedir(d);

    /* Garde ensemble vide : "0 fail" ne doit pas pouvoir signifier
     * "rien n'a ete examine". */
    if (g_cases == 0) {
        g_fail++;
        fprintf(stderr, "signature: CORPUS VIDE — aucune fixture appariee dans %s. "
                        "Un controle qui n'examine rien ne prouve rien.\n", SIG_DIR);
    }

    cleanup_workdir();
    OWL_RMDIR(g_workdir);

    fprintf(stderr, "signature: %d cas executes (%d pass, %d fail)\n",
            g_cases, g_pass, g_fail);
    return g_fail;
}
