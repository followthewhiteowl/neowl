// SPDX-License-Identifier: AGPL-3.0-only
/* vm_dispatch_test.c — Tests de coherence dispatch VM.
 *
 * Execute via `owl --self-test-vm-dispatch` (cf main.c). Couvre :
 *   - Coherence structurelle table dispatch ↔ enum OpCode (via OP_COUNT)
 *   - Execution dynamique d'un programme synthétique couvrant toutes
 *     les grandes familles d'opcodes (constants, arith, comparaisons,
 *     control-flow, calls, struct, array) pour garantir qu'aucun ne
 *     tombe sur L_BAD_OP (miroir du default: runtime_error actuel).
 *
 * **STUB PHASE** (test-first strict) : ce test DOIT rougir au commit
 * initial car il reference OP_COUNT qui n'existe pas encore dans chunk.h.
 * C'est la digue sentinelle pour l'implementer — le test ne compilera
 * pas tant que l'enum n'est pas augmente de la sentinelle OP_COUNT.
 *
 * Approche hybrid :
 *   - Compile-time : _Static_assert sur la taille de l'enum (via OP_COUNT).
 *   - Runtime : execution d'un programme minimal qui touche tous les opcodes
 *     connus (ou un sous-ensemble representatif — cf. note ci-dessous).
 *
 * Note granularite couverture : couvrir les 105 opcodes uniques individuellement
 * necessite un programme synthetique enorme (operateurs bitwise, pattern
 * matching string, POO multi-niveau, iterateurs, exceptions, etc.). On se
 * limite ici aux familles principales pour valider le principe du dispatch :
 *   - Constants : OP_CONSTANT, OP_NIL, OP_TRUE, OP_FALSE
 *   - Arithmetique : OP_ADD, OP_MULTIPLY, OP_NEGATE
 *   - Comparaisons : OP_EQUAL, OP_GREATER
 *   - Control-flow : OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP (via TANTQUE)
 *   - Calls : OP_CALL (Trace), OP_RETURN
 *   - Variables : OP_DEFINE_GLOBAL, OP_GET_GLOBAL, OP_SET_GLOBAL
 *   - Struct : OP_CLASS, OP_STRUCT_GET, OP_STRUCT_SET
 *   - Array : OP_ARRAY, OP_ARRAY_GET, OP_ARRAY_SET
 * Cette couverture est representative (17+ opcodes sur 105) sans exploser
 * la taille du test. Si un opcode tombe sur L_BAD_OP malgre l'init range,
 * le test rougit au runtime avec E_RT_UNKNOWN_OPCODE.
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../chunk.h"
#include "../vm.h"
#include "../object.h"
#include "../value.h"
#include "../table.h"

/* ================================================================
 * Compile-time check : sentinelle OP_COUNT
 * ================================================================ */

/* Hard rule : l'implementer doit ajouter OP_COUNT à la fin de l'enum
 * OpCode (chunk.h). Ce _Static_assert verifie que la sentinelle existe
 * ET que sa valeur correspond au nombre reel d'opcodes UNIQUES.
 * Compte actuel AVANT  = **105 opcodes uniques** dans l'enum
 * (mesure 2026-05-30). Le grep brut `case OP_*` dans vm.c renvoie 110
 * parce que 5 opcodes BIT_* (AND/OR/XOR/SHL/SHR) sont ecrits en
 * fall-through (2 `case` par opcode vers le meme corps avec switch
 * interne). En computed-goto, le label-stacking C reproduit le meme
 * comportement : 5 labels distincts pointent vers la meme sequence.
 * Si l'implementer ajoute OP_COUNT mais oublie un opcode dans la table
 * dispatch, la suite de tests runtime reveillera le L_BAD_OP. */
_Static_assert(OP_COUNT == 117,
               "OP_COUNT doit correspondre au nombre reel d'opcodes "
               "uniques (117 au 2026-10-16 : +4 variantes get-or-create JSON "
               "fix/2271 : OP_STRUCT_GET_OR_CREATE_OBJ/ARR + "
               "OP_ARRAY_GET_OR_CREATE_OBJ/ARR, auto-vivification des chemins "
               "JSON profonds en ecriture ; "
               "113 au 2026-08-07 : +OP_ARRAY_FILL_STRUCT fix/1398, "
               "pre-instanciation des cells d'un `tableau de N <Structure>` ; "
               "112 au 2026-08-06 : -OP_CONSTANT_LONG fix/1504, "
               "l'index de constante etant desormais 16 bits sur OP_CONSTANT "
               "lui-meme ; 113 au 2026-08-06 : +OP_TABLE_CELL_DESC feat/1452 ; "
               "112 au 2026-07-24 : +OP_MONCHAMP +OP_MAFENETRE "
               "feat/1220 ; 110 au 2026-07-05, +1 OP_EXC_RESTORE fix/0843). "
               "Si un opcode a ete ajoute, bumper cette assertion "
               "+ verifier que dispatch_table[] couvre bien tous les opcodes "
               "via l'init designee [OP_X]=&&L_OP_X.");

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
 * Test compile-time : OP_COUNT sentinel
 * ================================================================ */

static void test_op_count_sentinel_exists(void) {
    /* Le _Static_assert ci-dessus garantit deja la presence de OP_COUNT.
     * Ce test runtime est redondant mais documente le contrat pour le
     * reviewer : OP_COUNT doit etre >= 1 (au minimum un opcode existe). */
    CHECK(OP_COUNT > 0, "op_count_sentinel_exists");
}

/* ================================================================
 * Test runtime : execution d'un programme synthetique
 * ================================================================ */

static void test_dispatch_coverage_synthetic(void) {
    /* Programme OWL minimal qui force l'emission des opcodes cles (cf.
     * liste familles ci-dessus). Le runner compile + execute ; si un
     * opcode tombe sur L_BAD_OP le runtime hurle E_RT_UNKNOWN_OPCODE. */

    const char* source =
        "// feat/0147 : programme synthetique dispatch VM\n"
        "\n"
        "// Struct global pour OP_CLASS, OP_STRUCT_GET, OP_STRUCT_SET\n"
        "Point EST UNE STRUCTURE\n"
        "  x est un entier\n"
        "  y est un entier\n"
        "FIN\n"
        "\n"
        "PROCEDURE dispatch_coverage_synthetic()\n"
        "  // Constants : OP_CONSTANT, OP_NIL, OP_TRUE, OP_FALSE\n"
        "  val1 est un entier = 42\n"
        "  val2 est un reel = 3.14\n"
        "  flag1 est un booleen = Vrai\n"
        "  flag2 est un booleen = Faux\n"
        "  str1 est chaîne = Null\n"
        "\n"
        "  // Arithmetique : OP_ADD, OP_MULTIPLY, OP_NEGATE\n"
        "  x est un entier = val1 + 10\n"
        "  y est un entier = x * 2\n"
        "  z est un entier = -y\n"
        "\n"
        "  // Comparaisons : OP_EQUAL, OP_GREATER\n"
        "  SI x = 52 ALORS\n"
        "    Trace(\"ok\")\n"
        "  FIN\n"
        "  SI y > 50 ALORS\n"
        "    Trace(\"ok\")\n"
        "  FIN\n"
        "\n"
        "  // Control-flow : OP_LOOP (TANTQUE), OP_JUMP_IF_FALSE\n"
        "  idx est un entier = 0\n"
        "  TANTQUE idx < 3\n"
        "    idx = idx + 1\n"
        "  FIN\n"
        "\n"
        "  // Calls : OP_CALL, OP_RETURN\n"
        "  Trace(\"dispatch ok\")\n"
        "\n"
        "  // Array : OP_ARRAY, OP_ARRAY_SET, OP_ARRAY_GET\n"
        "  arr est un tableau d'entiers = [1, 2, 3]\n"
        "  val est un entier = arr[2]\n"
        "\n"
        "  // Struct instance + field access\n"
        "  p est un Point\n"
        "  p.x = 10\n"
        "  p.y = 20\n"
        "  rx est un entier = p.x\n"
        "FIN\n"
        "\n"
        "dispatch_coverage_synthetic()\n";

    /* Le runner va compiler + executer. InterpretResult doit etre
     * INTERPRET_OK. Si un opcode tombe sur L_BAD_OP (future sentinelle
     * computed-goto) → INTERPRET_RUNTIME_ERROR + E_RT_UNKNOWN_OPCODE. */
    InterpretResult res = vm_interpret(source);
    CHECK(res == INTERPRET_OK, "dispatch_coverage_synthetic_ok");
}

/* ================================================================
 * Test runtime : vm_call_reentrant a frameCount == 0
 * ================================================================
 *
 * Regression du bug d'auto-ouverture GO-mode (`owl FEN_X.owl`).
 * Quand une native s'execute au top-level CLI (vm.frameCount == 0, ex.
 * `Ouvre` lance par go_local_auto_open APRES la fin du script) et appelle
 * vm_call_reentrant pour evaluer une expression (ex. une prop dynamique
 * `Image = icones.add` d'un sous-controle), le OP_RETURN de l'eval tombait
 * dans la branche top-level (exitFrame == 0) qui pop le sentinel script et
 * JETAIT le resultat. La pile rendait alors la valeur posee dessous (ex.
 * l'ObjWindowInstance root) au lieu du resultat de l'eval -> icone jamais
 * appliquee au bouton.
 *
 * Le fix distingue la frontiere reentrante (g_run_reentrant_boundary) du
 * vrai exit top-level : un retour reentrant PUSH toujours son resultat,
 * meme a frame 0. Ce test exerce exactement cette condition : appel
 * reentrant d'une fonction globale alors que vm.frameCount == 0, et verifie
 * que le resultat (et non une valeur fantome de la pile) est bien rendu. */
static void test_reentrant_call_at_frame_zero(void) {
    /* Source : definit une fonction globale qui renvoie une valeur connue,
     * puis termine son top-level (le script se vide, frameCount revient a 0). */
    const char* source =
        "FONCTION reentrant_probe_0658()\n"
        "  RENVOYER 12345\n"
        "FIN\n";
    InterpretResult res = vm_interpret(source);
    CHECK(res == INTERPRET_OK, "reentrant_frame0_compile_run");

    /* A ce stade, le script top-level est termine : aucune frame active.
     * C'est l'etat exact de go_local_auto_open avant d'appeler Ouvre. */
    CHECK(vm.frameCount == 0, "reentrant_frame0_no_active_frame");

    /* Pose une sentinelle sur la pile SOUS l'appel reentrant. Avant le fix,
     * c'est cette valeur (et non le resultat de la fonction) qui etait rendue
     * — fidele a l'ObjWindowInstance qui squattait la pile dans le vrai bug. */
    vm_push(INTEGER_VAL(999));

    ObjString* fnName = obj_copy_string("reentrant_probe_0658", 20);
    Value fn;
    bool found = table_get(&vm.globals, fnName, &fn);
    CHECK(found, "reentrant_frame0_function_found");
    if (!found) return;

    /* Sequence canonique d'appel reentrant : push callee + args, puis call.
     * argCount == 0. Le resultat doit etre laisse au sommet de la pile. */
    vm_push(fn);
    InterpretResult cr = vm_call_reentrant(fn, 0);
    CHECK(cr == INTERPRET_OK, "reentrant_frame0_call_ok");

    Value result = vm_pop();
    CHECK(IS_INTEGER(result) && AS_INTEGER(result) == 12345,
          "reentrant_frame0_result_is_returned_value_not_phantom");

    /* La sentinelle 999 doit etre intacte juste dessous (l'appel reentrant
     * n'a pas devore la pile de l'appelant). */
    Value sentinel = vm_pop();
    CHECK(IS_INTEGER(sentinel) && AS_INTEGER(sentinel) == 999,
          "reentrant_frame0_caller_stack_preserved");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int vm_dispatch_test_main(void) {
    fprintf(stderr, "\n==== VM DISPATCH COHERENCE TESTS (feat/0147) ====\n");

    /* Compile-time : _Static_assert garantit OP_COUNT, mais on documente. */
    test_op_count_sentinel_exists();

    /* Runtime : execute programme synthetique (couvre ~17 opcodes cles). */
    test_dispatch_coverage_synthetic();

    /* Runtime : appel reentrant a frameCount == 0. */
    test_reentrant_call_at_frame_zero();

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "vm-dispatch: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }

    return g_fail;
}
