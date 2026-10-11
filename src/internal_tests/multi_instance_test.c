// SPDX-License-Identifier: AGPL-3.0-only
/* multi_instance_test.c — Tests structures multi-instance fenetre + stack.
 *
 *  test_writer : verification des 3 nouvelles structures Obj +
 * stack VM pour le chantier multi-instance fenetre (parent ).
 *
 * Statut attendu :
 *   - ROUGE compile-error (struct/fn pas encore declares)
 *   - VERT apres impl  implementer
 *
 * Tests :
 *   1. Allocation — ObjWindowInstance / ObjUiControlInstance / ObjUiControlRef
 *   2. Stack VM — push/pop/top/count/clear
 *   3. GC marking — 100+ instances + full GC cycle sans crash
 *
 * Cf. ADR_NNNN (sera ecrit en cloture , voir ticket). */

#include "../object.h"
#include "../vm.h"
#include "../memory.h"
#include "../ui_decl.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Mock decls pour eviter toute dependance sur compiler state. */
static OwlWindowDecl g_mock_window_decl = {
    .obj = { .type = OBJ_CLASS },  /* dummy type pour eviter NULL deref */
    .name = "MockFenetre",
    /* autres champs defaults a zero — pas important pour les tests alloc */
};

static OwlControlDecl g_mock_control_decl = {
    .kind = OWL_CONTROL_INPUT,  /* INPUT au lieu de CHAMP_SAISIE */
    .name = "MockChamp",
    /* autres champs defaults */
};

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
 * Test 1 : Allocation des 3 nouvelles structures Obj
 * ================================================================ */

static void test_obj_allocation(void) {
    /* 1.1 — ObjWindowInstance */
    ObjWindowInstance* win_inst = obj_new_window_instance(&g_mock_window_decl);
    CHECK(win_inst != NULL, "window_instance_non_null");
    CHECK(win_inst->obj.type == OBJ_WINDOW_INSTANCE, "window_instance_type");
    CHECK(win_inst->decl == &g_mock_window_decl, "window_instance_decl_ref");
    CHECK(win_inst->widget == NULL, "window_instance_widget_null_initial");
    CHECK(win_inst->instance_id >= 0, "window_instance_id_valid");
    CHECK(win_inst->controls == NULL, "window_instance_controls_null_initial");
    CHECK(win_inst->control_count == 0, "window_instance_control_count_zero");
    CHECK(IS_NIL(win_inst->valeur_renvoyee), "window_instance_valeur_renvoyee_nil");

    /* 1.2 — ObjUiControlInstance */
    ObjUiControlInstance* ctrl_inst = obj_new_ui_control_instance(&g_mock_control_decl, win_inst);
    CHECK(ctrl_inst != NULL, "control_instance_non_null");
    CHECK(ctrl_inst->obj.type == OBJ_UI_CONTROL_INSTANCE, "control_instance_type");
    CHECK(ctrl_inst->decl == &g_mock_control_decl, "control_instance_decl_ref");
    CHECK(ctrl_inst->widget == NULL, "control_instance_widget_null_initial");
    CHECK(ctrl_inst->headless_store == NULL, "control_instance_headless_store_null_initial");
    CHECK(ctrl_inst->owner == win_inst, "control_instance_owner_back_ref");

    /* 1.3 — ObjUiControlRef */
    ObjUiControlRef* ctrl_ref = obj_new_ui_control_ref(&g_mock_control_decl);
    CHECK(ctrl_ref != NULL, "control_ref_non_null");
    CHECK(ctrl_ref->obj.type == OBJ_UI_CONTROL_REF, "control_ref_type");
    CHECK(ctrl_ref->decl == &g_mock_control_decl, "control_ref_decl_ref");
    CHECK(ctrl_ref->kind == OWL_CONTROL_INPUT, "control_ref_kind_cache");
}

/* ================================================================
 * Test 2 : Stack VM — push/pop/top/count/clear
 * ================================================================ */

static void test_vm_window_stack(void) {
    /* 2.1 — Stack vide au demarrage */
    CHECK(vm_window_stack_top() == NULL, "stack_empty_top_null");
    CHECK(vm.window_stack_count == 0, "stack_empty_count_zero");

    /* 2.2 — Push 1 instance */
    ObjWindowInstance* inst1 = obj_new_window_instance(&g_mock_window_decl);
    vm_window_stack_push(inst1);
    CHECK(vm_window_stack_top() == inst1, "stack_1_top_inst1");
    CHECK(vm.window_stack_count == 1, "stack_1_count_one");

    /* 2.3 — Push 2e instance */
    ObjWindowInstance* inst2 = obj_new_window_instance(&g_mock_window_decl);
    vm_window_stack_push(inst2);
    CHECK(vm_window_stack_top() == inst2, "stack_2_top_inst2");
    CHECK(vm.window_stack_count == 2, "stack_2_count_two");

    /* 2.4 — Pop 1 instance */
    ObjWindowInstance* popped = vm_window_stack_pop();
    CHECK(popped == inst2, "stack_pop_inst2");
    CHECK(vm_window_stack_top() == inst1, "stack_after_pop_top_inst1");
    CHECK(vm.window_stack_count == 1, "stack_after_pop_count_one");

    /* 2.5 — Pop derniere instance */
    popped = vm_window_stack_pop();
    CHECK(popped == inst1, "stack_pop_inst1");
    CHECK(vm_window_stack_top() == NULL, "stack_empty_again_top_null");
    CHECK(vm.window_stack_count == 0, "stack_empty_again_count_zero");

    /* 2.6 — Clear sur stack non vide */
    vm_window_stack_push(inst1);
    vm_window_stack_push(inst2);
    vm_window_stack_clear();
    CHECK(vm_window_stack_top() == NULL, "stack_clear_top_null");
    CHECK(vm.window_stack_count == 0, "stack_clear_count_zero");
}

/* ================================================================
 * Test 3 : GC marking — 100+ instances + full GC cycle sans crash
 * ================================================================ */

static void test_gc_marking(void) {
    /* 3.1 — Allouer 100 window instances + 200 control instances */
    for (int i = 0; i < 100; i++) {
        ObjWindowInstance* win_inst = obj_new_window_instance(&g_mock_window_decl);
        vm_window_stack_push(win_inst);  /* root GC via vm.window_stack */

        /* Chaque fenetre contient 2 controles */
        ObjUiControlInstance* ctrl1 = obj_new_ui_control_instance(&g_mock_control_decl, win_inst);
        ObjUiControlInstance* ctrl2 = obj_new_ui_control_instance(&g_mock_control_decl, win_inst);

        /* Simuler l'ajout dans win_inst->controls[] (sera fait par impl).
         * Pour ce test on se contente que owner pointe vers win_inst. */
        (void)ctrl1;
        (void)ctrl2;
    }

    /* 3.2 — Declencher un full GC cycle */
    collectGarbage();

    /* 3.3 — Verifier que le stack contient toujours 100 instances */
    CHECK(vm.window_stack_count == 100, "gc_stack_count_preserved");
    CHECK(vm_window_stack_top() != NULL, "gc_stack_top_non_null");

    /* 3.4 — Clear stack et re-GC : sweep doit liberer tout */
    vm_window_stack_clear();
    collectGarbage();
    CHECK(vm.window_stack_count == 0, "gc_after_clear_stack_empty");

    /* Si on arrive ici sans crash = GC marking valide */
}

/* ================================================================
 * Harness
 * ================================================================ */

int multi_instance_test_main(void) {
    printf("Running multi_instance_test.c...\n");

    test_obj_allocation();
    test_vm_window_stack();
    test_gc_marking();

    /* Garde ensemble vide : "PASS: 0 assertions" est un vert vide. */
    if (g_pass + g_fail == 0) {
        printf("  CORPUS VIDE : 0 assertion executee. Un controle qui "
               "n'examine rien ne prouve rien.\n");
        return 1;
    }
    if (g_fail == 0) {
        printf("  PASS: %d assertions\n", g_pass);
        return 0;
    } else {
        printf("  FAIL: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }
}
