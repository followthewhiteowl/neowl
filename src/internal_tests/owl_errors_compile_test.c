// SPDX-License-Identifier: AGPL-3.0-only
/* owl_errors_compile_test.c — Tests pour les helpers compile-aware (Lot 2).
 *
 * Execute via `owl --self-test-errors` (cf main.c). Couvre :
 *   - owl_emit_error_at / owl_emit_warning_at : linkent sans crash
 *   - Stub phase : tests DOIVENT rougir car l'impl est no-op
 *   - Implementer jalon B remplit → tests deviennent verts
 *
 * STUB PHASE (jalon A test-first) : ces tests compilent/linkent mais les
 * assertions fortes sont en attente d'impl. L'implementer les active en
 * jalon B quand les helpers produisent du stderr real.
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "owl_errors.h"
#include "scanner.h"  /* Pour Token */
#include "compiler.h" /* CompileDiagnostic + capture buffer + introspection */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Capture des diagnostics emis : on detourne stderr vers un buffer
 * CompileDiagnostic via compile_set_error_capture (idem warning). Permet
 * d'assert sur message/line/column sans toucher stderr reel. */
#define DIAG_CAP 16
static CompileDiagnostic g_diag_buf[DIAG_CAP];
static int g_diag_count = 0;
static CompileDiagnostic g_warn_buf[DIAG_CAP];
static int g_warn_count = 0;

static void capture_begin(void) {
    g_diag_count = 0;
    g_warn_count = 0;
    memset(g_diag_buf, 0, sizeof(g_diag_buf));
    memset(g_warn_buf, 0, sizeof(g_warn_buf));
    compile_set_error_capture(g_diag_buf, DIAG_CAP, &g_diag_count);
    compile_set_warning_capture(g_warn_buf, DIAG_CAP, &g_warn_count);
    compile_reset_parser_error_state();
}

static void capture_end(void) {
    compile_set_error_capture(NULL, 0, NULL);
    compile_set_warning_capture(NULL, 0, NULL);
    compile_reset_parser_error_state();
}

/* ================================================================
 * Tests linkage stubs (jalon A : verifie que ca compile/linke)
 * ================================================================ */

static void test_emit_error_at_links_without_crash(void) {
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "foo", .length = 3,
                  .line = 42, .column = 10 };
    owl_emit_error_at(&tok, E_LEX_UNTERMINATED_STRING, NULL);
    CHECK(1, "emit_error_at_links_without_crash");
    capture_end();
}

static void test_emit_warning_at_links_without_crash(void) {
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "bar", .length = 3,
                  .line = 99, .column = 5 };
    owl_emit_warning_at(&tok, E_LEX_UNEXPECTED_CHAR, NULL);
    CHECK(1, "emit_warning_at_links_without_crash");
    capture_end();
}

static void test_emit_error_current_links_without_crash(void) {
    /* Sous capture pour ne pas polluer stderr reel (parser.current a 0/0
     * en mode self-test => message synthetique). */
    capture_begin();
    owl_emit_error_current(E_SYNTAX_EXPECTED_EXPR, NULL);
    CHECK(1, "emit_error_current_links_without_crash");
    capture_end();
}

/* ================================================================
 * Tests awaiting impl (jalon B : implementer active ces assertions)
 *
 * Ces tests sont SKIP en stub phase. L'implementer les remplace par
 * des assertions fortes une fois l'impl complete (capture stderr,
 * check format texte/JSON, verify hadError flag, etc.).
 * ================================================================ */

static void test_emit_error_at_writes_to_stderr_STUB(void) {
    /* En mode capture, on ne touche pas stderr reel mais le buffer
     * recoit le rendu (line/column/message). Verifie le format de base :
     * line/column propages, message = template rendu sans placeholders. */
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "foo", .length = 3,
                  .line = 42, .column = 10 };
    owl_emit_error_at(&tok, E_LEX_UNTERMINATED_STRING, NULL);
    CHECK(g_diag_count == 1, "emit_error_at_writes_to_buffer.count");
    CHECK(g_diag_buf[0].line == 42, "emit_error_at_writes_to_buffer.line");
    CHECK(g_diag_buf[0].column == 10, "emit_error_at_writes_to_buffer.column");
    CHECK(strstr(g_diag_buf[0].message, "Cha") != NULL,
          "emit_error_at_writes_to_buffer.message_has_chaine");
    capture_end();
}

static void test_emit_error_at_marks_had_error_STUB(void) {
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "foo", .length = 3,
                  .line = 1, .column = 1 };
    CHECK(compile_peek_had_error() == 0, "before_emit.hadError_false");
    owl_emit_error_at(&tok, E_LEX_UNTERMINATED_STRING, NULL);
    CHECK(compile_peek_had_error() == 1, "after_emit.hadError_true");
    capture_end();
}

static void test_emit_warning_at_does_not_mark_error_STUB(void) {
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "bar", .length = 3,
                  .line = 7, .column = 2 };
    owl_emit_warning_at(&tok, E_LEX_UNEXPECTED_CHAR, NULL);
    CHECK(compile_peek_had_error() == 0,
          "warning_does_not_mark_error.hadError_false");
    CHECK(g_warn_count == 1, "warning_does_not_mark_error.warn_buffered");
    CHECK(g_diag_count == 0,
          "warning_does_not_mark_error.no_error_buffered");
    capture_end();
}

static void test_emit_error_at_args_substitution_STUB(void) {
    /* E_LEX_UNEXPECTED_CHAR_QUOTED contient {char}, on passe "@" et on
     * verifie que la substitution a eu lieu (pas de placeholder litteral
     * "{char}" dans le rendu). */
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "@", .length = 1,
                  .line = 3, .column = 5 };
    owl_emit_error_at(&tok, E_LEX_UNEXPECTED_CHAR_QUOTED, "char", "@", NULL);
    CHECK(g_diag_count == 1, "args_substitution.count");
    CHECK(strstr(g_diag_buf[0].message, "{char}") == NULL,
          "args_substitution.placeholder_replaced");
    CHECK(strstr(g_diag_buf[0].message, "'@'") != NULL,
          "args_substitution.value_present");
    capture_end();
}

static void test_emit_error_at_json_mode_STUB(void) {
    /* En mode capture, la sortie reste structuree (CompileDiagnostic) :
     * elle court-circuite le mode JSON/texte (cf. compile_emit_diagnostic_at
     * passe par diagBuffer en priorite). Le format JSON stderr est teste
     * indirectement par les snapshots tests/compile-error expected.json
     * via la suite owl test. Ici on verifie au moins que le toggle ne
     * crash pas et que le buffer recoit toujours le rendu. */
    compile_set_json_errors(true);
    capture_begin();
    Token tok = { .type = TOKEN_IDENTIFIER, .start = "x", .length = 1,
                  .line = 12, .column = 3 };
    owl_emit_error_at(&tok, E_LEX_UNEXPECTED_CHAR_QUOTED, "char", "x", NULL);
    CHECK(g_diag_count == 1, "json_mode.buffer_still_filled");
    CHECK(g_diag_buf[0].line == 12, "json_mode.line_propagated");
    capture_end();
    compile_set_json_errors(false);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int owl_errors_compile_run_tests(void) {
    fprintf(stderr, "\n==== OWL ERRORS COMPILE-AWARE TESTS ====\n");

    /* Linkage stubs (jalon A) — toujours executes, garde-fou ABI. */
    test_emit_error_at_links_without_crash();
    test_emit_warning_at_links_without_crash();
    test_emit_error_current_links_without_crash();

    /* Tests fonctionnels (jalon B) — actives par implementer. */
    test_emit_error_at_writes_to_stderr_STUB();
    test_emit_error_at_marks_had_error_STUB();
    test_emit_warning_at_does_not_mark_error_STUB();
    test_emit_error_at_args_substitution_STUB();
    test_emit_error_at_json_mode_STUB();

    fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
            g_pass, g_fail);

    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE — 0 assertion exercee. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}

int owl_errors_compile_case_count(void) { return g_pass + g_fail; }
