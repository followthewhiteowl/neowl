// SPDX-License-Identifier: AGPL-3.0-only
/* owl_errors_native_test.c — Tests contrat helper owl_emit_native_error.
 *
 *  jalon B implementer : tests actifs du contrat de
 * owl_emit_native_error (API native-aware pour soft-error stdlib).
 *
 * Statut :
 *   - test 1 (linkage)                 : VERT
 *   - test 2 (stores_canonical_code)   : VERT (jalon B)
 *   - test 3 (stores_message)          : VERT (jalon B)
 *   - test 4 (args_substitution)       : VERT (jalon B)
 *   - test 5 (native_name_visible)     : VERT (jalon B)
 *   - test 6 (fresh_flag_set)          : VERT (jalon B)
 *
 * Cf.  (pattern registry erreurs) + ADR_NNNN (migration
 * ErreurInfo int -> string canonique). */

#include "../owl_errors.h"
#include "../owl_stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Accesseurs internes (impl dans stdlib_erreur.c) — lecture seule de
 * l'etat global soft-error. Pas d'usage user-facing. */
extern const char* owl_err_get_code(void);
extern const char* owl_err_get_message(void);
extern const char* owl_err_get_native_name(void);
extern bool        owl_err_get_fresh(void);
extern void        owl_clear_error_full(void);  /* impl stdlib_erreur.c */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { \
        g_fail++; \
        fprintf(stderr, "  FAIL [%s] at line %d: " #expr "\n", (label), __LINE__); \
    } \
} while (0)

/* ================================================================
 * Test 1 : linkage — compile + link + execution basique sans crash
 * ================================================================ */

static void test_emit_native_error_linkage(void) {
    /* Appel du helper avec arguments minimaux. */
    owl_emit_native_error(E_NATIVE_ARG_INVALID, "TestNative",
                          "arg", "valeur_test", NULL);
    /* Si on arrive ici sans crash = linkage OK. */
    CHECK(1, "linkage");
    owl_clear_error_full();
}

/* ================================================================
 * Test 2 : g_err_code recoit la chaine canonique du code symbole
 * ================================================================ */

static void test_emit_native_error_stores_canonical_code(void) {
    owl_clear_error_full();
    owl_emit_native_error(E_NATIVE_ARG_INVALID, "TestNative", NULL);
    const char* code = owl_err_get_code();
    CHECK(code != NULL && strcmp(code, "E_NATIVE_ARG_INVALID") == 0,
          "stores_canonical_code");
    owl_clear_error_full();
}

/* ================================================================
 * Test 3 : g_err_message recoit le template FR rendu
 * ================================================================ */

static void test_emit_native_error_stores_message(void) {
    owl_clear_error_full();
    /* Template E_NATIVE_ARG_INVALID FR = "{native}: argument invalide." */
    owl_emit_native_error(E_NATIVE_ARG_INVALID, "TestNative",
                          "native", "TestNative", NULL);
    const char* msg = owl_err_get_message();
    CHECK(msg != NULL, "stores_message_not_null");
    /* Le rendu doit contenir "TestNative" (substitution {native}) +
     * "argument invalide" (texte litteral du template FR). */
    CHECK(msg != NULL && strstr(msg, "TestNative") != NULL,
          "stores_message_native_substituted");
    CHECK(msg != NULL && strstr(msg, "argument invalide") != NULL,
          "stores_message_template_text");
    owl_clear_error_full();
}

/* ================================================================
 * Test 4 : substitution des args nommes dans le template
 * ================================================================ */

static void test_emit_native_error_args_substitution(void) {
    owl_clear_error_full();
    /* Template E_NATIVE_ARG_MISSING FR = "{native}: argument '{arg}' manquant." */
    owl_emit_native_error(E_NATIVE_ARG_MISSING, "HashChaine",
                          "native", "HashChaine",
                          "arg", "chaine",
                          NULL);
    const char* msg = owl_err_get_message();
    CHECK(msg != NULL && strstr(msg, "HashChaine") != NULL,
          "args_substitution_native");
    CHECK(msg != NULL && strstr(msg, "chaine") != NULL,
          "args_substitution_arg");
    CHECK(msg != NULL && strstr(msg, "manquant") != NULL,
          "args_substitution_template_text");
    owl_clear_error_full();
}

/* ================================================================
 * Test 5 : g_err_native_name accessible via ErreurInfo(errFonction)
 * ================================================================ */

static void test_emit_native_error_native_name_visible(void) {
    owl_clear_error_full();
    owl_emit_native_error(E_NATIVE_DIV_BY_ZERO, "Modulo",
                          "native", "Modulo", NULL);
    const char* name = owl_err_get_native_name();
    CHECK(name != NULL && strcmp(name, "Modulo") == 0,
          "native_name_visible");
    owl_clear_error_full();
}

/* ================================================================
 * Test 6 : g_err_fresh pose a true (sentinelle anti-RAZ )
 * ================================================================ */

static void test_emit_native_error_fresh_flag_set(void) {
    owl_clear_error_full();
    /* Apres clear_error_full, fresh doit etre faux. */
    CHECK(owl_err_get_fresh() == false, "fresh_flag_false_after_clear");
    owl_emit_native_error(E_NATIVE_REGEX_ERROR, "RegEx",
                          "native", "RegEx",
                          "detail", "pattern invalide",
                          NULL);
    /* Apres emit, fresh doit etre vrai (protege du RAZ pre-call suivant). */
    CHECK(owl_err_get_fresh() == true, "fresh_flag_true_after_emit");
    owl_clear_error_full();
}

/* ================================================================
 * Main runner
 * ================================================================ */

int owl_errors_native_run_tests(void) {
    g_pass = 0;
    g_fail = 0;
    fprintf(stderr, "==== OWL ERRORS NATIVE-AWARE TESTS ====\n");
    test_emit_native_error_linkage();
    test_emit_native_error_stores_canonical_code();
    test_emit_native_error_stores_message();
    test_emit_native_error_args_substitution();
    test_emit_native_error_native_name_visible();
    test_emit_native_error_fresh_flag_set();
    fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
            g_pass, g_fail);
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE — 0 assertion exercee. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}

int owl_errors_native_case_count(void) { return g_pass + g_fail; }
