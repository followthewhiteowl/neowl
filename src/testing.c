// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "testing.h"
#include "vm.h"
#include "value.h"
#include "object.h"
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "project.h"
#include "prescan.h"
#include "compiler.h"
#include "memory.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <stdlib.h>   /* _fullpath */
#else
#  include <limits.h>
#endif

/* ================================================================
 * COLORS (ANSI escape codes for terminal output)
 * Activé sur Windows 10+ (VT100 support natif), désactivé si stdout
 * n'est pas un TTY (redirection vers fichier) ou si NO_COLOR env var.
 * ================================================================ */

#ifdef _WIN32
#  include <io.h>
#  define isatty_fd(fd) _isatty(fd)
#  define fileno_fd(f)  _fileno(f)
#else
#  include <unistd.h>
#  define isatty_fd(fd) isatty(fd)
#  define fileno_fd(f)  fileno(f)
#endif

static bool g_color_enabled = false;

/* Activation des couleurs ANSI :
 *   - Désactivé si NO_COLOR env var (norme no-color.org)
 *   - Désactivé si stdout n'est pas un TTY (redirection vers fichier)
 *   - Sur Windows : sinon laissé activé. Sur Windows 10+ et avec les
 *     terminaux modernes (Windows Terminal, conhost récent, MSYS2),
 *     les codes VT100 sont interprétés nativement. Sur les anciens
 *     terminaux Windows, ils apparaîtront comme du texte brut.
 *     Si nécessaire, exporter NO_COLOR=1. */
static void color_init(void) {
    if (getenv("NO_COLOR") != NULL) { g_color_enabled = false; return; }
    if (!isatty_fd(fileno_fd(stdout))) { g_color_enabled = false; return; }
    g_color_enabled = true;
}

#define C_GREEN  (g_color_enabled ? "\033[1;32m" : "")
#define C_RED    (g_color_enabled ? "\033[1;31m" : "")
#define C_YELLOW (g_color_enabled ? "\033[1;33m" : "")
#define C_DIM    (g_color_enabled ? "\033[2m"    : "")
#define C_RESET  (g_color_enabled ? "\033[0m"    : "")

/* ================================================================
 * STATE
 * ================================================================ */

static int assert_pass_count = 0;
static int assert_fail_count = 0;
static int test_verbosity = 2;  /* default: show all (backward compat for single-file) */
static bool test_mode_unitaire    = false;

/* ================================================================
 * EXPECTED WARNINGS — meta API pour valider que le compilo OWL
 * emet les warnings attendus dans les tests qui les couvrent.
 *
 * Le test runner alimente ces pointeurs avant chaque vm_execute :
 *   - g_test_warnings  : tableau des warnings compile-time du fichier
 *   - g_test_warnings_count : nombre effectif d'entrees
 *   - g_test_warnings_ack[] : flag "acknowledged" parallele
 *
 * La native __dbgAvertissementAttendu(pattern, msg) scanne le buffer
 * pour trouver un warning non-ack matchant le pattern (substring),
 * le marque comme ack, et compte une assertion PASS / FAIL.
 *
 * Au moment du bilan, les warnings ack sont retires du total global,
 * les non-ack restent visibles pour signaler les intrus.
 * ================================================================ */
static const CompileDiagnostic* g_test_warnings = NULL;
static int g_test_warnings_count = 0;
static bool g_test_warnings_ack[64];

static void test_warnings_bind(const CompileDiagnostic* warns, int count) {
    g_test_warnings = warns;
    g_test_warnings_count = count > 64 ? 64 : count;
    for (int i = 0; i < g_test_warnings_count; i++) g_test_warnings_ack[i] = false;
    /* Compteurs d'execution NON resetes ici : ils sont cumulatifs sur tout
     * le process pour capter les runs eager/boot du top-level (qui tournent
     * avant le bind). Reset explicite via __dbgResetExecutions() si besoin. */
}

static int test_warnings_acknowledged_count(void) {
    int n = 0;
    for (int i = 0; i < g_test_warnings_count; i++) if (g_test_warnings_ack[i]) n++;
    return n;
}

/* ================================================================
 * EXECUTION COUNTERS — meta API interne pour les tests qui valident
 * le modele d'execution (combien de fois un bout de code a tourne).
 *
 * Une variable OWL ne convient pas : re-declaree a chaque passe, elle
 * repart de zero. Le compteur vit donc cote VM, indexe par un tag
 * chaine fourni par le test.
 *
 * Cycle de vie : CUMULATIF sur tout le process. Pas de reset auto par
 * fichier de test — sinon les runs eager/boot du top-level d'une fenetre
 * (qui tournent au chargement du projet, AVANT le bind des warnings) ne
 * seraient pas comptes, or ce sont justement ceux qu'on veut mesurer.
 * Chaque test du chantier utilise un tag UNIQUE (un FEN par sous-dossier)
 * donc pas de collision inter-fichiers. Reset explicite possible via
 * __dbgResetExecutions().
 * ================================================================ */
#define EXEC_COUNTER_MAX 64
#define EXEC_TAG_LEN     64
typedef struct {
    char tag[EXEC_TAG_LEN];
    int  count;
} ExecCounter;
static ExecCounter g_exec_counters[EXEC_COUNTER_MAX];
static int g_exec_counters_count = 0;

static void exec_counters_reset(void) {
    g_exec_counters_count = 0;
}

/* Renvoie l'index du tag (cree l'entree si absente et create=true).
 * -1 si absent et !create, ou si la table est pleine. */
static int exec_counter_index(const char* tag, bool create) {
    for (int i = 0; i < g_exec_counters_count; i++) {
        if (strcmp(g_exec_counters[i].tag, tag) == 0) return i;
    }
    if (!create || g_exec_counters_count >= EXEC_COUNTER_MAX) return -1;
    int idx = g_exec_counters_count++;
    strncpy(g_exec_counters[idx].tag, tag, EXEC_TAG_LEN - 1);
    g_exec_counters[idx].tag[EXEC_TAG_LEN - 1] = '\0';
    g_exec_counters[idx].count = 0;
    return idx;
}

/* ================================================================
 * ASSERTION NATIVES
 * ================================================================ */

/* Assert(condition, message) */
static Value native_assert(int argCount, Value* args) {
    (void)argCount;
    bool condition = false;
    value_to_bool(args[0], &condition);

    const char* msg = "";
    if (argCount >= 2 && IS_STRING(args[1])) {
        msg = AS_CSTRING(args[1]);
    }

    if (condition) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) printf("  FAIL: %s\n", msg);
    }
    return NIL_VAL;
}

/* AssertEgal(a, b, message) */
static Value native_assert_equal(int argCount, Value* args) {
    (void)argCount;
    /*  — un widget DSL nu (ObjUiControlRef) ou une cellule ZR
     * headless materialisee (ObjUiControl widget==NULL) passe en operande
     * d'egalite se deballe vers sa prop par defaut, comme tout consommateur
     * scalaire (cf  pour les autres natives). values_equal ne connait
     * pas les controles : on deballe au chokepoint avant la comparaison. */
    Value a = owl_instance_default_unwrap(args[0]);
    Value b = owl_instance_default_unwrap(args[1]);
    bool equal = values_equal(a, b);

    const char* msg = "";
    if (argCount >= 3 && IS_STRING(args[2])) {
        msg = AS_CSTRING(args[2]);
    }

    if (equal) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: %s -- got ", msg);
            value_print(a);
            printf(" expected ");
            value_print(b);
            printf("\n");
        }
    }
    return NIL_VAL;
}

/* dbgVerifieFaux(condition, message) — passes if condition is falsy */
static Value native_assert_false(int argCount, Value* args) {
    bool condition = false;
    value_to_bool(args[0], &condition);
    const char* msg = "";
    if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);
    if (!condition) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) printf("  FAIL: %s (expected falsy)\n", msg);
    }
    return NIL_VAL;
}

/* dbgVerifieNul(value, message) — passes if value is Null */
static Value native_assert_null(int argCount, Value* args) {
    bool isNull = IS_NIL(args[0]);
    const char* msg = "";
    if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);
    if (isNull) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: %s -- got ", msg);
            value_print(args[0]);
            printf(" expected Null\n");
        }
    }
    return NIL_VAL;
}

/* dbgVerifieNonNul(value, message) — passes if value is NOT Null */
static Value native_assert_not_null(int argCount, Value* args) {
    bool isNull = IS_NIL(args[0]);
    const char* msg = "";
    if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);
    if (!isNull) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) printf("  FAIL: %s -- got Null\n", msg);
    }
    return NIL_VAL;
}

/* dbgVérifieÉgalitéRéel(obtenu, attendu [, message [, tolérance]])
 *   Default absolute tolerance: 1e-9. Promotes int/numeric/currency to double. */
static Value native_assert_equal_real(int argCount, Value* args) {
    double a = 0.0, b = 0.0;
    if (!value_to_real(args[0], &a) || !value_to_real(args[1], &b)) {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: dbgVérifieÉgalitéRéel attend des réels, reçu ");
            value_print(args[0]); printf(" et "); value_print(args[1]); printf("\n");
        }
        return NIL_VAL;
    }

    const char* msg = "";
    if (argCount >= 3 && IS_STRING(args[2])) msg = AS_CSTRING(args[2]);

    double tol = 1e-9;
    if (argCount >= 4) {
        double t;
        if (value_to_real(args[3], &t) && t >= 0.0) tol = t;
    }

    /* Handle NaN as always-not-equal (IEEE 754 convention). */
    double diff = a - b;
    if (diff < 0) diff = -diff;
    bool equal = (diff <= tol);

    if (equal) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: %s -- got %.17g expected %.17g (|diff|=%.3g > tol=%.3g)\n",
                   msg, a, b, diff, tol);
        }
    }
    return NIL_VAL;
}

/* __dbgAvertissementAttendu(motif, message) — meta-API interne.
 *
 * Cherche dans le buffer des warnings compile-time du fichier en cours
 * un warning non-encore-consomme dont le message contient `motif`
 * (substring match). Si trouve : marque le warning comme acknowledged
 * et compte une assertion PASS. Sinon : assertion FAIL.
 *
 * Usage exclusif des tests qui valident le comportement du compilo
 * lui-meme (shadow de constantes, magic d'entier sur enum, etc.).
 * Marquee OWL_NAT_INTERNAL — invisible depuis owl list / owl explain
 * sans recherche explicite, et absente de l'autocomplete LSP. */
static Value native_dbg_avertissement_attendu(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: __dbgAvertissementAttendu requiert un motif chaine\n");
        }
        return NIL_VAL;
    }
    const char* pattern = AS_CSTRING(args[0]);
    const char* msg = "";
    if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);

    for (int i = 0; i < g_test_warnings_count; i++) {
        if (g_test_warnings_ack[i]) continue;  /* deja consomme */
        if (strstr(g_test_warnings[i].message, pattern) != NULL) {
            g_test_warnings_ack[i] = true;
            assert_pass_count++;
            if (test_verbosity >= 2) {
                printf("  PASS: %s (warning l.%d matche '%s')\n",
                       msg, g_test_warnings[i].line, pattern);
            }
            return NIL_VAL;
        }
    }

    assert_fail_count++;
    if (test_verbosity >= 1) {
        printf("  FAIL: %s -- aucun warning non-consomme ne matche motif '%s' "
               "(buffer: %d warnings, %d deja consommes)\n",
               msg, pattern, g_test_warnings_count,
               test_warnings_acknowledged_count());
    }
    return NIL_VAL;
}

/* __dbgNoteExecution(tag) — meta-API interne.
 *
 * Incremente le compteur d'execution associe au tag chaine. Cree
 * l'entree (count=1) si le tag n'a jamais ete note. Sans retour.
 *
 * Usage exclusif des tests qui valident le modele d'execution des
 * fenetres (combien de fois un bout de code tourne). Marquee
 * OWL_NAT_INTERNAL — invisible du catalogue user. */
static Value native_dbg_note_execution(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    int idx = exec_counter_index(AS_CSTRING(args[0]), true);
    if (idx >= 0) g_exec_counters[idx].count++;
    return NIL_VAL;
}

/* __dbgNombreExecutions(tag) : entier — meta-API interne.
 *
 * Renvoie le compteur courant pour le tag (0 si jamais note).
 * Pendant utilisee pour asserter combien de fois un code a tourne.
 * Marquee OWL_NAT_INTERNAL. */
static Value native_dbg_nombre_executions(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return INTEGER_VAL(0);
    int idx = exec_counter_index(AS_CSTRING(args[0]), false);
    return INTEGER_VAL(idx < 0 ? 0 : g_exec_counters[idx].count);
}

/* __dbgResetExecutions() — meta-API interne.
 *
 * Vide tous les compteurs d'execution. Les compteurs etant cumulatifs
 * sur tout le process (pour capter les runs eager/boot), ce reset est
 * la seule remise a zero disponible — a appeler explicitement par un
 * test qui veut repartir d'une base propre. Marquee OWL_NAT_INTERNAL. */
static Value native_dbg_reset_executions(int argCount, Value* args) {
    (void)argCount; (void)args;
    exec_counters_reset();
    return NIL_VAL;
}

/* AssertDifferent(a, b, message) */
static Value native_assert_not_equal(int argCount, Value* args) {
    (void)argCount;
    /*  — deballe les operandes (cf native_assert_equal). */
    Value a = owl_instance_default_unwrap(args[0]);
    Value b = owl_instance_default_unwrap(args[1]);
    bool equal = values_equal(a, b);

    const char* msg = "";
    if (argCount >= 3 && IS_STRING(args[2])) {
        msg = AS_CSTRING(args[2]);
    }

    if (!equal) {
        assert_pass_count++;
        if (test_verbosity >= 2) printf("  PASS: %s\n", msg);
    } else {
        assert_fail_count++;
        if (test_verbosity >= 1) {
            printf("  FAIL: %s -- values should differ but both are ", msg);
            value_print(a);
            printf("\n");
        }
    }
    return NIL_VAL;
}

/* BilanTest() / TestSummary() — kept for backward compat in single-file mode */
static Value native_test_summary(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    int total = assert_pass_count + assert_fail_count;
    printf("\n--- BILAN: %d/%d tests passes", assert_pass_count, total);
    if (assert_fail_count > 0) {
        printf(", %d ECHECS ---\n", assert_fail_count);
    } else {
        printf(" ---\n");
    }
    /* Return fail count; don't reset counters here — the test runner reads them
     * after vm_interpret() and does its own testing_reset() before each file. */
    return INTEGER_VAL(assert_fail_count);
}

/* ================================================================
 * PUBLIC API
 * ================================================================ */

/* ================================================================
 * Registry declarations — Test family (dbgVérifie*).
 * OWL extension: dbgBilanTest / dbgTestSummary (no canonical equivalent).
 * ================================================================ */

static const OwlParam p_cond[]  = { {"condition", "booléen", "condition à vérifier"},
                                     {"message",   "chaîne",  "description optionnelle"}, {0} };
static const OwlParam p_val[]   = { {"valeur",  "tout", "valeur à vérifier"},
                                     {"message", "chaîne",  "description optionnelle"}, {0} };
static const OwlParam p_eq[]    = { {"obtenu",  "tout", "valeur obtenue"},
                                     {"attendu", "tout", "valeur attendue"},
                                     {"message", "chaîne",  "description optionnelle"}, {0} };
static const OwlParam p_eq_real[] = { {"obtenu",    "réel",   "valeur obtenue"},
                                      {"attendu",   "réel",   "valeur attendue"},
                                      {"message",   "chaîne", "description optionnelle"},
                                      {"tolérance", "réel",   "tolérance absolue (défaut 1e-9)"}, {0} };

static const OwlSignature sig_assert[]    = { {-1, p_cond,    "rien", "PASS/FAIL"}, {0} };
static const OwlSignature sig_null[]      = { {-1, p_val,     "rien", "PASS/FAIL"}, {0} };
static const OwlSignature sig_equal[]     = { {-1, p_eq,      "rien", "PASS/FAIL"}, {0} };
static const OwlSignature sig_equal_real[]= { {-1, p_eq_real, "rien", "PASS/FAIL"}, {0} };
static const OwlSignature sig_summary[]   = { {0,  NULL,      "rien", "bilan imprimé"}, {0} };

/* (motif, message) — 2 chaines pour __dbgAvertissementAttendu */
static const OwlParam p_warn_expect[] = {
    {"motif",   "chaîne", "sous-chaine a chercher dans le message du warning"},
    {"message", "chaîne", "description de l'assertion (affichage PASS/FAIL)"}, {0}
};
static const OwlSignature sig_warn_expect[] = { {2, p_warn_expect, "rien", "PASS/FAIL"}, {0} };

/* (tag) — un identifiant chaine pour les compteurs d'execution internes */
static const OwlParam p_exec_tag[] = {
    {"tag", "chaîne", "identifiant du compteur d'execution"}, {0}
};
static const OwlSignature sig_exec_note[]  = { {1, p_exec_tag, "rien",   "incremente le compteur"}, {0} };
static const OwlSignature sig_exec_count[] = { {1, p_exec_tag, "entier", "valeur courante du compteur"}, {0} };
static const OwlSignature sig_exec_reset[] = { {0, NULL,       "rien",   "compteurs remis a zero"}, {0} };

/* Forward decls — natives definies en bas (apres DEFS pour clarte). */
static Value native_in_test_unitaire(int argCount, Value* args);
static Value native_in_stress_gc(int argCount, Value* args);

static const OwlNativeDef DEFS[] = {
    {
        .canonical      = "dbgV\xc3\xa9" "rifieVrai",          /* dbgVérifieVrai */
        .canonicalAscii = "dbgVerifieVrai",
        .nameEN         = "dbgCheckTrue",
        .category       = "Test",
        .descFR         = "Vérifie qu'une condition est vraie.",
        .descEN         = "Assert that a condition is true.",
        .signatures     = sig_assert,
        .impl           = native_assert,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifieFaux",          /* dbgVérifieFaux */
        .canonicalAscii = "dbgVerifieFaux",
        .nameEN         = "dbgCheckFalse",
        .category       = "Test",
        .descFR         = "Vérifie qu'une condition est fausse.",
        .descEN         = "Assert that a condition is false.",
        .signatures     = sig_assert,
        .impl           = native_assert_false,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifieNull",          /* dbgVérifieNull */
        .canonicalAscii = "dbgVerifieNull",
        .nameEN         = "dbgCheckNull",
        .category       = "Test",
        .descFR         = "Vérifie qu'une valeur vaut Null.",
        .descEN         = "Assert that a value is Null.",
        .signatures     = sig_null,
        .impl           = native_assert_null,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifieNonNull",       /* dbgVérifieNonNull */
        .canonicalAscii = "dbgVerifieNonNull",
        .nameEN         = "dbgCheckNotNull",
        .category       = "Test",
        .descFR         = "Vérifie qu'une valeur n'est pas Null.",
        .descEN         = "Assert that a value is not Null.",
        .signatures     = sig_null,
        .impl           = native_assert_not_null,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifieEgalit\xc3\xa9", /* dbgVérifieEgalité */
        .canonicalAscii = "dbgVerifieEgalite",
        .nameEN         = "dbgCheckEqual",
        .category       = "Test",
        .descFR         = "Vérifie que deux valeurs sont égales.",
        .descEN         = "Assert that two values are equal.",
        .signatures     = sig_equal,
        .impl           = native_assert_equal,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifie\xc3\x89" "galit\xc3\xa9R\xc3\xa9" "el",  /* dbgVérifieÉgalitéRéel */
        .canonicalAscii = "dbgVerifieEgaliteReel",
        .nameEN         = "dbgCheckEqualReal",
        .category       = "Test",
        .descFR         = "Vérifie que deux réels sont égaux à une tolérance près (défaut 1e-9).",
        .descEN         = "Assert two reals are equal within a tolerance (default 1e-9).",
        .signatures     = sig_equal_real,
        .impl           = native_assert_equal_real,
    },
    {
        .canonical      = "dbgV\xc3\xa9" "rifieDiff\xc3\xa9" "rence", /* dbgVérifieDifférence */
        .canonicalAscii = "dbgVerifieDifference",
        .nameEN         = "dbgCheckDifferent",
        .category       = "Test",
        .descFR         = "Vérifie que deux valeurs sont différentes.",
        .descEN         = "Assert that two values are different.",
        .signatures     = sig_equal,
        .impl           = native_assert_not_equal,
    },
    {
        .canonical      = "dbgBilanTest",
        .nameEN         = "dbgTestSummary",
        .category       = "Test",
        .descFR         = "Affiche le bilan des assertions (extension OWL).",
        .descEN         = "Print the assertion summary (OWL extension).",
        .signatures     = sig_summary,
        .impl           = native_test_summary,
    },
    {
        .canonical      = "EnModeTestUnitaire",
        .nameEN         = "InUnitTestMode",
        .category       = "Test",
        .descFR         = "Renvoie Vrai si le code tourne via 'owl test' (test runner OWL). Permet au code de detecter le contexte test pour adapter son comportement (skip dialogs bloquantes, mocks, etc.).",
        .descEN         = "Returns True if running under 'owl test' (OWL test runner). Allows code to detect test context to adapt behavior (skip blocking dialogs, mocks, etc.).",
        .signatures     = sig_summary,
        .impl           = native_in_test_unitaire,
    },
    {
        .canonical      = "EnModeStressGC",
        .nameEN         = "InStressGCMode",
        .category       = "Test",
        .descFR         = "Renvoie Vrai si le GC tourne en mode stress (OWL_STRESS_GC=1 ou OWL_GC_POISON=1). Permet aux tests de mesure de performance d'assouplir leurs seuils (le stress-GC trigge le collect a chaque allocation, ce qui ralentit drastiquement la suite).",
        .descEN         = "Returns True if GC runs in stress mode (OWL_STRESS_GC=1 or OWL_GC_POISON=1). Allows perf tests to relax thresholds (stress-GC triggers collect on each alloc, drastically slowing the suite).",
        .signatures     = sig_summary,
        .impl           = native_in_stress_gc,
    },
    {
        /* Meta API interne — invisible du catalogue user. */
        .canonical      = "__dbgAvertissementAttendu",
        .nameEN         = "__dbgWarningExpected",
        .category       = "Test",
        .descFR         = "Meta interne : consomme un warning compile-time matchant le motif (substring). Usage exclusif des tests qui valident les warnings emis par le compilo OWL. Marque OWL_NAT_INTERNAL.",
        .descEN         = "Internal meta: consume a compile-time warning matching the pattern (substring). Sole purpose: tests that validate warnings emitted by the OWL compiler itself. Marked OWL_NAT_INTERNAL.",
        .signatures     = sig_warn_expect,
        .impl           = native_dbg_avertissement_attendu,
        .flags          = OWL_NAT_INTERNAL,
    },
    {
        /* Meta API interne — invisible du catalogue user. */
        .canonical      = "__dbgNoteExecution",
        .nameEN         = "__dbgNoteExecution",
        .category       = "Test",
        .descFR         = "Meta interne : incremente un compteur d'execution indexe par tag. Usage exclusif des tests du modele d'execution. Marque OWL_NAT_INTERNAL.",
        .descEN         = "Internal meta: increment a per-tag execution counter. Sole purpose: execution-model tests. Marked OWL_NAT_INTERNAL.",
        .signatures     = sig_exec_note,
        .impl           = native_dbg_note_execution,
        .flags          = OWL_NAT_INTERNAL,
    },
    {
        /* Meta API interne — invisible du catalogue user. */
        .canonical      = "__dbgNombreExecutions",
        .nameEN         = "__dbgExecutionCount",
        .category       = "Test",
        .descFR         = "Meta interne : renvoie le compteur d'execution courant pour un tag (0 si jamais note). Usage exclusif des tests du modele d'execution. Marque OWL_NAT_INTERNAL.",
        .descEN         = "Internal meta: return the current execution counter for a tag (0 if never noted). Sole purpose: execution-model tests. Marked OWL_NAT_INTERNAL.",
        .signatures     = sig_exec_count,
        .impl           = native_dbg_nombre_executions,
        .flags          = OWL_NAT_INTERNAL,
    },
    {
        /* Meta API interne — invisible du catalogue user. */
        .canonical      = "__dbgResetExecutions",
        .nameEN         = "__dbgResetExecutions",
        .category       = "Test",
        .descFR         = "Meta interne : remet a zero tous les compteurs d'execution (cumulatifs sur le process). Usage exclusif des tests du modele d'execution. Marque OWL_NAT_INTERNAL.",
        .descEN         = "Internal meta: reset all execution counters (process-cumulative). Sole purpose: execution-model tests. Marked OWL_NAT_INTERNAL.",
        .signatures     = sig_exec_reset,
        .impl           = native_dbg_reset_executions,
        .flags          = OWL_NAT_INTERNAL,
    },
};

static Value native_in_test_unitaire(int argCount, Value* args) {
    (void)argCount; (void)args;
    return BOOL_VAL(test_mode_unitaire);
}

static Value native_in_stress_gc(int argCount, Value* args) {
    (void)argCount; (void)args;
    return BOOL_VAL(g_stress_gc || g_gc_poison);
}

void testing_register_natives(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}

void testing_set_verbosity(int level) {
    test_verbosity = level;
}

void testing_set_unitaire(bool active) { test_mode_unitaire = active; }
bool testing_is_unitaire(void)         { return test_mode_unitaire; }

int testing_get_pass_count(void) {
    return assert_pass_count;
}

int testing_get_fail_count(void) {
    return assert_fail_count;
}

void testing_reset(void) {
    assert_pass_count = 0;
    assert_fail_count = 0;
}

/* ================================================================
 * SCENARIO + FIXTURE DETECTION (charte de test §4-§5)
 * ================================================================ */

/* Préfixes acceptés pour les procédures-scénarios (charte §4).
 * Les trois sont équivalents — choix stylistique. */
static bool is_scenario_name(const char* name) {
    if (!name) return false;
    return strncmp(name, "scenario_", 9) == 0
        || strncmp(name, "test_",     5) == 0
        || strncmp(name, "should_",   7) == 0;
}

/* Appelle une procédure globale par nom si elle existe dans vm.globals.
 * Retourne true si appelée + OK, false si pas trouvée OU erreur runtime.
 * Utilise vm_call_global (push+call_value+run) qui gère l'invariant
 * "top-level exit" : OP_RETURN pop le callee sans laisser de résultat
 * sur la stack — pas besoin de pop supplémentaire après. */
static bool call_proc_if_exists(const char* name) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    Value fn;
    if (!table_get(&vm.globals, k, &fn)) return false;
    if (!IS_CALLABLE(fn)) return false;
    return vm_call_global(name) == INTERPRET_OK;
}

/* Parse l'en-tête du fichier scénario (charte §3) :
 *   // @sujet : <symbole>
 *   // @résumé : <phrase>
 * Stocke dans des buffers fournis par le caller. Annotations facultatives,
 * stockées vides si absentes. */
static void parse_test_header(const char* src, char* outSujet, int sujetCap,
                              char* outResume, int resumeCap) {
    outSujet[0] = '\0';
    outResume[0] = '\0';
    if (!src) return;
    /* Scanne les 20 premières lignes pour ne pas pénaliser les gros fichiers. */
    int line = 0;
    const char* p = src;
    while (*p && line < 20) {
        /* Skip whitespace de début */
        while (*p == ' ' || *p == '\t') p++;
        /* Reconnaît // @sujet et // @résumé / @resume */
        if (p[0] == '/' && p[1] == '/') {
            const char* q = p + 2;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '@') {
                q++;
                const char* tag = q;
                while (*q && *q != ':' && *q != '\n' && *q != ' ') q++;
                int tagLen = (int)(q - tag);
                const char* colon = q;
                while (*colon == ' ') colon++;
                if (*colon == ':') {
                    colon++;
                    while (*colon == ' ' || *colon == '\t') colon++;
                    /* Lit la valeur jusqu'à \n */
                    const char* val = colon;
                    while (*val && *val != '\n' && *val != '\r') val++;
                    int valLen = (int)(val - colon);
                    bool isSujet  = (tagLen == 5 && strncmp(tag, "sujet", 5) == 0);
                    /* "résumé" UTF-8 = r\xc3\xa9sum\xc3\xa9 (8 bytes). ASCII "resume" (6). */
                    bool isResume = (tagLen == 8 && strncmp(tag, "r\xc3\xa9sum\xc3\xa9", 8) == 0)
                                 || (tagLen == 6 && strncmp(tag, "resume", 6) == 0);
                    if (isSujet && sujetCap > 0) {
                        int n = valLen < sujetCap - 1 ? valLen : sujetCap - 1;
                        memcpy(outSujet, colon, n);
                        outSujet[n] = '\0';
                    } else if (isResume && resumeCap > 0) {
                        int n = valLen < resumeCap - 1 ? valLen : resumeCap - 1;
                        memcpy(outResume, colon, n);
                        outResume[n] = '\0';
                    }
                }
            }
        }
        /* Avance à la prochaine ligne */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line++;
    }
}

/* ================================================================
 * MULTI-FICHIER (charte de test : poo_multifile)
 *
 * Convention "autodiscovery local sous-dossier" (cf. ).
 * Quand un fichier "test.owl" (stem exact) vit dans un sous-dossier
 * contenant d'autres .owl, le runner les charge tous dans la meme VM
 * AVANT d'executer le test.owl. Tri alphabetique sur le stem pour
 * deterministe / OS-agnostique. Le prescan supporte les forward-refs
 * donc l'ordre n'est pas semantiquement critique — mais autant fixer
 * un ordre stable pour les eventuels effets de bord top-level.
 *
 * Depuis  le perimetre des compagnons n'est plus le simple
 * "meme dirname" mais la frontiere resolue (resolve_project_root) :
 * sous un projet.owl -> arbre racine du projet (recursif) ; sinon ->
 * dossier du test seul (regime historique). Helpers locaux ci-dessous.
 * ================================================================ */

/* Retourne la longueur du dirname (sans le separateur final).
 * Path peut contenir '/' ou '\\' (Windows). 0 si pas de separateur. */
static int dirname_len(const char* path) {
    int len = (int)strlen(path);
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/' || path[i] == '\\') return i;
    }
    return 0;
}

/* Resout `path` en chemin absolu normalise (separateurs '/'). Sert au
 * filtrage des compagnons en mode PROJECT : les chemins du Project sont
 * relatifs au cwd, la racine resolue par resolve_project_root est absolue.
 * On ramene tout en absolu '/' pour un prefix-match fiable. */
static void abs_normalized(const char* path, char* out, size_t outSize) {
    char tmp[1024];
#ifdef _WIN32
    if (_fullpath(tmp, path, sizeof(tmp)) == NULL) {
        snprintf(tmp, sizeof(tmp), "%s", path);
    }
#else
    if (realpath(path, tmp) == NULL) {
        snprintf(tmp, sizeof(tmp), "%s", path);
    }
#endif
    snprintf(out, outSize, "%s", tmp);
    for (char* c = out; *c; c++) {
        if (*c == '\\') *c = '/';
    }
}

/* Vrai si le fichier `filePath` est situe sous (ou egal a) l'arbre raciné
 * `rootAbs` (chemin absolu '/'). Compare les formes absolues normalisees.
 * Le separateur apres le prefixe garantit qu'on ne matche pas un dossier
 * voisin de prefixe commun (ex : root="/a/proj" ne doit pas avaler
 * "/a/projets/x"). */
static bool path_under_root(const char* filePath, const char* rootAbs) {
    char fileAbs[1024];
    abs_normalized(filePath, fileAbs, sizeof(fileAbs));
    size_t rlen = strlen(rootAbs);
    if (rlen == 0) return false;
    /* Comparaison case-insensitive : Windows + invariant OWL. */
    if (strncasecmp(fileAbs, rootAbs, rlen) != 0) return false;
    char next = fileAbs[rlen];
    return next == '/' || next == '\0';
}

/* Compare deux files par stem (alphabetique ASCII). Utilise par qsort
 * sur le tableau d'indices compagnons. */
static const Project* g_sort_project = NULL;
static int compare_companion_indices(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return strcmp(g_sort_project->files[ia].name,
                  g_sort_project->files[ib].name);
}

/* Collecte les indices des fichiers compagnons du test.owl[testIndex] :
 * .owl, non-isTest, dans le perimetre de la *frontiere* du test (,
 * via resolve_project_root) — et non plus le simple "meme dirname". Deux
 * regimes :
 *   - PROJECT (projet.owl ancetre) : contexte = l'arbre racine du projet,
 *     donc tous les non-test .owl situes sous cette racine (recursif). Un
 *     test sous tests/ voit ainsi core/, models/, etc. du meme projet.
 *   - SCRIPT / ENTRYPOINT (pas de projet.owl ancetre) : contexte = le
 *     dossier du test seul (comportement historique "meme dirname"),
 *     preserve pour l'immense majorite des tests isoles.
 * Trie alphabetiquement par stem. Retourne le nombre de compagnons
 * (0 si test isole). outIndices doit pointer sur un buffer >= maxOut. */
static int collect_companions(const Project* project, int testIndex,
                              int* outIndices, int maxOut) {
    const char* testPath = project->files[testIndex].path;
    int testDirLen = dirname_len(testPath);
    if (testDirLen == 0) return 0;

    char projectRoot[1024];
    ProjectMode mode = resolve_project_root(testPath, projectRoot,
                                            sizeof(projectRoot));

    int count = 0;
    for (int i = 0; i < project->fileCount; i++) {
        if (i == testIndex) continue;
        if (project->files[i].isTest) continue;
        const char* p = project->files[i].path;
        bool inScope;
        if (mode == PROJECT_MODE_PROJECT) {
            /* Tout non-test sous l'arbre racine du projet. */
            inScope = path_under_root(p, projectRoot);
            /*  — meme digue que le vrai project_run : en
             * mode projet, un compagnon PLAT (aucun bloc DSL au top-level) est
             * un script prive, invisible au projet. Il n'est ni collecte, ni
             * prescane, ni compile, ni execute par le runner (la liste pilote
             * les trois). Mode SCRIPT : inchange. */
            if (inScope
                && !prescan_source_is_dsl_block_file(project->files[i].source)) {
                inScope = false;
            }
        } else {
            /* Meme dirname uniquement (script / point d'entree). */
            int dlen = dirname_len(p);
            inScope = (dlen == testDirLen && memcmp(p, testPath, dlen) == 0);
        }
        if (!inScope) continue;
        if (count >= maxOut) break;
        outIndices[count++] = i;
    }

    if (count > 1) {
        g_sort_project = project;
        qsort(outIndices, count, sizeof(int), compare_companion_indices);
        g_sort_project = NULL;
    }
    return count;
}

/* Tri topologique des compagnons par chaine d'heritage : partage avec
 * project_run (owl <fichier>). Voir prescan.c /
 * topo_sort_companions_by_inheritance. */

/* ================================================================
 * TEST RUNNER
 * ================================================================ */

int run_tests_ex(const TestOptions* opts) {
    Project project;
    project_init(&project);

    /*  : dissocier le PERIMETRE DE DECOUVERTE (racine projet, pour
     * que les compagnons cross-sous-dossier soient charges) de la SELECTION
     * des tests a executer (la cible d'origine).
     *
     * Quand on cible un fichier (`owl test x/tests/test_a.owl`) ou un
     * sous-dossier sous un `projet.owl`, le walk doit partir de la racine du
     * projet — sinon le `Project` ne contient pas `core/...` et un test sous
     * `tests/` ne voit pas son arbre projet (CRASH "Type inconnu"). On
     * restreint ensuite l'execution aux seuls tests situes sous la cible.
     *
     * Cas sans `projet.owl` ancetre (mode SCRIPT/ENTRYPOINT) : walk depuis la
     * cible comme avant — le depot OWL entier n'a pas de `projet.owl` racine,
     * donc `owl test` / `owl test tests/` restent strictement inchanges. */
    const char* discoverDir = opts->dir;
    char selectionRootAbs[1024];
    selectionRootAbs[0] = '\0';
    {
        /* Construit un chemin de sonde representant la cible pour
         * resolve_project_root (qui attend un *fichier* et remonte depuis son
         * dossier). Fichier cible -> le fichier lui-meme ; dossier cible ->
         * une sonde fictive dans ce dossier (le composant final est strippe). */
        char probe[1024];
        if (opts->singleFile != NULL) {
            snprintf(probe, sizeof(probe), "%s/%s", opts->dir, opts->singleFile);
        } else {
            snprintf(probe, sizeof(probe), "%s/__owl_probe.owl", opts->dir);
        }
        char projectRoot[1024];
        ProjectMode mode = resolve_project_root(probe, projectRoot,
                                                sizeof(projectRoot));
        if (mode == PROJECT_MODE_PROJECT) {
            char targetDirAbs[1024];
            abs_normalized(opts->dir, targetDirAbs, sizeof(targetDirAbs));
            /* Strip un eventuel separateur final (`owl test dir/`) : sinon le
             * prefix-match contre projectRoot (sans slash) echoue et une cible
             * = racine projet serait prise a tort pour un sous-dossier. */
            size_t tlen = strlen(targetDirAbs);
            while (tlen > 1 && targetDirAbs[tlen - 1] == '/') {
                targetDirAbs[--tlen] = '\0';
            }
            /* Si la cible EST deja la racine projet, rien a separer : walk
             * depuis la cible (comportement actuel preserve). Sinon, walk
             * depuis la racine projet + selection restreinte a la cible. */
            if (strcasecmp(targetDirAbs, projectRoot) != 0) {
                discoverDir = projectRoot;
                snprintf(selectionRootAbs, sizeof(selectionRootAbs), "%s",
                         targetDirAbs);
            }
        }
    }

    if (!project_discover_ex(&project, discoverDir, false)) {
        project_free(&project);
        return 1;
    }

    /* Mode debug --list-files : dump tous les .owl découverts avec leur
     * statut isTest. Précieux pour diagnostiquer une autodiscovery trop
     * large (ex : fichier hors-tests qui se retrouve exécuté). Cf. 
     * follow-up — la liste sort sur stderr pour ne pas polluer un éventuel
     * --json sur stdout. */
    if (opts->listFiles) {
        fprintf(stderr, "--- owl test --list-files : %d fichiers découverts dans '%s' ---\n",
                project.fileCount, opts->dir);
        for (int i = 0; i < project.fileCount; i++) {
            fprintf(stderr, "  %s  %s\n",
                    project.files[i].isTest ? "[TEST]   " : "[loaded] ",
                    project.files[i].path);
        }
        fprintf(stderr, "----------------------------------------\n");
    }

    bool quiet = (opts->verbosity < 0);
    color_init();

    if (!quiet) {
        printf("========================================\n");
        printf("   OWL Test Suite\n");
        printf("========================================\n\n");
    }

    /* Compteurs distincts par catégorie de résultat (cf audit BILAN) :
     *   - suitesOK         : compile + run + 0 assertion failed
     *   - suitesCrashed    : compile error OU runtime error → exit anormal
     *   - suitesAssertFail : exécution OK mais une ou plusieurs Vérifie* ont
     *                        retourné Faux
     *   - totalWarnings    : warnings compile-time (variables inutilisées etc.)
     *
     * On stocke aussi les détails (nom, message) pour affichage en fin. */
    int suitesPassed = 0;
    int suitesCrashed = 0;
    int suitesAssertFail = 0;
    /* Vrai des qu'une campagne a consomme au moins un couple vm_init/vm_free
     * (cf. la restauration en fin de fonction). */
    bool vmConsommee = false;
    /* Fichiers dont l'échec provient uniquement de scénarios (proc renvoyant
     * une erreur runtime capturée par le runner), sans assertion Vérifie* en
     * échec ni crash de la suite. Distinct de suitesAssertFail pour un STATUS
     * et un libellé corrects. */
    int suitesScenarioFail = 0;
    int totalAssertions = 0;
    int totalAssertionsFailed = 0;
    int totalWarnings = 0;
    /* Compteurs scénarios — mode procédural (charte de test §4).
     * Les fichiers en mode script libre n'incrémentent pas ces compteurs ;
     * seuls les fichiers avec ≥1 procédure scenario_/test_/should_ comptent. */
    int totalScenarios = 0;
    int totalScenariosFailed = 0;

    /* Buffers détaillés pour affichage final. */
    char* crashNames[512];
    char  crashMsgs[512][256];
    int   crashCount = 0;
    char* assertFailNames[512];
    int   assertFailPass[512];
    int   assertFailFail[512];
    int   assertFailNamesCount = 0;
    char  warnSummary[512][320];   /* "file.owl:LINE — message" */
    int   warnSummaryCount = 0;

    for (int i = 0; i < project.fileCount; i++) {
        /* Cible unique explicite (`owl test <fichier.owl>`) : l'utilisateur a
         * nomme CE fichier, il devient la suite a executer meme sans le prefixe
         * `test_`. Le match est evalue AVANT le gate isTest — sinon un fichier
         * non prefixe passe par-dessus la maille du filet : ecarte
         * en amont, aucun fichier n'est retenu et le runner sort en
         * "ALL CLEAR (0 suites)" avec exit 0, masquant meme une erreur de
         * compilation. Le gate isTest reste actif pour les runs de dossier
         * (on n'y execute pas n'importe quel .owl comme un test). */
        bool isSingleTarget = false;
        if (opts->singleFile != NULL) {
            size_t sfLen = strlen(opts->singleFile);
            const char* stem = project.files[i].name;
            bool match = (strcmp(stem, opts->singleFile) == 0);
            if (!match && sfLen > 4) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s.owl", stem);
                match = (strcmp(buf, opts->singleFile) == 0);
            }
            if (!match) continue;
            isSingleTarget = true;
        }

        if (!isSingleTarget && !project.files[i].isTest) continue;

        /*  : si la decouverte a ete elargie a la racine projet (cible
         * = fichier ou sous-dossier d'un projet), n'executer que les tests
         * situes SOUS la cible d'origine. Les autres tests du projet ne sont
         * la que pour fournir le contexte compagnon. */
        if (selectionRootAbs[0] != '\0' &&
            !path_under_root(project.files[i].path, selectionRootAbs)) {
            continue;
        }

        /* Filter by pattern (substring match) */
        if (opts->filter != NULL) {
            if (strstr(project.files[i].name, opts->filter) == NULL) {
                continue;
            }
        }

        /* Fresh VM + reset counters for each test file */
        vm_init();
        vmConsommee = true;
        testing_reset();
        /*  : reset egalement les tables POO compile-time persistantes
         * (g_class_super, g_class_field_vis, g_class_abstract, ...). Sans ce
         * reset, un test precedent qui declare cFoo abstraite pollue la
         * compile du suivant qui declare cFoo en classe normale — d'ou refus
         * d'instanciation, faux warning de masquage de champ herite, etc. */
        compiler_reset_poo_state();
        /*  : reset l'etat FORMAT PROJET compile-time (FormatDate/Heure/
         * Duree + FormatNombre/Monetaire, captes au parse du bloc `est un
         * Projet`) a CHAQUE frontiere de test. Sans ca, un test dont un
         * compagnon projet.owl pose FormatDate leguerait ce format au fichier de
         * test suivant qui n'en declare pas (fuite cross-projet dans le meme
         * process). Miroir de project_run.c (owl <fichier>), que le runner ne
         * traverse pas. Le compagnon projet.owl du test courant, compile juste
         * apres, re-arme le format pour CE test. */
        compiler_projet_boot_langue_reset();
        testing_set_verbosity(opts->verbosity);
        /* Active EnModeTestUnitaire() pour la duree de l'execution : le code
         * teste peut detecter le contexte (canon) et adapter (skip
         * dialogs bloquantes, mocks, etc.). */
        testing_set_unitaire(true);
        /*  : mode headless ON par defaut dans owl test. Reset pour
         * chaque fichier (vm_init() vient juste de remettre le flag a false).
         * Un test peut le re-toggler localement via TestModeHeadless(Faux),
         * mais le fichier suivant repartira ON. */
        vm.headless_mode = true;

        /* Capture des warnings compile-time (variables inutilisées, etc.).
         * Buffer local par fichier pour rattachement au nom de la suite.
         * Lié aussi au runtime via test_warnings_bind() pour que la native
         * __dbgAvertissementAttendu puisse consommer les warnings depuis
         * le code OWL du test. */
        CompileDiagnostic warnings[64];
        int warnCount = 0;
        compile_set_warning_capture(warnings, 64, &warnCount);

        /* Prescan le fichier de test pour permettre les forward refs de
         * globals top-level (cf project_run mode single-file). Le test
         * compile + exécute son propre code, comme un script standalone. */
        SymbolTable* testSymbols = symtable_create();

        /* Mode multi-fichier (poo_multifile / ) : un test_*.owl
         * qui a des .owl voisins non-test dans son dossier active
         * automatiquement le chargement multi-fichier. Les compagnons sont
         * chargés dans la même VM avant le test, dans l'ordre alphabétique.
         * Si le dossier ne contient que le test (pas de voisin non-test),
         * mode single classique (companionCount == 0). */
        int companionIndices[MAX_PROJECT_FILES];
        int companionCount = collect_companions(&project, i,
                                                companionIndices,
                                                MAX_PROJECT_FILES);
        bool multifile = (companionCount > 0);

        /*  : sous OWL_GC_POISON, log le nom AVANT execution sur
         * stderr line-buffered. En cas de SEGV UAF, on identifie le test
         * coupable sans dependre du buffer stdout. */
        if (g_gc_poison) {
            fprintf(stderr, "[POISON-START] %s\n", project.files[i].name);
            fflush(stderr);
        }

        if (multifile) {
            /* Enregistre les fileNames pour la resolution qualifiee +
             * prescan chaque compagnon AVANT le test.owl. Le prescan
             * remplit symtable avec les classes/procedures de chaque
             * compagnon, ce qui permet a test.owl de les referencer. */
            for (int c = 0; c < companionCount; c++) {
                int ci = companionIndices[c];
                if (ci < MAX_PROJECT_FILES) {
                    size_t nlen = strlen(project.files[ci].name);
                    if (nlen >= 128) nlen = 127;
                    memcpy(testSymbols->fileNames[ci],
                           project.files[ci].name, nlen);
                    testSymbols->fileNames[ci][nlen] = '\0';
                    if (ci >= testSymbols->fileNameCount) {
                        testSymbols->fileNameCount = ci + 1;
                    }
                }
            }
            /* Idem pour le test.owl lui-meme. */
            if (i < MAX_PROJECT_FILES) {
                size_t nlen = strlen(project.files[i].name);
                if (nlen >= 128) nlen = 127;
                memcpy(testSymbols->fileNames[i], project.files[i].name, nlen);
                testSymbols->fileNames[i][nlen] = '\0';
                if (i >= testSymbols->fileNameCount) {
                    testSymbols->fileNameCount = i + 1;
                }
            }
            for (int c = 0; c < companionCount; c++) {
                int ci = companionIndices[c];
                prescan_file(testSymbols, project.files[ci].source, ci);
            }
            /*  : prescan rempli — on connait la chaine Herite de
             * (testSymbols->inheritances[]). On reordonne les compagnons
             * par profondeur d'heritage croissante avant le compile loop,
             * pour qu'un parent (ex: cVivant) compile toujours avant son
             * enfant (cAnimal Herite de cVivant), meme si l'ordre alpha
             * mettait l'enfant en premier. Fallback alpha stable entre
             * classes de meme profondeur ou fichiers sans heritage. */
            topo_sort_companions_by_inheritance(testSymbols, companionIndices,
                                                companionCount);
        }
        prescan_file(testSymbols, project.files[i].source, i);

        /* En multi-fichier : compile + execute chaque compagnon dans
         * l'ordre alphabetique AVANT le test.owl, pour que ses
         * declarations top-level (classes, procedures) soient dans
         * vm.globals au moment de l'execution du test. Si un compagnon
         * casse au compile ou au runtime, on traite ca comme un crash
         * de la suite test. */
        /*  — attribution : quand la faute est dans un COMPAGNON, le
         * bilan doit le nommer. Sans ca l'utilisateur voit ses fichiers de test
         * en CRASH et doit deviner que le fautif est un troisieme fichier. */
        bool companionFailed = false;
        const char* companionFailPath = NULL;
        bool companionFailAtCompile = false;
        if (multifile) {
            if (opts->listFiles) {
                fprintf(stderr, "[multifile] test=%s, %d companion(s)\n",
                        project.files[i].path, companionCount);
            }
            for (int c = 0; c < companionCount && !companionFailed; c++) {
                int ci = companionIndices[c];
                if (opts->listFiles) {
                    fprintf(stderr, "  -> exec companion: %s\n", project.files[ci].path);
                }
                ObjFunction* compFn = compile_in_project(
                    project.files[ci].source,
                    project.files[ci].path,
                    testSymbols, ci);
                if (compFn == NULL) {
                    companionFailed = true;
                    companionFailPath = project.files[ci].path;
                    companionFailAtCompile = true;
                    break;
                }
                if (vm_execute(compFn) != INTERPRET_OK) {
                    companionFailed = true;
                    companionFailPath = project.files[ci].path;
                    break;
                }
            }
        }

        ObjFunction* fn = companionFailed ? NULL :
                          compile_in_project(project.files[i].source,
                                             project.files[i].path,
                                             testSymbols, i);
        /* Expose le buffer warnings au runtime APRES le compile (qui l'a
         * rempli) et AVANT vm_execute (qui executera les assertions
         * __dbgAvertissementAttendu du test). */
        test_warnings_bind(warnings, warnCount);
        InterpretResult result = INTERPRET_COMPILE_ERROR;
        int fileScenarios = 0;
        int fileScenariosFailed = 0;
        if (fn != NULL) {
            result = vm_execute(fn);
            /* Si le top-level OK, on regarde si le fichier déclare des
             * scénarios (charte §4) : présence d'au moins une PROCEDURE
             * scenario_X / test_X / should_X. Si oui, on bascule en mode
             * procédural (fixtures + boucle scenarios). Sinon, on retombe
             * sur le path historique (Main() / Fermeture()) — coexistence
             * stricte avec les ~800 tests script libre existants. */
            if (result == INTERPRET_OK) {
                /* Énumère vm.globals pour découvrir scénarios + fixtures.
                 * Stocke les KEY ObjString* (rootés via vm.globals donc GC-safe)
                 * plutôt que les const char* — l'interning peut bouger la
                 * table d'entries entre l'iteration et les calls. */
                #define MAX_SCEN 256
                ObjString* scenKeys[MAX_SCEN];
                int scenCount = 0;
                for (int g = 0; g < vm.globals.capacity && scenCount < MAX_SCEN; g++) {
                    Entry* e = &vm.globals.entries[g];
                    if (e->key == NULL || e->key->chars == NULL) continue;
                    if (!is_scenario_name(e->key->chars)) continue;
                    if (!IS_CALLABLE(e->value)) continue;
                    scenKeys[scenCount++] = e->key;
                }
                if (scenCount > 0) {
                    /* Mode procédural : avantTout → loop(avantChaque → scen
                     * → apresChaque) → apresTout. */
                    fileScenarios = scenCount;
                    /* Header @sujet / @résumé pour affichage verbose. */
                    if (opts->verbosity >= 1) {
                        char sujet[128], resume[256];
                        parse_test_header(project.files[i].source,
                                          sujet, sizeof sujet,
                                          resume, sizeof resume);
                        if (sujet[0]) {
                            printf("  %s@sujet  %s %s\n", C_DIM, C_RESET, sujet);
                        }
                        if (resume[0]) {
                            printf("  %s@résumé %s %s\n", C_DIM, C_RESET, resume);
                        }
                    }
                    call_proc_if_exists("avantToutTest");
                    call_proc_if_exists("beforeAllTests");
                    for (int s = 0; s < scenCount; s++) {
                        int passBefore = testing_get_pass_count();
                        int failBefore = testing_get_fail_count();
                        call_proc_if_exists("avantChaqueTest");
                        call_proc_if_exists("beforeEachTest");
                        /* Appel via vm_call_global pour gérer l'invariant
                         * top-level exit (pop callee sans push résultat). */
                        bool scenOk = call_proc_if_exists(scenKeys[s]->chars);
                        call_proc_if_exists("apresChaqueTest");
                        call_proc_if_exists("afterEachTest");
                        /* Échec si la proc a renvoyé runtime error OU si une
                         * assertion a fail pendant son exécution. */
                        int failAfter = testing_get_fail_count();
                        if (!scenOk || failAfter > failBefore) {
                            fileScenariosFailed++;
                            if (opts->verbosity >= 1) {
                                printf("  %s%s%s scenario %s (%d pass, %d fail)\n",
                                       C_RED, "\xE2\x9C\x97", C_RESET,
                                       scenKeys[s]->chars,
                                       testing_get_pass_count() - passBefore,
                                       failAfter - failBefore);
                            }
                        } else if (opts->verbosity >= 2) {
                            printf("  %s%s%s scenario %s (%d assertions)\n",
                                   C_GREEN, "\xE2\x9C\x93", C_RESET,
                                   scenKeys[s]->chars,
                                   testing_get_pass_count() - passBefore);
                        }
                    }
                    call_proc_if_exists("apresToutTest");
                    call_proc_if_exists("afterAllTests");
                    /* Le résultat reste OK même si un scénario a fail : la
                     * remontée d'erreur passe par les compteurs assertion +
                     * scénarios pour préserver la granularité du bilan. */
                } else {
                    /* Aucune procédure scenario_/test_/should_ : mode script
                     * libre.  : Main() n'est plus auto-appelée ; seul
                     * le teardown Fermeture()/Closing() subsiste. */
                    result = call_project_teardown();
                }
                #undef MAX_SCEN
            }
        }
        totalScenarios += fileScenarios;
        totalScenariosFailed += fileScenariosFailed;
        symtable_destroy(testSymbols);
        compile_set_warning_capture(NULL, 0, NULL);

        int pass = testing_get_pass_count();
        int fail = testing_get_fail_count();
        int total = pass + fail;
        totalAssertions += total;
        totalAssertionsFailed += fail;

        /* Capturer warnings (mais ne pas les compter comme échec sauf strict).
         * Les warnings consommés via __dbgAvertissementAttendu (acknowledged)
         * sont silencieusement retirés du bilan global — le test les a
         * explicitement validés comme attendus. */
        for (int w = 0; w < warnCount; w++) {
            if (w < 64 && g_test_warnings_ack[w]) continue;  /* acknowledged */
            if (warnSummaryCount < 512) {
                /*  — la ligne de diagnostic nomme le fichier ou la
                 * declaration se trouve, pas le fichier de test : en mode
                 * multi-fichier les compagnons compilent dans le meme buffer,
                 * et leur numero de ligne annonce sous le nom du test designe
                 * une ligne qui existe mais n'a aucun rapport. Le libelle de la
                 * SUITE, lui, reste le fichier de test (c'est lui qu'on relance). */
                if (warnings[w].origin[0] != '\0') {
                    snprintf(warnSummary[warnSummaryCount], sizeof(warnSummary[0]),
                             "%s:%d — %s",
                             warnings[w].origin, warnings[w].line, warnings[w].message);
                } else {
                    snprintf(warnSummary[warnSummaryCount], sizeof(warnSummary[0]),
                             "%s.owl:%d — %s",
                             project.files[i].name, warnings[w].line, warnings[w].message);
                }
                warnSummaryCount++;
            }
            totalWarnings++;
        }
        /* Reset le binding (le buffer va etre invalide au prochain tour). */
        test_warnings_bind(NULL, 0);

        /* Catégorisation : 4 cas distincts.
         * Un scénario échoué (runtime error capturée par le runner OU
         * assertion fail interne) compte comme échec du fichier : sans ça
         * le fichier s'afficherait ✓ alors que le bilan dit "N FAIL". */
        bool suiteOk = (result == INTERPRET_OK && fail == 0 && fileScenariosFailed == 0);
        bool suiteCrashed = (result != INTERPRET_OK);
        /* suiteAssertFail = result OK mais fail > 0 ou scénario échoué */

        const char* check = "\xE2\x9C\x93";  /* ✓ */
        const char* cross = "\xE2\x9C\x97";  /* ✗ */
        const char* warn  = "\xE2\x9A\xA0";  /* ⚠ */

        /* Warn tag par-fichier : compteur des warnings NON acknowledged
         * uniquement. Les warnings consommés par __dbgAvertissementAttendu
         * sont silencieusement retirés du tag. */
        int warnNotAck = warnCount;
        for (int w = 0; w < warnCount && w < 64; w++) {
            if (g_test_warnings_ack[w]) warnNotAck--;
        }
        /*  — suffixe d'attribution : un CRASH cause par un fichier
         * voisin nomme ce voisin des la ligne du fichier, pas seulement dans le
         * recapitulatif. */
        char crashVia[320] = "";
        if (companionFailPath != NULL) {
            snprintf(crashVia, sizeof crashVia, " via %s", companionFailPath);
        }

        char warnTag[32] = "";
        if (warnNotAck > 0) {
            snprintf(warnTag, sizeof(warnTag), " %s%s%d warn%s%s",
                     C_DIM, C_YELLOW, warnNotAck, warnNotAck > 1 ? "s" : "", C_RESET);
        }

        /* Compact mode (verbosity 0): show only pass/fail status */
        if (opts->verbosity == 0) {
            if (suiteOk) {
                printf("%s%s%s %s.owl (%d assertions)%s\n",
                       C_GREEN, check, C_RESET,
                       project.files[i].name, total, warnTag);
            } else if (suiteCrashed) {
                printf("%s%s%s %s.owl %s(CRASH%s)%s%s\n",
                       C_RED, cross, C_RESET,
                       project.files[i].name, C_RED, crashVia, C_RESET, warnTag);
            } else if (fileScenariosFailed > 0 && fail == 0) {
                printf("%s%s%s %s.owl (%d scenarios FAIL)%s\n",
                       C_RED, cross, C_RESET,
                       project.files[i].name, fileScenariosFailed, warnTag);
            } else {
                printf("%s%s%s %s.owl (%d/%d passed)%s\n",
                       C_RED, cross, C_RESET,
                       project.files[i].name, pass, total, warnTag);
            }
        } else if (!quiet) {
            if (suiteOk) {
                printf("%sOK  %s %s.owl (%d assertions)%s\n",
                       C_GREEN, C_RESET, project.files[i].name, total, warnTag);
            } else if (suiteCrashed) {
                printf("%sFAIL%s %s.owl %s(CRASH%s)%s%s\n",
                       C_RED, C_RESET, project.files[i].name, C_RED, crashVia,
                       C_RESET, warnTag);
            } else if (fileScenariosFailed > 0 && fail == 0) {
                printf("%sFAIL%s %s.owl (%d scenarios FAIL)%s\n",
                       C_RED, C_RESET, project.files[i].name, fileScenariosFailed, warnTag);
            } else {
                printf("%sFAIL%s %s.owl (%d pass, %d fail)%s\n",
                       C_RED, C_RESET, project.files[i].name, pass, fail, warnTag);
            }
        }
        (void)warn;

        if (suiteOk) {
            suitesPassed++;
        } else if (suiteCrashed) {
            suitesCrashed++;
            if (crashCount < 512) {
                crashNames[crashCount] = project.files[i].name;
                /* Message concis : "compile error" ou "runtime error" */
                if (companionFailPath != NULL) {
                    snprintf(crashMsgs[crashCount], sizeof(crashMsgs[0]),
                             "%s dans le fichier voisin %s (voir stderr)",
                             companionFailAtCompile ? "compile error"
                                                    : "runtime error",
                             companionFailPath);
                } else {
                    snprintf(crashMsgs[crashCount], sizeof(crashMsgs[0]), "%s",
                             (fn == NULL) ? "compile error (voir stderr)"
                                          : "runtime error (voir stderr)");
                }
                crashCount++;
            }
            if (opts->failFast) { vm_free(); break; }
        } else if (fail > 0) {
            suitesAssertFail++;
            if (assertFailNamesCount < 512) {
                assertFailNames[assertFailNamesCount] = project.files[i].name;
                assertFailPass[assertFailNamesCount] = pass;
                assertFailFail[assertFailNamesCount] = fail;
                assertFailNamesCount++;
            }
            if (opts->failFast) { vm_free(); break; }
        } else {
            /* Échec scénario pur : la suite n'a ni crashé ni d'assertion en
             * échec, mais ≥1 scénario a renvoyé une erreur runtime. */
            suitesScenarioFail++;
            if (opts->failFast) { vm_free(); break; }
        }

        vm_free();
    }

    int totalSuites = suitesPassed + suitesCrashed + suitesAssertFail + suitesScenarioFail;
    bool strictMode = opts->strict;
    bool warningsFatal = strictMode && totalWarnings > 0;
    bool anyFailure = (suitesCrashed > 0) || (suitesAssertFail > 0)
                      || (totalScenariosFailed > 0) || warningsFatal;

    if (!quiet) {
        printf("\n========================================\n");
        printf("   BILAN: %d suites%s\n", totalSuites,
               opts->filter ? " (filtré)" : "");
        if (suitesPassed > 0) {
            printf("     %s%s%s %3d OK         (%d assertions PASS)\n",
                   C_GREEN, "\xE2\x9C\x93", C_RESET, suitesPassed,
                   totalAssertions - totalAssertionsFailed);
        }
        if (suitesCrashed > 0) {
            printf("     %s%s%s %3d CRASH      (compile / runtime fatal)\n",
                   C_RED, "\xE2\x9C\x97", C_RESET, suitesCrashed);
        }
        if (suitesAssertFail > 0) {
            printf("     %s%s%s %3d FAILURES   (assertions Vérifie* retournant Faux)\n",
                   C_RED, "\xE2\x9C\x97", C_RESET, suitesAssertFail);
        }
        /* Ligne scénarios — uniquement si au moins un fichier en mode
         * procédural (charte §4). Co-existence avec les ~800 tests script
         * libre qui n'incrémentent pas ce compteur. */
        if (totalScenarios > 0) {
            if (totalScenariosFailed == 0) {
                printf("     %s%s%s %3d SCENARIOS  (charte de test §4)\n",
                       C_GREEN, "\xE2\x9C\x93", C_RESET, totalScenarios);
            } else {
                printf("     %s%s%s %3d SCENARIOS  (%d FAIL, charte de test §4)\n",
                       C_RED, "\xE2\x9C\x97", C_RESET, totalScenarios,
                       totalScenariosFailed);
            }
        }
        if (totalWarnings > 0) {
            printf("     %s%s%s %3d WARNINGS   (compile-time, %s%s)\n",
                   C_YELLOW, "\xE2\x9A\xA0", C_RESET, totalWarnings,
                   strictMode ? "fatal en --strict" : "non bloquants",
                   "");
        }

        if (suitesCrashed > 0) {
            printf("\n%sCrashes :%s\n", C_RED, C_RESET);
            int max = crashCount > 20 ? 20 : crashCount;
            for (int k = 0; k < max; k++) {
                printf("  %s%s%s %s.owl — %s\n",
                       C_RED, "\xE2\x9C\x97", C_RESET, crashNames[k], crashMsgs[k]);
            }
            if (crashCount > 20) {
                printf("  %s... et %d autres%s\n", C_DIM, crashCount - 20, C_RESET);
            }
        }
        if (suitesAssertFail > 0) {
            printf("\n%sAssertions failed :%s\n", C_RED, C_RESET);
            int max = assertFailNamesCount > 20 ? 20 : assertFailNamesCount;
            for (int k = 0; k < max; k++) {
                printf("  %s%s%s %s.owl (%d/%d)\n",
                       C_RED, "\xE2\x9C\x97", C_RESET,
                       assertFailNames[k], assertFailPass[k],
                       assertFailPass[k] + assertFailFail[k]);
            }
            if (assertFailNamesCount > 20) {
                printf("  %s... et %d autres%s\n", C_DIM,
                       assertFailNamesCount - 20, C_RESET);
            }
        }
        if (totalWarnings > 0) {
            printf("\n%sWarnings :%s\n", C_YELLOW, C_RESET);
            int max = warnSummaryCount > 30 ? 30 : warnSummaryCount;
            for (int k = 0; k < max; k++) {
                printf("  %s%s%s %s\n", C_YELLOW, "\xE2\x9A\xA0", C_RESET,
                       warnSummary[k]);
            }
            if (warnSummaryCount > 30) {
                printf("  %s... et %d autres%s\n", C_DIM,
                       warnSummaryCount - 30, C_RESET);
            }
        }

        printf("\n");
        if (!anyFailure) {
            /* Stats inline pour avoir le bilan complet sous l'oeil :
             * suites + assertions + (scenarios) + (warnings tolérés). */
            printf("   %sSTATUS: ALL CLEAR%s (%d suites, %d assertions",
                   C_GREEN, C_RESET, totalSuites,
                   totalAssertions - totalAssertionsFailed);
            if (totalScenarios > 0) {
                printf(", %d scenarios", totalScenarios);
            }
            if (totalWarnings > 0) {
                printf(", %d warnings", totalWarnings);
            }
            printf(")\n");
        } else if (warningsFatal && suitesCrashed == 0 && suitesAssertFail == 0
                   && suitesScenarioFail == 0) {
            printf("   %sSTATUS: WARNINGS FATAL (--strict)%s\n",
                   C_YELLOW, C_RESET);
        } else {
            /* Compose le STATUS à partir des catégories en échec présentes :
             * "N CRASHES + N FAILURES + N SCENARIOS FAILED". */
            char status[160] = "";
            int parts = 0;
            if (suitesCrashed > 0) {
                snprintf(status + strlen(status), sizeof(status) - strlen(status),
                         "%s%d CRASHES", parts++ ? " + " : "", suitesCrashed);
            }
            if (suitesAssertFail > 0) {
                snprintf(status + strlen(status), sizeof(status) - strlen(status),
                         "%s%d FAILURES", parts++ ? " + " : "", suitesAssertFail);
            }
            if (suitesScenarioFail > 0) {
                snprintf(status + strlen(status), sizeof(status) - strlen(status),
                         "%s%d SCENARIOS FAILED", parts++ ? " + " : "", suitesScenarioFail);
            }
            printf("   %sSTATUS: %s%s\n", C_RED, status, C_RESET);
        }
        printf("========================================\n");
    } else {
        /* Quiet mode: only show failures + warnings résumé */
        if (anyFailure) {
            fprintf(stderr, "FAILED: %d crash, %d fail, %d scenario, %d warn (%d/%d suites OK, %d/%d assertions)\n",
                    suitesCrashed, suitesAssertFail, suitesScenarioFail, totalWarnings,
                    suitesPassed, totalSuites,
                    totalAssertions - totalAssertionsFailed, totalAssertions);
            for (int k = 0; k < crashCount; k++) {
                fprintf(stderr, "  CRASH %s.owl — %s\n", crashNames[k], crashMsgs[k]);
            }
            for (int k = 0; k < assertFailNamesCount; k++) {
                fprintf(stderr, "  FAIL  %s.owl (%d/%d)\n", assertFailNames[k],
                        assertFailPass[k],
                        assertFailPass[k] + assertFailFail[k]);
            }
        }
    }

    project_free(&project);

    /* Restaure une VM vivante avant de rendre la main.
     *
     * La boucle ci-dessus fait vm_init() par fichier et vm_free() en sortie de
     * chaque fichier : au dernier tour, le process se retrouve SANS registre.
     * Depuis la CLI c'est invisible (le process sort juste apres), mais un
     * appelant in-process — les self-tests C qui drivent `handle_test_command`
     * avec un argv synthetique — perd son registre sous les pieds. Le symptome
     * n'accuse pas la campagne : c'est le PROCHAIN `--check` qui rougit, avec
     * « Type inconnu 'Projet' » et « 'langueFrançais' n'est pas une constante
     * langue officielle » — deux symboles qui vivent dans le registre.
     *
     * Le contrat devient donc : `run_tests_ex` rend la main avec un registre
     * peuple, quel que soit l'etat d'entree. On ne re-initialise que si la
     * boucle a reellement tourne — sinon vm_init() sur une VM intacte
     * re-table_init les globales de l'appelant et fuit ses allocations.
     *
     * Depuis la CLI, ce registre n'est plus libere : un registre en vie au
     * moment du exit(), sur un chemin qui sort dans la foulee. Le rendre a
     * l'OS demanderait de propager jusqu'au dispatch argv l'information
     * « la campagne a-t-elle tourne » — plus de mecanique que de valeur. */
    if (vmConsommee) vm_init();

    /* Code retour : 0 si vraiment ALL CLEAR, sinon non-zéro.
     *   1 = crashes, assertion failures ou scénarios échoués
     *   2 = warnings en mode --strict */
    if (suitesCrashed > 0 || suitesAssertFail > 0 || totalScenariosFailed > 0) return 1;
    if (warningsFatal) return 2;
    return 0;
}

