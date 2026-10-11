// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "gc.h"
#include "vm.h"
#include "object.h"
#include "table.h"
#include "memory.h"
#include "compiler.h"
#include "mock.h"
#include "owl_stdlib.h"  /*  : stdlib_<module>_mark_roots */
#include "ui_decl.h"    /* OwlWindowDecl / OwlControlDecl / OwlEventBlock... */
#include "etat_decl.h"  /* OwlEtatDecl / OwlEtatBlockDecl */

size_t gc_bytesAllocated = 0;
size_t gc_nextGC = GC_INITIAL_THRESHOLD;

/* Forward declaration */
extern Table owl_strings;
extern Obj* owl_objects;
extern VM vm;

/*  : registre unique de racines persistantes. Voir gc.h pour le
 * design. Slot pointeur stable + flag is_value pour discriminer Obj** vs
 * Value*. Cap 64 — devrait suffire pour ~20 globals stdlib actuels +
 * marge confort. */
#define OWL_PERSISTENT_ROOTS_CAP 64
typedef struct {
    void* slot;       /* Obj** ou Value* selon is_value */
    bool  is_value;
} PersistentRoot;
static PersistentRoot g_persistent_roots[OWL_PERSISTENT_ROOTS_CAP];
static int g_persistent_roots_count = 0;

/* Idempotent : safe a appeler N fois sur le meme slot (le test runner
 * re-appelle stdlib_X_init() a chaque vm_init()). Si le slot est deja
 * enregistre, retour silencieux. Coût O(N) sur N=15-20 slots, trivial. */
void owl_register_persistent_obj_root(Obj** slot) {
    for (int i = 0; i < g_persistent_roots_count; i++) {
        if (g_persistent_roots[i].slot == (void*)slot) return;
    }
    if (g_persistent_roots_count >= OWL_PERSISTENT_ROOTS_CAP) {
        fprintf(stderr, "owl: OWL_PERSISTENT_ROOTS_CAP (%d) depasse — augmenter dans gc.c\n",
                OWL_PERSISTENT_ROOTS_CAP);
        abort();
    }
    g_persistent_roots[g_persistent_roots_count].slot = slot;
    g_persistent_roots[g_persistent_roots_count].is_value = false;
    g_persistent_roots_count++;
}

void owl_register_persistent_value_root(Value* slot) {
    for (int i = 0; i < g_persistent_roots_count; i++) {
        if (g_persistent_roots[i].slot == (void*)slot) return;
    }
    if (g_persistent_roots_count >= OWL_PERSISTENT_ROOTS_CAP) {
        fprintf(stderr, "owl: OWL_PERSISTENT_ROOTS_CAP (%d) depasse — augmenter dans gc.c\n",
                OWL_PERSISTENT_ROOTS_CAP);
        abort();
    }
    g_persistent_roots[g_persistent_roots_count].slot = slot;
    g_persistent_roots[g_persistent_roots_count].is_value = true;
    g_persistent_roots_count++;
}

static void mark_persistent_roots(void) {
    for (int i = 0; i < g_persistent_roots_count; i++) {
        PersistentRoot* pr = &g_persistent_roots[i];
        if (pr->is_value) {
            Value v = *(Value*)pr->slot;
            gc_mark_value(v);
        } else {
            Obj* obj = *(Obj**)pr->slot;
            if (obj != NULL) gc_mark_object(obj);
        }
    }
}

/*  : reset_all des persistent roots — appele par vm_free APRES le
 * walk owl_objects qui libere tous les Obj. A ce stade, tout slot Obj** qui
 * pointe vers un objet du heap est devenu dangling : on le reset a NULL
 * (resp. NIL_VAL pour Value). Sans ce reset, le test runner qui boucle
 * vm_free/vm_init voit le prochain GC lire un slot dangling (mark_persistent_roots
 * appelle gc_mark_object sur un Obj freed) -> heap UAF non-deterministe.
 *
 * Historique : les `stdlib_X_reset_globals()` font ce reset mais sont gardes
 * par `#ifdef OWL_HAS_GUI` pour stdlib_ui — owl_core ne voyant pas le define,
 * le reset de certaines racines etait stripe a la compile. Au lieu de fragmenter
 * la responsabilite par module avec des gardes a maintenir, on centralise ici. */
void owl_persistent_roots_reset_all(void) {
    for (int i = 0; i < g_persistent_roots_count; i++) {
        PersistentRoot* pr = &g_persistent_roots[i];
        if (pr->is_value) {
            *(Value*)pr->slot = NIL_VAL;
        } else {
            *(Obj**)pr->slot = NULL;
        }
    }
}

/* ================================================================
 * GRAY STACK (worklist for iterative marking)
 *
 * Classic tricolor abstraction without recursion: objects pushed here
 * are "gray" (marked but children not yet visited). The mark phase
 * drains this stack iteratively via trace_references, which keeps the
 * C call stack flat even for deeply-nested structures (e.g. linked
 * lists or trees with 1000+ levels).
 *
 * This replaces the previous recursive gc_mark_object implementation
 * which could cause stack overflow on deep graphs -- bug observed
 * with depth-10 binary trees built in a hot loop.
 *
 * We use system realloc() here (not owl_realloc) to avoid re-entering
 * the GC from within the GC.
 * ================================================================ */

static Obj** gc_grayStack   = NULL;
static int   gc_grayCount   = 0;
static int   gc_grayCapacity = 0;

static void gc_push_gray(Obj* object) {
    if (gc_grayCount + 1 > gc_grayCapacity) {
        gc_grayCapacity = gc_grayCapacity < 8 ? 8 : gc_grayCapacity * 2;
        Obj** resized = (Obj**)realloc(gc_grayStack,
                                       sizeof(Obj*) * (size_t)gc_grayCapacity);
        if (resized == NULL) {
            fprintf(stderr, "owl: out of memory (gc gray stack)\n");
            exit(74);
        }
        gc_grayStack = resized;
    }
    gc_grayStack[gc_grayCount++] = object;
}

static void trace_references(Obj* object);

/* Public API: mark an object as reachable.
 * Non-recursive: queues the object for later child traversal.
 * Callers from within trace_references() are safe: each enqueued
 * object is drained in the caller's loop, not via C recursion. */
void gc_mark_object(Obj* object) {
    if (object == NULL) return;
    if (object->isMarked) return;

#ifdef DEBUG_LOG_GC
    printf("  gc mark %p type=%d\n", (void*)object, object->type);
#endif

    object->isMarked = true;

    /* Leaf types (no outgoing references) skip the worklist entirely.
     * Saves a push/pop roundtrip for the most common case. */
    switch (object->type) {
        case OBJ_NATIVE:
        case OBJ_BUFFER:
        case OBJ_VECTOR:    /*  : double[] pur, aucune ref OWL sortante */
        case OBJ_MATRIX:    /*  : double[] row-major pur, aucune ref OWL sortante */
        case OBJ_MLARBRE:   /*  : arbre CART C-heap pur, aucune ref OWL sortante */
        case OBJ_MLFORET:   /*  : ensemble d'arbres CART C-heap pur, aucune ref OWL sortante */
        case OBJ_MLBOOSTING: /*  : ensemble séquentiel d'arbres CART C-heap pur, aucune ref OWL sortante */
        case OBJ_MLLINEAIRE: /*  : poids OLS C-heap pur, aucune ref OWL sortante */
        case OBJ_MLLOGISTIQUE: /*  : poids softmax C-heap pur, aucune ref OWL sortante */
        case OBJ_MLKMEANS:  /*  : centroïdes C-heap pur, aucune ref OWL sortante */
        case OBJ_NUMERIC:   /* limbes mpdecimal en C-land, aucune ref OWL sortante */
        case OBJ_DATE:
        case OBJ_TIME:
        case OBJ_DATETIME:
        case OBJ_DURATION:
        case OBJ_ZIP_ARCHIVE:
            /* Zip archives own a C-land backend pointer (mz_zip_archive /
             * LZMA state / FILE*), no OWL Value refs to trace. Leaf. */
            return;
        case OBJ_XLS_DOCUMENT:
            /*  — feuilles/cellules/fusions en C-heap pur.  :
             * chaque cellule peut porter un Police/Cadre materialise (Value OWL),
             * donc plus une feuille — trace au blacken. */
            gc_push_gray(object);
            return;
        case OBJ_XLS_REF:
            /*  — reference de style : pointe le classeur (Value OWL). */
            gc_push_gray(object);
            return;
        case OBJ_LLM:
            /*  — handles llama C-heap uniquement, aucune ref OWL
             * sortante. Feuille (finalizer C au sweep, cf object_free). */
            return;
        case OBJ_OPTIONS_GENERATION: {
            /*  — feuille tant que ..SurToken n'est pas affecte ; des
             * qu'une callable y est posee, il faut la tracer (meme raison que
             * OptionsEntraînement/onEpoch : sinon UAF entre affectation et appel). */
            ObjOptionsGeneration* o = (ObjOptionsGeneration*)object;
            if (!IS_OBJ(o->onToken) && !IS_OBJ(o->constraint)) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_OPTIONS_ENTRAINEMENT: {
            /*  — champs scalaires + onEpoch (reference de procedure).
             * Feuille tant que ..SurÉpoque n'est pas affecte ; des qu'une
             * callable y est posee, il faut la tracer, sinon la procedure est
             * collectee entre l'affectation et l'appel (use-after-free). */
            ObjOptionsEntrainement* o = (ObjOptionsEntrainement*)object;
            if (!IS_OBJ(o->onEpoch)) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_MOCK_NATIVE:
        case OBJ_PATTERN:
            /* Mock natives and patterns are leaves - their Value refs are
             * traced via mock_mark_roots() in mark_roots(). */
            return;
        case OBJ_UI_CONTROL: {
            /* Wrapper sur widget Qt — le widget est gere par son parent
             * QMainWindow.  : un headless_store (Table) optionnel
             * tient les props ecrites avant attach widget Qt — refs Value
             * a tracer. Si pas de store : leaf. */
            ObjUiControl* uc = (ObjUiControl*)object;
            if (uc->headless_store == NULL) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_UI_CONTROL_REF: {
            /*  — name ObjString a tracer si present (la cle de
             * resolution stable). decl pointe vers de la memoire freed
             * apres parse (cf. owl_window_decl_add_control), donc on ne
             * peut pas le tracer. */
            ObjUiControlRef* r = (ObjUiControlRef*)object;
            if (r->name == NULL) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_TENSOR:
            /*  — PREMIER type OWL a references internes non-triviales :
             * data (Vecteur), grad (Vecteur) ET les Tenseurs parents. NON-feuille.
             * Sans ce tracage, un parent collecte pendant que le graphe vit =
             * use-after-free. Delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_IAMODELE:
            /*  — references internes : les Tenseurs poids/biais de
             * chaque couche dense. Sans tracage, un poids collecte pendant que
             * le modele vit = use-after-free. Delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_IMAGE_ETIQUETEE:
            /*  — references internes : chemin, etiquette, Image
             * materialisee. NON-feuille, delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_IMAGE_ANNOTEE:
            /*  — references internes : chemin, tableau de boites,
             * Image materialisee. NON-feuille, delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_DATASET:
            /*  — references internes : les exemples, les noms de
             * classes et la source. NON-feuille, delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_WINDOW_INSTANCE:
        case OBJ_UI_CONTROL_INSTANCE:
            /*  : tracage non-trivial (controls[], owner, stores).
             * Delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_ETAT_INSTANCE:
            /*  lot D : tracage controls[]. Delegue a trace_references. */
            gc_push_gray(object);
            return;
        case OBJ_WINDOW_REF: {
            /*  lot B — name ObjString + fallback_store a tracer si
             * presents (decl pointe vers vm.windows[], rooted ailleurs). */
            ObjWindowRef* r = (ObjWindowRef*)object;
            if (r->name == NULL && r->fallback_store == NULL) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_ICONE: {
            /*  Lot 2b — un seul Obj* a tracer (svg_raw, ObjString).
             * Si pas de svg_raw : leaf, pas de gray push. */
            ObjIcone* icn = (ObjIcone*)object;
            if (icn->svg_raw == NULL) return;
            gc_push_gray(object);
            return;
        }
        case OBJ_ICONES_NAMESPACE:
            /*  Lot 2e-1 — leaf : aucun Obj* interne (singleton
             * sans champ a tracer). Le catalogue est statique en .rodata. */
            return;
        case OBJ_THEME_NAMESPACE:
            /*  Lot 2b+3 — leaf : la resolution passe par les globals
             * (deja roots GC), le singleton n'a aucun Obj* propre. */
            return;
        case OBJ_STRING: {
            /* Strings are leaf-like EXCEPT when they hold a frozen_cache
             * (lazy snapshot of a buffer-mode string for OP_GET_*). The
             * cache lives in owl_strings but that table is weak (cleared
             * by table_remove_white), so we must explicitly trace it. */
            ObjString* s = (ObjString*)object;
            if (s->frozen_cache != NULL) {
                gc_push_gray(object);
            }
            return;
        }
        default:
            gc_push_gray(object);
            break;
    }
}

/* ================================================================
 * MARK PHASE -- child traversal
 *
 * Called iteratively from drain_gray_stack(). Visits the children of
 * a single object and marks them (which may add them to the gray
 * stack). Does NOT recurse in C.
 * ================================================================ */

static void trace_references(Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            /* Only reached when a buffer-mode string holds a frozen_cache
             * snapshot (see gc_mark_object). Mark it so it survives the
             * sweep -- owl_strings is weak. */
            ObjString* s = (ObjString*)object;
            if (s->frozen_cache != NULL) {
                gc_mark_object((Obj*)s->frozen_cache);
            }
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            if (function->name != NULL) {
                gc_mark_object((Obj*)function->name);
            }
            if (function->filename != NULL) {
                gc_mark_object((Obj*)function->filename);
            }
            if (function->chunk != NULL) {
                /* Mark all constants in the function's chunk */
                for (int i = 0; i < function->chunk->constants.count; i++) {
                    gc_mark_value(function->chunk->constants.values[i]);
                }
            }
            /*  : mark function->defaults[] qui contient les valeurs
             * par defaut des parametres optionnels (multi-arity constructor /
             * procedure). Bug exhibe par test_constructeur_args : ObjString
             * defaut ("inconnu") collected → constructeur recoit garbage. */
            if (function->defaults != NULL) {
                for (int i = 0; i < function->defaultCount; i++) {
                    gc_mark_value(function->defaults[i]);
                }
            }
            /*  : mark les ObjString de la table de noms de locales
             * (debug info). Sans ca, le nom canonique d'une locale serait
             * collecte et le debogueur lirait de la memoire freed. */
            for (int i = 0; i < function->localNameCount; i++) {
                if (function->localNames[i].name != NULL) {
                    gc_mark_object((Obj*)function->localNames[i].name);
                }
            }
            break;
        }
        case OBJ_NATIVE:
            /* Natives have no outgoing references */
            break;
        case OBJ_ARRAY: {
            ObjArray* array = (ObjArray*)object;
            for (int i = 0; i < array->elements.count; i++) {
                gc_mark_value(array->elements.values[i]);
            }
            if (array->elementStructType != NULL) {
                gc_mark_object((Obj*)array->elementStructType);
            }
            if (array->owner_window != NULL) {
                gc_mark_object((Obj*)array->owner_window);
            }
            break;
        }
        case OBJ_BUFFER:
            /* buffers hold raw bytes — no child objects to mark */
            break;
        case OBJ_ASSOC_ARRAY: {
            ObjAssocArray* a = (ObjAssocArray*)object;
            for (int i = 0; i < a->count; i++) {
                gc_mark_object((Obj*)a->keys[i]);
                gc_mark_value(a->values[i]);
            }
            if (a->valueTypeName != NULL) {
                gc_mark_object((Obj*)a->valueTypeName);  /*  */
            }
            break;
        }
        case OBJ_MULTILINGUE: {
            /*  lot B : on delegue au storage. gc_mark_object le
             * recursera pour marker chaque cle/valeur. */
            ObjMultilingue* m = (ObjMultilingue*)object;
            if (m->storage != NULL) {
                gc_mark_object((Obj*)m->storage);
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            if (instance->structName != NULL) {
                gc_mark_object((Obj*)instance->structName);
            }
            if (instance->klass != NULL) {
                gc_mark_object((Obj*)instance->klass);
            }
            if (instance->fieldOrder != NULL) {
                gc_mark_object((Obj*)instance->fieldOrder);
            }
            /*  — back-ref forte vers le controle proprietaire + sa cle
             * composite (evite le pointeur pendouillant si l'instance est
             * capturee hors du store). */
            if (instance->ui_owner != NULL) {
                gc_mark_object((Obj*)instance->ui_owner);
            }
            if (instance->ui_owner_key != NULL) {
                gc_mark_object((Obj*)instance->ui_owner_key);
            }
            /* Mark all field values */
            for (int i = 0; i < instance->fields.capacity; i++) {
                Entry* entry = &instance->fields.entries[i];
                if (entry->key != NULL) {
                    gc_mark_object((Obj*)entry->key);
                    gc_mark_value(entry->value);
                }
            }
            break;
        }
        case OBJ_PROC_CHAIN: {
            ObjProcChain* chain = (ObjProcChain*)object;
            if (IS_OBJ(chain->avant)) gc_mark_object(AS_OBJ(chain->avant));
            if (IS_OBJ(chain->apres)) gc_mark_object(AS_OBJ(chain->apres));
            for (int i = 0; i < chain->count; i++) {
                if (IS_OBJ(chain->procs[i])) gc_mark_object(AS_OBJ(chain->procs[i]));
            }
            break;
        }
        case OBJ_JSON: {
            ObjJson* j = (ObjJson*)object;
            switch (j->jtype) {
                case JSON_T_STRING:
                    gc_mark_object((Obj*)j->as.strVal);
                    break;
                case JSON_T_NUMERIC:
                    gc_mark_object((Obj*)j->as.numericVal);
                    break;
                case JSON_T_ARRAY:
                    for (int i = 0; i < j->as.arrayVal.count; i++)
                        gc_mark_object((Obj*)j->as.arrayVal.items[i]);
                    break;
                case JSON_T_OBJECT:
                    for (int i = 0; i < j->as.objectVal.count; i++) {
                        gc_mark_object((Obj*)j->as.objectVal.keys[i]);
                        gc_mark_object((Obj*)j->as.objectVal.values[i]);
                    }
                    break;
                default: break;
            }
            break;
        }
        case OBJ_COLLECTION: {
            ObjCollection* coll = (ObjCollection*)object;
            for (int i = 0; i < coll->elements.count; i++) {
                gc_mark_value(coll->elements.values[i]);
            }
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            if (klass->name != NULL) {
                gc_mark_object((Obj*)klass->name);
            }
            if (klass->superclass != NULL) {
                gc_mark_object((Obj*)klass->superclass);
            }
            /* Mark methods */
            for (int i = 0; i < klass->methods.capacity; i++) {
                Entry* entry = &klass->methods.entries[i];
                if (entry->key != NULL) {
                    gc_mark_object((Obj*)entry->key);
                    gc_mark_value(entry->value);
                }
            }
            /* Mark defaults */
            for (int i = 0; i < klass->defaults.capacity; i++) {
                Entry* entry = &klass->defaults.entries[i];
                if (entry->key != NULL) {
                    gc_mark_object((Obj*)entry->key);
                    gc_mark_value(entry->value);
                }
            }
            /*  : mark static (GLOBAL) fields */
            for (int i = 0; i < klass->staticFields.capacity; i++) {
                Entry* entry = &klass->staticFields.entries[i];
                if (entry->key != NULL) {
                    gc_mark_object((Obj*)entry->key);
                    gc_mark_value(entry->value);
                }
            }
            /*  : mark interfaces set (key = interface name string).
             * Values sont NIL_VAL donc rien a faire cote value. */
            for (int i = 0; i < klass->interfaces.capacity; i++) {
                Entry* entry = &klass->interfaces.entries[i];
                if (entry->key != NULL) {
                    gc_mark_object((Obj*)entry->key);
                }
            }
            break;
        }
        /* Date/Time/DateTime/Duration have no outgoing references */
        case OBJ_NUMERIC:
        case OBJ_DATE:
        case OBJ_TIME:
        case OBJ_DATETIME:
        case OBJ_DURATION:
        case OBJ_ZIP_ARCHIVE:
        case OBJ_LLM:
            /* Leaf — handled by the early-return in gc_mark_object, this
             * case is only here to satisfy -Wswitch. */
            break;
        case OBJ_OPTIONS_GENERATION: {
            /*  — tracer la reference de procedure ..SurToken si
             * affectee (gray-push conditionnel dans gc_mark_object). Le reste
             * des champs est scalaire. */
            ObjOptionsGeneration* o = (ObjOptionsGeneration*)object;
            gc_mark_value(o->onToken);
            gc_mark_value(o->constraint);
            break;
        }
        case OBJ_MESSAGE_LLM: {
            /*  — rôle + contenu (ObjString). Gray-poussé par le
             * default de gc_mark_object ; ici on marque les deux champs. */
            ObjMessageLLM* m = (ObjMessageLLM*)object;
            gc_mark_value(m->role);
            gc_mark_value(m->content);
            break;
        }
        case OBJ_RESPONSE_LLM: {
            /*  — contenu (ObjString) ; raison + compteurs scalaires. */
            ObjResponseLLM* r = (ObjResponseLLM*)object;
            gc_mark_value(r->content);
            break;
        }
        case OBJ_DETECTION: {
            /*  — boite (ObjArray de 4 reels) ; classe + score scalaires. */
            ObjDetection* d = (ObjDetection*)object;
            gc_mark_value(d->box);
            break;
        }
        case OBJ_OPTIONS_ENTRAINEMENT: {
            /*  — tracer la reference de procedure ..SurÉpoque si
             * affectee (gray-push conditionnel dans gc_mark_object). Le reste
             * des champs est scalaire. */
            ObjOptionsEntrainement* o = (ObjOptionsEntrainement*)object;
            gc_mark_value(o->onEpoch);
            break;
        }
        case OBJ_MOCK_NATIVE:
        case OBJ_PATTERN:
            /* Handled via mock_mark_roots() */
            break;
        case OBJ_UI_CONTROL: {
            /*  — trace les entries du headless_store. Early-return
             * en haut quand store==NULL. */
            ObjUiControl* uc = (ObjUiControl*)object;
            if (uc->headless_store != NULL) {
                Table* t = uc->headless_store;
                for (int i = 0; i < t->capacity; i++) {
                    Entry* e = &t->entries[i];
                    if (e->key != NULL) {
                        gc_mark_object((Obj*)e->key);
                        gc_mark_value(e->value);
                    }
                }
            }
            break;
        }
        case OBJ_WINDOW_INSTANCE: {
            /*  : trace controls[] + valeur_renvoyee. decl pointe
             * vers vm.windows[] qui est root via mark_table(&vm.globals)
             * mais aussi via la traversee des fenetres top-level dans
             * registry — pas besoin de re-marquer (et de toute facon decl
             * n'est pas un Obj* sur lequel gc_mark_object peut operer). */
            ObjWindowInstance* w = (ObjWindowInstance*)object;
            for (int i = 0; i < w->control_count; i++) {
                gc_mark_object((Obj*)w->controls[i]);
            }
            gc_mark_value(w->valeur_renvoyee);
            /*  lot B — trace les entrees du casier de props. */
            if (w->headless_store != NULL) {
                Table* t = w->headless_store;
                for (int i = 0; i < t->capacity; i++) {
                    Entry* e = &t->entries[i];
                    if (e->key != NULL) {
                        gc_mark_object((Obj*)e->key);
                        gc_mark_value(e->value);
                    }
                }
            }
            break;
        }
        case OBJ_UI_CONTROL_INSTANCE: {
            /*  : back-ref vers owner (cycle gere par mark-and-sweep
             * standard, pas de ref counting) + headless_store eventuel. */
            ObjUiControlInstance* c = (ObjUiControlInstance*)object;
            if (c->owner != NULL) {
                gc_mark_object((Obj*)c->owner);
            }
            if (c->headless_store != NULL) {
                Table* t = c->headless_store;
                for (int i = 0; i < t->capacity; i++) {
                    Entry* e = &t->entries[i];
                    if (e->key != NULL) {
                        gc_mark_object((Obj*)e->key);
                        gc_mark_value(e->value);
                    }
                }
            }
            /*  — trace le set des cles override imperatif (ObjString). */
            if (c->headless_override_keys != NULL) {
                Table* t = c->headless_override_keys;
                for (int i = 0; i < t->capacity; i++) {
                    Entry* e = &t->entries[i];
                    if (e->key != NULL) {
                        gc_mark_object((Obj*)e->key);
                        gc_mark_value(e->value);
                    }
                }
            }
            break;
        }
        case OBJ_UI_CONTROL_REF: {
            /*  — trace le name (cle de resolution stable). */
            ObjUiControlRef* r = (ObjUiControlRef*)object;
            if (r->name) gc_mark_object((Obj*)r->name);
            break;
        }
        case OBJ_WINDOW_REF: {
            /*  lot B — trace name + fallback_store. */
            ObjWindowRef* r = (ObjWindowRef*)object;
            if (r->name) gc_mark_object((Obj*)r->name);
            if (r->fallback_store != NULL) {
                Table* t = r->fallback_store;
                for (int i = 0; i < t->capacity; i++) {
                    Entry* e = &t->entries[i];
                    if (e->key != NULL) {
                        gc_mark_object((Obj*)e->key);
                        gc_mark_value(e->value);
                    }
                }
            }
            break;
        }
        case OBJ_ETAT_INSTANCE: {
            /*  lot D — trace controls[]. decl pointe vers vm.etats
             * qui est root via mark_ui_decl_roots (mark_table sur vm.etats).
             * controls[i] sont des ObjUiControlInstance independantes. */
            ObjEtatInstance* e = (ObjEtatInstance*)object;
            for (int i = 0; i < e->control_count; i++) {
                gc_mark_object((Obj*)e->controls[i]);
            }
            break;
        }
        case OBJ_ICONE: {
            /*  Lot 2b — un seul ref Obj* a marquer. Early-return
             * dans gc_mark_object garantit svg_raw != NULL ici. */
            ObjIcone* icn = (ObjIcone*)object;
            gc_mark_object((Obj*)icn->svg_raw);
            break;
        }
        case OBJ_ICONES_NAMESPACE:
            /* Leaf, jamais gray-pushed : la branche existe pour la
             * couverture -Wswitch + auto-doc. */
            break;
        case OBJ_THEME_NAMESPACE:
            /* Leaf, jamais gray-pushed (cf. OBJ_ICONES_NAMESPACE). */
            break;
        case OBJ_VECTOR:
            /*  — leaf, jamais gray-pushed (early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MATRIX:
            /*  — leaf (early-return dans gc_mark_object). Branche
             * presente pour la couverture -Wswitch. */
            break;
        case OBJ_MLARBRE:
            /*  — leaf (arbre C-heap pur, early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MLFORET:
            /*  — leaf (ensemble d'arbres C-heap pur, early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MLBOOSTING:
            /*  — leaf (ensemble séquentiel d'arbres C-heap pur,
             * early-return dans gc_mark_object). Branche presente pour -Wswitch. */
            break;
        case OBJ_MLLINEAIRE:
            /*  — leaf (poids OLS C-heap pur, early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MLLOGISTIQUE:
            /*  — leaf (poids softmax C-heap pur, early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MLKMEANS:
            /*  — leaf (centroïdes C-heap pur, early-return dans
             * gc_mark_object). Branche presente pour la couverture -Wswitch. */
            break;
        case OBJ_MATRIX_ROW: {
            /*  — la vue de ligne reference sa matrice source : la
             * tracer pour qu'elle survive tant que la vue est vivante. */
            ObjMatrixRow* rv = (ObjMatrixRow*)object;
            if (rv->mat != NULL) gc_mark_object((Obj*)rv->mat);
            break;
        }
        case OBJ_TENSOR: {
            /*  — tracer TOUTES les references internes : donnee,
             * gradient et Tenseurs parents. C'est ce qui garde le graphe de
             * calcul entier vivant tant que le Tenseur de sortie l'est, et
             * evite le use-after-free d'un parent collecte. */
            ObjTensor* t = (ObjTensor*)object;
            if (t->data != NULL) gc_mark_object((Obj*)t->data);
            if (t->grad != NULL) gc_mark_object((Obj*)t->grad);
            for (int i = 0; i < t->nParents; i++) {
                if (t->parents[i] != NULL) gc_mark_object((Obj*)t->parents[i]);
            }
            break;
        }
        case OBJ_TENSOR_VIEW: {
            /*  — la vue reference le buffer plat (ObjVector/ObjMatrix)
             * qui la sous-tend : le tracer pour qu'il survive tant que la vue
             * est vivante (le double* lui appartient). */
            ObjTensorView* vw = (ObjTensorView*)object;
            if (vw->backing != NULL) gc_mark_object(vw->backing);
            break;
        }
        case OBJ_IAMODELE: {
            /*  — tracer les Tenseurs poids/biais de chaque couche
             * dense (les activations n'ont pas de tenseur). Le tableau
             * `layers` lui-meme est C-heap (pas un Obj), donc pas a marquer. */
            ObjIAModele* m = (ObjIAModele*)object;
            for (int i = 0; i < m->count; i++) {
                if (m->layers[i].weights != NULL)
                    gc_mark_object((Obj*)m->layers[i].weights);
                if (m->layers[i].bias != NULL)
                    gc_mark_object((Obj*)m->layers[i].bias);
                /*  — parametres batchnorm (gamma/beta) references par la
                 * couche : a tracer aussi, sinon collectes pendant que le modele vit. */
                if (m->layers[i].gamma != NULL)
                    gc_mark_object((Obj*)m->layers[i].gamma);
                if (m->layers[i].beta != NULL)
                    gc_mark_object((Obj*)m->layers[i].beta);
                /*  — stats mobiles batchnorm (moyenne/variance par canal). */
                if (m->layers[i].running_mean != NULL)
                    gc_mark_object((Obj*)m->layers[i].running_mean);
                if (m->layers[i].running_var != NULL)
                    gc_mark_object((Obj*)m->layers[i].running_var);
            }
            /*  — graphe general : chaque nœud porte les memes Tenseurs
             * (poids/biais/stats) qu'une couche. Sans ce tracage, un poids importe
             * serait collecte pendant que le modele vit (use-after-free a la 2e
             * inference). La table de valeurs d'execution est rootee separement
             * (ObjArray sur la pile VM pendant le forward). */
            for (int i = 0; i < m->ngnodes; i++) {
                IALayer* L = &m->gnodes[i].layer;
                if (L->weights      != NULL) gc_mark_object((Obj*)L->weights);
                if (L->bias         != NULL) gc_mark_object((Obj*)L->bias);
                if (L->gamma        != NULL) gc_mark_object((Obj*)L->gamma);
                if (L->beta         != NULL) gc_mark_object((Obj*)L->beta);
                if (L->running_mean != NULL) gc_mark_object((Obj*)L->running_mean);
                if (L->running_var  != NULL) gc_mark_object((Obj*)L->running_var);
            }
            break;
        }
        case OBJ_IMAGE_ETIQUETEE: {
            /*  — chemin + etiquette (ObjString) et l'Image
             * materialisee (ObjInstance) si deja chargee. */
            ObjImageEtiquetee* ie = (ObjImageEtiquetee*)object;
            if (ie->path  != NULL) gc_mark_object((Obj*)ie->path);
            if (ie->label != NULL) gc_mark_object((Obj*)ie->label);
            if (ie->image != NULL) gc_mark_object((Obj*)ie->image);
            break;
        }
        case OBJ_IMAGE_ANNOTEE: {
            /*  — chemin (ObjString), tableau de boites (ObjArray) et
             * l'Image materialisee (ObjInstance) si deja chargee. */
            ObjImageAnnotee* ia = (ObjImageAnnotee*)object;
            if (ia->path  != NULL) gc_mark_object((Obj*)ia->path);
            if (ia->boxes != NULL) gc_mark_object((Obj*)ia->boxes);
            if (ia->image != NULL) gc_mark_object((Obj*)ia->image);
            break;
        }
        case OBJ_XLS_DOCUMENT: {
            /*  — trace les composites de style (Police/Cadre)
             * materialises sur les cellules. Le reste (valeurs texte, dims) est
             * du C-heap pur, hors GC. */
            ObjXlsDocument* d = (ObjXlsDocument*)object;
            for (int i = 0; i < d->sheetCount; i++) {
                XlsSheet* s = &d->sheets[i];
                for (int c = 0; c < s->cellCount; c++) {
                    if (IS_OBJ(s->cells[c].font))   gc_mark_value(s->cells[c].font);
                    if (IS_OBJ(s->cells[c].border)) gc_mark_value(s->cells[c].border);
                }
            }
            break;
        }
        case OBJ_XLS_REF: {
            /*  — garde le classeur cible en vie. */
            ObjXlsRef* r = (ObjXlsRef*)object;
            if (r->doc != NULL) gc_mark_object((Obj*)r->doc);
            break;
        }
        case OBJ_DATASET: {
            /*  — tous les exemples, les noms de classes et la source.
             * Les tableaux `items`/`classes` sont C-heap (pas des Obj), mais
             * leurs elements sont des Obj GC a marquer un a un. */
            ObjDataset* ds = (ObjDataset*)object;
            for (int i = 0; i < ds->count; i++) {
                if (ds->items[i] != NULL) gc_mark_object((Obj*)ds->items[i]);
            }
            for (int i = 0; i < ds->classCount; i++) {
                if (ds->classes[i] != NULL) gc_mark_object((Obj*)ds->classes[i]);
            }
            if (ds->source != NULL) gc_mark_object((Obj*)ds->source);
            break;
        }
    }
}

void gc_mark_value(Value value) {
    if (IS_OBJ(value)) {
        gc_mark_object(AS_OBJ(value));
    }
}

static void drain_gray_stack(void) {
    while (gc_grayCount > 0) {
        Obj* object = gc_grayStack[--gc_grayCount];
        trace_references(object);
    }
}

static void mark_table(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            gc_mark_object((Obj*)entry->key);
            gc_mark_value(entry->value);
        }
    }
}

/* ================================================================
 * UI / ETAT DECL ROOTS
 *
 * Les OwlWindowDecl / OwlEtatDecl sont des structs C-land allouees au
 * compile-time (calloc), stockees dans vm.windows / vm.etats et liberees
 * manuellement dans vm_free. Ce ne sont PAS des Obj* du heap GC (leur
 * champ obj.type n'est pas initialise, elles ne sont pas dans
 * owl_objects). En revanche elles PORTENT des ObjFunction* bien reels et
 * vivants sur le heap GC : top_level de fenetre/champ/sous-champ d'Etat,
 * impl des events (Ouverture/Fermeture/Clic/AvantImpression...), impl des
 * user_procs, eval des proprietes dynamiques, init des variables fenetre,
 * param_capture, et body des blocs d'Etat.
 *
 * Ces ObjFunction ne sont referencees par AUCUN root pendant l'execution
 * (elles ne vivent que dans la decl C-land). Quand un GC se declenche au
 * milieu de l'execution d'un top_level (ou de l'unwind d'une erreur dans
 * ce top_level) -- les allocations de strings du body suffisent a le
 * declencher -- la fonction soeur (ou meme la fonction en cours, une fois
 * son frame depile par l'unwind) est collectee -> son chunk est free ->
 * use-after-free -> segfault.
 *
 * Fix : marquer EXHAUSTIVEMENT toutes les ObjFunction des decls vivantes
 * comme roots. Tant qu'une decl est dans vm.windows / vm.etats, tout son
 * code executable doit survivre au sweep. */

static void mark_property_inits(OwlPropertyInit* props, int count) {
    for (int i = 0; i < count; i++) {
        /* eval : ObjFunction de resolution dynamique (prop non-constante). */
        if (props[i].eval != NULL) {
            gc_mark_object((Obj*)props[i].eval);
        }
        /* const_value peut porter un Obj* vivant : soit un literal compose
         * (ObjInstance Cadre/Police, ObjMultilingue, ObjString), soit la
         * valeur RESOLUE in-place par owl_etat_prepare_props/build_window
         * (resolve_dynamic_props ecrit le resultat de eval ici puis passe
         * is_constant=true). Ce payload ne vit que dans la decl C-land donc
         * il n'a aucun autre root -> a marquer sinon collecte = heap corruption. */
        gc_mark_value(props[i].const_value);
    }
}

static void mark_event_blocks(OwlEventBlock* events, int count) {
    for (int i = 0; i < count; i++) {
        if (events[i].impl != NULL) {
            gc_mark_object((Obj*)events[i].impl);
        }
    }
}

/*  — descente recursive dans un arbre de menu classique. */
static void mark_menu_children(OwlMenuNode** children, int count) {
    for (int i = 0; i < count; i++) {
        OwlMenuNode* n = children[i];
        if (n == NULL) continue;
        mark_property_inits(n->properties, n->property_count);
        mark_event_blocks(n->events, n->event_count);
        mark_menu_children(n->children, n->child_count);
    }
}

static void mark_control_decl(const OwlControlDecl* c) {
    mark_property_inits(c->properties, c->property_count);
    mark_event_blocks(c->events, c->event_count);
    mark_event_blocks(c->user_procs, c->user_proc_count);
    for (int j = 0; j < c->column_count; j++) {
        mark_property_inits(c->columns[j].properties, c->columns[j].property_count);
    }
    if (c->top_level != NULL) {
        gc_mark_object((Obj*)c->top_level);
    }
}

/* Expose pour  lot 3-bis A1 : compiler.c marque la mini-stack des
 * decls de champ EN COURS de parse (g_current_control_decl_stack). Pendant
 * window_parse_subfield/etat_parse_block_*, le control n'est pas encore
 * append dans decl->controls[] (ajout en fin pour eviter realloc pendant
 * parse), donc mark_ui_decl_roots ne descend pas dedans. Cette racine
 * supplementaire couvre les ObjFunction events/user_procs/top_level deja
 * compilees pour CE control parent (et tous les parents en cas de
 * Conteneur/ZoneRepetee imbriques). */
void gc_mark_control_decl(const OwlControlDecl* c) {
    if (c != NULL) mark_control_decl(c);
}

static void mark_window_decl(const OwlWindowDecl* w) {
    mark_property_inits(w->properties, w->property_count);
    for (int i = 0; i < w->variable_count; i++) {
        if (w->variables[i].init != NULL) {
            gc_mark_object((Obj*)w->variables[i].init);
        }
    }
    /* controls[] est un tableau de OwlControlDecl par valeur. */
    for (int i = 0; i < w->control_count; i++) {
        mark_control_decl(&w->controls[i]);
    }
    /*  — arbres de menu : props (valeurs const + eval) et events
     * surSelection portent des Obj heap qui doivent survivre tant que le
     * decl vit dans vm.windows. Le noeud est rattache a son parent des sa
     * creation, donc l'arbre est deja joignable pendant le parse. */
    for (int i = 0; i < w->menu_count; i++) {
        mark_menu_children(w->menus[i]->children, w->menus[i]->child_count);
        /*  — `Style = theme.<nom>` : l'ObjFunction compilee au parse et
         * l'instance qu'elle produit au montage vivent dans le decl C seul. */
        if (w->menus[i]->has_style_ref)
            mark_property_inits(&w->menus[i]->style_ref, 1);
    }
    mark_event_blocks(w->events, w->event_count);
    mark_event_blocks(w->user_procs, w->user_proc_count);
    /*  — defauts des params portes par `Fenetre(p = <litteral>)`. Un
     * defaut chaine est un ObjString heap : sans ce marquage il serait collecte
     * alors que le decl vit toujours dans vm.windows (use-after-free a l'Ouvre). */
    for (int i = 0; i < w->param_count; i++) {
        if (w->params[i].has_default) {
            gc_mark_value(w->params[i].default_value);
        }
    }
    if (w->top_level != NULL) {
        gc_mark_object((Obj*)w->top_level);
    }
}

static void mark_etat_decl(const OwlEtatDecl* e) {
    mark_property_inits(e->properties, e->property_count);
    for (int i = 0; i < e->block_count; i++) {
        const OwlEtatBlockDecl* b = &e->blocks[i];
        mark_property_inits(b->properties, b->property_count);
        if (b->body != NULL) {
            gc_mark_object((Obj*)b->body);
        }
        /* controls[] d'un bloc d'Etat = tableau de OwlControlDecl* (les
         * sous-champs du bloc : Libelle/Image/... avec leur propre code). */
        for (int j = 0; j < b->control_count; j++) {
            if (b->controls[j] != NULL) {
                mark_control_decl(b->controls[j]);
            }
        }
    }
    mark_event_blocks(e->events, e->event_count);
    mark_event_blocks(e->user_procs, e->user_proc_count);
    /*  — defauts des params portes par `Etat(p = <litteral>)`. Meme
     * risque que la fenetre : un defaut chaine (ObjString heap) doit survivre
     * tant que le decl vit dans vm.etats. */
    for (int i = 0; i < e->param_count; i++) {
        if (e->params[i].has_default) {
            gc_mark_value(e->params[i].default_value);
        }
    }
    if (e->param_capture != NULL) {
        gc_mark_object((Obj*)e->param_capture);
    }
    /*  — top-level Etat stocke sur le decl (endormi jusqu'a la 1re
     * impression). Doit survivre tant que le decl est dans vm.etats. */
    if (e->top_level != NULL) {
        gc_mark_object((Obj*)e->top_level);
    }
}

static void mark_ui_decl_roots(void) {
    for (int i = 0; i < vm.windowCount; i++) {
        if (vm.windows[i] != NULL) mark_window_decl(vm.windows[i]);
    }
    for (int i = 0; i < vm.etatCount; i++) {
        if (vm.etats[i] != NULL) mark_etat_decl(vm.etats[i]);
    }
}

static void mark_roots(void) {
    /* Mark the VM stack */
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
        gc_mark_value(*slot);
    }

    /* Mark call frame function pointers */
    for (int i = 0; i < vm.frameCount; i++) {
        gc_mark_object((Obj*)vm.frames[i].function);
    }

    /* Mark global variables */
    mark_table(&vm.globals);

    /*  : namespace types separe — meme discipline GC que globals
     * (templates ObjInstance + leur sous-graphe via gc_mark_value). */
    mark_table(&vm.types);

    /*  phase 2 — index secondaires canon -> verbatim. Les cles
     * canon et les valeurs verbatim (ObjString*) doivent rester rooted
     * tant que le mapping est actif (verbatim sert ensuite de cle dans la
     * table principale). Marquer table entiere maintient les deux cotes. */
    mark_table(&vm.globals_canon_idx);
    mark_table(&vm.types_canon_idx);

    /*  : stack runtime des fenetres ouvertes — chaque entree est
     * une ObjWindowInstance vivante (push par Ouvre/OuvreFille/OuvreSoeur,
     * pop par Ferme). Tant qu'une instance est dans la stack, elle et son
     * subgraph (controls[], valeur_renvoyee) doivent survivre au sweep. */
    for (int i = 0; i < vm.window_stack_count; i++) {
        gc_mark_object((Obj*)vm.window_stack[i]);
    }

    /*  lot D : stack runtime des Etats en cours d'impression. Miroir
     * de window_stack : push a l'entree de iImprimeEtat, pop a la sortie.
     * Maintient les ObjEtatInstance + leurs sous-champs vivants pendant
     * l'execution des top-levels et events. */
    for (int i = 0; i < vm.etat_stack_count; i++) {
        gc_mark_object((Obj*)vm.etat_stack[i]);
    }

    /*  : stack runtime des widgets emetteurs (MoiMême). Les entrees
     * sont des Values qui referencent typiquement des ObjUiControlRef /
     * ObjWindowRef rootes ailleurs (vm.globals), mais ce mark explicite
     * garantit la survie meme si la globale est temporairement masquee. */
    for (int i = 0; i < vm.event_sender_stack_count; i++) {
        gc_mark_value(vm.event_sender_stack[i]);
    }

    /*  : POUR TOUT iteration target + currentKey. vm.iterTarget[i]
     * contient l'ObjArray/ObjAssoc en cours d'iteration, vm.iterCurrentKey[i]
     * la cle/index courant (typiquement ObjString pour assoc). Sans mark,
     * un GC entre 2 itérations (ou pendant le corps de la boucle qui alloue)
     * collecte le tableau target → SEGV ou "Can only get length of arrays"
     * au prochain acces. Bug exhibe par test_pour_tout_complet sous stress-GC. */
    for (int i = 0; i < vm.iterDepth; i++) {
        gc_mark_value(vm.iterTarget[i]);
        gc_mark_value(vm.iterCurrentKey[i]);
    }

    /*  : ObjFunction portees par les decls UI / Etat vivantes
     * (top_level, events, user_procs, eval de props, body de blocs...).
     * Les decls sont C-land (pas dans owl_objects) donc leur code
     * executable n'a aucun autre root pendant l'execution -> sans ca un
     * GC au milieu d'un top_level collecte son chunk -> segfault. */
    mark_ui_decl_roots();

    /* Mark mock registry — structure complexe (iteration sur g_mockRegistry),
     * trop specifique pour le registre unifie . Reste ad-hoc. */
    mock_mark_roots();

    /*  : remplace les 6 stdlib_*_mark_roots() ad-hoc 
     * (stdlib_erreur, stdlib_image, stdlib_string, stdlib_ui,
     * vm_exception) par un registre unique. Chaque module s'enregistre
     * via owl_register_persistent_*_root() depuis son _init(). */
    mark_persistent_roots();

    /*  lot 2 : racines PAR-CONTEXTE. Le GC est desormais
     * « context-aware » — il itere le registre de contextes d'execution
     * vivants au lieu de lire un seul etat. A N=1 (un seul thread VM, aucun
     * yield/thread executeur) il existe exactement UN contexte, adosse au
     * singleton vm.* ci-dessus : la boucle tourne une fois sur le vrai etat
     * et le marquage est bit-identique. Aujourd'hui chaque contexte porte les
     * snapshots vivants de sa pile de sauvegarde du contexte d'exception
     * (, save/restore par niveau de handler ; ObjString* qui peuvent
     * n'etre references que d'ici). L'etat d'execution lui-meme (stack,
     * frames, iter, window/etat/event stacks) reste dans le singleton et est
     * marque directement ci-dessus (contexte courant) ; les copies parkees
     * des contextes non-courants seront marquees dans cette meme boucle au
     * lot 4 (park/restore). */
    int owl_exec_ctx_n = vm_exec_context_count();
    for (int c = 0; c < owl_exec_ctx_n; c++) {
        vm_mark_context_roots(c);
    }

    /* Mark compiler roots (if compiling) */
    gc_mark_compiler_roots();
}

/* ================================================================
 * SWEEP PHASE
 * ================================================================ */

static void sweep(void) {
    /* First remove dead strings from the interning table */
    table_remove_white(&owl_strings);

    /* Walk all objects and free unmarked ones */
    Obj** object = &owl_objects;
    while (*object != NULL) {
        if ((*object)->isMarked) {
            /* Reset mark for next cycle */
            (*object)->isMarked = false;
            object = &(*object)->next;
        } else {
            /* Unreachable: unlink and free */
            Obj* unreached = *object;
            *object = unreached->next;
#ifdef DEBUG_LOG_GC
            printf("  gc free %p type=%d\n", (void*)unreached, unreached->type);
#endif
            object_free(unreached);
        }
    }
}

/* ================================================================
 * PUBLIC: COLLECT
 * ================================================================ */

void collectGarbage(void) { gc_collect(); }

void gc_collect(void) {
#ifdef DEBUG_LOG_GC
    printf("-- gc begin (allocated=%zu, threshold=%zu)\n",
           gc_bytesAllocated, gc_nextGC);
#endif

    mark_roots();
    drain_gray_stack();
    sweep();

    gc_nextGC = gc_bytesAllocated * GC_HEAP_GROW_FACTOR;
    if (gc_nextGC < GC_INITIAL_THRESHOLD) {
        gc_nextGC = GC_INITIAL_THRESHOLD;
    }

#ifdef DEBUG_LOG_GC
    printf("-- gc end   (allocated=%zu, next=%zu)\n",
           gc_bytesAllocated, gc_nextGC);
#endif
}
