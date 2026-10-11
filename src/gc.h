// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_gc_h
#define owl_gc_h

#include "common.h"
#include "value.h"

/* Run a full GC cycle: mark all reachable objects, then sweep unreachable */
void gc_collect(void);

/* Mark a single Value as reachable (if it's an object) */
void gc_mark_value(Value value);

/* Mark an object as reachable and recursively mark its references */
void gc_mark_object(Obj* object);

/*  lot 3-bis A1 : expose pour gc_mark_compiler_roots, qui marque
 * la mini-stack des OwlControlDecl en cours de parse cote compiler.c.
 * Forward-declaration pour eviter de tirer ui_decl.h dans gc.h. */
struct OwlControlDecl;
void gc_mark_control_decl(const struct OwlControlDecl* c);

/* GC allocation tracking */
extern size_t gc_bytesAllocated;
extern size_t gc_nextGC;

#define GC_INITIAL_THRESHOLD (1024 * 1024) /* 1 MB */
#define GC_HEAP_GROW_FACTOR 2

/*  : registre unique de racines persistantes (Obj* ou Value
 * vivant dans des globals C-land qui survivent au cycle vm_init/vm_free
 * du test runner). Remplace les `stdlib_*_mark_roots()` ad-hoc.
 *
 * Usage typique : au moment de stdlib_X_init(), enregistrer chaque
 * static Obj* ou static Value du module via :
 *   owl_register_persistent_obj_root(&g_my_obj_ptr);
 *   owl_register_persistent_value_root(&g_my_value);
 *
 * Le GC itère le registre dans mark_roots() et marque les objets
 * non-NULL. Pas besoin de fonction _mark_roots() par module.
 *
 * Slot : pointeur stable vers la global (typiquement `&g_xxx`). La
 * cellule du registre stocke ce pointeur, donc on suit toute mutation
 * de la global au runtime.
 *
 * Capacité : 64 slots, augmentable plus tard si nécessaire (assert fail
 * si dépassement, ce qui est volontaire — on veut détecter l'oubli
 * d'augmenter la capacité, pas avoir un mode dégradé silencieux). */
void owl_register_persistent_obj_root(Obj** slot);
void owl_register_persistent_value_root(Value* slot);

/*  : reset all persistent root slots to NULL/NIL_VAL. Called from
 * vm_free() after the owl_objects walk to neutralize dangling pointers
 * before the next vm_init() cycle. */
void owl_persistent_roots_reset_all(void);

#endif
