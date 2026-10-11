// SPDX-License-Identifier: AGPL-3.0-only
/* owl_errors_runtime_test.c — Tests pour les helpers runtime-aware (Lot 3).
 *
 * Execute via `owl --self-test-errors` (cf main.c). Couvre :
 *   - owl_emit_runtime_error : linke + emet sur stderr via le transport
 *     vm.c runtime_error() (format texte ou JSON selon g_json_error_mode)
 *   - Substitution {placeholders} du template FR
 *   - Mode JSON : sortie structurée single-line
 *   - Comportement longjmp/exception handler : couvert par tests
 *     integration runtime/exception_handling (TODO setup unitaire trop
 *     invasif, cf. note in test_emit_runtime_error_longjmp_behavior)
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "owl_errors.h"
#include "vm.h"
#include "fixture_unique.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  define DUP   _dup
#  define DUP2  _dup2
#  define CLOSE _close
#  define FILENO _fileno
#else
#  include <unistd.h>
#  define DUP   dup
#  define DUP2  dup2
#  define CLOSE close
#  define FILENO fileno
#endif

/* ================================================================
 * Infra minimale
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { \
        g_fail++; \
        fprintf(stderr, "  FAIL [%s] at line %d: " #expr "\n", (label), __LINE__); \
    } \
} while (0)

#define SKIP(label) do { \
    fprintf(stderr, "  SKIP [%s] : stub phase, awaiting implementer\n", (label)); \
} while (0)

/* ================================================================
 * Capture stderr via freopen.
 *
 * runtime_error() écrit directement sur stderr (cf vm.c) — pas
 * d'équivalent compile_set_error_capture côté runtime. On redirige
 * stderr vers un fichier temporaire le temps de l'émission, puis on
 * relit le fichier pour assertion.
 *
 * Note : en contexte test, vm.frameCount == 0 donc pas de stack trace —
 * juste "Erreur d'execution : <msg>\n" ou JSON single-line.
 * ================================================================ */

#define CAPTURE_BUF_SIZE 4096

static char g_capture_buf[CAPTURE_BUF_SIZE];
static int  g_saved_stderr_fd = -1;
static FILE* g_capture_file = NULL;

/* Chemin de capture propre au processus : a nom constant, deux
 * executions lancees du meme dossier se partagent ce fichier — l'une le
 * tronque ("w+") pendant que l'autre le relit, et le message rendu arrive
 * vide. Les assertions accusent alors le rendu de l'erreur, qui est intact. */
static const char* capture_path(void) {
    static char p[128];
    if (p[0] == '\0') {
        fixture_unique(p, sizeof p, "owl_errors_runtime_capture_", ".tmp");
    }
    return p;
}

/* Capture stderr via dup2 sur file descriptor : preserve la possibilite
 * de restaurer correctement stderr a la fin (freopen("CONOUT$") echoue
 * quand le process est lance avec stderr deja redirige par le shell). */
static void capture_stderr_begin(void) {
    fflush(stderr);
    g_saved_stderr_fd = DUP(FILENO(stderr));
    g_capture_file = fopen(capture_path(), "w+");
    if (g_capture_file && g_saved_stderr_fd >= 0) {
        DUP2(FILENO(g_capture_file), FILENO(stderr));
    }
}

static const char* capture_stderr_end(void) {
    fflush(stderr);
    /* Restaure stderr original via dup2. */
    if (g_saved_stderr_fd >= 0) {
        DUP2(g_saved_stderr_fd, FILENO(stderr));
        CLOSE(g_saved_stderr_fd);
        g_saved_stderr_fd = -1;
    }
    /* Slurp le fichier capture dans g_capture_buf. */
    g_capture_buf[0] = '\0';
    if (g_capture_file) {
        fclose(g_capture_file);
        g_capture_file = NULL;
        FILE* f = fopen(capture_path(), "r");
        if (f) {
            size_t n = fread(g_capture_buf, 1, CAPTURE_BUF_SIZE - 1, f);
            g_capture_buf[n] = '\0';
            fclose(f);
        }
        remove(capture_path());
    }
    return g_capture_buf;
}

/* ================================================================
 * Tests linkage stubs (jalon A : verifie que ca compile/linke)
 * ================================================================ */

static void test_emit_runtime_error_links_without_crash(void) {
    /* Appel minimal sous capture pour ne pas polluer stderr reel. */
    capture_stderr_begin();
    owl_emit_runtime_error(E_RT_DIV_BY_ZERO, NULL);
    capture_stderr_end();
    CHECK(1, "emit_runtime_error_links_without_crash");
}

/* ================================================================
 * Tests fonctionnels (jalon B)
 * ================================================================ */

static void test_emit_runtime_error_writes_to_stderr(void) {
    /* Mode texte (g_json_error_mode = false par defaut) : on attend
     * le header "Erreur d'execution :" + rendu FR du template. Pas de
     * stack trace en contexte self-test (vm.frameCount == 0). */
    vm_set_json_errors(false);
    capture_stderr_begin();
    owl_emit_runtime_error(E_RT_DIV_BY_ZERO, NULL);
    const char* out = capture_stderr_end();
    CHECK(strstr(out, "Erreur d'execution") != NULL,
          "writes_to_stderr.header_present");
    CHECK(strstr(out, "Division par z") != NULL,
          "writes_to_stderr.message_rendered");
}

static void test_emit_runtime_error_args_substitution(void) {
    /* E_RT_BAD_ARITY a {expected} et {got}. On verifie que la
     * substitution a eu lieu : pas de placeholder litteral, valeurs
     * presentes dans le rendu. */
    vm_set_json_errors(false);
    capture_stderr_begin();
    owl_emit_runtime_error(E_RT_BAD_ARITY,
                           "expected", "3",
                           "got", "5",
                           NULL);
    const char* out = capture_stderr_end();
    CHECK(strstr(out, "{expected}") == NULL,
          "args_substitution.expected_placeholder_replaced");
    CHECK(strstr(out, "{got}") == NULL,
          "args_substitution.got_placeholder_replaced");
    CHECK(strstr(out, "3") != NULL,
          "args_substitution.expected_value_present");
    CHECK(strstr(out, "5") != NULL,
          "args_substitution.got_value_present");
}

static void test_emit_runtime_error_json_mode(void) {
    /* Mode JSON : sortie single-line {"kind":"runtime","message":"...",...}.
     * Verifie au moins kind/message presents. Pas de line/file/stack
     * (vm.frameCount == 0 en self-test). */
    vm_set_json_errors(true);
    capture_stderr_begin();
    owl_emit_runtime_error(E_RT_DIV_BY_ZERO, NULL);
    const char* out = capture_stderr_end();
    vm_set_json_errors(false);
    CHECK(strstr(out, "\"kind\":\"runtime\"") != NULL,
          "json_mode.kind_present");
    CHECK(strstr(out, "\"message\":") != NULL,
          "json_mode.message_key_present");
    CHECK(strstr(out, "Division par z") != NULL,
          "json_mode.message_rendered");
}

static void test_coerce_wall_codes_bilingual(void) {
    /* Murs de coercion (fn_entier_coerce / fn_monetaire_coerce) : les 4
     * codes doivent rendre un message non vide FR ET EN, accentué côté FR,
     * et distinct entre les deux langues. */
    const OwlErrorCode codes[] = {
        E_RT_NUMERIC_TO_INT_OVERFLOW,
        E_RT_REAL_TO_INT_OVERFLOW,
        E_RT_REAL_TO_INT_NON_FINITE,
        E_RT_NUMERIC_TO_CURRENCY_OVERFLOW,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        char fr[256];
        snprintf(fr, sizeof fr, "%s", owl_error_render_fr(codes[i], NULL));
        char en[256];
        snprintf(en, sizeof en, "%s", owl_error_render_en(codes[i], NULL));
        CHECK(fr[0] != '\0', "coerce_wall.fr_non_empty");
        CHECK(en[0] != '\0', "coerce_wall.en_non_empty");
        CHECK(strcmp(fr, en) != 0, "coerce_wall.fr_differs_from_en");
        /* FR accentué : "impossible" suit toujours un ':' précédé d'accents
         * (numérique / réel / monétaire). On vérifie la présence d'au moins
         * un octet UTF-8 de continuation (>= 0x80) côté FR. */
        bool fr_has_accent = false;
        for (const char* p = fr; *p; p++) {
            if ((unsigned char)*p >= 0x80) { fr_has_accent = true; break; }
        }
        CHECK(fr_has_accent, "coerce_wall.fr_accented");
    }
    /* Substrings de langue : EN ne doit pas réutiliser "impossible" (FR). */
    CHECK(strstr(owl_error_render_en(E_RT_REAL_TO_INT_NON_FINITE, NULL),
                 "non-finite") != NULL,
          "coerce_wall.en_real_nonfinite_wording");
}

static void test_emit_runtime_error_longjmp_behavior(void) {
    /* TODO jalon B : test unitaire trop invasif.
     *
     * Le comportement longjmp/exception-handler dépend de exceptionCount
     * et exceptionStack, statiques internes de vm.c. Sans setup complet
     * (vm_init + push d'un handler synthetique + setjmp wrapper), on ne
     * peut pas exercer cette branche en self-test isolé.
     *
     * Comportement couvert par les tests integration :
     *   - tests/runtime/exception_handling.owl (TENTER/QUAND EXCEPTION)
     *   - tests/runtime/exception_quand_dans.owl (handlers imbriqués)
     *
     * Le pont vm_emit_runtime_error_rendered() forwarde vers le static
     * runtime_error() de vm.c qui implémente la branche
     * `if (exceptionCount > 0) { capture_exception_context(...); return; }`
     * INCHANGÉ — aucune duplication, donc aucun risque de divergence avec
     * le legacy runtime_error. */
    SKIP("emit_runtime_error_longjmp_behavior (couvert par tests integration)");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int owl_errors_runtime_run_tests(void) {
    fprintf(stderr, "\n==== OWL ERRORS RUNTIME-AWARE TESTS ====\n");

    /* Linkage stubs (jalon A) — toujours executes, garde-fou ABI. */
    test_emit_runtime_error_links_without_crash();

    /* Tests fonctionnels (jalon B). */
    test_emit_runtime_error_writes_to_stderr();
    test_emit_runtime_error_args_substitution();
    test_emit_runtime_error_json_mode();
    test_coerce_wall_codes_bilingual();
    test_emit_runtime_error_longjmp_behavior();

    fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
            g_pass, g_fail);

    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE — 0 assertion exercee. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}

int owl_errors_runtime_case_count(void) { return g_pass + g_fail; }
