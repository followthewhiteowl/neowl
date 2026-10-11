// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test meta du test runner lui-meme.
 *
 * Execute via `owl --self-test-runner` (cf main.c). Couvre :
 *   - Exit code non-zero quand >=1 scenario echoue (runtime ou assertion)
 *   - STATUS final coherent (pas "ALL CLEAR" si scenarios FAIL)
 *   - Marquage fichier pas OK si scenarios dedans echouent
 *
 * Approche : in-process, appelle run_tests_ex() sur un fixture dans
 * tests/internal_fixtures/runner_crash/, verifie le rc + stdout capturé
 * pour le STATUS.
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "testing.h"
#include "vm.h"
#include "fixture_unique.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
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
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ================================================================
 * Infra
 * ================================================================ */

static int  g_pass = 0;
static int  g_fail = 0;
static char g_buf[128 * 1024];

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Verifie que `needle` apparait dans la derniere capture stdout. */
#define CHECK_HAS(needle, label) \
    CHECK(strstr(g_buf, (needle)) != NULL, label)

/* Verifie que `needle` n'apparait PAS dans la derniere capture stdout. */
#define CHECK_NO(needle, label) \
    CHECK(strstr(g_buf, (needle)) == NULL, label)

#define CHECK_RC(actual, expected, label) \
    CHECK((actual) == (expected), label)

/* Capture stdout dans g_buf le temps d'appeler run_tests_ex().
 *
 * Approche POSIX : dup le fd 1 pour le sauver, ouvre un fichier temp,
 * dup2 sur fd 1 pour rediriger les writes, appelle, fflush, dup2
 * inverse pour restaurer, puis on lit le fichier APRES restauration.
 *
 * On evite freopen() sur stdout qui invalide le FILE* d'origine — sur
 * MSVCRT cela casse les ecritures suivantes en cascade et sur MinGW
 * c'est tout autant fragile. Le pattern dup/dup2/open avec un fichier
 * en mode binaire est portable et robuste. */
static int capture_run_tests_ex(const TestOptions* opts) {
    /* Nom propre au processus : deux executions lancees du meme
     * dossier se volaient ce fichier, l'une tronquant la capture que l'autre
     * relisait -> assertions "bilan mentionne ..." rouges sur une capture
     * vide, sans qu'une ligne du runner n'ait bouge. */
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_runner_test_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);

    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return -1;

    int tmp_fd = owl_open(TMP,
                          O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                          0644);
    if (tmp_fd < 0) {
        owl_close(saved_fd);
        return -1;
    }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd);
        owl_close(saved_fd);
        return -1;
    }
    owl_close(tmp_fd);

    int rc = run_tests_ex(opts);
    fflush(stdout);

    /* Restaure stdout AVANT de lire le fichier (sinon la lecture
     * vrille la position du fd qu'on est en train de re-binder). */
    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    /* Lit le contenu via une FILE* fraiche en lecture. */
    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_buf, 1, sizeof(g_buf) - 1, r);
        g_buf[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    return rc;
}

/* ================================================================
 * Tests
 * ================================================================ */

static void test_scenario_crash_should_fail_suite(void) {
    /* Fixture : tests/internal_fixtures/runner_crash/test_scenario_crash.owl
     * contient un scenario qui plante au runtime (ExceptionDeclenche non
     * capturee). Aujourd'hui (BUG) : le fichier est marque OK, STATUS
     * "ALL CLEAR", exit=0, alors que le bilan affiche "1 SCENARIOS (1 FAIL)".
     *
     * ATTENDU apres fix : exit non-zero, STATUS pas "ALL CLEAR",
     * fichier PAS marque OK. */

    TestOptions opts = {0};
    opts.dir = "tests/internal_fixtures/runner_crash";
    opts.verbosity = 0;  /* compact, on verifie juste le rc + STATUS */
    opts.failFast = false;
    opts.filter = NULL;
    opts.jsonOutput = false;
    opts.strict = false;
    opts.listFiles = false;

    int rc = capture_run_tests_ex(&opts);

    /* ROUGE ATTENDU aujourd'hui (BUG non fixe) : rc == 0.
     * VERT apres  impl : rc != 0. */
    CHECK(rc != 0, "scenario crash doit produire exit code non-zero");

    /* Le bilan affiche bien "1 SCENARIOS (1 FAIL)" mais le STATUS
     * dit "ALL CLEAR" (BUG). Apres fix : STATUS doit etre coherent. */
    CHECK_HAS("SCENARIOS", "bilan mentionne scenarios");
    CHECK_HAS("FAIL", "bilan mentionne FAIL");

    /* ROUGE ATTENDU aujourd'hui : "ALL CLEAR" apparait.
     * VERT apres fix : "ALL CLEAR" ne doit PAS apparaitre. */
    CHECK_NO("ALL CLEAR", "STATUS ne doit pas etre ALL CLEAR si scenarios FAIL");

    /* Le fichier est marque "OK" aujourd'hui (BUG).
     * VERT apres fix : doit etre marque FAIL ou pas OK. */
    CHECK_NO("OK   test_scenario_crash.owl", "fichier avec scenario FAIL ne doit pas etre marque OK");
}

static void test_single_file_compile_error_should_fail(void) {
    /* Fixture : tests/internal_fixtures/runner_compile_error/demo_symbole_absent.owl
     * echoue a la compilation (symbole absent) et n'est PAS prefixe `test_`.
     *
     * BUG  : lance en cible unique (`owl test <fichier>`), le gate
     * `isTest` du runner l'ecartait AVANT le match single-file -> aucun
     * fichier retenu -> "ALL CLEAR (0 suites)", exit 0. Un scenario avec une
     * faute de frappe sur un symbole passait donc pour vert.
     *
     * ATTENDU apres fix : la cible unique explicite est traitee comme un test
     * quel que soit son nom -> compile error -> exit non-zero, CRASH nomme,
     * pas de "ALL CLEAR", pas de "0 suites". */

    TestOptions opts = {0};
    opts.dir = "tests/internal_fixtures/runner_compile_error";
    opts.singleFile = "demo_symbole_absent.owl";
    opts.verbosity = 0;
    opts.failFast = false;
    opts.filter = NULL;
    opts.jsonOutput = false;
    opts.strict = false;
    opts.listFiles = false;

    int rc = capture_run_tests_ex(&opts);

    CHECK(rc != 0, "cible unique non-compilable doit produire exit non-zero");
    CHECK_NO("ALL CLEAR", "STATUS ne doit pas etre ALL CLEAR sur cible non-compilable");
    CHECK_NO("0 suites", "le fichier ne doit pas etre silencieusement ignore (0 suites)");
    CHECK_HAS("CRASH", "le fichier fautif doit apparaitre comme CRASH");
    CHECK_HAS("demo_symbole_absent", "le bilan doit nommer le fichier fautif");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int test_runner_self_test_run(void) {
    fprintf(stderr, "Running test runner meta self-tests...\n");
    test_scenario_crash_should_fail_suite();
    test_single_file_compile_error_should_fail();

    if (g_fail > 0) {
        fprintf(stderr, "  %d/%d tests FAILED\n", g_fail, g_pass + g_fail);
    } else {
        fprintf(stderr, "  All %d tests PASSED\n", g_pass);
    }
    return g_fail;
}
