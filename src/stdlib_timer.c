// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_timer.c — Type Timer.
 *
 * Un Timer est un type composé : il porte un intervalle (ms), une procédure
 * sans paramètre à appeler périodiquement, un drapeau de répétition et un
 * état actif/inactif. Modèle d'exécution :
 *
 *   - GUI : un QTimer parenté à la fenêtre courante (créé via le hook
 *     owl_ui_timer_start, no-op en CLI). Il tire pendant la boucle
 *     d'événements (fenêtre ouverte) et pendant Temporise. Fenêtre fermée =
 *     timer mort (garantie runtime via le parentage Qt).
 *   - Headless (CLI / owl test) : aucun timer ne tire. Tout l'état (props,
 *     ..Actif, erreurs) reste fonctionnel ; la logique utilisateur se teste
 *     en appelant directement la procédure.
 *
 * État stocké dans les champs de l'ObjInstance (comme Chrono) :
 *   public  : Intervalle (entier ms), Repete (booléen), Procedure (procédure)
 *   computé : Actif (lecture seule, reflète _active via computeGetter)
 *   internes: _active (booléen), _fired (booléen — a-t-il tiré au moins une
 *             fois), _qtimer_id (handle opaque du QTimer GUI, 0 = aucun)
 *
 * Les validations d'affectation (..Actif lecture seule, ..Procédure
 * appelable sans paramètre) passent par le hook onFieldSet du type, branché
 * dans OP_STRUCT_SET. Démarre sans procédure lève une soft-error centralisée.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"
#include "gc.h"
#include "owl_errors.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 * REGISTRE DES TIMERS ARMÉS
 *
 * Un timer armé est conservé dans un ObjArray rooté (g_armed_timers,
 * enregistré comme racine persistante GC). Double bénéfice :
 *   - un timer armé reste vivant tant qu'il l'est (sémantiquement correct :
 *     un QTimer parenté à la fenêtre pourrait le déclencher), même si le
 *     code utilisateur n'en garde plus de référence directe ;
 *   - le warning de fin de process (owl_timer_warn_active_on_exit) ne
 *     déréférence jamais un pointeur libéré.
 *
 * L'array contient des Value OBJ_VAL(ObjInstance Timer). Désarmer retire
 * l'instance de l'array (elle redevient collectable).
 * ================================================================ */

static ObjArray* g_armed_timers = NULL;
static bool      g_armed_root_registered = false;

static void timer_registry_init(void) {
    /* Le slot &g_armed_timers n'est enregistré comme racine GC qu'une seule
     * fois (le pointeur de slot est stable). owl_persistent_roots_reset_all
     * (vm_free) remet g_armed_timers à NULL entre deux cycles ; on recrée
     * alors juste l'array, sans ré-enregistrer le slot. */
    if (!g_armed_root_registered) {
        owl_register_persistent_obj_root((Obj**)&g_armed_timers);
        g_armed_root_registered = true;
    }
    if (g_armed_timers == NULL) {
        g_armed_timers = obj_new_array();
    }
}

static void timer_registry_add(ObjInstance* t) {
    timer_registry_init();
    for (int i = 0; i < g_armed_timers->elements.count; i++) {
        Value v = g_armed_timers->elements.values[i];
        if (IS_INSTANCE(v) && AS_INSTANCE(v) == t) return;  /* déjà présent */
    }
    obj_array_push(g_armed_timers, OBJ_VAL(t));
}

static void timer_registry_remove(ObjInstance* t) {
    if (g_armed_timers == NULL) return;
    int w = 0;
    for (int r = 0; r < g_armed_timers->elements.count; r++) {
        Value v = g_armed_timers->elements.values[r];
        bool drop = IS_INSTANCE(v) && AS_INSTANCE(v) == t;
        if (!drop) {
            if (w != r) g_armed_timers->elements.values[w] = v;
            w++;
        }
    }
    g_armed_timers->elements.count = w;
}

/* ================================================================
 * HELPERS CHAMPS
 * ================================================================ */

static bool timer_field_bool(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, k, &v)) return false;
    return IS_BOOL(v) ? AS_BOOL(v) : false;
}

static int64_t timer_field_int(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, k, &v)) return 0;
    /*  — ..Intervalle est désormais typé durée : un littéral
     * { } ou une coercion produit un ObjDuration (totalMs). Un entier brut
     * (default ou coerce direct) reste accepté. */
    if (IS_DURATION(v)) return AS_DURATION(v)->totalMs;
    return IS_INTEGER(v) ? AS_INTEGER(v) : 0;
}

static bool timer_field_value(ObjInstance* inst, const char* name, Value* out) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    return table_get(&inst->fields, k, out);
}

/* La procédure assignée est-elle appelable sans argument ?
 *   - une procédure native (ObjFunction) : minArity == 0
 *   - une lambda 0-paramètre : ObjFunction minArity == 0
 *   - tout autre callable (proc-chain) : accepté (lambda figeant les args)
 *   - valeur nil / non-callable : pas de procédure utilisable */
static bool timer_proc_is_callable_zero(Value v) {
    if (IS_FUNCTION(v)) return AS_FUNCTION(v)->minArity == 0;
    if (IS_CALLABLE(v)) return true;  /* proc-chain, native */
    return false;
}

static bool timer_has_procedure(ObjInstance* inst) {
    Value proc;
    if (!timer_field_value(inst, "Procedure", &proc)) return false;
    return IS_CALLABLE(proc);
}

/* ================================================================
 * BACKEND GUI (no-op par défaut, override dans stdlib_ui.c en build GUI)
 * ================================================================ */

typedef void (*OwlTimerBackendFn)(ObjInstance* timer);
static void timer_backend_noop(ObjInstance* timer) { (void)timer; }
static OwlTimerBackendFn g_timer_start_impl  = timer_backend_noop;
static OwlTimerBackendFn g_timer_stop_impl   = timer_backend_noop;
static OwlTimerBackendFn g_timer_update_impl = timer_backend_noop;

void owl_timer_register_backend(OwlTimerBackendFn startFn, OwlTimerBackendFn stopFn,
                                OwlTimerBackendFn updateFn) {
    g_timer_start_impl  = startFn  ? startFn  : timer_backend_noop;
    g_timer_stop_impl   = stopFn   ? stopFn   : timer_backend_noop;
    g_timer_update_impl = updateFn ? updateFn : timer_backend_noop;
}

/* Le backend GUI signale qu'un vrai QTimer parenté à une fenêtre vivante a
 * été armé : une boucle d'événements va effectivement le pomper. Pose
 * _gui_engaged, qui exclut le timer du warning de fin de process (sa cible
 * pédagogique est le script CLI SANS pompe, pas une fenêtre fermée tôt). */
void owl_timer_mark_gui_engaged(ObjInstance* inst) {
    if (inst == NULL) return;
    inst_field_set_bool(inst, "_gui_engaged", true);
}

/* ================================================================
 * GETTER COMPUTÉ ..Actif (lecture seule) — branché via FIELDS_Timer
 * ================================================================ */

Value fn_timer_actif(int argCount, Value* args) {
    if (argCount >= 1 && IS_INSTANCE(args[0])) {
        return BOOL_VAL(timer_field_bool(AS_INSTANCE(args[0]), "_active"));
    }
    return BOOL_VAL(false);
}

/* ================================================================
 * HOOK onFieldSet — validations d'affectation
 *
 * Appelé après chaque écriture de champ sur une instance Timer (cf.
 * OP_STRUCT_SET dans vm.c). Reçoit le nom canon ASCII FR du champ écrit.
 *   - "Actif"      : lecture seule -> soft-error + neutralise le storage
 *   - "Procedure"  : doit être appelable sans paramètre -> soft-error sinon
 *   - "Intervalle" : si écrit sur un timer ACTIF adossé à un QTimer vivant,
 *                    propage le nouveau rythme au QTimer (pris en compte au
 *                    tic suivant) sans avoir à Arrête/Démarre.
 *   - "Repete"     : idem, bascule one-shot <-> répétitif à chaud.
 *
 * ..Procédure n'a pas besoin de propagation : le tic relit le champ live.
 * ================================================================ */

void owl_timer_on_field_set(ObjInstance* inst, const char* fieldName) {
    if (strcmp(fieldName, "Actif") == 0) {
        /* Le getter computeGetter ignore déjà le storage (il lit _active),
         * donc la lecture reste cohérente ; on signale juste l'erreur. */
        owl_emit_native_error(E_NATIVE_TIMER_ACTIF_READONLY, "Timer",
                              "native", "Timer", NULL);
        return;
    }
    if (strcmp(fieldName, "Procedure") == 0) {
        Value proc;
        if (timer_field_value(inst, "Procedure", &proc)
            && IS_CALLABLE(proc)
            && !timer_proc_is_callable_zero(proc)) {
            owl_emit_native_error(E_NATIVE_TIMER_PROCEDURE_NEEDS_ARGS, "Timer",
                                  "native", "Timer", NULL);
        }
        return;
    }
    /* ..Intervalle / ..Répète modifiés à chaud : propage au QTimer vivant.
     * Seulement si le timer tourne (_active) ET est réellement adossé à une
     * fenêtre (_gui_engaged) — sinon il n'y a rien à mettre à jour (CLU
     * headless, ou timer pas encore armé : le prochain Démarre relira la
     * valeur fraîche de toute façon). Le backend GUI relit les champs live. */
    if (strcmp(fieldName, "Intervalle") == 0 || strcmp(fieldName, "Repete") == 0) {
        if (timer_field_bool(inst, "_active")
            && timer_field_bool(inst, "_gui_engaged")) {
            g_timer_update_impl(inst);
        }
    }
}

/* ================================================================
 * NATIVES Démarre / Arrête (= méthodes .Démarre() / .Arrête())
 * ================================================================ */

/* Retourne l'ObjInstance Timer depuis args[0] (forme native TimerXxx(t) ET
 * forme méthode t.Xxx()). NULL si l'argument n'est pas un Timer. */
static ObjInstance* timer_target(int argCount, Value* args) {
    if (argCount >= 1 && IS_INSTANCE(args[0])) {
        ObjInstance* inst = AS_INSTANCE(args[0]);
        if (inst->structName != NULL
            && strcmp(inst->structName->chars, "Timer") == 0) {
            return inst;
        }
    }
    return NULL;
}

static Value fn_timer_demarre(int argCount, Value* args) {
    ObjInstance* inst = timer_target(argCount, args);
    if (inst == NULL) return NIL_VAL;

    if (!timer_has_procedure(inst)) {
        owl_emit_native_error(E_NATIVE_TIMER_NO_PROCEDURE, "TimerD\xc3\xa9marre",
                              "native", "TimerD\xc3\xa9marre", NULL);
        return NIL_VAL;  /* reste inactif */
    }

    timer_field_int(inst, "Intervalle");  /* (lecture validée côté backend) */
    inst_field_set_bool(inst, "_active", true);
    inst_field_set_bool(inst, "_fired",  false);
    inst_field_set_bool(inst, "_gui_engaged", false);  /* (re)posé par le backend GUI si fenêtre vivante */
    timer_registry_add(inst);

    /* Backend GUI : crée/arme le QTimer parenté à la fenêtre courante.
     * No-op en headless (le timer ne tirera jamais). */
    g_timer_start_impl(inst);
    return NIL_VAL;
}

static Value fn_timer_arrete(int argCount, Value* args) {
    ObjInstance* inst = timer_target(argCount, args);
    if (inst == NULL) return NIL_VAL;

    inst_field_set_bool(inst, "_active", false);
    timer_registry_remove(inst);
    g_timer_stop_impl(inst);
    return NIL_VAL;
}

/* ================================================================
 * WARNING DE FIN DE PROCESS
 *
 * Appelé par vm.c après l'exécution du top-level (mode run / non-unitaire).
 * Si des timers sont encore actifs et n'ont jamais tiré, c'est qu'aucune
 * boucle d'événements ne les a pompés : warning pédagogique centralisé.
 * ================================================================ */

void owl_timer_warn_active_on_exit(void) {
    if (g_armed_timers == NULL) return;
    int never = 0;
    for (int i = 0; i < g_armed_timers->elements.count; i++) {
        Value v = g_armed_timers->elements.values[i];
        if (!IS_INSTANCE(v)) continue;
        ObjInstance* t = AS_INSTANCE(v);
        /* Cible : un timer armé que personne n'a jamais pompé (script CLI sans
         * boucle d'événements). Un timer adossé à une vraie fenêtre GUI
         * (_gui_engaged) a EU une pompe : si l'utilisateur ferme la fenêtre
         * avant l'échéance, ce n'est pas un oubli pédagogique -> pas de warning. */
        if (timer_field_bool(t, "_active") && !timer_field_bool(t, "_fired")
            && !timer_field_bool(t, "_gui_engaged")) {
            never++;
        }
    }
    if (never == 0) return;

    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d", never);
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(W_RT_TIMER_NEVER_FIRED, "count", countBuf, NULL)
        : owl_error_render_fr(W_RT_TIMER_NEVER_FIRED, "count", countBuf, NULL);
    fputs("ATTENTION: ", stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

static const OwlMethodBinding TIMER_DEMARRE_METHODS[] = {
    {"Timer", "D\xc3\xa9marre", -1},  /* Démarre (canon FR) */
    {"Timer", "Demarre",        -1},  /* alias ASCII */
    {"Timer", "Start",          -1},  /* alias EN */
    {NULL, NULL, 0}
};
static const OwlMethodBinding TIMER_ARRETE_METHODS[] = {
    {"Timer", "Arr\xc3\xaate", -1},   /* Arrête (canon FR) */
    {"Timer", "Arrete",        -1},   /* alias ASCII */
    {"Timer", "Stop",          -1},   /* alias EN */
    {NULL, NULL, 0}
};

static const OwlParam p_timer[] = {
    {"timer", "Timer", "timer à piloter"}, {0}
};
static const OwlSignature sig_timer_demarre[] = {
    {1, p_timer, "rien", "arme le timer"}, {0}
};
static const OwlSignature sig_timer_arrete[] = {
    {1, p_timer, "rien", "désarme le timer"}, {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical      = "TimerD\xc3\xa9marre",   /* TimerDémarre */
        .canonicalAscii = "TimerDemarre",
        .nameEN         = "TimerStart",
        .category       = "Timer",
        .descFR         = "Arme un timer : sa procédure sera appelée toutes "
                          "les ..Intervalle millisecondes pendant une boucle "
                          "d'événements. Sans procédure assignée, déclenche "
                          "une erreur.",
        .descEN         = "Arm a timer: its procedure is called every "
                          "..Interval milliseconds during an event loop. "
                          "Without an assigned procedure, raises an error.",
        .signatures     = sig_timer_demarre,
        .impl           = fn_timer_demarre,
        .methodBindings = TIMER_DEMARRE_METHODS,
    },
    {
        .canonical      = "TimerArr\xc3\xaate",    /* TimerArrête */
        .canonicalAscii = "TimerArrete",
        .nameEN         = "TimerStop",
        .category       = "Timer",
        .descFR         = "Désarme un timer : sa procédure n'est plus "
                          "appelée. ..Actif repasse à Faux.",
        .descEN         = "Disarm a timer: its procedure is no longer "
                          "called. ..Active goes back to False.",
        .signatures     = sig_timer_arrete,
        .impl           = fn_timer_arrete,
        .methodBindings = TIMER_ARRETE_METHODS,
    },
};

void stdlib_timer_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}
