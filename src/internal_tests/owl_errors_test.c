// SPDX-License-Identifier: AGPL-3.0-only
/* owl_errors_test.c — Tests unitaires pour le registry erreurs.
 *
 * Execute via `owl --self-test-errors` (cf main.c). Couvre :
 *   - Invariants registry : count/at/find/by_code retournent coherent
 *   - Rendu templates : substitution placeholders {var} → valeur
 *   - Edge cases : code inexistant, lookup case-insensitive
 *
 * Approche : assert direct sur les API exposees dans owl_errors.h.
 * **STUB PHASE** : tant que l'implementer n'a pas rempli les fonctions,
 * ces tests DOIVENT rougir. C'est le pattern test-first C idiomatique :
 *   - Build : vert (compile + link)
 *   - Tests : rouge sur assertions (valeurs dummy 0/NULL/"")
 *   - Implementer remplit → tests deviennent verts
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "owl_errors.h"

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

/* ================================================================
 * Tests invariants registry
 * ================================================================ */

static void test_registry_count_non_zero(void) {
    /* On verifie juste que le registry n'est pas vide. Le nombre exact
     * de codes seed evoluera (lot 2 ajoute ~50-80 codes compiler, etc.),
     * pas la peine d'un seuil arbitraire qui devra etre bumpe a chaque
     * lot. Test reel de coherence enum/tableau : voir self-test rule 42. */
    int n = owl_registry_error_count();
    CHECK(n > 0, "registry_count_non_zero");
}

static void test_registry_at_returns_valid(void) {
    int n = owl_registry_error_count();
    /* Registre vide = echec, pas un skip : c'est precisement le cas ou ce
     * test cesserait d'examiner quoi que ce soit (cf. ). */
    CHECK(n > 0, "registry_at_corpus_non_empty");
    for (int i = 0; i < n; i++) {
        const OwlErrorDef* e = owl_registry_error_at(i);
        CHECK(e != NULL, "registry_at_returns_valid");
        if (e) {
            CHECK(e->name != NULL && *e->name != '\0', "error_name_non_empty");
            CHECK(e->category != NULL && *e->category != '\0', "error_category_non_empty");
            CHECK(e->templateFR != NULL && *e->templateFR != '\0', "error_templateFR_non_empty");
            CHECK(e->templateEN != NULL && *e->templateEN != '\0', "error_templateEN_non_empty");
        }
    }
}

static void test_registry_find_lex_unterminated_string(void) {
    /* Lookup case-insensitive sur un code seed connu. */
    const OwlErrorDef* e1 = owl_registry_error_find("E_LEX_UNTERMINATED_STRING");
    const OwlErrorDef* e2 = owl_registry_error_find("e_lex_unterminated_string");
    const OwlErrorDef* e3 = owl_registry_error_find("E_Lex_Unterminated_String");
    CHECK(e1 != NULL, "find_lex_unterminated_string_upper");
    CHECK(e2 != NULL, "find_lex_unterminated_string_lower");
    CHECK(e3 != NULL, "find_lex_unterminated_string_mixed");
    /* Verifier coherence : les 3 doivent pointer le meme code. */
    if (e1 && e2 && e3) {
        CHECK(strcmp(e1->name, e2->name) == 0, "find_case_insensitive_consistency");
        CHECK(strcmp(e1->name, e3->name) == 0, "find_case_insensitive_consistency_mixed");
    }
}

static void test_registry_find_unknown_returns_null(void) {
    const OwlErrorDef* e = owl_registry_error_find("E_DOES_NOT_EXIST_XYZABC");
    CHECK(e == NULL, "find_unknown_returns_null");
}

static void test_registry_by_code_valid(void) {
    /* Lookup direct par code enum (E_LEX_UNTERMINATED_STRING est le 1er
     * code seed du .def, apres OWL_ERR_NONE=0). */
    const OwlErrorDef* e = owl_registry_error_by_code(E_LEX_UNTERMINATED_STRING);
    CHECK(e != NULL, "by_code_valid");
    if (e) {
        CHECK(strcmp(e->name, "E_LEX_UNTERMINATED_STRING") == 0, "by_code_name_match");
    }
}

static void test_registry_by_code_out_of_range_returns_null(void) {
    /* Code hors plage enum → NULL attendu. */
    const OwlErrorDef* e = owl_registry_error_by_code((OwlErrorCode)999);
    CHECK(e == NULL, "by_code_out_of_range_returns_null");
}

/* ================================================================
 * Tests alias numerique
 * ================================================================ */

static void test_num_first_e_code_is_e0001(void) {
    /* Le 1er code E_ du .def (E_LEX_UNTERMINATED_STRING) doit deriver E0001. */
    const char* num = owl_registry_error_num(E_LEX_UNTERMINATED_STRING);
    CHECK(num != NULL, "num_first_e_not_null");
    if (num) CHECK(strcmp(num, "E0001") == 0, "num_first_e_is_e0001");
}

static void test_num_first_w_code_is_w0001(void) {
    /* Le 1er code W_ du .def doit deriver W0001 (espace separe des E). */
    const char* num = owl_registry_error_num(W_SEM_VAR_UNUSED);
    CHECK(num != NULL, "num_first_w_not_null");
    if (num) CHECK(strcmp(num, "W0001") == 0, "num_first_w_is_w0001");
}

static void test_num_format_padded_prefix(void) {
    /* Format E0NNN : prefixe + 4 chiffres pour un code E_ quelconque. */
    const char* num = owl_registry_error_num(E_RT_DIV_BY_ZERO);
    CHECK(num != NULL, "num_div_not_null");
    if (num) {
        CHECK(num[0] == 'E', "num_div_prefix_e");
        CHECK(strlen(num) == 5, "num_div_len_5");
    }
}

static void test_by_num_roundtrip_e(void) {
    /* by_num(num(code)) doit retrouver le meme code (roundtrip). */
    const char* num = owl_registry_error_num(E_RT_DIV_BY_ZERO);
    CHECK(num != NULL, "by_num_roundtrip_num_not_null");
    if (num) {
        const OwlErrorDef* d = owl_registry_error_by_num(num);
        CHECK(d != NULL, "by_num_roundtrip_found");
        if (d) CHECK(strcmp(d->name, "E_RT_DIV_BY_ZERO") == 0,
                     "by_num_roundtrip_name_match");
    }
}

static void test_by_num_e0001_is_first_e(void) {
    const OwlErrorDef* d = owl_registry_error_by_num("E0001");
    CHECK(d != NULL, "by_num_e0001_found");
    if (d) CHECK(strcmp(d->name, "E_LEX_UNTERMINATED_STRING") == 0,
                 "by_num_e0001_name_match");
}

static void test_by_num_w0001_is_first_w(void) {
    const OwlErrorDef* d = owl_registry_error_by_num("W0001");
    CHECK(d != NULL, "by_num_w0001_found");
    if (d) CHECK(strcmp(d->name, "W_SEM_VAR_UNUSED") == 0,
                 "by_num_w0001_name_match");
}

static void test_by_num_separate_spaces(void) {
    /* E0001 et W0001 doivent pointer des codes differents (espaces separes). */
    const OwlErrorDef* e = owl_registry_error_by_num("E0001");
    const OwlErrorDef* w = owl_registry_error_by_num("W0001");
    CHECK(e != NULL && w != NULL, "by_num_both_found");
    if (e && w) CHECK(strcmp(e->name, w->name) != 0, "by_num_e_w_differ");
}

static void test_by_num_invalid_returns_null(void) {
    CHECK(owl_registry_error_by_num("E9999") == NULL, "by_num_out_of_range_null");
    CHECK(owl_registry_error_by_num("E0000") == NULL, "by_num_zero_null");
    CHECK(owl_registry_error_by_num("XYZ") == NULL, "by_num_garbage_null");
    CHECK(owl_registry_error_by_num("E_LEX_UNTERMINATED_STRING") == NULL,
          "by_num_symbolic_name_null");
    CHECK(owl_registry_error_by_num(NULL) == NULL, "by_num_null_input_null");
}

/* ================================================================
 * Tests rendu templates
 * ================================================================ */

/* Les cinq tests de rendu portaient une echappatoire d'epoque stub
 * (`|| strlen(msg) == 0`, `if (strlen(msg) > 0)`) : un rendu qui renvoie
 * la chaine vide les laissait tous verts. Mesure  : rendu FR+EN
 * neutralise a "", les cinq passaient encore. Assertions rendues fermes —
 * un rendu vide est desormais un echec. */

static void test_render_fr_no_placeholder(void) {
    /* Template sans placeholder : "Chaîne non terminée." */
    const char* msg = owl_error_render_fr(E_LEX_UNTERMINATED_STRING, NULL);
    CHECK(msg != NULL, "render_fr_no_placeholder_not_null");
    if (msg) {
        CHECK(msg[0] != '\0', "render_fr_no_placeholder_non_empty");
        CHECK(strstr(msg, "non termin") != NULL,
              "render_fr_no_placeholder_wording");
    }
}

static void test_render_en_no_placeholder(void) {
    /* Template sans placeholder : "Unterminated string." */
    const char* msg = owl_error_render_en(E_LEX_UNTERMINATED_STRING, NULL);
    CHECK(msg != NULL, "render_en_no_placeholder_not_null");
    if (msg) {
        CHECK(msg[0] != '\0', "render_en_no_placeholder_non_empty");
        CHECK(strstr(msg, "Unterminated") != NULL,
              "render_en_no_placeholder_wording");
    }
}

static void test_render_fr_with_placeholder(void) {
    /* Template avec placeholder : "Caractère inattendu : '{char}'." */
    const char* msg = owl_error_render_fr(E_LEX_UNEXPECTED_CHAR_QUOTED,
                                          "char", "@", NULL);
    CHECK(msg != NULL, "render_fr_with_placeholder_not_null");
    if (msg) {
        CHECK(msg[0] != '\0', "render_fr_with_placeholder_non_empty");
        CHECK(strstr(msg, "@") != NULL, "render_fr_placeholder_substitution");
        CHECK(strstr(msg, "{char}") == NULL, "render_fr_placeholder_consumed");
    }
}

static void test_render_en_with_placeholder(void) {
    /* Template avec placeholder : "Unexpected character: '{char}'." */
    const char* msg = owl_error_render_en(E_LEX_UNEXPECTED_CHAR_QUOTED,
                                          "char", "@", NULL);
    CHECK(msg != NULL, "render_en_with_placeholder_not_null");
    if (msg) {
        CHECK(msg[0] != '\0', "render_en_with_placeholder_non_empty");
        CHECK(strstr(msg, "@") != NULL, "render_en_placeholder_substitution");
        CHECK(strstr(msg, "{char}") == NULL, "render_en_placeholder_consumed");
    }
}

static void test_render_fr_io_with_path(void) {
    /* Template E_IO_FILE_NOT_FOUND : "Fichier introuvable : '{path}'." */
    const char* msg = owl_error_render_fr(E_IO_FILE_NOT_FOUND,
                                          "path", "/tmp/missing.txt", NULL);
    CHECK(msg != NULL, "render_fr_io_path_not_null");
    if (msg) {
        CHECK(msg[0] != '\0', "render_fr_io_path_non_empty");
        CHECK(strstr(msg, "/tmp/missing.txt") != NULL,
              "render_fr_io_path_substitution");
    }
}

/* ================================================================
 * Tests CLI --lang — diagnostics bilingues via owl --check
 *
 * Vehicule test-first pour le Front B : on lance le binaire owl.exe sur
 * une fixture qui leve E_SEM_UNKNOWN_SYMBOL, en variant le flag --lang, et
 * on assert sur la langue du message rendu dans la sortie JSON de --check.
 *
 *   - sans flag         -> message FR ("inconnue ou inaccessible")
 *   - --lang en         -> message EN ("unknown or inaccessible")
 *   - --lang fr         -> message FR (explicite)
 *
 * Subprocess (popen) plutot qu'appel in-process : --lang est parse dans
 * main.c (dispatch argv), pas dans un handler isolable. CWD = racine repo
 * (cas make all), donc build/owl.exe + le path fixture sont relatifs.
 *
 * RED-FIRST : avant impl, owl rejette --lang (aide CLI, aucun message EN)
 * -> ces tests rougissent. Apres impl -> verts. */

#ifdef _WIN32
#define OWL_POPEN  _popen
#define OWL_PCLOSE _pclose
/* cmd.exe (via _popen) interprete '/' comme separateur d'option et mal-parse
 * une commande debutant par un guillemet : on passe des chemins en
 * backslash, sans guillemets (aucun n'a d'espace). */
#define LANG_FIXTURE "tests\\internal_fixtures\\lang_flag\\unknown_var.owl"
#define ENUM_FIXTURE "tests\\internal_fixtures\\lang_flag\\enum_arg_unknown.owl"
#define ENUM_LIT_FIXTURE "tests\\internal_fixtures\\lang_flag\\enum_literal_numeric.owl"
#define OWL_EXE_REL  "build\\owl.exe"
#else
#define OWL_POPEN  popen
#define OWL_PCLOSE pclose
#define LANG_FIXTURE "tests/internal_fixtures/lang_flag/unknown_var.owl"
#define ENUM_FIXTURE "tests/internal_fixtures/lang_flag/enum_arg_unknown.owl"
#define ENUM_LIT_FIXTURE "tests/internal_fixtures/lang_flag/enum_literal_numeric.owl"
#define OWL_EXE_REL  "build/owl.exe"
#endif

/* Lance `owl <args> --check <fixture>` (args avant --check), capture stdout
 * dans out (taille cap). Retourne le nombre d'octets lus. */
static int run_owl_check_capture(const char* langArgs, char* out, int cap) {
    char cmd[1024];
    /* langArgs eventuellement vide. Ordre : owl <langArgs> --check <fixture>.
     * Pas de guillemets autour de l'exe ni du fixture : aucun n'a d'espace,
     * et cmd.exe (via _popen) mal-parse une commande qui DEBUTE par un
     * guillemet (strip des quotes capricieux -> "not recognized"). */
    snprintf(cmd, sizeof cmd, "%s %s --check %s 2>&1",
             OWL_EXE_REL, langArgs, LANG_FIXTURE);
    FILE* p = OWL_POPEN(cmd, "r");
    if (!p) { out[0] = '\0'; return 0; }
    int total = 0;
    int n;
    while (total < cap - 1 &&
           (n = (int)fread(out + total, 1, (size_t)(cap - 1 - total), p)) > 0) {
        total += n;
    }
    out[total] = '\0';
    OWL_PCLOSE(p);
    return total;
}

/* Variante fixture explicite : `owl <langArgs> --check <fixturePath>`. Sert au
 * test language-aware du sample d'enum, qui vise une fixture
 * differente de la fixture symbole-inconnu. */
static int run_owl_check_capture_fixture(const char* langArgs,
                                         const char* fixture,
                                         char* out, int cap) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s --check %s 2>&1",
             OWL_EXE_REL, langArgs, fixture);
    FILE* p = OWL_POPEN(cmd, "r");
    if (!p) { out[0] = '\0'; return 0; }
    int total = 0;
    int n;
    while (total < cap - 1 &&
           (n = (int)fread(out + total, 1, (size_t)(cap - 1 - total), p)) > 0) {
        total += n;
    }
    out[total] = '\0';
    OWL_PCLOSE(p);
    return total;
}

static void test_check_default_is_french(void) {
    char out[16384];
    run_owl_check_capture("", out, sizeof out);
    CHECK(strstr(out, "inconnue ou inaccessible") != NULL,
          "check_default_french_message");
    CHECK(strstr(out, "unknown or inaccessible") == NULL,
          "check_default_no_english_leak");
}

static void test_check_lang_en_is_english(void) {
    char out[16384];
    run_owl_check_capture("--lang en", out, sizeof out);
    CHECK(strstr(out, "unknown or inaccessible") != NULL,
          "check_lang_en_english_message");
    CHECK(strstr(out, "inconnue ou inaccessible") == NULL,
          "check_lang_en_no_french_leak");
}

static void test_check_lang_fr_is_french(void) {
    char out[16384];
    run_owl_check_capture("--lang fr", out, sizeof out);
    CHECK(strstr(out, "inconnue ou inaccessible") != NULL,
          "check_lang_fr_french_message");
}

/* ================================================================
 * Sample d'enum language-aware dans E_SEM_ENUM_ARG_UNKNOWN
 *
 * Le placeholder {sample} liste les membres autorises d'un enum contraint.
 * Depuis , les .values du registre ne portent que la forme FR canon
 *. Le sample doit donc resoudre chaque valeur en constante et
 * afficher sa forme EN canon quand l'erreur est rendue en anglais — sinon
 * franglais (formes FR dans un message EN). FR reste FR.
 *
 * modeArrondi = { arrondiDemiPair (roundHalfEven),
 *                 arrondiDemiEloigne (roundHalfAwayFromZero) }.
 * ================================================================ */

static void test_enum_sample_default_is_french(void) {
    char out[16384];
    run_owl_check_capture_fixture("", ENUM_FIXTURE, out, sizeof out);
    /* FR : formes FR canon presentes, aucune forme EN. */
    CHECK(strstr(out, "arrondiDemiPair") != NULL,
          "enum_sample_fr_has_french_form");
    CHECK(strstr(out, "roundHalfEven") == NULL,
          "enum_sample_fr_no_english_leak");
}

static void test_enum_sample_lang_en_is_english(void) {
    char out[16384];
    run_owl_check_capture_fixture("--lang en", ENUM_FIXTURE, out, sizeof out);
    /* EN : formes EN canon presentes, aucune forme FR. */
    CHECK(strstr(out, "roundHalfEven") != NULL,
          "enum_sample_en_has_english_form");
    CHECK(strstr(out, "roundHalfAwayFromZero") != NULL,
          "enum_sample_en_has_second_english_form");
    CHECK(strstr(out, "arrondiDemiPair") == NULL,
          "enum_sample_en_no_french_leak");
}

/* ================================================================
 * Exemple language-aware dans W_SEM_ENUM_LITERAL_NUMERIC
 *
 * Le warning « littéral numérique direct sur un param enum » suggère une
 * constante nommée via {example}, resolu depuis values[0] du registre — donc
 * forme FR canon. L'exemple doit basculer en forme EN canon quand
 * l'erreur est rendue en anglais, sinon franglais. FR reste FR.
 *
 * modeArrondi.values[0] = arrondiDemiPair (roundHalfEven).
 * ================================================================ */

static void test_enum_literal_example_default_is_french(void) {
    char out[16384];
    run_owl_check_capture_fixture("", ENUM_LIT_FIXTURE, out, sizeof out);
    CHECK(strstr(out, "arrondiDemiPair") != NULL,
          "enum_literal_fr_has_french_form");
    CHECK(strstr(out, "roundHalfEven") == NULL,
          "enum_literal_fr_no_english_leak");
}

static void test_enum_literal_example_lang_en_is_english(void) {
    char out[16384];
    run_owl_check_capture_fixture("--lang en", ENUM_LIT_FIXTURE, out, sizeof out);
    CHECK(strstr(out, "roundHalfEven") != NULL,
          "enum_literal_en_has_english_form");
    CHECK(strstr(out, "arrondiDemiPair") == NULL,
          "enum_literal_en_no_french_leak");
}

/* ================================================================
 * i18n du mot-cle de bloc PROPRIETES / PROPERTIES
 *
 * Le mot-cle de bloc DSL est bilingue : forme FR canon = "PROPRIETES",
 * forme EN canon = "PROPERTIES" (scanner accepte les deux). Les messages
 * d'erreur EN doivent rendre "PROPERTIES", pas la forme FR -> sinon
 * franglais en locale anglaise (support #30). Le cote FR garde
 * "PROPRIETES".
 * ================================================================ */

static void test_proprietes_block_keyword_fr_keeps_proprietes(void) {
    /* FR = forme canon FR "PROPRIETES" (inchangee). */
    const char* missing = owl_error_render_fr(
        E_SEM_DSL_PROPRIETES_BLOCK_MISSING_WINDOW, "name", "FEN_T", NULL);
    CHECK(missing != NULL && strstr(missing, "PROPRIETES") != NULL,
          "proprietes_fr_missing_window_keeps_proprietes");

    const char* dup = owl_error_render_fr(
        E_SYNTAX_DUPLICATE_PROPRIETES_WINDOW, NULL);
    CHECK(dup != NULL && strstr(dup, "PROPRIETES") != NULL,
          "proprietes_fr_duplicate_window_keeps_proprietes");
}

static void test_proprietes_block_keyword_en_is_properties(void) {
    /* EN = forme canon EN "PROPERTIES", zero residu "PROPRIETES". */
    const char* missing = owl_error_render_en(
        E_SEM_DSL_PROPRIETES_BLOCK_MISSING_WINDOW, "name", "FEN_T", NULL);
    CHECK(missing != NULL && strstr(missing, "PROPERTIES") != NULL,
          "proprietes_en_missing_window_is_properties");
    CHECK(missing != NULL && strstr(missing, "PROPRIETES") == NULL,
          "proprietes_en_missing_window_no_franglais");

    const char* missingCtl = owl_error_render_en(
        E_SEM_DSL_PROPRIETES_BLOCK_MISSING_CONTROL, "name", "BTN_T", NULL);
    CHECK(missingCtl != NULL && strstr(missingCtl, "PROPERTIES") != NULL,
          "proprietes_en_missing_control_is_properties");
    CHECK(missingCtl != NULL && strstr(missingCtl, "PROPRIETES") == NULL,
          "proprietes_en_missing_control_no_franglais");

    const char* dup = owl_error_render_en(
        E_SYNTAX_DUPLICATE_PROPRIETES_WINDOW, NULL);
    CHECK(dup != NULL && strstr(dup, "PROPERTIES") != NULL,
          "proprietes_en_duplicate_window_is_properties");
    CHECK(dup != NULL && strstr(dup, "PROPRIETES") == NULL,
          "proprietes_en_duplicate_window_no_franglais");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int owl_errors_run_tests(void) {
    fprintf(stderr, "\n==== OWL ERRORS REGISTRY TESTS ====\n");

    /* Invariants registry */
    test_registry_count_non_zero();
    test_registry_at_returns_valid();
    test_registry_find_lex_unterminated_string();
    test_registry_find_unknown_returns_null();
    test_registry_by_code_valid();
    test_registry_by_code_out_of_range_returns_null();

    /* Alias numerique */
    test_num_first_e_code_is_e0001();
    test_num_first_w_code_is_w0001();
    test_num_format_padded_prefix();
    test_by_num_roundtrip_e();
    test_by_num_e0001_is_first_e();
    test_by_num_w0001_is_first_w();
    test_by_num_separate_spaces();
    test_by_num_invalid_returns_null();

    /* Rendu templates */
    test_render_fr_no_placeholder();
    test_render_en_no_placeholder();
    test_render_fr_with_placeholder();
    test_render_en_with_placeholder();
    test_render_fr_io_with_path();

    /* Diagnostics CLI bilingues --lang */
    test_check_default_is_french();
    test_check_lang_en_is_english();
    test_check_lang_fr_is_french();

    /* Sample d'enum language-aware */
    test_enum_sample_default_is_french();
    test_enum_sample_lang_en_is_english();

    /* Exemple language-aware du warning littéral numérique */
    test_enum_literal_example_default_is_french();
    test_enum_literal_example_lang_en_is_english();

    /* i18n mot-cle de bloc PROPRIETES/PROPERTIES */
    test_proprietes_block_keyword_fr_keeps_proprietes();
    test_proprietes_block_keyword_en_is_properties();

    fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
            g_pass, g_fail);

    /* Garde ensemble vide : le gros du corpus est une boucle sur
     * le registre d'erreurs. Registre vide = zero assertion, verdict vert. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE — 0 assertion exercee. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}

int owl_errors_case_count(void) { return g_pass + g_fail; }
