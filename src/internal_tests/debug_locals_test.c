// SPDX-License-Identifier: AGPL-3.0-only
/* debug_locals_test.c — Tests de resolution des locales par nom dans owl_debug_eval.
 *
 *  test_writer : verifie que owl_debug_eval peut resoudre une variable locale
 * ou un parametre par son NOM dans la frame courante (table de noms de locales dans
 * ObjFunction — fonctionnalite non encore implementee, tests ROUGES attendus).
 *
 * Contexte :  a remplace le mensonge -2147469824 par "(locale non resolvable
 * par nom dans cette version)". Ce ticket apporte la VRAIE resolution.
 * Les tests ci-dessous definissent le contrat de la Voie B.
 *
 *  test_writer : expose le bug de recyclage de slot. end_scope decremente
 * localCount (les slots sont recycles) mais localNames garde toutes les entrees.
 * Deux SI sequentiels avec des locales differentes tombent sur le meme slot ;
 * local_name_at (first-match) retourne le nom de la premiere entree (scope perime).
 * Tests 8-11 couvrent ce cas.
 *
 * Statut attendu :
 *   ROUGE avant impl  (tests 1-5 : resolution locales/params/ombrage/noms)
 *   VERT  apres impl  implementer
 *   Tests 6-7 : VERT des maintenant (comportements hors-pause deja corrects)
 *   Test 8    : ROUGE avant impl  (nom perime affiche pour slot recycle)
 *   Tests 9-11: VERT des maintenant (non-regression : cas sans bug actif)
 *
 * Tests :
 *   1. Locale chaine   : eval("sTexte") dans une frame avec sTexte="abc" -> "abc"
 *   2. Locale entier   : eval("nCompte") avec nCompte=42 -> "42"
 *   3. Parametre       : eval("nParam") avec nParam=77 (arg passe) -> "77"
 *   4. Ombrage local>global : locale gValeur=123, globale gValeur=999 -> "123"
 *   5. Panneau vars    : owl_debug_get_locals retourne des vrais noms (pas "slot N")
 *   6. Hors-pause      : owl_debug_eval hors DEBUG_PAUSED -> retourne false
 *   7. Nom inconnu     : eval d'un nom vraiment absent -> message honnete (pas crash)
 *   8. Slot recycle    : pause dans SI2 -> get_locals nomme le slot "nScopeB" (ROUGE)
 *   9. Slot non recycle : pause dans SI1 -> get_locals nomme le slot "nScopeA" (vert)
 *  10. Hover dans SI2  : eval("nScopeB") retourne 20 (non-regression, vert)
 *  11. Cas simple      : une seule locale, nom toujours correct (non-regression, vert)
 *
 * Approche harnais : on compile+execute un programme OWL synthetique avec un
 * breakpoint sur une ligne precise. Le pause_callback est appele AVANT la boucle
 * busy-wait (debug_api.c:266), dans le meme thread ; on fait les assertions, puis
 * on appelle owl_debug_continue() pour sortir sans bloquer.
 *
 * Branche sur `owl --self-test-debug-locals` (cf src/main.c).
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "../debug_api.h"
#include "../vm.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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

#define CHECK_STR_EQ(actual, expected, label) do { \
    if (strcmp((actual), (expected)) == 0) { g_pass++; } \
    else { \
        g_fail++; \
        fprintf(stderr, "  FAIL [%s] at line %d: got \"%s\", want \"%s\"\n", \
                (label), __LINE__, (actual), (expected)); \
    } \
} while (0)

/* ================================================================
 * Fixture OWL synthetique
 *
 * Lignes numerotees pour le breakpoint :
 *   1  // debut fixture 
 *   2  (vide)
 *   3  PROCEDURE TestLocales(nParam est un entier)
 *   4    sTexte est une chaine = "abc"
 *   5    nCompte est un entier = 42
 *   6    nUsed est un entier = nParam + 0
 *   7    Trace("pause ici")   <- BREAKPOINT ligne 7
 *   8  FIN
 *   9  (vide)
 *  10  TestLocales(77)
 *
 * Pas de globale homonyme avec une locale : OWL refuse la redeclaration
 * d'une locale qui masque une globale (erreur compile volontaire, hors
 * scope ). Le test d'ombrage est remplace par le test d'une
 * locale nUsed derivee du parametre.
 *
 * Le breakpoint est pose sur la ligne 7 (Trace), premiere instruction
 * executee apres que toutes les locales sont initialisees.
 * ================================================================ */

#define FIXTURE_BREAKPOINT_LINE 7

static const char* g_fixture_source =
    "// debut fixture feat/0523\n"                           /* ligne 1  */
    "\n"                                                      /* ligne 2  */
    "PROCEDURE TestLocales(nParam est un entier)\n"           /* ligne 3  */
    "  sTexte est une chaine = \"abc\"\n"                     /* ligne 4  */
    "  nCompte est un entier = 42\n"                          /* ligne 5  */
    "  nUsed est un entier = nParam + 0\n"                    /* ligne 6  */
    "  Trace(\"pause ici\")\n"                                /* ligne 7  <- BREAKPOINT */
    "FIN\n"                                                   /* ligne 8  */
    "\n"                                                      /* ligne 9  */
    "TestLocales(77)\n";                                      /* ligne 10 */

/* ================================================================
 * Resultats collectes par le callback (partages entre callback et
 * la fonction de verification post-interpret). On ne peut pas faire
 * de CHECK() depuis le callback car les macros ecrivent sur stderr
 * et il faudrait des g_pass/g_fail thread-locaux — on garde simple :
 * copie de strings dans des buffers statiques, verifie apres retour.
 * ================================================================ */

#define RES_BUF 256

static char g_res_sTexte_val[RES_BUF];
static char g_res_sTexte_type[RES_BUF];
static bool g_res_sTexte_ok;

static char g_res_nCompte_val[RES_BUF];
static char g_res_nCompte_type[RES_BUF];
static bool g_res_nCompte_ok;

static char g_res_nParam_val[RES_BUF];
static char g_res_nParam_type[RES_BUF];
static bool g_res_nParam_ok;

/* nUsed : locale derivee du param (verifie que les locales non-param sont aussi
 * resolues par nom, pas seulement les parametres). Valeur attendue : 77 (= nParam+0). */
static char g_res_nUsed_val[RES_BUF];
static char g_res_nUsed_type[RES_BUF];
static bool g_res_nUsed_ok;

/* Nom du premier slot non-Procedure retourne par get_locals (test 5). */
static char g_res_first_local_name[RES_BUF];
static int  g_res_locals_count;

/* Indicateur : le callback a bien ete appele. */
static bool g_callback_called = false;

/* ================================================================
 * Pause callback : appele depuis owl_debug_hook AVANT le busy-wait.
 * Meme thread que vm_interpret. On collecte les resultats et on
 * continue immediatement (pas de blocage).
 * ================================================================ */

static void pause_cb(const char* file, int line, const char* reason) {
    (void)file; (void)line; (void)reason;
    g_callback_called = true;

    /* Test 1 : locale chaine sTexte */
    g_res_sTexte_ok = owl_debug_eval("sTexte", g_res_sTexte_val, g_res_sTexte_type);

    /* Test 2 : locale entier nCompte */
    g_res_nCompte_ok = owl_debug_eval("nCompte", g_res_nCompte_val, g_res_nCompte_type);

    /* Test 3 : parametre nParam */
    g_res_nParam_ok = owl_debug_eval("nParam", g_res_nParam_val, g_res_nParam_type);

    /* Test 4 : locale nUsed = nParam + 0 = 77.
     * Verifie que les locales (non-param) sont aussi resolues par nom.
     * Apres  : doit retourner "77". */
    g_res_nUsed_ok = owl_debug_eval("nUsed", g_res_nUsed_val, g_res_nUsed_type);

    /* Test 5 : panneau variables — vrais noms dans get_locals.
     * Apres  : le premier slot non-Procedure doit avoir un vrai nom,
     * pas "slot N". */
    OwlDebugVariable locals[16];
    owl_debug_get_locals(0, locals, 16, &g_res_locals_count);
    if (g_res_locals_count > 0) {
        /* Premier nom attendu : "nParam" (parametre en slot 0 dans la convention
         * appelante OWL, apres le slot implicit fonction). */
        strncpy(g_res_first_local_name, locals[0].name, RES_BUF - 1);
        g_res_first_local_name[RES_BUF - 1] = '\0';
    } else {
        g_res_first_local_name[0] = '\0';
    }

    /* Sortie sans bloquer. */
    owl_debug_continue();
}

/* ================================================================
 * Test 1-5 : resolution dans la frame
 * ================================================================ */

static void test_eval_in_frame(void) {
    /* Reset resultats. */
    g_callback_called = false;
    memset(g_res_sTexte_val,   0, RES_BUF);
    memset(g_res_nCompte_val,  0, RES_BUF);
    memset(g_res_nParam_val,   0, RES_BUF);
    memset(g_res_nUsed_val,    0, RES_BUF);
    memset(g_res_first_local_name, 0, RES_BUF);

    /* Pose breakpoint wildcard (file=NULL) sur la ligne cible. */
    owl_debug_set_breakpoint(NULL, FIXTURE_BREAKPOINT_LINE);

    /* Mode debug ON. */
    owl_debug_attach();

    /* Installe le callback qui fait les assertions. */
    owl_debug_set_pause_callback(pause_cb);

    /* Interprete le programme. La VM s'arrete sur le breakpoint,
     * appelle pause_cb, puis continue. */
    InterpretResult r = vm_interpret(g_fixture_source);

    /* Detache + nettoie. */
    owl_debug_detach();
    owl_debug_clear_all_breakpoints();
    owl_debug_set_pause_callback(NULL);

    /* Verifie que l'execution s'est bien passee. */
    CHECK(r == INTERPRET_OK, "fixture_interpret_ok");

    /* Verifie que le callback a bien ete declenche. */
    CHECK(g_callback_called, "pause_callback_called");

    /* Test 1 : sTexte */
    CHECK(g_res_sTexte_ok, "eval_sTexte_returns_true");
    CHECK_STR_EQ(g_res_sTexte_val, "\"abc\"", "eval_sTexte_value_abc");
    CHECK_STR_EQ(g_res_sTexte_type, "chaine", "eval_sTexte_type_chaine");

    /* Test 2 : nCompte */
    CHECK(g_res_nCompte_ok, "eval_nCompte_returns_true");
    CHECK_STR_EQ(g_res_nCompte_val, "42", "eval_nCompte_value_42");
    CHECK_STR_EQ(g_res_nCompte_type, "entier", "eval_nCompte_type_entier");

    /* Test 3 : nParam (parametre) */
    CHECK(g_res_nParam_ok, "eval_nParam_returns_true");
    CHECK_STR_EQ(g_res_nParam_val, "77", "eval_nParam_value_77");
    CHECK_STR_EQ(g_res_nParam_type, "entier", "eval_nParam_type_entier");

    /* Test 4 : locale nUsed = nParam + 0 = 77.
     * Verifie que les locales ordinaires (non-param) sont resolues par nom. */
    CHECK(g_res_nUsed_ok, "eval_nUsed_returns_true");
    CHECK_STR_EQ(g_res_nUsed_val, "77", "eval_nUsed_value_77");
    CHECK_STR_EQ(g_res_nUsed_type, "entier", "eval_nUsed_type_entier");

    /* Test 5 : get_locals donne des vrais noms.
     * Avant  : g_res_first_local_name = "slot N" -> test rouge.
     * Apres   : g_res_first_local_name = "nParam" (1er parametre). */
    CHECK(g_res_locals_count > 0, "get_locals_nonempty");
    /* strncmp avec "slot" pour detecter le pattern "slot N" qui doit disparaitre. */
    CHECK(strncmp(g_res_first_local_name, "slot", 4) != 0, "first_local_not_slot_N");
    CHECK_STR_EQ(g_res_first_local_name, "nParam", "first_local_name_is_nParam");
}

/* ================================================================
 * Test 6 : hors pause — owl_debug_eval retourne false immediatement
 * ================================================================ */

static void test_eval_out_of_pause(void) {
    /* S'assurer qu'on est bien hors pause (mode DEBUG_OFF apres vm_free/init). */
    char val[RES_BUF], typ[RES_BUF];
    bool r = owl_debug_eval("sTexte", val, typ);

    /* Contrat : retourne false quand VM pas en pause. */
    CHECK(!r, "eval_hors_pause_retourne_false");
}

/* ================================================================
 * Test 7 : nom vraiment inconnu dans la frame
 * Avant  : eval("__inconnu__") retournait "(locale non resolvable)"
 * Apres   : resout les locales d'abord, puis si absent -> erreur compile
 *                    ("__inconnu__" n'est ni local ni global -> erreur honnete).
 * ================================================================ */

typedef struct {
    char val[RES_BUF];
    char typ[RES_BUF];
    bool ok;
} UnknownResult;

static UnknownResult g_unknown_result;

static void pause_cb_unknown(const char* file, int line, const char* reason) {
    (void)file; (void)line; (void)reason;
    g_unknown_result.ok = owl_debug_eval(
        "__inconnu_feat0523__",
        g_unknown_result.val,
        g_unknown_result.typ
    );
    owl_debug_continue();
}

static void test_eval_unknown_name(void) {
    memset(&g_unknown_result, 0, sizeof g_unknown_result);

    owl_debug_set_breakpoint(NULL, FIXTURE_BREAKPOINT_LINE);
    owl_debug_attach();
    owl_debug_set_pause_callback(pause_cb_unknown);

    InterpretResult r = vm_interpret(g_fixture_source);

    owl_debug_detach();
    owl_debug_clear_all_breakpoints();
    owl_debug_set_pause_callback(NULL);

    CHECK(r == INTERPRET_OK, "fixture_interpret_ok_unknown_test");

    /* Un nom vraiment absent : owl_debug_eval doit retourner false
     * (compile error "symbole inconnu"), pas crash, pas message trompeuse.
     * Le val doit contenir un message d'erreur lisible (pas vide, pas "(locale
     * non resolvable)"). */
    CHECK(!g_unknown_result.ok, "eval_unknown_retourne_false");
    CHECK(strlen(g_unknown_result.val) > 0, "eval_unknown_val_non_vide");
    /* Ne doit PAS retourner le message de l'ancien comportement 
     * qui detectait les sentinelles. Apres , un nom absent en local
     * qui n'existe pas non plus en global -> compile error honnete. */
    CHECK(strncmp(g_unknown_result.val, "(locale non resolvable", 22) != 0,
          "eval_unknown_message_ne_parle_plus_de_non_resolvable");
}

/* ================================================================
 *  — Fixture : deux SI sequentiels, locales sur le meme slot
 *
 * Lignes numerotees pour les breakpoints :
 *   1  // fixture  slot recycle
 *   2  (vide)
 *   3  PROCEDURE TestRecycle(bCond est un booleen)
 *   4    SI bCond ALORS
 *   5      nScopeA est un entier = 10
 *   6      Trace("scope A")      <- BREAKPOINT_SCOPE_A (ligne 6)
 *   7    FIN
 *   8    SI Vrai ALORS
 *   9      nScopeB est un entier = 20
 *  10      Trace("scope B")      <- BREAKPOINT_SCOPE_B (ligne 10)
 *  11    FIN
 *  12  FIN
 *  13  (vide)
 *  14  TestRecycle(Faux)         <- appel qui passe UNIQUEMENT par scope B
 *
 * Compile-time : nScopeA et nScopeB ont le meme slot (end_scope recycle).
 * Bug  : local_name_at(fn, slot_nScopeB) -> "nScopeA" (first-match).
 * Apres fix   : local_name_at(fn, slot_nScopeB, ip_pause) -> "nScopeB".
 *
 * On utilise deux sources distinctes pour les deux sous-tests :
 *  - g_fixture_recycle_source_b : appel TestRecycle(Faux) -> pause dans scope B
 *  - g_fixture_recycle_source_a : appel TestRecycle(Vrai) -> pause dans scope A
 * ================================================================ */

/* Partie commune de la fixture (lignes 1-12). */
#define FIXTURE_RECYCLE_PROC \
    "// fixture fix/0555 slot recycle\n"               /* ligne 1  */ \
    "\n"                                               /* ligne 2  */ \
    "PROCEDURE TestRecycle(bCond est un booleen)\n"    /* ligne 3  */ \
    "  SI bCond ALORS\n"                               /* ligne 4  */ \
    "    nScopeA est un entier = 10\n"                 /* ligne 5  */ \
    "    Trace(\"scope A\")\n"                         /* ligne 6  <- BREAKPOINT_SCOPE_A */ \
    "  FIN\n"                                          /* ligne 7  */ \
    "  SI Vrai ALORS\n"                                /* ligne 8  */ \
    "    nScopeB est un entier = 20\n"                 /* ligne 9  */ \
    "    Trace(\"scope B\")\n"                         /* ligne 10 <- BREAKPOINT_SCOPE_B */ \
    "  FIN\n"                                          /* ligne 11 */ \
    "FIN\n"                                            /* ligne 12 */

/* Source B : entre par scope B uniquement (bCond=Faux). */
static const char* g_fixture_recycle_source_b =
    FIXTURE_RECYCLE_PROC
    "\n"                                               /* ligne 13 */
    "TestRecycle(Faux)\n";                             /* ligne 14 */

/* Source A : entre par scope A puis scope B (bCond=Vrai). */
static const char* g_fixture_recycle_source_a =
    FIXTURE_RECYCLE_PROC
    "\n"                                               /* ligne 13 */
    "TestRecycle(Vrai)\n";                             /* ligne 14 */

#define BREAKPOINT_SCOPE_A 6
#define BREAKPOINT_SCOPE_B 10

/* ================================================================
 * Resultats collectes par les callbacks 
 *
 * On stocke tous les locals (jusqu'a 16) et on cherche le slot dont
 * la valeur correspond a ce qu'on attend (20 pour nScopeB, 10 pour
 * nScopeA). C'est plus robuste que l'ordre d'index car le parametre
 * bCond est toujours en premier slot non-fonction.
 * ================================================================ */

#define MAX_LOCALS_CAPTURED 16

/* Paire nom/valeur capturee depuis get_locals. */
typedef struct {
    char name[RES_BUF];
    char value[RES_BUF];
} CapturedLocal;

/* Test 8 et 10 : pause dans scope B. */
static CapturedLocal g_scope_b_locals[MAX_LOCALS_CAPTURED];
static int           g_scope_b_locals_count;
static char g_scope_b_eval_nScopeB_val[RES_BUF];
static char g_scope_b_eval_nScopeB_type[RES_BUF];
static bool g_scope_b_eval_nScopeB_ok;
static bool g_scope_b_callback_called = false;

/* Test 9 : pause dans scope A. */
static CapturedLocal g_scope_a_locals[MAX_LOCALS_CAPTURED];
static int           g_scope_a_locals_count;
static bool g_scope_a_callback_called = false;

/* Test 11 : cas simple. */
static CapturedLocal g_simple_locals[MAX_LOCALS_CAPTURED];
static int           g_simple_locals_count;
static bool g_simple_callback_called = false;

/* Helper : cherche parmi les locals captures celui dont la valeur
 * correspond a `val`. Retourne le nom ou NULL si absent. */
static const char* find_local_name_by_value(
        const CapturedLocal* arr, int count, const char* val) {
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i].value, val) == 0) return arr[i].name;
    }
    return NULL;
}

/* ================================================================
 * Callbacks 
 * ================================================================ */

static void pause_cb_scope_b(const char* file, int line, const char* reason) {
    (void)file; (void)line; (void)reason;
    g_scope_b_callback_called = true;

    OwlDebugVariable locals[MAX_LOCALS_CAPTURED];
    int n = 0;
    owl_debug_get_locals(0, locals, MAX_LOCALS_CAPTURED, &n);
    g_scope_b_locals_count = n < MAX_LOCALS_CAPTURED ? n : MAX_LOCALS_CAPTURED;
    for (int i = 0; i < g_scope_b_locals_count; i++) {
        strncpy(g_scope_b_locals[i].name,  locals[i].name,  RES_BUF - 1);
        strncpy(g_scope_b_locals[i].value, locals[i].value, RES_BUF - 1);
        g_scope_b_locals[i].name[RES_BUF - 1]  = '\0';
        g_scope_b_locals[i].value[RES_BUF - 1] = '\0';
    }

    /* Hover : eval("nScopeB") doit retourner 20 (non-regression). */
    g_scope_b_eval_nScopeB_ok = owl_debug_eval(
        "nScopeB",
        g_scope_b_eval_nScopeB_val,
        g_scope_b_eval_nScopeB_type
    );

    owl_debug_continue();
}

static void pause_cb_scope_a(const char* file, int line, const char* reason) {
    (void)file; (void)line; (void)reason;
    g_scope_a_callback_called = true;

    OwlDebugVariable locals[MAX_LOCALS_CAPTURED];
    int n = 0;
    owl_debug_get_locals(0, locals, MAX_LOCALS_CAPTURED, &n);
    g_scope_a_locals_count = n < MAX_LOCALS_CAPTURED ? n : MAX_LOCALS_CAPTURED;
    for (int i = 0; i < g_scope_a_locals_count; i++) {
        strncpy(g_scope_a_locals[i].name,  locals[i].name,  RES_BUF - 1);
        strncpy(g_scope_a_locals[i].value, locals[i].value, RES_BUF - 1);
        g_scope_a_locals[i].name[RES_BUF - 1]  = '\0';
        g_scope_a_locals[i].value[RES_BUF - 1] = '\0';
    }

    owl_debug_continue();
}

/* ================================================================
 * Fixture simple (cas sans recyclage, test 11)
 *
 *   1  // fixture  simple
 *   2  (vide)
 *   3  PROCEDURE TestSimple()
 *   4    nUnique est un entier = 99
 *   5    Trace("pause unique")   <- BREAKPOINT_SIMPLE (ligne 5)
 *   6  FIN
 *   7  (vide)
 *   8  TestSimple()
 * ================================================================ */

#define FIXTURE_SIMPLE_BREAKPOINT 5

static const char* g_fixture_simple_source =
    "// fixture fix/0555 simple\n"                     /* ligne 1 */
    "\n"                                               /* ligne 2 */
    "PROCEDURE TestSimple()\n"                         /* ligne 3 */
    "  nUnique est un entier = 99\n"                   /* ligne 4 */
    "  Trace(\"pause unique\")\n"                      /* ligne 5  <- BREAKPOINT_SIMPLE */
    "FIN\n"                                            /* ligne 6 */
    "\n"                                               /* ligne 7 */
    "TestSimple()\n";                                  /* ligne 8 */

static void pause_cb_simple(const char* file, int line, const char* reason) {
    (void)file; (void)line; (void)reason;
    g_simple_callback_called = true;

    OwlDebugVariable locals[MAX_LOCALS_CAPTURED];
    int n = 0;
    owl_debug_get_locals(0, locals, MAX_LOCALS_CAPTURED, &n);
    g_simple_locals_count = n < MAX_LOCALS_CAPTURED ? n : MAX_LOCALS_CAPTURED;
    for (int i = 0; i < g_simple_locals_count; i++) {
        strncpy(g_simple_locals[i].name,  locals[i].name,  RES_BUF - 1);
        strncpy(g_simple_locals[i].value, locals[i].value, RES_BUF - 1);
        g_simple_locals[i].name[RES_BUF - 1]  = '\0';
        g_simple_locals[i].value[RES_BUF - 1] = '\0';
    }

    owl_debug_continue();
}

/* ================================================================
 * Test 8 (ROUGE attendu avant ) :
 * Pause dans scope B (SI2). local_name_at retourne "nScopeA" (first-match
 * perime) au lieu de "nScopeB". Apres  : "nScopeB".
 * ================================================================ */

static void test_slot_recycle_scope_b(void) {
    g_scope_b_callback_called = false;
    memset(g_scope_b_locals, 0, sizeof g_scope_b_locals);
    memset(g_scope_b_eval_nScopeB_val, 0, RES_BUF);
    memset(g_scope_b_eval_nScopeB_type, 0, RES_BUF);
    g_scope_b_locals_count = 0;

    /* Breakpoint sur la ligne dans SI2 (scope B). bCond=Faux : seul scope B
     * est execute, nScopeA ne peuple jamais le slot au runtime. Le bug est
     * purement dans les metadonnees de debug (localNames first-match). */
    owl_debug_set_breakpoint(NULL, BREAKPOINT_SCOPE_B);
    owl_debug_attach();
    owl_debug_set_pause_callback(pause_cb_scope_b);

    InterpretResult r = vm_interpret(g_fixture_recycle_source_b);

    owl_debug_detach();
    owl_debug_clear_all_breakpoints();
    owl_debug_set_pause_callback(NULL);

    CHECK(r == INTERPRET_OK, "recycle_scope_b_interpret_ok");
    CHECK(g_scope_b_callback_called, "recycle_scope_b_callback_called");

    /* Test 8 (ROUGE avant ) :
     * Bug : local_name_at first-match retourne "nScopeA" meme quand on est
     * dans scope B. get_locals expose ce nom errone dans le panneau Variables.
     * On recherche le slot dont la valeur est "20" (= nScopeB = 20) et on
     * verifie que son nom est "nScopeB" pas "nScopeA".
     * Apres fix : le nom retourne sera "nScopeB". */
    CHECK(g_scope_b_locals_count > 0, "recycle_scope_b_locals_nonempty");
    {
        const char* nameOf20 = find_local_name_by_value(
            g_scope_b_locals, g_scope_b_locals_count, "20");
        /* nameOf20 doit exister : nScopeB est dans le scope et vaut 20. */
        CHECK(nameOf20 != NULL, "recycle_scope_b_slot_value_20_found");
        if (nameOf20) {
            /* ROUGE avant  : nameOf20 = "nScopeA" (first-match perime). */
            CHECK_STR_EQ(nameOf20, "nScopeB",
                         "recycle_scope_b_slot_named_nScopeB");
        }
    }
}

/* ================================================================
 * Test 9 (VERT attendu, non-regression) :
 * Pause dans scope A (SI1). local_name_at doit retourner "nScopeA".
 * Verifie que le fix ne casse pas le cas symetrique.
 * ================================================================ */

static void test_slot_recycle_scope_a(void) {
    g_scope_a_callback_called = false;
    memset(g_scope_a_locals, 0, sizeof g_scope_a_locals);
    g_scope_a_locals_count = 0;

    /* Breakpoint sur la ligne dans SI1 (scope A). bCond=Vrai : scope A execute. */
    owl_debug_set_breakpoint(NULL, BREAKPOINT_SCOPE_A);
    owl_debug_attach();
    owl_debug_set_pause_callback(pause_cb_scope_a);

    InterpretResult r = vm_interpret(g_fixture_recycle_source_a);

    owl_debug_detach();
    owl_debug_clear_all_breakpoints();
    owl_debug_set_pause_callback(NULL);

    CHECK(r == INTERPRET_OK, "recycle_scope_a_interpret_ok");
    CHECK(g_scope_a_callback_called, "recycle_scope_a_callback_called");

    /* Test 9 (VERT attendu) : dans scope A, le slot dont la valeur est "10"
     * (nScopeA = 10) doit etre nomme "nScopeA". Ce cas n'est pas affecte par
     * le bug (first-match trouve nScopeA en premier, ce qui est correct ici). */
    CHECK(g_scope_a_locals_count > 0, "recycle_scope_a_locals_nonempty");
    {
        const char* nameOf10 = find_local_name_by_value(
            g_scope_a_locals, g_scope_a_locals_count, "10");
        CHECK(nameOf10 != NULL, "recycle_scope_a_slot_value_10_found");
        if (nameOf10) {
            CHECK_STR_EQ(nameOf10, "nScopeA",
                         "recycle_scope_a_slot_named_nScopeA");
        }
    }
}

/* ================================================================
 * Test 10 (VERT attendu, non-regression hover) :
 * eval("nScopeB") dans scope B retourne 20.
 * Le hover par nom (owl_debug_eval) n'est pas affecte par le bug
 * (il cherche par nom, pas par slot). Deja correct avant .
 * Resultat collecte dans pause_cb_scope_b (meme pause que test 8).
 * Ce test exploite g_fixture_recycle_source_b deja execute en test 8.
 * ATTENTION : test 8 et test 10 partagent le meme interpret (test 8).
 * Les resultats sont deja dans g_scope_b_eval_* — pas de 2e interpret.
 * ================================================================ */

static void test_hover_nScopeB_in_scope_b(void) {
    /* Les resultats viennent du meme interpret que test 8 (test_slot_recycle_scope_b).
     * Appeler cette fonction APRES test_slot_recycle_scope_b. */
    CHECK(g_scope_b_callback_called,
          "hover_scope_b_callback_called_prerequisite");
    CHECK(g_scope_b_eval_nScopeB_ok,
          "hover_eval_nScopeB_returns_true");
    CHECK_STR_EQ(g_scope_b_eval_nScopeB_val, "20",
                 "hover_eval_nScopeB_value_20");
    CHECK_STR_EQ(g_scope_b_eval_nScopeB_type, "entier",
                 "hover_eval_nScopeB_type_entier");
}

/* ================================================================
 * Test 11 (VERT attendu, non-regression simple) :
 * Un seul scope, une seule locale "nUnique". get_locals retourne
 * "nUnique". Verifie que la mecanique de base reste intacte.
 * ================================================================ */

static void test_simple_no_recycle(void) {
    g_simple_callback_called = false;
    memset(g_simple_locals, 0, sizeof g_simple_locals);
    g_simple_locals_count = 0;

    owl_debug_set_breakpoint(NULL, FIXTURE_SIMPLE_BREAKPOINT);
    owl_debug_attach();
    owl_debug_set_pause_callback(pause_cb_simple);

    InterpretResult r = vm_interpret(g_fixture_simple_source);

    owl_debug_detach();
    owl_debug_clear_all_breakpoints();
    owl_debug_set_pause_callback(NULL);

    CHECK(r == INTERPRET_OK, "simple_no_recycle_interpret_ok");
    CHECK(g_simple_callback_called, "simple_no_recycle_callback_called");

    /* Test 11 (VERT attendu) : la seule locale non-vide vaut 99. Son nom
     * doit etre "nUnique". Pas de recyclage possible = nom toujours correct. */
    CHECK(g_simple_locals_count > 0, "simple_no_recycle_locals_nonempty");
    {
        const char* nameOf99 = find_local_name_by_value(
            g_simple_locals, g_simple_locals_count, "99");
        CHECK(nameOf99 != NULL, "simple_no_recycle_slot_value_99_found");
        if (nameOf99) {
            CHECK_STR_EQ(nameOf99, "nUnique",
                         "simple_no_recycle_slot_named_nUnique");
        }
    }
}

/* ================================================================
 * Entry point
 * ================================================================ */

int debug_locals_test_main(void) {
    fprintf(stderr, "\n==== DEBUG LOCALS RESOLUTION TESTS (feat/0523) ====\n");
    fprintf(stderr, "Statut attendu AVANT impl : ROUGE (tests 1-5 + partie 7)\n");
    fprintf(stderr, "Statut attendu APRES impl : VERT\n\n");

    /* Test 6 : hors pause, aucun interpret necessaire. */
    test_eval_out_of_pause();

    /* Tests 1-5 : require un interpret + breakpoint. */
    test_eval_in_frame();

    /* Test 7 : nom inconnu dans la frame. */
    test_eval_unknown_name();

    fprintf(stderr, "\n-- fix/0555 : recyclage de slot --\n");
    fprintf(stderr, "Test 8 ROUGE attendu (bug slot recycle), tests 9-11 VERT\n\n");

    /* Test 8 (ROUGE) + test 10 (VERT, meme interpret) : pause scope B. */
    test_slot_recycle_scope_b();
    test_hover_nScopeB_in_scope_b();

    /* Test 9 (VERT) : pause scope A. */
    test_slot_recycle_scope_a();

    /* Test 11 (VERT) : cas simple sans recyclage. */
    test_simple_no_recycle();

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "debug-locals: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }

    return g_fail;
}
