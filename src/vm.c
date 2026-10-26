// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "vm.h"
#include "compiler.h"
#include "owl_errors.h"
#include "debug.h"
#include "debug_api.h"
#include "memory.h"
#include "object.h"
#include "mpdecimal.h"   /* OBJ_NUMERIC : mpd_iszero dans is_falsy / arith */
#include "gc.h"
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_event_catalog.h"
#include "scanner.h"
#include "sentinels.h"
#include "testing.h"
#include "mock.h"
#include "ui_decl.h"   /* V1.5-D : owl_ui_on_instance_modified hook */
#include "ui_catalog.h" /*  : owl_control_catalog_find pour broadcast Groupe */
#include "etat_decl.h" /* vm_find_widget_info_by_name lookup dans Etats */
#include "formatter_canon.h" /* pose le provider de canon du formatter post-stdlib_init */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

VM vm;

/* Interrupt flag set from a signal / console handler. volatile + sig_atomic_t
 * makes it safe to mutate from async signal context. Read in the dispatch
 * loop at back-edges (OP_LOOP) so an infinite loop can be broken without
 * corrupting mid-instruction state. */
#include <signal.h>
static volatile sig_atomic_t g_interrupt_requested = 0;

void vm_request_interrupt(void) { g_interrupt_requested = 1; }

static bool g_json_error_mode = false;
void vm_set_json_errors(bool enabled) { g_json_error_mode = enabled; }

void vm_current_location(const char** outFile, int* outLine) {
    if (!outFile || !outLine) return;
    *outFile = NULL;
    *outLine = 0;
    if (vm.frameCount == 0) return;
    CallFrame* top = &vm.frames[vm.frameCount - 1];
    ObjFunction* fn = top->function;
    if (!fn || !fn->chunk) return;
    size_t instr = top->ip - fn->chunk->code - 1;
    *outLine = fn->chunk->lines[instr];
    *outFile = fn->filename ? fn->filename->chars : NULL;
}

/* Minimal JSON string escaper shared with runtime_error's structured path. */
static void json_esc_stderr(const char* s) {
    if (!s) { fputs("null", stderr); return; }
    fputc('"', stderr);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"')       fputs("\\\"", stderr);
        else if (*p == '\\') fputs("\\\\", stderr);
        else if (*p == '\n') fputs("\\n", stderr);
        else if (*p == '\r') fputs("\\r", stderr);
        else if (*p == '\t') fputs("\\t", stderr);
        else if (*p < 0x20)  fprintf(stderr, "\\u%04x", *p);
        else                 fputc(*p, stderr);
    }
    fputc('"', stderr);
}

/* String interning table - referenced by object.c */
Table owl_strings;

/* ================================================================
 * EXCEPTION HANDLER STACK
 * ================================================================ */

#define EXCEPTION_STACK_MAX 64

typedef struct {
    uint8_t* ip;            /* IP to jump to on exception */
    Value* stackTop;        /* stack state to restore */
    int frameCount;         /* frame count to restore */
    CallFrame frame;        /* frame to restore */
    int savedCtxDepth;      /*  : excSaveDepth juste apres le snapshot
                             * pose par cet OP_TRY. Quand ce handler attrape,
                             * on remet excSaveDepth a cette valeur pour que la
                             * sortie du handler restaure le bon niveau. */
} ExceptionHandler;

static ExceptionHandler exceptionStack[EXCEPTION_STACK_MAX];
static int exceptionCount = 0;

/* Current exception context. The language distinguishes a rich set of fields
 * exposed through ExceptionInfo(errXxx). We snapshot them when an exception
 * is raised (either by runtime_error or ExceptionDeclenche) so any handler
 * can query them. */
static ObjString* currentException = NULL;      /* message (errMessage) */
static ObjString* g_exc_code        = NULL;     /* errCode — chaîne canonique (, aligné g_err_code) */
static int        g_exc_line        = 0;        /* errLigne */
static ObjString* g_exc_filename    = NULL;     /* errÉlément */
static ObjString* g_exc_procedure   = NULL;     /* errTraitement — procédure courante au site de capture */
static ObjString* g_exc_origin      = NULL;     /* errPendant — fonction qui a effectivement levé */
static ObjString* g_exc_native_name = NULL;     /* errFonction — native C qui a levé (, "" si levée user) */
static ObjString* g_exc_info        = NULL;     /* errInfo — 3e arg d'ExceptionDéclenche */
static ObjString* g_exc_stack       = NULL;     /* errPile (preformatted) */
static int64_t    g_exc_timestamp_ms = 0;       /* errDateHeure (Unix ms) */
static int        g_exc_sys_code    = 0;        /* errCodeSystème (opt-in) */
static ObjString* g_exc_sys_message = NULL;     /* errMessageSystème (opt-in) */

/* Le caller pose ces flags juste avant la levée pour que
 * capture_exception_context ne reset PAS la valeur correspondante.
 * Cohérent avec le mécanisme opt-in errCodeSystème : la native syscall
 * appelle owl_set_exception_system_error() puis lève, et la capture
 * conserve les valeurs posées plutôt que les écraser. */
static bool g_exc_sys_pending    = false;
static bool g_exc_origin_pending = false;
/*  : mirror du flag origin pour errFonction. Posé par
 * owl_vm_raise_native / vm_throw_from_erreur_coded avant la capture ; sinon
 * capture_exception_context reset g_exc_native_name à NULL (errFonction ""). */
static bool g_exc_native_name_pending = false;

static bool throwPending = false;

/*  — quand armé, une erreur runtime NON rattrapée (exceptionCount == 0)
 * capture quand même son contexte (currentException + g_exc_*) avant l'unwind,
 * pour qu'un hook de service (surErreur) invoqué APRÈS le crash puisse le lire
 * via ExceptionInfo(). Posé/relâché par l'exécuteur de service autour de
 * l'appel du handler de requête. */
static bool g_capture_uncaught = false;

/*  — marqueur "fatale-VM en vol". Pose au site d'un debordement de
 * pile d'appels (call() a FRAMES_MAX), la seule fatale qui remonte via un
 * retour `false` de call_value/call jusqu'a un pont OWL_RT_RECOVER_DISPATCH.
 * Le pont le consulte : si arme, il REFUSE le rattrapage QUAND EXCEPTION et
 * laisse la fatale se propager (le contrat exclut les fatales-VM). Les autres
 * fatales (E_RT_UNKNOWN_OPCODE, E_RT_EXCEPTION_STACK_OVERFLOW) `return`
 * directement sans pont, elles n'ont pas besoin du marqueur. Reset a l'entree
 * de run() (nouvelle unite d'execution : REPL, script, appel reentrant) et
 * auto-efface a la consultation du pont. */
static bool g_rt_fatal_vm = false;

/*  : vm_exception_mark_roots() supprime — les 7 globals exception
 * sont enregistres au registre unique dans vm_init() via
 * owl_register_persistent_obj_root() (idempotent). */

/*  — pile de sauvegarde du contexte d'exception, parallele a
 * exceptionStack. Le contexte (currentException + g_exc_*) est un etat statique
 * global : sans sauvegarde/restauration il persiste apres qu'un handler l'a
 * consomme (ExceptionInfo renvoie du stale hors handler, ExceptionPropage
 * re-leve un residu). On snapshot le contexte courant a l'entree d'un try
 * (OP_TRY), et on le restaure a la sortie NORMALE d'un handler (OP_EXC_RESTORE)
 * ou au depop d'un try sans exception (OP_END_TRY). Consequences :
 *   - hors de tout handler, le contexte revient a vide (snapshot du top-level) ;
 *   - un handler imbrique restaure le contexte du handler englobant en sortie
 *     (et pas le contexte interne devenu stale).
 * Le chemin de PROPAGATION (OP_RERAISE / unwind) ne restaure PAS : l'exception
 * doit conserver son contexte pour le handler externe qui la rattrape. */
typedef struct {
    ObjString* message;
    ObjString* code;          /*  : chaîne canonique (était int) */
    int        line;
    ObjString* filename;
    ObjString* procedure;
    ObjString* origin;
    ObjString* native_name;   /*  : errFonction */
    ObjString* info;
    ObjString* stack;
    int64_t    timestamp_ms;
    int        sys_code;
    ObjString* sys_message;
} ExcContextSnapshot;

/*  lot 2 — contexte d'exécution (M2, arbitrage A1).
 *
 * Abstraction minimale qui rend le GC « context-aware » : mark_roots itère un
 * REGISTRE de contextes d'exécution vivants au lieu de lire un seul état
 * global. À N=1 (aujourd'hui : un seul thread VM, aucun yield, aucun thread
 * exécuteur) il existe exactement UN contexte — le contexte courant, adossé
 * au singleton `vm.*` — donc la boucle tourne une fois sur le vrai état et le
 * marquage est bit-identique.
 *
 * Ce lot ne fait migrer dans le contexte QUE la pile de sauvegarde du contexte
 * d'exception : c'est le seul état déjà save/restore par snapshot,
 * le précédent direct du park/restore. L'état d'exécution lui-même (stack,
 * frames, window_stack, etat_stack, iter*, event_sender) RESTE dans le
 * singleton (M2) : le contexte courant EST le singleton. Le park/restore par
 * COPIE de cet état (pour les contextes parkés au yield) est LOT 4 — il ne
 * s'exécuterait jamais à N=1, on ne l'introduit donc pas ici (pas de
 * scaffolding mort). Les lots 3-4 ajouteront des entrées au registre. */
typedef struct OwlExecContext {
    ExcContextSnapshot excCtxSaveStack[EXCEPTION_STACK_MAX];
    int                excSaveDepth;
} OwlExecContext;

/* Borne du registre : dimensionne le pool exécuteur des lots 3-4
 * (OWL_VM_THREADS, arbitrage A4, défaut 4) avec de la marge. À N=1 seule
 * l'entrée 0 (le contexte principal) est vivante. */
#define OWL_EXEC_CONTEXT_MAX 64

static OwlExecContext  g_exec_ctx_main;
static OwlExecContext* g_exec_ctx_cur = &g_exec_ctx_main;
static OwlExecContext* g_exec_contexts[OWL_EXEC_CONTEXT_MAX] = { &g_exec_ctx_main };
static int             g_exec_context_count = 1;

/*  lot 4 — park/restore M2 (arbitrage A1).
 *
 * Quand un handler qui tient le GIL va BLOQUER (contre-pression flux,
 * owl_http_rep_flux_pousse), il PARK son état d'exécution vif : une COPIE
 * possédée (heap) de la pile de valeurs, des frames, du contexte d'exception
 * et des piles auxiliaires. L'état RESTE physiquement dans le singleton vm.* /
 * statics (M2) — on en copie les octets de côté, on remet le singleton à un
 * baseline vide, on relâche le GIL. Un autre thread reprend alors le GIL et
 * s'exécute SUR LES MÊMES arrays globaux. À la reprise, restore recopie le
 * snapshot dans ces mêmes arrays physiques : les pointeurs absolus
 * (frame->slots, stackTop, ExceptionHandler.stackTop/frame.slots) se
 * réalignent puisque la base (vm.stack, adresse fixe du singleton) n'a pas
 * bougé.
 *
 * PIÈGE MARQUAGE (§3 du lot) : pendant qu'un snapshot est parké, le thread
 * courant ÉCRASE les arrays globaux. mark_roots marque donc :
 *   - le contexte COURANT depuis les arrays globaux (comme avant) ;
 *   - chaque snapshot PARKÉ depuis SA COPIE heap — JAMAIS en suivant
 *     frame->slots du snapshot (qui pointe dans l'array global, désormais =
 *     les données du thread courant). Le marquage lit les Values directement
 *     dans la copie de pile ; des frames on ne marque que ->function.
 *
 * Registre GIL-sérialisé : park (register) / restore (deregister) / mark_roots
 * (lecture) s'exécutent TOUS sous le GIL (le GC ne tourne que sous GIL, les
 * contextes parkés sont gelés dans cond_attend) → aucun verrou supplémentaire,
 * aucune contention interning/allocateur/registry (contrat  §2). */
/*  — slot ambiant « flux courant » (pont transport -> VM). void*
 * OPAQUE : c'est un OwlHttpProdCtx* cote transport, JAMAIS dereferences ici.
 * Miroir dans OwlVmParkSnapshot, sauve/restaure comme un scalaire (pas une
 * racine GC). */
static void* g_vm_flux_courant = NULL;

void  owl_vm_flux_courant_set(void* flux) { g_vm_flux_courant = flux; }
void* owl_vm_flux_courant_get(void)       { return g_vm_flux_courant; }

typedef struct OwlVmParkSnapshot {
    /* Pile de valeurs et frames : portion utilisée seulement (heap). */
    Value*     stack;       int stackCount;
    CallFrame* frames;      int frameCount;
    /* Piles auxiliaires bornées : copiées inline. */
    ExceptionHandler   exceptionStack[EXCEPTION_STACK_MAX]; int exceptionCount;
    Value              outParamReturns[16];                 int outParamCount;
    Value              iterCurrentKey[16];
    Value              iterTarget[16];                      int iterDepth;
    Value              event_sender_stack[32];              int event_sender_stack_count;
    /*  — slot ambiant « flux courant » : void* OPAQUE, sauvé comme un
     * scalaire (PAS une racine GC -> aucun marquage dans mark_park_snapshot). */
    void*              flux_courant;
    /* Piles UI (heap dynamique du singleton) : contenu copié à la demande. */
    Obj**  window_stack;   int window_stack_count;
    Obj**  etat_stack;     int etat_stack_count;
    /* Contexte d'exception statique (currentException + g_exc_*). */
    ObjString* exc_message;
    ObjString* exc_code;
    int        exc_line;
    ObjString* exc_filename;
    ObjString* exc_procedure;
    ObjString* exc_origin;
    ObjString* exc_native_name;
    ObjString* exc_info;
    ObjString* exc_stack;
    int64_t    exc_timestamp_ms;
    int        exc_sys_code;
    ObjString* exc_sys_message;
    bool       exc_sys_pending;
    bool       exc_origin_pending;
    bool       exc_native_name_pending;
    bool       throw_pending;
    bool       capture_uncaught;
    bool       rt_fatal_vm;
    /* Pile de sauvegarde du contexte d'exception (, du contexte courant). */
    ExcContextSnapshot excCtxSaveStack[EXCEPTION_STACK_MAX]; int excSaveDepth;
} OwlVmParkSnapshot;

static OwlVmParkSnapshot* g_park_snapshots[OWL_EXEC_CONTEXT_MAX];
static int                g_park_count = 0;

static void exc_context_capture(ExcContextSnapshot* s) {
    s->message      = currentException;
    s->code         = g_exc_code;
    s->line         = g_exc_line;
    s->filename     = g_exc_filename;
    s->procedure    = g_exc_procedure;
    s->origin       = g_exc_origin;
    s->native_name  = g_exc_native_name;
    s->info         = g_exc_info;
    s->stack        = g_exc_stack;
    s->timestamp_ms = g_exc_timestamp_ms;
    s->sys_code     = g_exc_sys_code;
    s->sys_message  = g_exc_sys_message;
}

static void exc_context_apply(const ExcContextSnapshot* s) {
    currentException   = s->message;
    g_exc_code         = s->code;
    g_exc_line         = s->line;
    g_exc_filename     = s->filename;
    g_exc_procedure    = s->procedure;
    g_exc_origin       = s->origin;
    g_exc_native_name  = s->native_name;
    g_exc_info         = s->info;
    g_exc_stack        = s->stack;
    g_exc_timestamp_ms = s->timestamp_ms;
    g_exc_sys_code     = s->sys_code;
    g_exc_sys_message  = s->sys_message;
}

/* Marquage GC des snapshots vivants : leurs ObjString* peuvent n'etre
 * references que depuis cette pile (ex. message du handler englobant pendant
 * qu'un handler interne s'execute). Appele depuis mark_roots() (gc.c). */
/*  lot 2 — nombre de contextes d'exécution vivants (registre). À N=1
 * vaut toujours 1 ; les lots 3-4 le font varier. Lu par mark_roots (gc.c). */
int vm_exec_context_count(void) {
    /* Contextes COURANTS (adossés au singleton, toujours 1) + snapshots PARKÉS
     * (lot 4). mark_roots itère [0, count) et route chaque index ci-dessous. */
    return g_exec_context_count + g_park_count;
}

/* Marque les 9 ObjString* d'une pile de sauvegarde du contexte d'exception
 *. Partagé entre le contexte courant et les snapshots parkés. */
static void mark_exc_save_stack(const ExcContextSnapshot* stack, int depth) {
    for (int i = 0; i < depth; i++) {
        gc_mark_object((Obj*)stack[i].message);
        gc_mark_object((Obj*)stack[i].code);
        gc_mark_object((Obj*)stack[i].filename);
        gc_mark_object((Obj*)stack[i].procedure);
        gc_mark_object((Obj*)stack[i].origin);
        gc_mark_object((Obj*)stack[i].native_name);
        gc_mark_object((Obj*)stack[i].info);
        gc_mark_object((Obj*)stack[i].stack);
        gc_mark_object((Obj*)stack[i].sys_message);
    }
}

/* Marque l'état d'exécution PARKÉ d'un contexte non-courant, depuis SA COPIE
 * heap. PIÈGE §3 : on lit tout dans le snapshot, jamais dans les arrays
 * globaux (que le thread courant écrase) ; des frames on ne marque que
 * ->function (JAMAIS ->slots, qui pointe dans l'array global d'un autre). */
static void mark_park_snapshot(const OwlVmParkSnapshot* s) {
    for (int i = 0; i < s->stackCount; i++) gc_mark_value(s->stack[i]);
    for (int i = 0; i < s->frameCount; i++) gc_mark_object((Obj*)s->frames[i].function);
    for (int i = 0; i < s->exceptionCount; i++)
        gc_mark_object((Obj*)s->exceptionStack[i].frame.function);
    for (int i = 0; i < s->outParamCount; i++) gc_mark_value(s->outParamReturns[i]);
    for (int i = 0; i < s->iterDepth; i++) {
        gc_mark_value(s->iterTarget[i]);
        gc_mark_value(s->iterCurrentKey[i]);
    }
    for (int i = 0; i < s->event_sender_stack_count; i++)
        gc_mark_value(s->event_sender_stack[i]);
    for (int i = 0; i < s->window_stack_count; i++)
        gc_mark_object((Obj*)s->window_stack[i]);
    for (int i = 0; i < s->etat_stack_count; i++)
        gc_mark_object((Obj*)s->etat_stack[i]);
    gc_mark_object((Obj*)s->exc_message);
    gc_mark_object((Obj*)s->exc_code);
    gc_mark_object((Obj*)s->exc_filename);
    gc_mark_object((Obj*)s->exc_procedure);
    gc_mark_object((Obj*)s->exc_origin);
    gc_mark_object((Obj*)s->exc_native_name);
    gc_mark_object((Obj*)s->exc_info);
    gc_mark_object((Obj*)s->exc_stack);
    gc_mark_object((Obj*)s->exc_sys_message);
    mark_exc_save_stack(s->excCtxSaveStack, s->excSaveDepth);
}

/*  lot 2+4 — marque les racines par-contexte du contexte #index.
 * index < g_exec_context_count : contexte COURANT (adossé au singleton). Sa
 * pile de valeurs / ses frames / ses piles auxiliaires sont déjà marqués par
 * mark_roots depuis les arrays globaux ; il ne reste ici que sa pile de
 * sauvegarde du contexte d'exception.
 * index >= g_exec_context_count : snapshot PARKÉ (lot 4), marqué depuis sa
 * copie heap (cf. mark_park_snapshot, piège §3). */
void vm_mark_context_roots(int index) {
    if (index < g_exec_context_count) {
        OwlExecContext* ctx = g_exec_contexts[index];
        mark_exc_save_stack(ctx->excCtxSaveStack, ctx->excSaveDepth);
        return;
    }
    mark_park_snapshot(g_park_snapshots[index - g_exec_context_count]);
}

/*  lot 4 — park : copie l'état d'exécution vif dans un snapshot
 * possédé (heap), l'enregistre dans le registre parké (que mark_roots itère),
 * et remet le singleton à un baseline vide pour le prochain détenteur du GIL.
 * Appelé SOUS le GIL (owl_http_rep_flux_pousse, au point de blocage flux).
 * Aucune allocation OWL ici (calloc/malloc C pur) : aucun GC déclenché entre
 * la copie et l'enregistrement. Renvoie le handle passé plus tard à restore. */
void* owl_vm_exec_park(void) {
    OwlVmParkSnapshot* s = (OwlVmParkSnapshot*)calloc(1, sizeof(OwlVmParkSnapshot));
    if (!s) return NULL;   /* park impossible : le caller ne yield pas, attente historique */

    /* Pile de valeurs + frames : portion utilisée seulement. */
    s->stackCount = (int)(vm.stackTop - vm.stack);
    s->frameCount = vm.frameCount;
    if (s->stackCount > 0) {
        s->stack = (Value*)malloc((size_t)s->stackCount * sizeof(Value));
        memcpy(s->stack, vm.stack, (size_t)s->stackCount * sizeof(Value));
    }
    if (s->frameCount > 0) {
        s->frames = (CallFrame*)malloc((size_t)s->frameCount * sizeof(CallFrame));
        memcpy(s->frames, vm.frames, (size_t)s->frameCount * sizeof(CallFrame));
    }

    /* Piles auxiliaires bornées (inline). */
    s->exceptionCount = exceptionCount;
    memcpy(s->exceptionStack, exceptionStack,
           (size_t)exceptionCount * sizeof(ExceptionHandler));
    s->outParamCount = vm.outParamCount;
    memcpy(s->outParamReturns, vm.outParamReturns, sizeof(vm.outParamReturns));
    s->iterDepth = vm.iterDepth;
    memcpy(s->iterCurrentKey, vm.iterCurrentKey, sizeof(vm.iterCurrentKey));
    memcpy(s->iterTarget, vm.iterTarget, sizeof(vm.iterTarget));
    s->event_sender_stack_count = vm.event_sender_stack_count;
    memcpy(s->event_sender_stack, vm.event_sender_stack, sizeof(vm.event_sender_stack));
    s->flux_courant = g_vm_flux_courant;   /*  : scalaire opaque */

    /* Piles UI (heap dynamique) : contenu copié, l'array du singleton reste. */
    s->window_stack_count = vm.window_stack_count;
    if (s->window_stack_count > 0) {
        s->window_stack = (Obj**)malloc((size_t)s->window_stack_count * sizeof(Obj*));
        memcpy(s->window_stack, vm.window_stack,
               (size_t)s->window_stack_count * sizeof(Obj*));
    }
    s->etat_stack_count = vm.etat_stack_count;
    if (s->etat_stack_count > 0) {
        s->etat_stack = (Obj**)malloc((size_t)s->etat_stack_count * sizeof(Obj*));
        memcpy(s->etat_stack, vm.etat_stack,
               (size_t)s->etat_stack_count * sizeof(Obj*));
    }

    /* Contexte d'exception statique. */
    s->exc_message         = currentException;
    s->exc_code            = g_exc_code;
    s->exc_line            = g_exc_line;
    s->exc_filename        = g_exc_filename;
    s->exc_procedure       = g_exc_procedure;
    s->exc_origin          = g_exc_origin;
    s->exc_native_name     = g_exc_native_name;
    s->exc_info            = g_exc_info;
    s->exc_stack           = g_exc_stack;
    s->exc_timestamp_ms    = g_exc_timestamp_ms;
    s->exc_sys_code        = g_exc_sys_code;
    s->exc_sys_message     = g_exc_sys_message;
    s->exc_sys_pending         = g_exc_sys_pending;
    s->exc_origin_pending      = g_exc_origin_pending;
    s->exc_native_name_pending = g_exc_native_name_pending;
    s->throw_pending       = throwPending;
    s->capture_uncaught    = g_capture_uncaught;
    s->rt_fatal_vm         = g_rt_fatal_vm;

    /* Pile de sauvegarde du contexte d'exception. */
    s->excSaveDepth = g_exec_ctx_cur->excSaveDepth;
    memcpy(s->excCtxSaveStack, g_exec_ctx_cur->excCtxSaveStack,
           (size_t)s->excSaveDepth * sizeof(ExcContextSnapshot));

    /* Enregistre AVANT de vider le singleton : à partir d'ici, un GC (depuis un
     * autre thread qui aura pris le GIL) marquera l'état vif depuis ce snapshot. */
    g_park_snapshots[g_park_count++] = s;

    /* Baseline vide : le prochain détenteur du GIL démarre proprement (handler
     * neuf) ou restaurera son propre snapshot. */
    vm.stackTop                = vm.stack;
    vm.frameCount              = 0;
    exceptionCount             = 0;
    vm.outParamCount           = 0;
    vm.iterDepth               = 0;
    vm.event_sender_stack_count = 0;
    g_vm_flux_courant          = NULL;   /*  : baseline (prochain detenteur propre) */
    vm.window_stack_count      = 0;
    vm.etat_stack_count        = 0;
    currentException           = NULL;
    g_exc_code                 = NULL;
    g_exc_line                 = 0;
    g_exc_filename             = NULL;
    g_exc_procedure            = NULL;
    g_exc_origin               = NULL;
    g_exc_native_name          = NULL;
    g_exc_info                 = NULL;
    g_exc_stack                = NULL;
    g_exc_timestamp_ms         = 0;
    g_exc_sys_code             = 0;
    g_exc_sys_message          = NULL;
    g_exc_sys_pending          = false;
    g_exc_origin_pending       = false;
    g_exc_native_name_pending  = false;
    throwPending               = false;
    g_capture_uncaught         = false;
    g_rt_fatal_vm              = false;
    g_exec_ctx_cur->excSaveDepth = 0;

    return s;
}

/*  lot 4 — restore : recopie le snapshot dans les MÊMES arrays
 * physiques du singleton (invariant M2 : les pointeurs absolus se réalignent),
 * le retire du registre parké, libère sa mémoire. Appelé SOUS le GIL, juste
 * après réacquisition, par le thread qui l'avait parké. */
void owl_vm_exec_restore(void* handle) {
    OwlVmParkSnapshot* s = (OwlVmParkSnapshot*)handle;
    if (!s) return;

    /* Pile de valeurs + frames dans les MÊMES arrays (base vm.stack inchangée
     * → frame->slots / stackTop / ExceptionHandler.* se réalignent). */
    if (s->stackCount > 0) memcpy(vm.stack, s->stack, (size_t)s->stackCount * sizeof(Value));
    vm.stackTop = vm.stack + s->stackCount;
    if (s->frameCount > 0) memcpy(vm.frames, s->frames, (size_t)s->frameCount * sizeof(CallFrame));
    vm.frameCount = s->frameCount;

    exceptionCount = s->exceptionCount;
    memcpy(exceptionStack, s->exceptionStack,
           (size_t)s->exceptionCount * sizeof(ExceptionHandler));
    vm.outParamCount = s->outParamCount;
    memcpy(vm.outParamReturns, s->outParamReturns, sizeof(vm.outParamReturns));
    vm.iterDepth = s->iterDepth;
    memcpy(vm.iterCurrentKey, s->iterCurrentKey, sizeof(vm.iterCurrentKey));
    memcpy(vm.iterTarget, s->iterTarget, sizeof(vm.iterTarget));
    vm.event_sender_stack_count = s->event_sender_stack_count;
    memcpy(vm.event_sender_stack, s->event_sender_stack, sizeof(vm.event_sender_stack));
    g_vm_flux_courant = s->flux_courant;   /*  : restaure le slot opaque */

    /* Piles UI : l'array du singleton est présent (capacité ≥ compte parké,
     * elle ne fait que croître) ; on y recopie le contenu. */
    if (s->window_stack_count > 0)
        memcpy(vm.window_stack, s->window_stack,
               (size_t)s->window_stack_count * sizeof(Obj*));
    vm.window_stack_count = s->window_stack_count;
    if (s->etat_stack_count > 0)
        memcpy(vm.etat_stack, s->etat_stack,
               (size_t)s->etat_stack_count * sizeof(Obj*));
    vm.etat_stack_count = s->etat_stack_count;

    currentException          = s->exc_message;
    g_exc_code                = s->exc_code;
    g_exc_line                = s->exc_line;
    g_exc_filename            = s->exc_filename;
    g_exc_procedure           = s->exc_procedure;
    g_exc_origin              = s->exc_origin;
    g_exc_native_name         = s->exc_native_name;
    g_exc_info                = s->exc_info;
    g_exc_stack               = s->exc_stack;
    g_exc_timestamp_ms        = s->exc_timestamp_ms;
    g_exc_sys_code            = s->exc_sys_code;
    g_exc_sys_message         = s->exc_sys_message;
    g_exc_sys_pending         = s->exc_sys_pending;
    g_exc_origin_pending      = s->exc_origin_pending;
    g_exc_native_name_pending = s->exc_native_name_pending;
    throwPending              = s->throw_pending;
    g_capture_uncaught        = s->capture_uncaught;
    g_rt_fatal_vm             = s->rt_fatal_vm;

    g_exec_ctx_cur->excSaveDepth = s->excSaveDepth;
    memcpy(g_exec_ctx_cur->excCtxSaveStack, s->excCtxSaveStack,
           (size_t)s->excSaveDepth * sizeof(ExcContextSnapshot));

    /* Désenregistre (swap-remove par pointeur : robuste aux parks imbriqués,
     * l'ordre du registre n'importe pas pour le marquage). */
    for (int i = 0; i < g_park_count; i++) {
        if (g_park_snapshots[i] == s) {
            g_park_snapshots[i] = g_park_snapshots[--g_park_count];
            g_park_snapshots[g_park_count] = NULL;
            break;
        }
    }

    free(s->stack);
    free(s->frames);
    free(s->window_stack);
    free(s->etat_stack);
    free(s);
}

/* ================================================================
 * NATIVE FUNCTIONS (builtins)
 * ================================================================ */

/* Natives that were here (Trace, Taille, Ajoute, Supprime, Occurrence)
 * have been moved to stdlib_io.c, stdlib_string.c, and stdlib_array.c. */

/* __StructNew(name) - create a new ObjInstance */
static Value native_struct_new(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* name = AS_STRING(args[0]);
    ObjInstance* instance = obj_new_instance(name);
    return OBJ_VAL(instance);
}

/* __EnumVal(enumTypeName, valueName, value) - create enum value instance */
static Value native_enum_val(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return NIL_VAL;
    ObjString* typeName = AS_STRING(args[0]);
    ObjString* valName = AS_STRING(args[1]);
    Value valeur = args[2];

    /*  : instance transient (C-stack uniquement) doit etre push
     * avant les obj_copy_string + table_set qui peuvent grow. */
    ObjInstance* instance = obj_new_instance(typeName);
    vm_push(OBJ_VAL(instance));

    /* Set Nom field */
    ObjString* nomKey = obj_copy_string("Nom", 3);
    vm_push(OBJ_VAL(nomKey));
    table_set(&instance->fields, nomKey, OBJ_VAL(valName));
    vm_pop(); /* nomKey */

    /* Set Valeur field */
    ObjString* valKey = obj_copy_string("Valeur", 6);
    vm_push(OBJ_VAL(valKey));
    if (IS_OBJ(valeur)) vm_push(valeur);
    table_set(&instance->fields, valKey, valeur);
    if (IS_OBJ(valeur)) vm_pop();
    vm_pop(); /* valKey */

    vm_pop(); /* instance */
    return OBJ_VAL(instance);
}

static Value value_deep_clone(Value v);

/* __StructClone(template) - clone an ObjInstance en deleguant CHAQUE champ a
 * value_deep_clone. Le template porte les valeurs initiales declarees ; sans
 * copie, toutes les instances partagent le meme objet et une ecriture en place
 * (Ajoute sur un tableau, BufferEcritOctet, doc["k"]=, ml["fr"]=) corrompt les
 * autres instances.
 *
 * Cette fonction a longtemps enumere ses cas (assoc, puis tableau, puis...),
 * ce qui a fait decouvrir la meme faille trois fois de suite, type par type.
 * La delegation ferme la famille : la question "quels types cloner" a une
 * seule reponse, celle de deep_clone_obj. Ne PAS reintroduire d'enumeration
 * ici — un type mutable en place qui manquerait se corrige dans
 * deep_clone_obj, ou il profite aussi a tous les autres appelants. */
static Value native_struct_clone(int argCount, Value* args) {
    (void)argCount;
    if (!IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* tmpl = AS_INSTANCE(args[0]);
    ObjInstance* instance = obj_new_instance(tmpl->structName);
    /* Propager fieldOrder + klass — partagés en lecture (immutables côté
     * runtime). Permet aux clients runtime (sérialisation, AVEC, etc.) de
     * connaître l'ordre déclaré des champs sur les copies aussi. */
    instance->fieldOrder = tmpl->fieldOrder;
    instance->klass      = tmpl->klass;
    /* Root the fresh instance while we recurse — sub-clone may trigger GC. */
    vm_push(OBJ_VAL(instance));
    for (int i = 0; i < tmpl->fields.capacity; i++) {
        Entry* e = &tmpl->fields.entries[i];
        if (e->key == NULL) continue;
        Value v = value_deep_clone(e->value);
        /*   : push v avant table_set qui peut grow → GC.
         * Sans ce push, v (clone fresh) est unrooted entre la recursion et
         * l'insertion ; le slot recoit du poison → lookup ulterieur du
         * champ retourne NIL → "Only struct instances have fields". */
        if (IS_OBJ(v)) vm_push(v);
        table_set(&instance->fields, e->key, v);
        if (IS_OBJ(v)) vm_pop();
    }
    vm_pop();
    return OBJ_VAL(instance);
}

/*  : clone une instance template enregistree dans vm.types
 * ( : migration depuis vm.globals vers vm.types pour namespacer
 * les templates). Retourne une instance fraiche deep-clonee (chaque
 * sous-objet duplique), ou NIL_VAL si le template est absent ou n'est
 * pas une instance. Utilise par le runtime UI pour materialiser une prop
 * composee (OWL_PROP_TYPE_OBJECT) a la demande en mode headless. */
Value owl_clone_global_template(const char* name) {
    if (!name) return NIL_VAL;
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    Value tplVal;
    if (!table_get(&vm.types, k, &tplVal) || !IS_INSTANCE(tplVal)) return NIL_VAL;
    return native_struct_clone(1, &tplVal);
}

/*  — convergence des raccourcis Police plats vers la prop composee.
 * `LIB_X..PoliceNom` et `LIB_X..Police..Nom` doivent etre strictement
 * equivalents : l'instance Police materialisee (headless_store["police"])
 * est l'UNIQUE source de verite. Cette table mappe chaque raccourci coeur
 * (forme FR et EN, lowercase ASCII apres strip-accent du scanner) vers le
 * sous-champ canon de l'instance Police.
 *
 * Hors-scope (Lot B, ticket 0146) : Condensee/Etendue/Large/TailleMin
 * restent des props plates et ne figurent PAS dans cette table. */
static const char* police_shortcut_to_subfield(const char* prop) {
    if (!prop) return NULL;
    static const struct { const char* shortcut; const char* sub; } MAP[] = {
        { "policenom",       "Nom" },      { "fontname",       "Nom" },
        { "policetaille",    "Taille" },   { "fontsize",       "Taille" },
        { "policegras",      "Gras" },     { "fontbold",       "Gras" },
        { "policeitalique",  "Italique" }, { "fontitalic",     "Italique" },
        { "policesoulignee", "Souligne" }, { "fontunderlined", "Souligne" },
        { "policebarree",    "Barre" },    { "fontstrikeout",  "Barre" },
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (strcasecmp(prop, MAP[i].shortcut) == 0) return MAP[i].sub;
    }
    return NULL;
}

/* Materialise (ou recupere) l'instance Police headless d'un control. Clone
 * lazy du template global au premier acces, stockee sous "police" dans le
 * headless_store — exactement le meme storage que la cascade composee
 * `..Police..SousChamp` (cf. OP_STRUCT_GET ). Garantit l'unicite de
 * la source de verite entre raccourcis et cascade. Retourne NULL si le clone
 * echoue (template absent). */
static ObjInstance* vm_headless_police_instance(ObjUiControl* uic) {
    Value existing;
    if (obj_ui_control_headless_get(uic, "police", &existing)
        && IS_INSTANCE(existing)) {
        return AS_INSTANCE(existing);
    }
    Value inst = owl_clone_global_template("Police");
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst); /* root pendant le set (peut grossir la table -> GC) */
    obj_ui_control_headless_set(uic, "police", inst);
    vm_pop();
    return AS_INSTANCE(inst);
}

/*  — pendant ObjUiControlInstance pour le modele C. Materialise
 * l'instance Police a la demande dans le headless_store de l'uici. */
static ObjInstance* vm_headless_police_instance_for_uici(ObjUiControlInstance* uici) {
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, "police", &existing)
        && IS_INSTANCE(existing)) {
        /*  lot 2 — re-assure la liaison composite->widget (idempotent)
         * au cas ou l'instance Police a ete materialisee avant le montage Qt. */
        if (uici->widget != NULL) {
            owl_ui_register_instance_composite(uici, AS_INSTANCE(existing));
        }
        return AS_INSTANCE(existing);
    }
    Value inst = owl_clone_global_template("Police");
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst);
    obj_ui_control_instance_headless_set(uici, "police", inst);
    /*  lot 2 — relie la Police materialisee a son widget monte. */
    if (uici->widget != NULL) {
        owl_ui_register_instance_composite(uici, AS_INSTANCE(inst));
    }
    vm_pop();
    return AS_INSTANCE(inst);
}

/*  — convergence des props plates d'apparence du Champ Saisie vers
 * l'instance Style du store (meme principe que les raccourcis Police ci-dessus,
 * ADR ). Le domicile unique des props de base est la ZoneSaisie de
 * l'instance styleSaisie ; le plat n'est qu'un alias fin :
 *   CouleurFond (plat)   <-> Style..ZoneSaisie..CouleurFond
 *   PoliceCouleur (plat) <-> Style..ZoneSaisie..Couleur (couleur du texte)
 * `Couleur` nu n'existe pas pour Saisie (redondant, cf. registry). Le mapping
 * ne vaut QUE pour un champ Saisie : Libelle/Bouton gardent leur chemin plat
 * direct (PoliceCouleur peint le texte via la Police, cf. couleur_police_
 * raccourci). Retourne le sous-champ canon de la brique, ou NULL. */
static const char* saisie_style_shortcut_to_subfield(const char* prop) {
    if (!prop) return NULL;
    if (strcasecmp(prop, "CouleurFond") == 0 ||
        strcasecmp(prop, "BackgroundColor") == 0) return "CouleurFond";
    if (strcasecmp(prop, "PoliceCouleur") == 0 ||
        strcasecmp(prop, "FontColor") == 0) return "Couleur";
    return NULL;
}

/*  — materialise (ou recupere) la brique ZoneSaisie de l'instance
 * Style du store d'un uici, source de verite unique des props de base d'un
 * Champ Saisie. Clone lazy du template styleSaisie au premier acces, stocke
 * sous la cle canon "Style" (meme storage que la cascade `..Style..ZoneSaisie`,
 * cf. OP_STRUCT_GET prop OBJECT). Retourne NULL si le template est absent ou si
 * la structure ne porte pas de ZoneSaisie. */
static ObjInstance* vm_zonesaisie_field(ObjInstance* style) {
    if (!style) return NULL;
    ObjString* k = obj_copy_string("ZoneSaisie", 10);
    Value v;
    if (table_get(&style->fields, k, &v) && IS_INSTANCE(v)) return AS_INSTANCE(v);
    return NULL;
}

static ObjInstance* vm_style_zonesaisie_for_uici(ObjUiControlInstance* uici) {
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, "Style", &existing)
        && IS_INSTANCE(existing)) {
        if (uici->widget != NULL)
            owl_ui_register_instance_composite(uici, AS_INSTANCE(existing));
        return vm_zonesaisie_field(AS_INSTANCE(existing));
    }
    Value inst = owl_clone_global_template("styleSaisie");
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst); /* root pendant le set (peut grossir la table -> GC) */
    obj_ui_control_instance_headless_set(uici, "Style", inst);
    if (uici->widget != NULL)
        owl_ui_register_instance_composite(uici, AS_INSTANCE(inst));
    vm_pop();
    return vm_zonesaisie_field(AS_INSTANCE(inst));
}

static ObjInstance* vm_style_zonesaisie_for_uic(ObjUiControl* uic) {
    Value existing;
    if (obj_ui_control_headless_get(uic, "Style", &existing)
        && IS_INSTANCE(existing)) {
        return vm_zonesaisie_field(AS_INSTANCE(existing));
    }
    Value inst = owl_clone_global_template("styleSaisie");
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst);
    obj_ui_control_headless_set(uic, "Style", inst);
    vm_pop();
    return vm_zonesaisie_field(AS_INSTANCE(inst));
}

/*  — convergence des props plates d'apparence des widgets mono-element
 * (Libellé, Bouton) vers l'instance Style du store. Contrairement au Saisie
 * (brique unique ZoneSaisie), chaque prop route vers une sous-brique differente :
 *   CouleurFond (plat)   <-> Style..CadreExtérieur..CouleurFond (le fond)
 *   PoliceCouleur (plat) <-> Style..Libellé..Couleur           (le texte)
 * Renseigne *outBrick (sous-element interne, ASCII) + *outSub (sous-champ) et
 * retourne true si `prop` est un raccourci mono connu pour ce kind. */
static bool mono_style_shortcut_bricksub_vm(int kind, const char* prop,
                                            const char** outBrick,
                                            const char** outSub) {
    if (kind != OWL_CONTROL_LABEL && kind != OWL_CONTROL_BUTTON) return false;
    if (!prop) return false;
    if (strcasecmp(prop, "CouleurFond") == 0 ||
        strcasecmp(prop, "BackgroundColor") == 0) {
        *outBrick = "CadreExterieur";
        *outSub   = "CouleurFond";
        return true;
    }
    if (strcasecmp(prop, "PoliceCouleur") == 0 ||
        strcasecmp(prop, "FontColor") == 0) {
        *outBrick = "Libelle";
        *outSub   = "Couleur";
        return true;
    }
    /*  — Bouton uniquement : BTN..Image (plat) unifie avec
     * Style..Image..Image (feuille styleImageBouton). */
    if (kind == OWL_CONTROL_BUTTON && strcasecmp(prop, "Image") == 0) {
        *outBrick = "Image";
        *outSub   = "Image";
        return true;
    }
    return false;
}

static const char* mono_style_template_name_vm(int kind) {
    if (kind == OWL_CONTROL_LABEL)  return "styleLibell\xc3\xa9";  /* styleLibellé */
    if (kind == OWL_CONTROL_BUTTON) return "styleBouton";
    return NULL;
}

/* Navigue vers la sous-brique `brickName` de l'instance `style`. */
static ObjInstance* vm_style_brick_field(ObjInstance* style, const char* brickName) {
    if (!style) return NULL;
    ObjString* k = obj_copy_string(brickName, (int)strlen(brickName));
    Value v;
    if (table_get(&style->fields, k, &v) && IS_INSTANCE(v)) return AS_INSTANCE(v);
    return NULL;
}

/* Materialise (clone lazy) l'instance Style d'un uici mono selon son kind
 * (styleLibellé / styleBouton), stockee sous la cle "Style" (meme storage que la
 * cascade `..Style..SousElement`), puis navigue vers `brickName`. */
static ObjInstance* vm_style_mono_brick_for_uici(ObjUiControlInstance* uici,
                                                 int kind, const char* brickName) {
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, "Style", &existing)
        && IS_INSTANCE(existing)) {
        if (uici->widget != NULL)
            owl_ui_register_instance_composite(uici, AS_INSTANCE(existing));
        return vm_style_brick_field(AS_INSTANCE(existing), brickName);
    }
    const char* tpl = mono_style_template_name_vm(kind);
    if (!tpl) return NULL;
    Value inst = owl_clone_global_template(tpl);
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst); /* root pendant le set (peut grossir la table -> GC) */
    obj_ui_control_instance_headless_set(uici, "Style", inst);
    if (uici->widget != NULL)
        owl_ui_register_instance_composite(uici, AS_INSTANCE(inst));
    vm_pop();
    return vm_style_brick_field(AS_INSTANCE(inst), brickName);
}

static ObjInstance* vm_style_mono_brick_for_uic(ObjUiControl* uic,
                                                int kind, const char* brickName) {
    Value existing;
    if (obj_ui_control_headless_get(uic, "Style", &existing)
        && IS_INSTANCE(existing)) {
        return vm_style_brick_field(AS_INSTANCE(existing), brickName);
    }
    const char* tpl = mono_style_template_name_vm(kind);
    if (!tpl) return NULL;
    Value inst = owl_clone_global_template(tpl);
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst);
    obj_ui_control_headless_set(uic, "Style", inst);
    vm_pop();
    return vm_style_brick_field(AS_INSTANCE(inst), brickName);
}

/* ============================================================
 *  — mise en forme par cellule / par ligne de Table.
 *
 * Deux receveurs, un seul stockage. `TABLE_X[ligne]` rend la rep de ligne du
 * store d'items (, taguee `$tblsrc`) ; `TABLE_X[ligne][colonne]` rend
 * un descripteur leger qui pointe cette meme rep + le nom canon de la colonne.
 * Les proprietes atterrissent donc TOUJOURS sur la rep, sous une cle reservee
 * `$fmt.<colonne>.<PropCanonFR>` (partie colonne vide = niveau ligne). La rep
 * vit dans le store d'items, donc la valeur persiste et se relit par le meme
 * chemin — la lecture headless est deterministe, sans Qt.
 *
 * Pas de nouveau type GC : miroir du proxy-colonne de .
 * ============================================================ */

/*  — holder persistant de mise en forme PAR COLONNE, porte par la Table
 * dans son headless_store sous OWL_TABLE_COLFMT_HOLDER_KEY. Un unique
 * ObjInstance dont les fields portent `$fmt.<col>.<PropCanon>` pour toutes les
 * colonnes : meme schema de cles que les reps de ligne, donc le stockage/lecture
 * fonctionnent inchanges. Le holder vit sur l'uici de la Table (root GC) : il
 * survit entre deux acces, contrairement au proxy-colonne ephemere.
 *
 * `proxy` est rootee pendant l'alloc car outCol pointera dans un de ses fields :
 * obj_new_instance + headless_set peuvent declencher un collect. */
static ObjInstance* vm_table_colfmt_holder(ObjInstance* proxy,
                                           ObjUiControlInstance* uici) {
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, OWL_TABLE_COLFMT_HOLDER_KEY,
                                             &existing)
        && IS_INSTANCE(existing)) {
        return AS_INSTANCE(existing);
    }
    vm_push(OBJ_VAL(proxy));    /* root : outCol pointera dans un field du proxy */
    ObjInstance* holder = obj_new_instance(NULL);
    vm_push(OBJ_VAL(holder));   /* root pendant le headless_set (table grow) */
    /*  Lot C — back-ref vers la Table proprietaire (symetrie avec les
     * reps de ligne). Le hook de rendu owl_ui_table_fmt_applied en deduit le
     * widget pour peindre toute la colonne ; le read-back Lot B (cle $fmt.*)
     * ignore ce field. */
    inst_field_set_value(holder, OWL_TABLE_REP_SRC_KEY, OBJ_VAL(uici));
    obj_ui_control_instance_headless_set(uici, OWL_TABLE_COLFMT_HOLDER_KEY,
                                         OBJ_VAL(holder));
    vm_pop();                   /* holder (desormais joignable via le store) */
    vm_pop();                   /* proxy */
    return holder;
}

/* Resout le receveur d'une propriete de mise en forme.
 *   - descripteur de cellule : rep cible + nom de colonne ;
 *   - rep de ligne taguee `$tblsrc` : la rep elle-meme, colonne vide ;
 *   - proxy-colonne : holder persistant de la Table + nom de colonne.
 * Faux pour toute autre instance (struct user, rep ZoneRepetee, combinaison). */
static bool vm_table_fmt_target(ObjInstance* inst, ObjInstance** outRep,
                                const char** outCol) {
    if (inst == NULL) return false;
    ObjString* k = table_find_key_case_insensitive(&inst->fields,
                                                   OWL_TABLE_CELLDESC_REP_KEY);
    Value v;
    if (k != NULL && table_get(&inst->fields, k, &v) && IS_INSTANCE(v)) {
        ObjString* ck = table_find_key_case_insensitive(
            &inst->fields, OWL_TABLE_CELLDESC_COL_KEY);
        Value cv;
        if (ck == NULL || !table_get(&inst->fields, ck, &cv) || !IS_STRING(cv))
            return false;
        *outRep = AS_INSTANCE(v);
        *outCol = AS_CSTRING(cv);
        return true;
    }
    k = table_find_key_case_insensitive(&inst->fields, OWL_TABLE_REP_SRC_KEY);
    if (k != NULL && table_get(&inst->fields, k, &v)
        && IS_UI_CONTROL_INSTANCE(v)) {
        *outRep = inst;
        *outCol = "";
        return true;
    }
    /*  (ferme ) — proxy-colonne receveur : `COL_A..Couleur =`
     * en forme nue vise ce proxy. On route vers le holder persistant de la
     * Table, la couleur atterrit sous `$fmt.<col>.<PropCanon>` et se relit a
     * l'identique. Sans cette branche, l'ecriture retombait sur le fallback
     * generique (table_set dans un ObjInstance recree a chaque acces) -> no-op
     * silencieux, relecture Null. */
    k = table_find_key_case_insensitive(&inst->fields,
                                        OWL_TABLE_COLPROXY_REF_KEY);
    ObjString* colK = table_find_key_case_insensitive(
        &inst->fields, OWL_TABLE_COLPROXY_COL_KEY);
    Value refV, colV;
    if (k != NULL && colK != NULL
        && table_get(&inst->fields, k, &refV) && IS_UI_CONTROL_REF(refV)
        && table_get(&inst->fields, colK, &colV) && IS_STRING(colV)) {
        ObjUiControlInstance* uici = resolve_ui_ref(AS_UI_CONTROL_REF(refV));
        if (uici != NULL && uici->decl != NULL
            && uici->decl->kind == OWL_CONTROL_TABLE) {
            ObjInstance* holder = vm_table_colfmt_holder(inst, uici);
            if (holder != NULL) {
                *outRep = holder;
                *outCol = AS_CSTRING(colV);   /* proxy rootee dans le holder */
                return true;
            }
        }
    }
    return false;
}

static void vm_table_fmt_key(char* out, size_t cap, const char* col,
                             const char* propCanon) {
    snprintf(out, cap, "%s.%s.%s", OWL_TABLE_FMT_KEY_PREFIX, col, propCanon);
}

/* Commun aux deux sens : valide que `field` est une prop de mise en forme, que
 * `inst` est un receveur de Table, et rend le casier de stockage.
 *
 * Une colonne homonyme garde la priorite au niveau LIGNE : `TABLE_X[l]..Couleur`
 * sur une Table qui declare une colonne nommee Couleur reste la lecture de la
 * VALEUR de cellule. Ne jamais masquer un chemin livre pour ouvrir
 * le nouveau. */
static bool vm_table_fmt_slot(ObjInstance* inst, const char* field,
                              ObjInstance** outRep, char* keyBuf,
                              size_t keyCap, const char** outCol,
                              const char** outPropCanon) {
    const OwlControlPropDef* def = owl_table_cell_catalog_find(field);
    if (def == NULL) return false;
    ObjInstance* rep = NULL;
    const char* col = NULL;
    if (!vm_table_fmt_target(inst, &rep, &col)) return false;
    if (col[0] == '\0'
        && table_find_key_case_insensitive(&rep->fields, field) != NULL)
        return false;
    vm_table_fmt_key(keyBuf, keyCap, col, def->nameFR);
    *outRep = rep;
    if (outCol != NULL)       *outCol = col;
    if (outPropCanon != NULL) *outPropCanon = def->nameFR;
    return true;
}

/*  — liste DECLAREE de la colonne combo <col> (accents strippes) :
 * parcourt les props de declaration de la Table. "" si aucune. */
static const char* vm_table_col_declared_content(ObjUiControlInstance* uici,
                                                 const char* col) {
    const OwlControlDecl* d = uici->decl;
    for (int ci = 0; ci < d->column_count; ci++) {
        const char* cn = d->columns[ci].name;
        if (cn == NULL) continue;
        char cstr[256];
        int clen = scanner_strip_accents(cn, (int)strlen(cn), cstr, sizeof(cstr));
        if (clen <= 0 || clen >= (int)sizeof(cstr)) continue;
        cstr[clen] = '\0';
        if (strcasecmp(cstr, col) != 0) continue;
        for (int pi = 0; pi < d->columns[ci].property_count; pi++) {
            const OwlPropertyInit* p = &d->columns[ci].properties[pi];
            if (!p->is_constant) continue;
            if ((strcasecmp(p->name, "Contenu") == 0
                 || strcasecmp(p->name, "Content") == 0)
                && IS_STRING(p->const_value))
                return AS_CSTRING(p->const_value);
        }
        return "";
    }
    return "";
}

/*  — liste EFFECTIVE de la colonne combo <col> : override runtime
 * pose au niveau colonne (`COL..Contenu =`, store $fmt.<col>.Contenu du holder)
 * si present, sinon la liste declaree dans le bloc `colonne`. Sert de fallback
 * de relecture d'une cellule jamais surchargee (divergence Contenu vs Couleur).
 * `uici` est resolu depuis la back-ref $tblsrc du receveur (rep de ligne ou
 * holder colonne). */
static void vm_table_col_effective_content(ObjUiControlInstance* uici,
                                           const char* col, Value* out) {
    char fmtKey[192];
    vm_table_fmt_key(fmtKey, sizeof(fmtKey), col, "Contenu");
    Value holderV;
    if (obj_ui_control_instance_headless_get(uici, OWL_TABLE_COLFMT_HOLDER_KEY,
                                             &holderV)
        && IS_INSTANCE(holderV)) {
        ObjInstance* holder = AS_INSTANCE(holderV);
        ObjString* hk = table_find_key_case_insensitive(&holder->fields, fmtKey);
        Value stored;
        if (hk != NULL && table_get(&holder->fields, hk, &stored)
            && IS_STRING(stored)) {
            *out = stored;
            return;
        }
    }
    const char* decl = vm_table_col_declared_content(uici, col);
    *out = OBJ_VAL(obj_copy_string(decl, (int)strlen(decl)));
}

static bool vm_table_fmt_get(ObjInstance* inst, const char* field, Value* out) {
    ObjInstance* rep = NULL;
    char key[192];
    const char* col = NULL;
    const char* propCanon = NULL;
    if (!vm_table_fmt_slot(inst, field, &rep, key, sizeof(key), &col, &propCanon))
        return false;
    *out = NIL_VAL;
    ObjString* k = table_find_key_case_insensitive(&rep->fields, key);
    if (k != NULL) {
        table_get(&rep->fields, k, out);
        return true;
    }
    /*  — divergence Contenu : une cellule (ou une colonne) jamais
     * surchargee ne rend PAS Null comme les couleurs, mais la liste effective
     * de la colonne. Le receveur (rep de ligne OU holder colonne) porte la
     * back-ref $tblsrc vers l'uici de la Table. */
    if (propCanon != NULL && strcasecmp(propCanon, "Contenu") == 0
        && col != NULL && col[0] != '\0') {
        Value srcV;
        ObjString* sk = table_find_key_case_insensitive(&rep->fields,
                                                        OWL_TABLE_REP_SRC_KEY);
        if (sk != NULL && table_get(&rep->fields, sk, &srcV)
            && IS_UI_CONTROL_INSTANCE(srcV)) {
            vm_table_col_effective_content(AS_UI_CONTROL_INSTANCE(srcV), col, out);
        }
    }
    return true;
}

/* Descripteur de cellule : rep de ligne cible + nom canon de colonne.
 * `repVal` doit etre DEJA joignable par le GC (operande de pile ou rootee par
 * l'appelant) : obj_new_instance peut declencher un collect. */
static ObjInstance* vm_table_cell_descriptor(Value repVal, const char* colKey) {
    ObjInstance* cell = obj_new_instance(NULL);
    vm_push(OBJ_VAL(cell));   /* root pendant les set (table grow) */
    inst_field_set_value(cell, OWL_TABLE_CELLDESC_REP_KEY, repVal);
    inst_field_set_str(cell, OWL_TABLE_CELLDESC_COL_KEY, colKey);
    vm_pop();
    return cell;
}

static bool vm_table_fmt_set(ObjInstance* inst, const char* field, Value v) {
    ObjInstance* rep = NULL;
    char key[192];
    const char* col = NULL;
    const char* propCanon = NULL;
    if (!vm_table_fmt_slot(inst, field, &rep, key, sizeof(key), &col, &propCanon))
        return false;
    inst_field_set_value(rep, key, v);   /* helper canon : root rep + v */
    /* Le store etant pose, le widget suit. No-op en headless et en CLI : la
     * relecture ne passe jamais par Qt, l'affichage n'est qu'un miroir. */
    owl_ui_table_fmt_applied(rep, col, propCanon, v);
    return true;
}

/*  — pont pour la famille ListeXxx ciblant une COLONNE combo. Le
 * receveur `TABLE.COL` desucre en proxy-colonne (COLPROXY_*) ; on lit/ecrit sa
 * liste par defaut via EXACTEMENT le meme chemin que la propriete `..Contenu`
 * (holder $colfmt, cle $fmt.<col>.Contenu + rebuild des cellules), garantissant
 * la coherence bidirectionnelle testee. Le gating "colonne combo uniquement"
 * est fait cote appelant (stdlib_ui.c) avant d'appeler ces primitives. */
bool owl_vm_table_colproxy_content_get(Value proxy, Value* out) {
    if (!IS_INSTANCE(proxy)) return false;
    return vm_table_fmt_get(AS_INSTANCE(proxy), "Contenu", out);
}
bool owl_vm_table_colproxy_content_set(Value proxy, Value strVal) {
    if (!IS_INSTANCE(proxy)) return false;
    return vm_table_fmt_set(AS_INSTANCE(proxy), "Contenu", strVal);
}

/* ============================================================
 *   (amendement ) — receveur colonne MasqueSaisie/InputMask.
 *
 *  MasqueSaisie est une prop COLONNE (PROPS_COLUMN), pas une prop de mise en
 *  forme cellule (PROPS_TABLE_CELL). Elle ne transite donc PAS par vm_table_fmt
 *  (gate owl_table_cell_catalog_find) : le receveur nu/qualifie `COL_X..MasqueSaisie`
 *  desucre vers le proxy-colonne (COLPROXY_*), qu'on intercepte ici.
 *
 *  Source de verite = store OWL-side sur l'uici de la Table, cle $colmask.<col>
 *  (headless-deterministe : la relecture ne passe jamais par Qt). Fallback au
 *  masque DECLARE tant qu'aucun set runtime n'a ecrase la colonne.
 *  Cote GUI, le set pousse aussi le masque au widget (col_mask_<col0>) pour que
 *  createEditor du delegate colonne l'applique a l'edition (Path A). ============ */

/* Field == MasqueSaisie/InputMask ? (identifiant deja accent-strippe au scan,
 * et ces deux formes sont sans accent : simple casse-insensible). */
static bool vm_field_is_input_mask(const char* field) {
    return strcasecmp(field, "MasqueSaisie") == 0
        || strcasecmp(field, "InputMask") == 0;
}

/* Reconnait le proxy-colonne (COLPROXY_REF + COLPROXY_COL) et rend l'uici de la
 * Table + le nom de colonne canon (accents strippes, deja pose par OP_STRUCT_GET). */
static bool vm_colproxy_resolve(ObjInstance* inst, ObjUiControlInstance** uiciOut,
                                const char** colOut) {
    if (inst == NULL) return false;
    ObjString* refK = table_find_key_case_insensitive(&inst->fields,
                                                       OWL_TABLE_COLPROXY_REF_KEY);
    ObjString* colK = table_find_key_case_insensitive(&inst->fields,
                                                       OWL_TABLE_COLPROXY_COL_KEY);
    Value refV, colV;
    if (refK == NULL || colK == NULL) return false;
    if (!table_get(&inst->fields, refK, &refV) || !IS_UI_CONTROL_REF(refV))
        return false;
    if (!table_get(&inst->fields, colK, &colV) || !IS_STRING(colV)) return false;
    ObjUiControlInstance* uici = resolve_ui_ref(AS_UI_CONTROL_REF(refV));
    if (uici == NULL || uici->decl == NULL
        || uici->decl->kind != OWL_CONTROL_TABLE)
        return false;
    *uiciOut = uici;
    *colOut  = AS_CSTRING(colV);
    return true;
}

static void vm_table_colmask_key(char* out, size_t cap, const char* col) {
    snprintf(out, cap, "$colmask.%s", col);
}

/* Masque DECLARE de la colonne <col> (accents strippes) : parcourt les props de
 * declaration de la Table. "" si aucune. */
static const char* vm_table_col_declared_mask(ObjUiControlInstance* uici,
                                              const char* col) {
    const OwlControlDecl* d = uici->decl;
    for (int ci = 0; ci < d->column_count; ci++) {
        const char* cn = d->columns[ci].name;
        if (cn == NULL) continue;
        char cstr[256];
        int clen = scanner_strip_accents(cn, (int)strlen(cn), cstr, sizeof(cstr));
        if (clen <= 0 || clen >= (int)sizeof(cstr)) continue;
        cstr[clen] = '\0';
        if (strcasecmp(cstr, col) != 0) continue;
        for (int pi = 0; pi < d->columns[ci].property_count; pi++) {
            const OwlPropertyInit* p = &d->columns[ci].properties[pi];
            if (!p->is_constant) continue;
            if ((strcasecmp(p->name, "MasqueSaisie") == 0
                 || strcasecmp(p->name, "InputMask") == 0)
                && IS_STRING(p->const_value))
                return AS_CSTRING(p->const_value);
        }
        return "";
    }
    return "";
}

/* 1-based index de la colonne <col> (strippe) dans la decl ; 0 si inconnue. */
static int vm_table_col_index_1based(ObjUiControlInstance* uici, const char* col) {
    const OwlControlDecl* d = uici->decl;
    for (int ci = 0; ci < d->column_count; ci++) {
        const char* cn = d->columns[ci].name;
        if (cn == NULL) continue;
        char cstr[256];
        int clen = scanner_strip_accents(cn, (int)strlen(cn), cstr, sizeof(cstr));
        if (clen <= 0 || clen >= (int)sizeof(cstr)) continue;
        cstr[clen] = '\0';
        if (strcasecmp(cstr, col) == 0) return ci + 1;
    }
    return 0;
}

static bool vm_table_colmask_get(ObjInstance* inst, const char* field,
                                 Value* out) {
    if (!vm_field_is_input_mask(field)) return false;
    ObjUiControlInstance* uici = NULL;
    const char* col = NULL;
    if (!vm_colproxy_resolve(inst, &uici, &col)) return false;
    char key[288];
    vm_table_colmask_key(key, sizeof(key), col);
    Value stored;
    if (obj_ui_control_instance_headless_get(uici, key, &stored)
        && IS_STRING(stored)) {
        *out = stored;
        return true;
    }
    /* Jamais ecrit : echo du masque declare, "" a defaut. */
    const char* decl = vm_table_col_declared_mask(uici, col);
    *out = OBJ_VAL(obj_copy_string(decl, (int)strlen(decl)));
    return true;
}

static bool vm_table_colmask_set(ObjInstance* inst, const char* field, Value v) {
    if (!vm_field_is_input_mask(field)) return false;
    ObjUiControlInstance* uici = NULL;
    const char* col = NULL;
    if (!vm_colproxy_resolve(inst, &uici, &col)) return false;
    /* Normalise en chaine (le masque canon est du texte, parite SAI..MasqueSaisie). */
    ObjString* s = value_to_string(v);
    if (s == NULL) return true;
    vm_push(OBJ_VAL(s));   /* root : les set/headless allouent (grow -> GC) */
    char key[288];
    vm_table_colmask_key(key, sizeof(key), col);
    obj_ui_control_instance_headless_set(uici, key, OBJ_VAL(s));
    /* Miroir GUI : pousse au widget (createEditor lira col_mask_<col0>). No-op
     * headless (widget NULL) — le store ci-dessus porte la valeur. */
    if (uici->widget != NULL) {
        int col1 = vm_table_col_index_1based(uici, col);
        if (col1 > 0)
            owl_ui_table_colmask_applied(uici->widget, col1, s->chars);
    }
    vm_pop();
    return true;
}

/* ============================================================
 *   — receveur colonne MasqueAffichage/DisplayMask.
 *
 *  CANAL DISTINCT de MasqueSaisie (frappe). MasqueAffichage formate la valeur
 *  AFFICHEE des cellules typees Date/Heure/Duree ; il prime sur le format
 *  defaut/projet au rendu (owl_ui_format_cell_typed lit col_dispmask_<col0>).
 *  Miroir EXACT du canal MasqueSaisie ci-dessus, cle de store $coldispmask.<col>
 *  et property widget col_dispmask_<col0> — surtout PAS $colmask/col_mask_, qui
 *  resteraient le masque de frappe. ============================================ */

static void vm_table_coldispmask_key(char* out, size_t cap, const char* col) {
    snprintf(out, cap, "$coldispmask.%s", col);
}

/* Field == MasqueAffichage/DisplayMask ? (identifiant deja accent-strippe au
 * scan, ces deux formes sont sans accent : simple casse-insensible). */
static bool vm_field_is_display_mask(const char* field) {
    return strcasecmp(field, "MasqueAffichage") == 0
        || strcasecmp(field, "DisplayMask") == 0;
}

/* Masque d'affichage DECLARE de la colonne <col> (accents strippes) : parcourt
 * les props de declaration de la Table. "" si aucune. */
static const char* vm_table_col_declared_dispmask(ObjUiControlInstance* uici,
                                                  const char* col) {
    const OwlControlDecl* d = uici->decl;
    for (int ci = 0; ci < d->column_count; ci++) {
        const char* cn = d->columns[ci].name;
        if (cn == NULL) continue;
        char cstr[256];
        int clen = scanner_strip_accents(cn, (int)strlen(cn), cstr, sizeof(cstr));
        if (clen <= 0 || clen >= (int)sizeof(cstr)) continue;
        cstr[clen] = '\0';
        if (strcasecmp(cstr, col) != 0) continue;
        for (int pi = 0; pi < d->columns[ci].property_count; pi++) {
            const OwlPropertyInit* p = &d->columns[ci].properties[pi];
            if (!p->is_constant) continue;
            if ((strcasecmp(p->name, "MasqueAffichage") == 0
                 || strcasecmp(p->name, "DisplayMask") == 0)
                && IS_STRING(p->const_value))
                return AS_CSTRING(p->const_value);
        }
        return "";
    }
    return "";
}

static bool vm_table_coldispmask_get(ObjInstance* inst, const char* field,
                                     Value* out) {
    if (!vm_field_is_display_mask(field)) return false;
    ObjUiControlInstance* uici = NULL;
    const char* col = NULL;
    if (!vm_colproxy_resolve(inst, &uici, &col)) return false;
    char key[288];
    vm_table_coldispmask_key(key, sizeof(key), col);
    Value stored;
    if (obj_ui_control_instance_headless_get(uici, key, &stored)
        && IS_STRING(stored)) {
        *out = stored;
        return true;
    }
    /* Jamais ecrit : echo du masque d'affichage declare, "" a defaut. */
    const char* decl = vm_table_col_declared_dispmask(uici, col);
    *out = OBJ_VAL(obj_copy_string(decl, (int)strlen(decl)));
    return true;
}

static bool vm_table_coldispmask_set(ObjInstance* inst, const char* field,
                                     Value v) {
    if (!vm_field_is_display_mask(field)) return false;
    ObjUiControlInstance* uici = NULL;
    const char* col = NULL;
    if (!vm_colproxy_resolve(inst, &uici, &col)) return false;
    ObjString* s = value_to_string(v);
    if (s == NULL) return true;
    vm_push(OBJ_VAL(s));   /* root : les set/headless allouent (grow -> GC) */
    char key[288];
    vm_table_coldispmask_key(key, sizeof(key), col);
    obj_ui_control_instance_headless_set(uici, key, OBJ_VAL(s));
    /* Miroir GUI : pousse au widget (le rendu lira col_dispmask_<col0> pour
     * reformater les cellules live). No-op headless (widget NULL). */
    if (uici->widget != NULL) {
        int col1 = vm_table_col_index_1based(uici, col);
        if (col1 > 0)
            owl_ui_table_coldispmask_applied(uici->widget, col1, s->chars);
    }
    vm_pop();
    return true;
}

/*  addendum — trace opt-in du chemin de lecture UI.
 * Active uniquement si la variable d'env OWL_TRACE_UI est posee
 * (cache une fois pour eviter un getenv() par OP_STRUCT_GET). Sortie
 * stderr, fflush apres chaque ligne (le dev lit en console live). */
static bool owl_trace_ui_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
    return cached == 1;
}

static void owl_trace_ui_value(const char* ctrl, const char* prop,
                                const char* tag, Value v) {
    ObjString* s = value_to_string(v);
    if (s != NULL) {
        fprintf(stderr, "[ui-read] %s.%s %s='%.*s'\n",
                ctrl ? ctrl : "?", prop ? prop : "?", tag,
                s->length, s->chars);
    } else {
        fprintf(stderr, "[ui-read] %s.%s %s=<nil>\n",
                ctrl ? ctrl : "?", prop ? prop : "?", tag);
    }
    fflush(stderr);
}

/*  — lecture Qt-first pour une whitelist conservatrice de props
 * "live" quand l'ObjUiControlInstance porte un widget monte. Symetrise
 * le path apply_instance_scalar_impl (ecriture) : sans cela, la lecture
 * via OP_STRUCT_GET branche IS_UI_CONTROL_REF tape uniquement le
 * headless_store, qui n'est jamais synchronise depuis Qt
 * (QLineEdit::textChanged declenche le handler OWL mais ne pousse rien
 * dans le store) -> ..Valeur renvoie "" alors que le widget contient
 * "42".
 *
 * Whitelist (case + ASCII-strip-accent insensibles, le scanner strip
 * deja les accents pour les identifiants) : cf. le tableau WHITELIST
 * ci-dessous, chaque famille commentee a son entree.
 *
 * Hors whitelist OU widget == NULL -> return false : le caller retombe
 * sur son flow standard (store + fallback default). Retour true => out
 * contient la valeur Qt et le caller doit l'utiliser telle quelle.
 *
 * Le proxy ObjUiControl ephemere reprend le pattern de
 * apply_instance_scalar_impl (stdlib_ui.c:2908). */
static bool try_live_qt_prop_for_uici(ObjUiControlInstance* uici,
                                       const char* prop, Value* out) {
    if (!uici || !uici->widget || !uici->decl || !prop || !out) {
        if (owl_trace_ui_enabled()) {
            const char* ctrl = (uici && uici->decl && uici->decl->name)
                               ? uici->decl->name : "?";
            fprintf(stderr,
                    "[ui-read] %s.%s widget=%p uici=%p -> skip qt-first\n",
                    ctrl, prop ? prop : "?",
                    (void*)(uici ? uici->widget : NULL), (void*)uici);
            fflush(stderr);
        }
        return false;
    }
    static const char* WHITELIST[] = {
        "valeur",             "value",
        "valeuraffichee",     "displayedvalue",
        "valeurmemorisee",    "storedvalue",
        /*  — CouleurJauge doit distinguer "jamais fixee" (live
         * m_progressColor == -1 -> Null) de "fixee a 0/noir" (-> 0). Le
         * fallback store rend un defaut 0 qui ecrase cette distinction ;
         * seul le widget live la porte. */
        "couleurjauge",       "progressbarcolor",
        /*  — la geometrie d'un champ monte est celle du rendu, pas
         * celle du fichier de description : en disposition automatique
         * c'est le layout qui la fixe, et le store ne le sait pas. Sans
         * ces entrees, un champ etire rend une largeur de 0. */
        "largeur",            "width",
        "hauteur",            "height",
        "x",                  "y",
        /*  — la hauteur de la bande de titre d'une Table est une
         * mesure du champ monte : le store ne connait que ce qui a ete
         * demande, pas ce qui est rendu (0 quand l'en-tete est masque). */
        "hauteurtitre",       "titleheight",
        NULL
    };
    bool match = false;
    for (int i = 0; WHITELIST[i] != NULL; i++) {
        if (strcasecmp(prop, WHITELIST[i]) == 0) { match = true; break; }
    }
    if (!match) {
        if (owl_trace_ui_enabled()) {
            fprintf(stderr,
                    "[ui-read] %s.%s widget=%p -> hors whitelist (fallback store)\n",
                    uici->decl->name ? uici->decl->name : "?", prop,
                    (void*)uici->widget);
            fflush(stderr);
        }
        return false;
    }
    ObjUiControl proxy;
    proxy.obj.type       = OBJ_UI_CONTROL;
    proxy.kind           = (int)uici->decl->kind;
    proxy.name           = uici->decl->name ? uici->decl->name : "?";
    proxy.widget         = uici->widget;
    proxy.headless_store = NULL;
    *out = owl_ui_control_get_prop(&proxy, prop);
    if (owl_trace_ui_enabled()) {
        fprintf(stderr, "[ui-read] %s.%s widget=%p ",
                proxy.name, prop, (void*)uici->widget);
        ObjString* s = value_to_string(*out);
        if (s != NULL) {
            fprintf(stderr, "live='%.*s'\n", s->length, s->chars);
        } else {
            fprintf(stderr, "live=<nil>\n");
        }
        fflush(stderr);
    }
    return true;
}

/*  — defaut headless typé pour SAI..Valeur. Le catalog INPUT
 * porte un slot "Valeur" de type STRING ; owl_ui_headless_default rend
 * deja "" pour ce type, mais on garde ce chemin explicite pour rester
 * symetrique du path GUI (real_ui_get_prop : wantsDef -> texte du line
 * edit) et robuste si le default catalog evolue. ValeurAffichee /
 * ValeurMemorisee ne partagent pas cette racine pour une Saisie
 * (concepts Liste/Combo/Selecteur) — non aliasees ici, coherent GUI. */
static Value headless_default_with_alias(ObjUiControl* proxy,
                                          const char* prop) {
    if (proxy && prop
        && (OwlControlKind)proxy->kind == OWL_CONTROL_INPUT
        && (strcasecmp(prop, "Valeur") == 0
            || strcasecmp(prop, "Value") == 0)) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    return owl_ui_headless_default(proxy, prop);
}

/* Forward decl — defined further down once CallFrame / run() helpers exist. */
static void capture_exception_context(const char* message, const char* code);

/* ExceptionInfo() constants : sentinelles centralisees dans sentinels.h.
 * NB : OWL_ERR_NB_SOUS_ERREUR (vm.c historique) <-> OWL_ERR_NB_SOUS_ERR
 * (sentinels.h) -- alias pour preserver les call sites. */
#define OWL_ERR_NB_SOUS_ERREUR  OWL_ERR_NB_SOUS_ERR

/* ExceptionDéclenche([code], [message], [info])
 *   - 0 arg                       : code=0, message="Exception", info=""
 *   - 1 arg chaîne                : message seul (code=0, info="")
 *   - 1 arg entier                : code seul
 *   - 2 args (entier, chaîne)     : code + message
 *   - 3 args (entier, chaîne, chaîne) : code + message + info (lisible via errInfo)
 *
 * Le 3e argument `info` est une extension OWL vs canon (2 params max) :
 * il alimente errInfo, qui sans ça serait toujours vide.
 *
 * Sets throwPending so the enclosing handler picks it up. */
static Value native_exception_throw(int argCount, Value* args) {
    const char* msg = "Exception";
    const char* info = "";
    int code = 0;
    if (argCount >= 1) {
        if (IS_STRING(args[0])) {
            msg = AS_CSTRING(args[0]);
        } else if (IS_INTEGER(args[0])) {
            code = (int)AS_INTEGER(args[0]);
            if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);
            if (argCount >= 3 && IS_STRING(args[2])) info = AS_CSTRING(args[2]);
        }
    }
    /* Pose l'origin AVANT capture_exception_context (qui consomme le flag). */
    g_exc_origin = obj_copy_string("ExceptionD\xc3\xa9" "clenche", 19);
    g_exc_origin_pending = true;
    /* Levée user explicite : aucune native en cause → errFonction reste vide.
     * native_name_pending laissé faux → capture reset g_exc_native_name. */
    /* Pose errInfo (toujours, même si vide — assure reset entre exceptions). */
    g_exc_info = obj_copy_string(info, (int)strlen(info));
    /* errCode = code user stringifié (ex "42"), aligné contrat chaîne. */
    char code_buf[32];
    snprintf(code_buf, sizeof(code_buf), "%d", code);
    capture_exception_context(msg, code_buf);
    throwPending = true;
    return NIL_VAL;
}

/* Test-only native : __SetSystemError(code, msg) — préfixe __ = interne,
 * non exposée au registry/LSP. Permet aux tests OWL de valider le
 * mécanisme opt-in errCodeSystème/errMessageSystème sans attendre qu'une
 * native syscall productive instrumente l'API. */
static Value native_set_system_error_test(int argCount, Value* args) {
    int code = (argCount >= 1 && IS_INTEGER(args[0])) ? (int)AS_INTEGER(args[0]) : 0;
    const char* msg = (argCount >= 2 && IS_STRING(args[1])) ? AS_CSTRING(args[1]) : "";
    owl_set_exception_system_error(code, msg);
    return NIL_VAL;
}

/* Helper for OWL_ERR_RESUME — first line of msg, capped to 80 bytes. Bytes
 * (not codepoints) — cohérent avec UTF-8 partout dans OWL. */
static ObjString* exception_summary(ObjString* msg) {
    if (msg == NULL || msg->length == 0) return obj_copy_string("", 0);
    const char* s = msg->chars;
    int n = msg->length;
    int line_end = n;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r') { line_end = i; break; }
    }
    if (line_end > 80) line_end = 80;
    return obj_copy_string(s, line_end);
}

/* ExceptionInfo([typeInfo], [indiceSousErreur])
 * typeInfo  : constante errXxx (cf. enum E_errInfo). Default = errMessage.
 * indiceSousErreur : accepté nominalement, ignoré silencieusement (pas
 *                    de mécanisme de sous-erreur en alpha). */
static Value native_exception_info(int argCount, Value* args) {
    int kind = OWL_ERR_MESSAGE;
    if (argCount >= 1 && IS_INTEGER(args[0])) kind = (int)AS_INTEGER(args[0]);
    /* argCount >= 2 : indiceSousErreur ignoré. */

    switch (kind) {
        case OWL_ERR_MESSAGE:
            return currentException != NULL ? OBJ_VAL(currentException)
                                            : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_CODE:
            /*  : chaîne canonique (symbole "E_..." pour une native, code
             * user stringifié "42" pour ExceptionDéclenche), aligné ErreurInfo. */
            return g_exc_code != NULL ? OBJ_VAL(g_exc_code)
                                      : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_LIGNE:
            return INTEGER_VAL(g_exc_line);
        case OWL_ERR_TRAITEMENT:
            /* Procédure courante au site de capture, "<script>" au top-level. */
            if (g_exc_procedure != NULL) return OBJ_VAL(g_exc_procedure);
            return OBJ_VAL(obj_copy_string("<script>", 8));
        case OWL_ERR_PENDANT:
            /* Fonction qui a réellement déclenché l'exception. */
            return g_exc_origin != NULL ? OBJ_VAL(g_exc_origin)
                                        : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_ELEMENT:
            /* Nom du fichier source où la levée a eu lieu. */
            return g_exc_filename != NULL ? OBJ_VAL(g_exc_filename)
                                          : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_PILE:
            return g_exc_stack != NULL ? OBJ_VAL(g_exc_stack)
                                       : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_DATE_HEURE:
            return INTEGER_VAL(g_exc_timestamp_ms);
        case OWL_ERR_NB_SOUS_ERREUR:
            return INTEGER_VAL(0);
        case OWL_ERR_INFO:
            return g_exc_info != NULL ? OBJ_VAL(g_exc_info)
                                      : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_FONCTION:
            /*  : nom de la native qui a levé (vide pour une levée user). */
            return g_exc_native_name != NULL ? OBJ_VAL(g_exc_native_name)
                                             : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_CODE_NUM: {
            /*  : alias numérique E0NNN/W0NNN du code s'il est un symbole
             * du registre erreurs, chaîne vide sinon (code user brut). Mirror de
             * fn_erreur_info (chemin ErreurInfo). */
            const char* num = "";
            if (g_exc_code != NULL) {
                const OwlErrorDef* d = owl_registry_error_find(g_exc_code->chars);
                if (d) {
                    const char* n = owl_registry_error_num(
                        owl_registry_error_code_of(d));
                    if (n) num = n;
                }
            }
            return OBJ_VAL(obj_copy_string(num, (int)strlen(num)));
        }
        case OWL_ERR_CODE_SYSTEME:
            return INTEGER_VAL(g_exc_sys_code);
        case OWL_ERR_MESSAGE_SYSTEME:
            return g_exc_sys_message != NULL ? OBJ_VAL(g_exc_sys_message)
                                             : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_RESUME:
            return OBJ_VAL(exception_summary(currentException));
        case OWL_ERR_COMPLET: {
            /* errComplet = message + localisation + call stack. */
            char buf[3072];
            int n = 0;
            const char* msg = currentException != NULL ? currentException->chars : "";
            const char* code = g_exc_code != NULL ? g_exc_code->chars : "";
            const char* native = g_exc_native_name != NULL ? g_exc_native_name->chars : "";
            const char* file = g_exc_filename != NULL ? g_exc_filename->chars : "?";
            const char* proc = g_exc_procedure != NULL ? g_exc_procedure->chars : "<script>";
            const char* stack = g_exc_stack != NULL ? g_exc_stack->chars : "";
            n += snprintf(buf + n, sizeof(buf) - n,
                "Message: %s\nCode: %s\nNative: %s\nFichier: %s\nLigne: %d\nTraitement: %s\n\nPile:\n%s",
                msg, code, native, file, g_exc_line, proc, stack);
            return OBJ_VAL(obj_copy_string(buf, n));
        }
    }
    return OBJ_VAL(obj_copy_string("", 0));
}

/* ExceptionPropage() — forward the current exception to the enclosing
 * handler (if any). Equivalent to the bare keyword `EXCEPTION` inside a
 * handler body. */
static Value native_exception_propagate(int argCount, Value* args) {
    (void)argCount; (void)args;
    if (currentException != NULL) {
        throwPending = true;
    }
    return NIL_VAL;
}

/*  phase 2b — lookup tolerant casse+accents dans vm.globals via
 * l'index secondaire `vm.globals_canon_idx`. Canonise la cle de recherche,
 * lit le verbatim associe, puis fetch dans la table principale.
 *
 * Retourne true et remplit *outValue si trouve. Si outVerbatim != NULL,
 * stocke aussi l'ObjString verbatim resolue (utile pour OP_SET_GLOBAL
 * qui doit ensuite table_set sur la cle verbatim canonique).
 *
 * Discipline GC : searchKey est suppose rooted par caller (READ_STRING
 * pointe sur les constantes du chunk via frame, et les call sites externes
 * pushent explicitement avant l'appel). canonKey est rooted pendant le
 * table_get (qui peut allouer indirectement via table_set d'invocation
 * imbriquee — quoique read-only ici, on garde le push par prudence). */
static bool vm_globals_canon_get(ObjString* searchKey, Value* outValue,
                                 ObjString** outVerbatim) {
    /* Pre-canonisation : conversion stack-buffer puis allocation transitoire. */
    char canonBuf[256];
    int canonLen = scanner_canonicalize_identifier(searchKey->chars,
                                                   searchKey->length,
                                                   canonBuf, sizeof(canonBuf));
    ObjString* canonKey = obj_copy_string(canonBuf, canonLen);
    vm_push(OBJ_VAL(canonKey));
    Value verbatimVal;
    bool found = table_get(&vm.globals_canon_idx, canonKey, &verbatimVal);
    vm_pop(); /* canonKey */
    if (!found) return false;
    ObjString* verbatimKey = AS_STRING(verbatimVal);
    if (outVerbatim) *outVerbatim = verbatimKey;
    return table_get(&vm.globals, verbatimKey, outValue);
}

/*  phase 2b — meme principe pour vm.types via vm.types_canon_idx. */
static bool vm_types_canon_get(ObjString* searchKey, Value* outValue) {
    char canonBuf[256];
    int canonLen = scanner_canonicalize_identifier(searchKey->chars,
                                                   searchKey->length,
                                                   canonBuf, sizeof(canonBuf));
    ObjString* canonKey = obj_copy_string(canonBuf, canonLen);
    vm_push(OBJ_VAL(canonKey));
    Value verbatimVal;
    bool found = table_get(&vm.types_canon_idx, canonKey, &verbatimVal);
    vm_pop(); /* canonKey */
    if (!found) return false;
    return table_get(&vm.types, AS_STRING(verbatimVal), outValue);
}

/* Public: used by all stdlib modules to register native functions */
void owl_define_native(const char* name, NativeFn function, int arity) {
    /* Root les deux transients sur la pile pendant table_set : la grow
     * potentielle de vm.globals peut declencher un GC qui collecte
     * nameStr ou native si elles ne sont referencees nulle part. */
    ObjString* nameStr = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(nameStr));
    ObjNative* native = obj_new_native(function, arity);
    vm_push(OBJ_VAL(native));
    /*  phase 2b : route via helper canonique pour peupler aussi
     * vm.globals_canon_idx. obj_globals_set_kv reprend la discipline GC. */
    obj_globals_set_kv(nameStr, OBJ_VAL(native));
    vm_pop();
    vm_pop();
}

void owl_define_native_wl(const char* name, NativeFn function, int arity) {
    owl_define_native(name, function, arity);
    char wlName[256];
    snprintf(wlName, sizeof(wlName), "__wl$%s", name);
    owl_define_native(wlName, function, arity);
}

/* Patch les ObjNative déjà enregistrés pour propager
 * firstOutParamArg/lastOutParamArg. Appelé depuis owl_registry_register_module
 * quand le DEF a le flag OWL_NAT_OUT_PARAMS. Couvre les deux entrées
 * (name + __wl$name) installées par owl_define_native_wl. */
void owl_native_set_out_params(const char* name, int firstOutParamArg, int lastOutParamArg) {
    /*  phase 2b : lookup via canon_idx pour suivre la meme regle
     * de resolution que les opcodes runtime. */
    ObjString* nameStr = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(nameStr));
    Value v;
    if (vm_globals_canon_get(nameStr, &v, NULL) && IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE) {
        ((ObjNative*)AS_OBJ(v))->firstOutParamArg = firstOutParamArg;
        ((ObjNative*)AS_OBJ(v))->lastOutParamArg  = lastOutParamArg;
    }
    vm_pop(); /* nameStr */
    char wlName[256];
    snprintf(wlName, sizeof(wlName), "__wl$%s", name);
    ObjString* wlNameStr = obj_copy_string(wlName, (int)strlen(wlName));
    vm_push(OBJ_VAL(wlNameStr));
    if (vm_globals_canon_get(wlNameStr, &v, NULL) && IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE) {
        ((ObjNative*)AS_OBJ(v))->firstOutParamArg = firstOutParamArg;
        ((ObjNative*)AS_OBJ(v))->lastOutParamArg  = lastOutParamArg;
    }
    vm_pop(); /* wlNameStr */
}

static void register_vm_internals(void) {
    /* Struct internals (used by compiler-generated code) */
    owl_define_native("__StructNew", native_struct_new, 1);
    owl_define_native("__StructClone", native_struct_clone, 1);
    owl_define_native("__EnumVal", native_enum_val, 3);

    /* Exception natives — accept variadic arg counts via -1 so callers
     * can use any overload. Canonical: ExceptionDéclenche.
     * Registered via owl_registry to get canonical accented form + EN aliases. */
    {
        static const OwlParam p_throw[]  = {
            {"code",    "entier", "code numérique de l'exception (optionnel, 0 par défaut)"},
            {"message", "chaîne", "message associé (optionnel, message générique par défaut)"},
            {"info",    "chaîne", "informations complémentaires (optionnel, vide par défaut) — lisibles via ExceptionInfo(errInfo)"},
            {0}
        };
        extern const OwlEnum E_errInfo;  /* declared in stdlib_erreur.c */
        static const OwlParam p_info[]   = {
            {"typeInfo",         "entier", "composant d'information à retourner (constante errXxx)", .allowedBase=&E_errInfo},
            {"indiceSousErreur", "entier", "indice de sous-erreur (réservé, sans effet en l'absence de sous-erreurs)"},
            {0}
        };

        static const OwlSignature sig_throw[]   = { {-1, p_throw, "rien",    "lève l'exception"}, {0} };
        static const OwlSignature sig_info[]    = {
            {-1, p_info, "chaîne|entier",
             "info demandée — chaîne pour Message/Code/Fonction/CodeNum/Pile/Complet/Traitement/Pendant/Élément/Info/Résumé/MessageSystème, entier pour Ligne/DateHeure/NombreSousErreur/CodeSystème"},
            {0}
        };
        static const OwlSignature sig_prop[]    = { {0,  NULL,    "rien",    "relance l'exception courante"}, {0} };

        static const OwlNativeDef DEFS_EXC[] = {
            { .canonical = "ExceptionD\xc3\xa9" "clenche",
              .canonicalAscii = "ExceptionDeclenche",
              .nameEN = "ExceptionThrow", .category = "Exceptions",
              .descFR = "Lève une exception avec code, message et informations complémentaires optionnels.\n"
                        "Formes d'appel acceptées : (), (message), (code), (code, message), (code, message, info).\n"
                        "Le 1er argument est discriminé par type : une chaîne est traitée comme message, "
                        "un entier comme code. Conséquence : la forme (message, info) sans code n'existe pas — "
                        "si le 1er argument est une chaîne, les arguments suivants sont ignorés silencieusement "
                        "(code reste 0, info reste vide). Pour transmettre une info, passer explicitement le code "
                        "en premier, par exemple ExceptionDéclenche(1, \"message\", \"info\").",
              .descEN = "Throw an exception with optional code, message and additional information.\n"
                        "Accepted call forms: (), (message), (code), (code, message), (code, message, info).\n"
                        "The 1st argument is discriminated by type: a string is treated as message, "
                        "an integer as code. Consequence: the form (message, info) without code does not exist — "
                        "if the 1st argument is a string, following arguments are silently ignored "
                        "(code stays 0, info stays empty). To pass an info, supply the code explicitly first, "
                        "e.g. ExceptionThrow(1, \"message\", \"info\").",
              .signatures = sig_throw, .impl = native_exception_throw },
            { .canonical = "ExceptionInfo", .nameEN = "ExceptionInfo", .category = "Exceptions",
              .descFR = "Retourne un composant d'information sur l'exception en cours. Sans argument, retourne le message. Avec une constante errXxx, retourne le champ correspondant. Hors d'un handler, retourne une valeur vide plutôt que de lever une erreur.",
              .descEN = "Return one piece of information about the current exception. Without argument, returns the message. With an errXxx constant, returns the matching field. Outside a handler, returns an empty value rather than raising.",
              .signatures = sig_info, .impl = native_exception_info },
            { .canonical = "ExceptionPropage", .nameEN = "ExceptionPropagate", .category = "Exceptions",
              .descFR = "Relance l'exception vers le bloc appelant.",
              .descEN = "Re-throw the current exception to the calling block.",
              .signatures = sig_prop, .impl = native_exception_propagate },
        };
        owl_registry_register_module(DEFS_EXC, (int)(sizeof(DEFS_EXC)/sizeof(DEFS_EXC[0])));
    }

    /* Test-only native pour valider le mécanisme errCodeSystème opt-in
     * depuis OWL pur. Préfixe __ = interne, hors registry/LSP. */
    owl_define_native("__SetSystemError", native_set_system_error_test, -1);

    /* errXxx constants used with ExceptionInfo(). */
    /* Les constantes errXxx (errMessage/errCode/errLigne/...) sont
     * desormais enregistrees par stdlib_erreur_init() via le registre +
     * auto-alias ASCII + register_ascii_alias pour les alias EN. Ne
     * plus dupliquer ici (redondant + source de drift). */

    /* Test / Assertions (registered from testing.c) */
    testing_register_natives();
}

/* ================================================================
 * VM INIT / FREE
 * ================================================================ */

/*  — forward decl : defini apres resolve_ui_ref, mais pose comme
 * hook des vm_init (ci-dessous). */
static Value vm_ui_ref_default_resolver(Value v);

void vm_init(void) {
    /*  : enregistre les globals exception au registre unique
     * des racines persistantes (idempotent — safe sur les N appels du
     * test runner). Remplace l'ancienne fonction vm_exception_mark_roots(). */
    owl_register_persistent_obj_root((Obj**)&currentException);
    owl_register_persistent_obj_root((Obj**)&g_exc_code);
    owl_register_persistent_obj_root((Obj**)&g_exc_filename);
    owl_register_persistent_obj_root((Obj**)&g_exc_procedure);
    owl_register_persistent_obj_root((Obj**)&g_exc_origin);
    owl_register_persistent_obj_root((Obj**)&g_exc_native_name);
    owl_register_persistent_obj_root((Obj**)&g_exc_info);
    owl_register_persistent_obj_root((Obj**)&g_exc_stack);
    owl_register_persistent_obj_root((Obj**)&g_exc_sys_message);

    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.objects = NULL;
    vm.iterDepth = 0;
    vm.outParamCount = 0;
    vm.headless_mode = false;   /*  : owl standard = GUI normale ;
                                 * le runner test re-active ce flag a true
                                 * apres vm_init() pour chaque fichier. */

    /*  — remise a zero de la capture compile-time des fenetres du
     * projet : sans ca, un fichier script tournant apres un projet dans le meme
     * process (owl test recursif) heriterait de la liste du projet precedent. */
    compiler_projet_windows_reset();

    /* DSL Fenetre v1 : table des declarations top-level. */
    vm.windows = NULL;
    vm.windowCount = 0;
    vm.windowCapacity = 0;

    /* DSL Etat V2 : table des declarations top-level. */
    vm.etats = NULL;
    vm.etatCount = 0;
    vm.etatCapacity = 0;

    /*  : stack runtime multi-instance + compteur global d'ID. */
    vm.window_stack          = NULL;
    vm.window_stack_count    = 0;
    vm.window_stack_capacity = 0;
    vm.next_instance_id      = 0;

    /*  lot D : stack runtime des Etats vivants. */
    vm.etat_stack            = NULL;
    vm.etat_stack_count      = 0;
    vm.etat_stack_capacity   = 0;

    /*  : stack runtime du widget emetteur pour MoiMême. */
    vm.event_sender_stack_count = 0;
    for (int i = 0; i < 32; i++) vm.event_sender_stack[i] = NIL_VAL;

    /* Seed the per-process key used to mask `<indétectable>` strings.
     * Idempotent: skips work after the first call. */
    obj_init_mask_key();

    /* Contexte mpdecimal (modele de nombre v2) : precision division = 50,
     * arrondi demi-pair, contexte exact pour +-*. Idempotent. */
    owl_numeric_init_context();

    /* Reset the registry in case this is a re-init in the same process —
     * register_module appends to flat arrays and would otherwise duplicate. */
    owl_registry_free();
    owl_event_catalog_reset();

    /* Initialize mock registry */
    mock_registry_init();

    table_init(&vm.globals);
    table_init(&vm.types);    /*  : namespace separe pour les types. */
    table_init(&vm.globals_canon_idx); /*  phase 2 : index canon. */
    table_init(&vm.types_canon_idx);   /*  phase 2 : index canon. */
    table_init(&vm.strings);
    table_init(&owl_strings);

    owl_objects = NULL;

    gc_bytesAllocated = 0;
    gc_nextGC = GC_INITIAL_THRESHOLD;

    exceptionCount = 0;
    /*  lot 2 — réétablit le registre de contextes à son état N=1 :
     * une seule entrée, le contexte principal, courant. Le runner test
     * ré-appelle vm_init par fichier ; sans ce reset un contexte parké d'un
     * fichier précédent (lots 3-4) fuiterait. */
    g_exec_ctx_main.excSaveDepth = 0;   /*  : pile save/restore du contexte d'exception */
    g_exec_ctx_cur = &g_exec_ctx_main;
    g_exec_contexts[0] = &g_exec_ctx_main;
    g_exec_context_count = 1;
    /*  lot 4 — repart d'un registre parké vide (le runner test
     * ré-appelle vm_init par fichier ; un snapshot resté parké fuiterait). */
    for (int i = 0; i < g_park_count; i++) g_park_snapshots[i] = NULL;
    g_park_count = 0;
    currentException = NULL;
    throwPending = false;

    /*  lot 1 : reset NULL des 6 autres ObjString* du contexte
     * d'exception. Sans cela, sous DEBUG_STRESS_GC un test runner qui
     * appelle vm_free->vm_init en boucle laisse ces globals pointer sur
     * des chunks affranchis -> 0xEE au mark suivant. Defense en profondeur
     * couplee a vm_exception_mark_roots(). */
    g_exc_code        = NULL;
    g_exc_filename    = NULL;
    g_exc_procedure   = NULL;
    g_exc_origin      = NULL;
    g_exc_native_name = NULL;
    g_exc_info        = NULL;
    g_exc_stack       = NULL;
    g_exc_sys_message = NULL;

    /*  lot 1 : reset des globals `static Obj*` des modules
     * stdlib_* avant tout init. Symetrique de stdlib_*_mark_roots.
     *
     *  : NE PAS rajouter ici de reset garde par OWL_HAS_GUI.
     * vm.c est compile dans owl_core qui ne voit jamais ce flag, donc
     * la branche serait strippe a la compile (piege historique 
     * sur une racine GUI static Obj*). Le reset des roots GUI est centralise via
     * `owl_persistent_roots_reset_all()` appele dans vm_free() ci-dessous,
     * mecanisme qui ne depend pas de l'unite de compilation. Garde-fou
     * automatique : tools/owl_core_guard/. */
    stdlib_erreur_reset_globals();
    stdlib_image_reset_globals();
    stdlib_string_reset_globals();

    register_vm_internals();
    stdlib_init();

    /*  — injecte le resolveur de propriete par defaut pour les noms
     * de widget DSL nus lus en contexte scalaire. La registry reste sans
     * dependance VM ; c'est la VM qui pose le hook. */
    owl_set_ui_ref_default_resolver(vm_ui_ref_default_resolver);

    /* Registre peuple : pose le provider de canon du formatter sur le backend
     * registry vivant. Couvre tous les chemins qui formatent apres vm_init
     * (owl fmt, self-test fmt-stability, MCP). owl-lsp.exe ne passe jamais par
     * vm_init et pose lui-meme son backend catalogue. */
    owl_format_set_canon(owl_fmt_canon_registry());

    owl_debug_init();
}

void vm_free(void) {
    owl_debug_free();

    /* Free mock registry before clearing globals */
    mock_registry_free();

    /* DSL Fenetre v1 : libere les declarations top-level. */
    for (int i = 0; i < vm.windowCount; i++) {
        owl_window_decl_free(vm.windows[i]);
    }
    free(vm.windows);
    vm.windows = NULL;
    vm.windowCount = 0;
    vm.windowCapacity = 0;

    /* DSL Etat V2 : libere les declarations top-level. */
    for (int i = 0; i < vm.etatCount; i++) {
        owl_etat_decl_free(vm.etats[i]);
    }
    free(vm.etats);
    vm.etats = NULL;
    vm.etatCount = 0;
    vm.etatCapacity = 0;

    /*  : libere le tableau de pointeurs de la stack (les
     * ObjWindowInstance elles-memes sont liberees via le GC sweep
     * declenche par le free de owl_objects plus bas). */
    free(vm.window_stack);
    vm.window_stack          = NULL;
    vm.window_stack_count    = 0;
    vm.window_stack_capacity = 0;
    vm.next_instance_id      = 0;

    /*  lot D : libere le tableau de pointeurs de la stack Etats. */
    free(vm.etat_stack);
    vm.etat_stack            = NULL;
    vm.etat_stack_count      = 0;
    vm.etat_stack_capacity   = 0;

    /*  : reset de la stack MoiMême (allocation inline, rien a free). */
    vm.event_sender_stack_count = 0;

    table_free(&vm.globals);
    table_free(&vm.types);    /*  : symetrique de table_init. */
    table_free(&vm.globals_canon_idx); /*  phase 2 : index canon. */
    table_free(&vm.types_canon_idx);   /*  phase 2 : index canon. */
    table_free(&vm.strings);
    table_free(&owl_strings);

    /*  audit memoire : trace bytesAllocated avant/apres free pour
     * verifier l'absence de leak (active uniquement via env OWL_MEM_TRACE). */
    const char* memTrace = getenv("OWL_MEM_TRACE");
    bool traceEnabled = (memTrace != NULL && memTrace[0] != '\0' && memTrace[0] != '0');
    size_t bytesBeforeFree = gc_bytesAllocated;

    Obj* object = owl_objects;
    while (object != NULL) {
        Obj* next = object->next;
        object_free(object);
        object = next;
    }
    owl_objects = NULL;

    /*  : neutralise tous les slots persistent roots qui pointaient
     * vers des Obj qu'on vient de liberer. Sans ca, le prochain vm_init suivi
     * d'un GC lit un Obj dangling (slot non reseté côté module dont le reset
     * `stdlib_X_reset_globals` peut etre strippe par un `#ifdef` invisible
     * cote owl_core) -> heap UAF transient. */
    owl_persistent_roots_reset_all();

    if (traceEnabled) {
        fprintf(stderr, "[MEM] vm_free: bytes before=%zu after=%zu (delta=%zd)\n",
                bytesBeforeFree, gc_bytesAllocated,
                (ssize_t)gc_bytesAllocated - (ssize_t)bytesBeforeFree);
        fflush(stderr);
    }

    owl_registry_free();
}

/* ================================================================
 * DSL Fenetre v1 — vm.windows
 * ================================================================ */

void vm_windows_clear(void) {
    for (int i = 0; i < vm.windowCount; i++) {
        owl_window_decl_free(vm.windows[i]);
    }
    vm.windowCount = 0;
}

bool vm_window_register(OwlWindowDecl* decl) {
    if (!decl || !decl->name) return false;

    /* Doublon de nom : refus.  — comparaison accent + case insensible
     * (invariants 1 & 2), alignee sur vm_window_lookup pour eviter qu'une
     * 2e fenetre FEN_Reglages masque silencieusement FEN_Reglades. */
    char declStripped[256];
    int declLen = scanner_strip_accents(decl->name, (int)strlen(decl->name),
                                        declStripped, sizeof(declStripped));
    for (int i = 0; i < vm.windowCount; i++) {
        const char* wn = vm.windows[i]->name;
        char wStripped[256];
        int wLen = scanner_strip_accents(wn, (int)strlen(wn),
                                         wStripped, sizeof(wStripped));
        if (declLen == wLen
            && strncasecmp(wStripped, declStripped, (size_t)declLen) == 0) {
            return false;
        }
    }

    /* Resize si besoin. */
    if (vm.windowCount >= vm.windowCapacity) {
        int new_cap = vm.windowCapacity < 4 ? 4 : vm.windowCapacity * 2;
        vm.windows = (OwlWindowDecl**)realloc(vm.windows,
                                               sizeof(OwlWindowDecl*) * (size_t)new_cap);
        vm.windowCapacity = new_cap;
    }

    vm.windows[vm.windowCount++] = decl;

    /*  lot C — plus de proxy legacy ObjUiControl(WINDOW) dans
     * vm.globals[FEN_X]. C'est window_declaration (compiler.c) qui pose un
     * ObjWindowRef sous cette cle au moment de la declaration, et il y
     * reste pour toute la vie du programme (lecture+ecriture via la
     * branche IS_WINDOW_REF de OP_STRUCT_GET/SET dans vm.c). */

    return true;
}

const OwlWindowDecl* vm_window_lookup(const char* name) {
    if (!name) return NULL;
    /*  — lookup accent + case insensible (invariants 1 & 2). */
    char qstripped[256];
    int qlen = scanner_strip_accents(name, (int)strlen(name),
                                     qstripped, sizeof(qstripped));
    for (int i = 0; i < vm.windowCount; i++) {
        const char* wn = vm.windows[i]->name;
        char wstripped[256];
        int wlen = scanner_strip_accents(wn, (int)strlen(wn),
                                         wstripped, sizeof(wstripped));
        if (qlen == wlen && strncasecmp(wstripped, qstripped, (size_t)qlen) == 0) {
            return vm.windows[i];
        }
    }
    return NULL;
}

/* ================================================================
 *  — Stack runtime multi-instance fenetre.
 *
 * Tableau de pointeurs ObjWindowInstance. Croit en puissance de 2.
 * Les instances pointees sont rootees pour le GC via mark_roots
 * (cf gc.c) tant qu'elles sont dans la stack. Une fois popees, elles
 * sont collectables sauf si referencees ailleurs (controls[] back-ref,
 * valeur_renvoyee retenue par le caller, etc.).
 * ================================================================ */

void vm_window_stack_push(ObjWindowInstance* inst) {
    if (vm.window_stack_count >= vm.window_stack_capacity) {
        int new_cap = vm.window_stack_capacity < 4 ? 4 : vm.window_stack_capacity * 2;
        vm.window_stack = (ObjWindowInstance**)realloc(vm.window_stack,
                                                       sizeof(ObjWindowInstance*) * (size_t)new_cap);
        vm.window_stack_capacity = new_cap;
    }
    vm.window_stack[vm.window_stack_count++] = inst;
}

ObjWindowInstance* vm_window_stack_pop(void) {
    if (vm.window_stack_count == 0) return NULL;
    return vm.window_stack[--vm.window_stack_count];
}

ObjWindowInstance* vm_window_stack_top(void) {
    if (vm.window_stack_count == 0) return NULL;
    return vm.window_stack[vm.window_stack_count - 1];
}

void vm_window_stack_clear(void) {
    vm.window_stack_count = 0;
}

bool vm_window_instance_is_open(const char* name) {
    if (!name) return false;
    char qstripped[256];
    int qlen = scanner_strip_accents(name, (int)strlen(name),
                                     qstripped, sizeof(qstripped));
    for (int i = 0; i < vm.window_stack_count; i++) {
        ObjWindowInstance* inst = vm.window_stack[i];
        if (!inst || !inst->decl || !inst->decl->name) continue;
        const char* wn = inst->decl->name;
        char wstripped[256];
        int wlen = scanner_strip_accents(wn, (int)strlen(wn),
                                         wstripped, sizeof(wstripped));
        if (qlen == wlen && strncasecmp(wstripped, qstripped, (size_t)qlen) == 0) {
            return true;
        }
    }
    return false;
}

/* ================================================================
 *  lot D — Stack runtime des Etats vivants.
 *
 * Miroir strict de window_stack. Push par fn_i_imprime_etat a l'entree,
 * pop a la sortie (succes ou erreur). resolve_ui_ref consulte cette stack
 * en repli quand window_stack est vide ou ne resout pas — c'est ce qui
 * permet aux sous-champs d'Etat d'etre adressables par leur nom.
 * ================================================================ */

void vm_etat_stack_push(ObjEtatInstance* inst) {
    if (vm.etat_stack_count >= vm.etat_stack_capacity) {
        int new_cap = vm.etat_stack_capacity < 4 ? 4 : vm.etat_stack_capacity * 2;
        vm.etat_stack = (ObjEtatInstance**)realloc(vm.etat_stack,
                                                    sizeof(ObjEtatInstance*) * (size_t)new_cap);
        vm.etat_stack_capacity = new_cap;
    }
    vm.etat_stack[vm.etat_stack_count++] = inst;
}

ObjEtatInstance* vm_etat_stack_pop(void) {
    if (vm.etat_stack_count == 0) return NULL;
    return vm.etat_stack[--vm.etat_stack_count];
}

ObjEtatInstance* vm_etat_stack_top(void) {
    if (vm.etat_stack_count == 0) return NULL;
    return vm.etat_stack[vm.etat_stack_count - 1];
}

void vm_etat_stack_clear(void) {
    vm.etat_stack_count = 0;
}

/* ================================================================
 *  — stack runtime du widget emetteur pour MoiMême.
 *
 * Push/pop en miroir autour de chaque dispatch d'evenement (stdlib_ui.c).
 * Profondeur fixe (32) — un nesting handler -> proc -> proc -> ... au-dela
 * est pathologique. Silent no-op si depasse pour ne pas faire planter le
 * dispatch en cas d'erreur exotique.
 * ================================================================ */

void vm_event_sender_push(Value widget) {
    if (vm.event_sender_stack_count >= 32) return;
    vm.event_sender_stack[vm.event_sender_stack_count++] = widget;
}

void vm_event_sender_pop(void) {
    if (vm.event_sender_stack_count == 0) return;
    vm.event_sender_stack_count--;
    /* Defensive : on remet NIL_VAL au slot relache pour ne pas garder une
     * ref ancienne accidentellement visible par mark_roots si la stack
     * recroit ensuite. */
    vm.event_sender_stack[vm.event_sender_stack_count] = NIL_VAL;
}

Value vm_event_sender_top(void) {
    if (vm.event_sender_stack_count == 0) return NIL_VAL;
    return vm.event_sender_stack[vm.event_sender_stack_count - 1];
}

int vm_event_sender_depth(void) {
    return vm.event_sender_stack_count;
}

/* ================================================================
 * DSL Etat V2 — vm.etats
 * ================================================================ */

bool vm_etat_register(OwlEtatDecl* decl) {
    if (!decl || !decl->name) return false;
    for (int i = 0; i < vm.etatCount; i++) {
        if (strcmp(vm.etats[i]->name, decl->name) == 0) return false;
    }
    if (vm.etatCount >= vm.etatCapacity) {
        int new_cap = vm.etatCapacity < 4 ? 4 : vm.etatCapacity * 2;
        vm.etats = (OwlEtatDecl**)realloc(vm.etats,
                                            sizeof(OwlEtatDecl*) * (size_t)new_cap);
        vm.etatCapacity = new_cap;
    }
    vm.etats[vm.etatCount++] = decl;
    /* Expose le nom comme global string (pour iImprimeEtat(ETA_X) au runtime).
     * nameStr est rootee sur la pile pendant table_set : une grow de
     * vm.globals peut declencher un GC qui collecterait la string. */
    ObjString* nameStr = obj_copy_string(decl->name, (int)strlen(decl->name));
    vm_push(OBJ_VAL(nameStr));
    /*  phase 2b : route via obj_globals_set_kv pour peupler aussi
     * vm.globals_canon_idx. */
    obj_globals_set_kv(nameStr, OBJ_VAL(nameStr));
    vm_pop();
    return true;
}

const OwlEtatDecl* vm_etat_lookup(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < vm.etatCount; i++) {
        if (strcmp(vm.etats[i]->name, name) == 0) return vm.etats[i];
    }
    return NULL;
}

void vm_etats_clear(void) {
    for (int i = 0; i < vm.etatCount; i++) {
        owl_etat_decl_free(vm.etats[i]);
    }
    vm.etatCount = 0;
}

/* Comparaison de noms de controles case ET accent-insensible (invariant 2).
 * Le caller peut passer une forme brute ou strippee : on strippe les deux
 * cotes avant de comparer. */
static bool ctrl_name_match(const char* a, const char* b) {
    if (!a || !b) return false;
    char sa[256], sb[256];
    int la = scanner_strip_accents(a, (int)strlen(a), sa, sizeof(sa));
    int lb = scanner_strip_accents(b, (int)strlen(b), sb, sizeof(sb));
    if (la != lb) return false;
    return strncasecmp(sa, sb, (size_t)la) == 0;
}

/* Cherche un widget user-declare par nom (case-insensible) dans toutes les
 * Fenetres et tous les Etats. Retourne le OwlControlKindInfo* du kind ou
 * NULL si absent. Utilise par compiler.c pour translater `WIDGET = X` en
 * `WIDGET..<defaultProperty> = X` conforme canon.
 *
 * Pourquoi un seul helper Fenetres+Etats : par symetrie, les widgets d'un
 * Etat (Libelle d'etat dans Corps, EnTeteDocument...) suivent la meme
 * convention que ceux d'une Fenetre. Source unique de verite, zero drift. */
const OwlControlKindInfo* vm_find_widget_info_by_name(const char* name) {
    if (!name) return NULL;

    /* Etape 1 — prescan widgets : couvre les forward-refs (widget declare
     * APRES le site d'usage, ex: PROCEDURE surClic dans BTN_X qui modifie
     * LIB_R declare plus bas dans la fenetre). Le compile principal n'a
     * pas encore enregistre les widgets internes dans vm.windows quand
     * il compile les bodies de procedure ; le prescan source les a deja
     * detectes. */
    const OwlControlKindInfo* fromPrescan = compiler_prescan_widget_find(name);
    if (fromPrescan) return fromPrescan;

    /* Etape 2 — vm.windows / vm.etats : couvre les usages post-declaration
     * et le runtime (Trace(LIB_X), etc. apres Ouvre()). */
    OwlControlKind found_kind = (OwlControlKind)-1;
    bool found = false;

    /* Fenetres : owl_window_decl_find_control fait deja le match CI. */
    for (int i = 0; i < vm.windowCount && !found; i++) {
        const OwlControlDecl* c = owl_window_decl_find_control(vm.windows[i], name);
        if (c) { found_kind = c->kind; found = true; }
    }

    /* Etats : itere blocks puis leurs controls (pointeurs OwlControlDecl*). */
    for (int e = 0; e < vm.etatCount && !found; e++) {
        OwlEtatDecl* etat = vm.etats[e];
        for (int b = 0; b < etat->block_count && !found; b++) {
            OwlEtatBlockDecl* bl = &etat->blocks[b];
            for (int c = 0; c < bl->control_count && !found; c++) {
                if (bl->controls[c] && bl->controls[c]->name &&
                    ctrl_name_match(bl->controls[c]->name, name)) {
                    found_kind = bl->controls[c]->kind;
                    found = true;
                }
            }
        }
    }
    if (!found) return NULL;

    /* Map kind → OwlControlKindInfo*. Itere CONTROL_KINDS via le helper
     * de mapping deja existant. */
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        OwlControlKind tmpKind;
        if (owl_control_kind_of_info(k, &tmpKind) && tmpKind == found_kind) {
            return k;
        }
    }
    return NULL;
}

int vm_count_controls_named(const char* name) {
    if (!name) return 0;
    int count = 0;
    for (int wi = 0; wi < vm.windowCount; wi++) {
        OwlWindowDecl* w = vm.windows[wi];
        /* Le nom de la fenetre elle-meme compte aussi (la fenetre est un
         * "champ" au sens canon — Occurrence(FEN_X) renvoie 1 si declaree). */
        if (strcasecmp(w->name, name) == 0) count++;
        for (int ci = 0; ci < w->control_count; ci++) {
            if (strcasecmp(w->controls[ci].name, name) == 0) count++;
        }
    }
    return count;
}

/* ================================================================
 * STACK OPERATIONS
 * ================================================================ */

void vm_push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

Value vm_pop(void) {
    vm.stackTop--;
    return *vm.stackTop;
}

Value vm_peek(int distance) {
    return vm.stackTop[-1 - distance];
}

/* ================================================================
 * ERROR REPORTING
 * ================================================================ */

static bool handle_runtime_exception(void);
void vm_throw_from_erreur(const char* msg, int code);

/* Print the N-th 1-based line from `path` to stderr, visually framed. Used
 * to give runtime errors a small source snippet for context. */
static void print_source_snippet(const char* path, int line) {
    if (path == NULL || line <= 0) return;
    FILE* f = fopen(path, "rb");
    if (f == NULL) return;
    char buf[512];
    int current = 1;
    int lineStart = (line > 2) ? line - 2 : 1;
    int lineEnd   = line + 1;
    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (current >= lineStart && current <= lineEnd) {
            /* strip trailing newline */
            size_t n = strlen(buf);
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
            fprintf(stderr, "   %s %4d | %s\n",
                    current == line ? ">" : " ", current, buf);
        }
        current++;
        if (current > lineEnd) break;
    }
    fclose(f);
}

/* Snapshot current VM state into the exception context fields. Called
 * whenever an exception is raised (runtime_error with a handler present,
 * or ExceptionDeclenche). */
static void capture_exception_context(const char* message, const char* code) {
    currentException = obj_copy_string(message, (int)strlen(message));
    /*  : errCode transporté en chaîne canonique. code NULL / vide =>
     * pas de code (errCode ""). currentException est déjà rooté (racine
     * persistante) avant cette 2e allocation. */
    g_exc_code = (code != NULL && code[0] != '\0')
        ? obj_copy_string(code, (int)strlen(code)) : NULL;
    /* Capture line + filename + procedure from the current top frame. */
    if (vm.frameCount > 0) {
        CallFrame* top = &vm.frames[vm.frameCount - 1];
        ObjFunction* fn = top->function;
        size_t instr = top->ip - fn->chunk->code - 1;
        g_exc_line = fn->chunk->lines[instr];
        g_exc_filename = fn->filename;
        g_exc_procedure = fn->name;
    } else {
        g_exc_line = 0;
        g_exc_filename = NULL;
        g_exc_procedure = NULL;
    }
    /* Reset opt-in slots si pas posés par le caller pour cette levée. */
    if (!g_exc_sys_pending) {
        g_exc_sys_code = 0;
        g_exc_sys_message = NULL;
    }
    g_exc_sys_pending = false;
    /* Origin : posé par native_exception_throw / vm_throw_from_erreur via flag.
     * Sinon (runtime_error sans instrumentation, OP_THROW legacy) fallback
     * générique pour rester distinct de g_exc_procedure. */
    if (!g_exc_origin_pending) {
        g_exc_origin = obj_copy_string("<runtime>", 9);
        /* g_exc_info pas reset ici : si une native instrumente plus tard
         * elle posera g_exc_info elle-même ; sinon ExceptionInfo(errInfo)
         * lit l'existant. Pour les levées user via ExceptionDéclenche le
         * flag origin_pending est posé et g_exc_info y est explicitement
         * reset. */
        if (g_exc_info == NULL) g_exc_info = obj_copy_string("", 0);
    }
    g_exc_origin_pending = false;
    /* errFonction : posé via flag par owl_vm_raise_native / vm_throw_from_erreur_coded.
     * Sinon (levée user, runtime_error, OP_THROW) aucune native → reset. */
    if (!g_exc_native_name_pending) {
        g_exc_native_name = NULL;
    }
    g_exc_native_name_pending = false;
    /* Build a preformatted stack trace. */
    char stackBuf[2048];
    int off = 0;
    for (int i = vm.frameCount - 1; i >= 0 && off < (int)sizeof(stackBuf) - 80; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* fn = frame->function;
        size_t instr = frame->ip - fn->chunk->code - 1;
        int line = fn->chunk->lines[instr];
        const char* name = fn->name == NULL ? "<script>" : fn->name->chars;
        const char* file = fn->filename == NULL ? "?" : fn->filename->chars;
        off += snprintf(stackBuf + off, sizeof(stackBuf) - off,
                        "  %s() [%s:%d]\n", name, file, line);
    }
    g_exc_stack = obj_copy_string(stackBuf, off);
    /* Timestamp (seconds-resolution is fine for now). */
    g_exc_timestamp_ms = (int64_t)time(NULL) * 1000;
}

/* Public API : lève une exception fatale rattrapable depuis une native
 * stdlib. Miroir de native_exception_throw (ExceptionDéclenche) : pose
 * l'origine, capture le contexte à partir du message déjà rendu, et arme
 * throwPending pour que le dispatch OP_CALL enclenche le handler
 * QUAND EXCEPTION englobant. Sans handler, la propagation se termine en
 * exception non rattrapée (E_RT_UNHANDLED_EXCEPTION). Le message est copié
 * immédiatement (obj_copy_string via capture_exception_context), le caller
 * peut passer un buffer partagé du registry erreurs. */
void owl_vm_raise_native(const char* origin, const char* code_canonical,
                         const char* message) {
    if (origin != NULL) {
        g_exc_origin = obj_copy_string(origin, (int)strlen(origin));
        g_exc_origin_pending = true;
        /*  : la native qui lève renseigne errFonction (= son nom). */
        g_exc_native_name = obj_copy_string(origin, (int)strlen(origin));
        g_exc_native_name_pending = true;
        g_exc_info = obj_copy_string("", 0);
    }
    capture_exception_context(message != NULL ? message : "", code_canonical);
    throwPending = true;
}

/* Public API : une native stdlib qui s'apprête à lever une exception et
 * dispose d'une info syscall pertinente (errno + strerror) appelle ceci
 * AVANT la levée. Le flag pending fait survivre les valeurs au reset
 * que capture_exception_context applique sinon. Lisible via
 * ExceptionInfo(errCodeSystème) / ExceptionInfo(errMessageSystème). */
void owl_set_exception_system_error(int sys_code, const char* sys_message) {
    g_exc_sys_code = sys_code;
    g_exc_sys_message = (sys_message != NULL)
        ? obj_copy_string(sys_message, (int)strlen(sys_message))
        : obj_copy_string("", 0);
    g_exc_sys_pending = true;
}

void vm_set_capture_uncaught(bool enabled) {
    g_capture_uncaught = enabled;
}

void vm_exception_context_reset(void) {
    currentException   = NULL;
    g_exc_code         = NULL;
    g_exc_line         = 0;
    g_exc_filename     = NULL;
    g_exc_procedure    = NULL;
    g_exc_origin       = NULL;
    g_exc_native_name  = NULL;
    g_exc_info         = NULL;
    g_exc_stack        = NULL;
    g_exc_timestamp_ms = 0;
    g_exc_sys_code     = 0;
    g_exc_sys_message  = NULL;
}

static void runtime_error(const char* format, ...) {
    /* Check if we can handle this with an exception handler */
    if (exceptionCount > 0) {
        /* Build the error message */
        char buf[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        capture_exception_context(buf, NULL);
        return; /* The caller will check and handle */
    }

    /* Build the formatted message once so it can precede the context line. */
    char msg[512];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    /*  : erreur NON rattrapée mais capture armée (exécuteur de service).
     * On snapshot le contexte AVANT les resets de pile/frames ci-dessous, pour
     * qu'un hook surErreur invoqué après l'unwind lise le crash via
     * ExceptionInfo(). */
    if (g_capture_uncaught) {
        capture_exception_context(msg, NULL);
    }

    if (g_json_error_mode) {
        /* Single-line JSON on stderr — callers parse with JSONL readers. */
        fputs("{\"kind\":\"runtime\",\"message\":", stderr);
        json_esc_stderr(msg);
        if (vm.frameCount > 0) {
            CallFrame* top = &vm.frames[vm.frameCount - 1];
            ObjFunction* fn = top->function;
            size_t instr = top->ip - fn->chunk->code - 1;
            int line = fn->chunk->lines[instr];
            fputs(",\"line\":", stderr); fprintf(stderr, "%d", line);
            if (fn->filename) {
                fputs(",\"file\":", stderr);
                json_esc_stderr(fn->filename->chars);
            }
        }
        fputs(",\"stack\":[", stderr);
        for (int i = vm.frameCount - 1; i >= 0; i--) {
            CallFrame* frame = &vm.frames[i];
            ObjFunction* function = frame->function;
            size_t instruction = frame->ip - function->chunk->code - 1;
            int line = function->chunk->lines[instruction];
            const char* fname = function->name == NULL ? "<script>" : function->name->chars;
            if (i != vm.frameCount - 1) fputc(',', stderr);
            fputs("{\"function\":", stderr); json_esc_stderr(fname);
            fprintf(stderr, ",\"line\":%d", line);
            if (function->filename) {
                fputs(",\"file\":", stderr);
                json_esc_stderr(function->filename->chars);
            }
            fputc('}', stderr);
        }
        fputs("]}\n", stderr);
        /* Guarantee the consumer sees the line even if stderr was
         * block-buffered (piped subprocess on Windows defaults to 4k+
         * block buffering, would hold the error until process exit). */
        fflush(stderr);
        vm.stackTop = vm.stack;
        vm.frameCount = 0;
        return;
    }

    fprintf(stderr, "\nErreur d'execution : %s\n", msg);

    /* Print the top frame's source snippet for immediate context. */
    if (vm.frameCount > 0) {
        CallFrame* top = &vm.frames[vm.frameCount - 1];
        ObjFunction* topFn = top->function;
        size_t instr = top->ip - topFn->chunk->code - 1;
        int line = topFn->chunk->lines[instr];
        const char* path = topFn->filename != NULL ? topFn->filename->chars : NULL;
        if (path != NULL) fprintf(stderr, "  a %s:%d\n", path, line);
        else              fprintf(stderr, "  a ligne %d\n", line);
        print_source_snippet(path, line);
    }

    if (vm.frameCount > 1) {
        fputs("\nPile d'appels :\n", stderr);
    }
    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->function;
        size_t instruction = frame->ip - function->chunk->code - 1;
        int line = function->chunk->lines[instruction];
        const char* fname = function->name == NULL ? "<script>" : function->name->chars;
        if (function->filename != NULL) {
            fprintf(stderr, "  a %s() [%s:%d]\n", fname, function->filename->chars, line);
        } else {
            fprintf(stderr, "  a %s() [ligne %d]\n", fname, line);
        }
    }

    vm.stackTop = vm.stack;
    vm.frameCount = 0;
}

/* Pont public pour le registry erreurs (cf owl_errors_runtime.c).
 * Le helper owl_emit_runtime_error rend le template FR puis nous passe
 * la chaine finale a injecter dans le format legacy via "%s". On
 * preserve ainsi 100% du comportement runtime_error() (stack trace,
 * capture handler, mode JSON, snippet) sans dupliquer la logique. */
void vm_emit_runtime_error_rendered(const char* message) {
    runtime_error("%s", message != NULL ? message : "");
}

static bool handle_runtime_exception(void) {
    if (exceptionCount > 0) {
        ExceptionHandler* handler = &exceptionStack[--exceptionCount];
        vm.stackTop = handler->stackTop;
        vm.frameCount = handler->frameCount;
        vm.frames[vm.frameCount - 1] = handler->frame;
        vm.frames[vm.frameCount - 1].ip = handler->ip;
        /*  : reamorce la pile de sauvegarde du contexte au niveau de
         * CE handler. Truncate les snapshots des try internes abandonnes par
         * la propagation ; garantit que la sortie normale du handler (via
         * OP_EXC_RESTORE) restaure exactement le contexte englobant. Le
         * contexte courant (celui de l'exception) reste tel quel : le handler
         * doit le lire. */
        g_exec_ctx_cur->excSaveDepth = handler->savedCtxDepth;
        return true;
    }
    return false;
}

/*  — frontiere reentrante : exit-frame de la boucle run() courante.
 * Au top-level c'est 0 (deroule tout) ; quand run() est lance en reentrant
 * depuis une native (vm_call_reentrant), l'appelant y met son propre frameCount
 * pour que la boucle imbriquee sache ou s'arreter sans devorer les frames de
 * l'appelant. Relocalise ici (etait plus bas) car exc_top_handler_in_scope() et
 * la branche native de call_value en ont besoin pour statuer sur la frontiere. */
static int g_run_exit_frame = 0;

/* Vrai pendant un run() lance par vm_call_reentrant. Distingue le retour a la
 * frontiere reentrante (OP_RETURN doit PUSH le resultat pour la native
 * appelante) du retour top-level du script (OP_RETURN pop le sentinel script et
 * jette le resultat). Sans ce flag, un appel reentrant lance alors que
 * vm.frameCount == 0 (ex. une native top-level comme Ouvre qui resout une prop
 * dynamique) etait confondu avec le top-level. */
static bool g_run_reentrant_boundary = false;

/*  — un handler QUAND EXCEPTION du sommet de exceptionStack appartient
 * au run() courant seulement si sa frame cible est STRICTEMENT au-dessus de la
 * frontiere reentrante (exitFrame). Un handler a/en-dessous de exitFrame vit chez
 * un ANCETRE, au-dela de la frontiere : on ne le consomme PAS ici, l'exception
 * doit etre propagee en vol jusqu'au run()/site qui englobe directement sa frame.
 * Au top-level (exitFrame == 0) tout handler (frameCount >= 1) est en scope. */
static bool exc_top_handler_in_scope(int exitFrame) {
    return exceptionCount > 0 &&
           exceptionStack[exceptionCount - 1].frameCount > exitFrame;
}

/* ================================================================
 * CALL
 * ================================================================ */

static bool call_value(Value callee, int argCount);
static Value value_deep_clone(Value v);
static void instance_copy_defaults(Table* from, ObjInstance* instance);

static bool call(ObjFunction* function, int argCount) {
    /* Pad with default values for any optional parameters not supplied.
     * minArity = first N required params; arity = total. Defaults are stored
     * in function->defaults, indexed by (paramIdx - minArity).
     * When defaultCount == 0, minArity equals arity (all required). */
    int minArity = (function->defaultCount > 0)
                       ? function->minArity
                       : function->arity;
    if (argCount < minArity || argCount > function->arity) {
        char expBuf[16], gotBuf[16], minBuf[16];
        snprintf(gotBuf, sizeof gotBuf, "%d", argCount);
        if (minArity == function->arity) {
            snprintf(expBuf, sizeof expBuf, "%d", function->arity);
            owl_emit_runtime_error(E_RT_BAD_ARITY,
                                   "expected", expBuf,
                                   "got", gotBuf, NULL);
        } else {
            snprintf(minBuf, sizeof minBuf, "%d", minArity);
            snprintf(expBuf, sizeof expBuf, "%d", function->arity);
            owl_emit_runtime_error(E_RT_BAD_ARITY_RANGE,
                                   "min", minBuf,
                                   "max", expBuf,
                                   "got", gotBuf, NULL);
        }
        return false;
    }
    while (argCount < function->arity) {
        int defaultIdx = argCount - minArity;
        if (defaultIdx < 0 || defaultIdx >= function->defaultCount) {
            char posBuf[16];
            snprintf(posBuf, sizeof posBuf, "%d", argCount + 1);
            owl_emit_runtime_error(E_RT_MISSING_DEFAULT,
                                   "position", posBuf, NULL);
            return false;
        }
        vm_push(function->defaults[defaultIdx]);
        argCount++;
    }

    if (vm.frameCount == FRAMES_MAX) {
        owl_emit_runtime_error(E_RT_STACK_OVERFLOW, NULL);
        g_rt_fatal_vm = true; /*  : fatale-VM, non rattrapable par le pont OP_CALL */
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->function = function;
    frame->ip = function->chunk->code;
    frame->slots = vm.stackTop - argCount - 1;
    frame->exceptionBase = exceptionCount;
    frame->excSaveBase = g_exec_ctx_cur->excSaveDepth;   /*  */
    return true;
}

/* Look up a method in a class hierarchy (walks superclass chain) */
static bool class_lookup_method(ObjClass* klass, ObjString* name, Value* method) {
    ObjClass* current_klass = klass;
    while (current_klass != NULL) {
        if (table_get(&current_klass->methods, name, method)) {
            return true;
        }
        current_klass = current_klass->superclass;
    }
    return false;
}

static bool call_value(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_FUNCTION:
                return call(AS_FUNCTION(callee), argCount);
            case OBJ_NATIVE: {
                ObjNative* native = (ObjNative*)AS_OBJ(callee);
                NativeFn fn = native->function;
                if (native->arity != -1 && argCount != native->arity) {
                    char expBuf[16], gotBuf[16];
                    snprintf(expBuf, sizeof expBuf, "%d", native->arity);
                    snprintf(gotBuf, sizeof gotBuf, "%d", argCount);
                    owl_emit_runtime_error(E_RT_BAD_ARITY,
                                           "expected", expBuf,
                                           "got", gotBuf, NULL);
                    return false;
                }

                /* MOCK INTERCEPTION: Try to intercept with mock system */
                Value mockResult;
                bool wasMocked = mock_try_intercept(native, argCount,
                                                   vm.stackTop - argCount,
                                                   &mockResult);

                /*  — sentinel pour detecter qu'une erreur runtime a
                 * ete levee a l'INTERIEUR de la native via vm_call_reentrant
                 * (typiquement Ouvre() qui execute FinInitialisation, qui
                 * leve une div/0). runtime_error() purge l'etat VM
                 * (vm.frameCount = 0, vm.stackTop = vm.stack). Sans cette
                 * garde, le code post-call ci-dessous (vm.stackTop -=
                 * argCount + 1, vm_push, retour dans OP_CALL) accede a une
                 * stack vide et au frame[-1] -> segfault. */
                int frameSentinel = vm.frameCount;

                Value result;
                if (wasMocked) {
                    result = mockResult;
                } else {
                    result = fn(argCount, vm.stackTop - argCount);
                }

                /*  — si la native a unwind via runtime_error()
                 * interne, propager l'echec comme un retour false standard.
                 * Le caller (OP_CALL dans run()) verra le false et retournera
                 * INTERPRET_RUNTIME_ERROR proprement. */
                if (vm.frameCount < frameSentinel) {
                    return false;
                }

                /* Capture out-params avant le pop : la native a pu muter
                 * args[firstOutParamArg .. min(lastOutParamArg, argCount-1)].
                 * On copie ces valeurs vers vm.outParamReturns[] pour que les
                 * opcodes OP_OUT_PARAM_TO_LOCAL/GLOBAL émis par le compilateur
                 * puissent les écrire dans les variables du caller. */
                vm.outParamCount = 0;
                if (native->firstOutParamArg >= 0 &&
                    native->firstOutParamArg < argCount) {
                    Value* args = vm.stackTop - argCount;
                    int last = (native->lastOutParamArg < 0)
                               ? (argCount - 1)
                               : native->lastOutParamArg;
                    if (last >= argCount) last = argCount - 1;
                    int n = last - native->firstOutParamArg + 1;
                    if (n < 0) n = 0;
                    if (n > (int)(sizeof(vm.outParamReturns) / sizeof(Value))) {
                        n = (int)(sizeof(vm.outParamReturns) / sizeof(Value));
                    }
                    for (int i = 0; i < n; i++) {
                        vm.outParamReturns[i] = args[native->firstOutParamArg + i];
                    }
                    vm.outParamCount = n;
                }

                vm.stackTop -= argCount + 1;
                vm_push(result);
                if (throwPending) {
                    throwPending = false;
                    /*  — la native a rendu la main avec une exception en
                     * vol (levee soit directement, soit dans un callback
                     * reentrant qui l'a propagee jusqu'ici). On consomme le
                     * handler seulement s'il vit dans le run() courant (au-dessus
                     * de g_run_exit_frame). Sinon il appartient a un ancetre
                     * au-dela de la frontiere reentrante : on re-arme throwPending
                     * et on remonte via `false`, le pont OP_CALL englobant tranche
                     * a son tour (consomme s'il est chez lui, propage sinon). */
                    if (exc_top_handler_in_scope(g_run_exit_frame)) {
                        if (handle_runtime_exception()) {
                            return true;
                        }
                    } else if (exceptionCount > 0 && currentException != NULL) {
                        throwPending = true;
                        return false;
                    }
                    owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION,
                        "message",
                        currentException ? currentException->chars : "unknown",
                        NULL);
                    return false;
                }
                return true;
            }
            case OBJ_PROC_CHAIN: {
                ObjProcChain* chain = AS_PROC_CHAIN(callee);
                if (argCount > 255) {
                    owl_emit_runtime_error(E_RT_PROC_CHAIN_TOO_MANY_ARGS, NULL);
                    return false;
                }
                /* Snapshot args so each step can re-push them. */
                Value argsBuf[256];
                for (int i = 0; i < argCount; i++) {
                    argsBuf[i] = vm.stackTop[-argCount + i];
                }
                /* Drop [callee, args...] from the original OP_CALL. */
                vm.stackTop -= argCount + 1;

                Value lastResult = NIL_VAL;

                #define CHAIN_INVOKE(CALLEE_VAL) \
                    do { \
                        Value cv = (CALLEE_VAL); \
                        vm_push(cv); \
                        for (int _i = 0; _i < argCount; _i++) vm_push(argsBuf[_i]); \
                        if (vm_call_reentrant(cv, argCount) != INTERPRET_OK) return false; \
                        lastResult = vm_pop(); \
                    } while (0)

                if (!IS_NIL(chain->avant)) CHAIN_INVOKE(chain->avant);
                for (int i = 0; i < chain->count; i++) CHAIN_INVOKE(chain->procs[i]);
                if (!IS_NIL(chain->apres)) CHAIN_INVOKE(chain->apres);

                #undef CHAIN_INVOKE

                vm_push(lastResult);
                return true;
            }
            case OBJ_CLASS: {
                /* Calling a class creates an instance and calls constructor */
                ObjClass* klass = AS_CLASS(callee);
                ObjInstance* instance = obj_new_instance(klass->name);
                instance->klass = klass;

                /* Replace the class on the stack with the instance AVANT de
                 * copier les defaults : instance_copy_defaults alloue des
                 * clones (tableaux/assoc/...) qui peuvent declencher le GC.
                 * L'instance doit donc deja etre racine sur la pile. */
                vm.stackTop[-argCount - 1] = OBJ_VAL(instance);

                /* Copy default fields from superclass chain (deep-clone des
                 * valeurs mutables — pas de partage entre instances). */
                ObjClass* k = klass;
                while (k != NULL) {
                    if (k->superclass != NULL) {
                        instance_copy_defaults(&k->superclass->defaults, instance);
                    }
                    k = k->superclass;
                }
                instance_copy_defaults(&klass->defaults, instance);

                /* Look for constructor (method with same name as class) */
                Value constructor;
                if (class_lookup_method(klass, klass->name, &constructor)) {
                    return call(AS_FUNCTION(constructor), argCount);
                } else if (argCount != 0) {
                    char gotBuf[16];
                    snprintf(gotBuf, sizeof gotBuf, "%d", argCount);
                    owl_emit_runtime_error(E_RT_CONSTRUCTOR_EXPECTED_ZERO_ARGS,
                                           "got", gotBuf, NULL);
                    return false;
                }
                return true;
            }
            default:
                break;
        }
    }
    owl_emit_runtime_error(E_RT_NOT_CALLABLE, NULL);
    return false;
}

/* ================================================================
 * COERCION: + operator handles numbers and string concat
 * ================================================================ */

static bool is_falsy(Value value) {
    if (IS_NIL(value)) return true;
    if (IS_BOOL(value)) return !AS_BOOL(value);
    if (IS_INTEGER(value)) return AS_INTEGER(value) == 0;
    if (IS_REAL(value)) return AS_REAL(value) == 0.0;
    if (IS_CURRENCY(value)) return AS_CURRENCY(value) == 0;
    if (IS_NUMERIC(value)) return mpd_iszero(AS_NUMERIC(value)->dec);
    return false;
}

/*  — borne de profondeur de la copie.
 *
 * Un graphe que l'utilisateur referme sur lui-meme (`x.oEnfant = x`, ou la
 * boucle a deux `b -> c -> b`) est constructible et legitime ; le copier
 * faisait descendre cette recursion sans fin -> SIGSEGV muet, code 139.
 *
 * = FRAMES_MAX, le plafond de recursion des procedures : une valeur imbriquee
 * au-dela est refusee exactement comme un appel recursif au-dela. Ce n'est
 * donc pas un chiffre choisi, c'est une coherence — le changer ici sans
 * changer FRAMES_MAX creerait deux plafonds differents pour la meme idee.
 *
 * Max observe = 6 sur tout le corpus de tests, mesure en instrumentant ce
 * compteur (scenarios 2, docsamples 2, regression 1, runtime 6). Ce 6 est la
 * chaine de style (Style -> element -> Cadre/Police -> sous-objets) : une
 * forme FIXE, qui ne se creuse pas avec les donnees. La borne est 42x
 * au-dessus.
 *
 * Pourquoi une borne et PAS une table des objets deja visites : la table
 * changerait le resultat des copies NON cycliques. Un meme sous-objet
 * reference deux fois (un gabarit de style partage — cas courant) est
 * aujourd'hui DUPLIQUE ; avec une table il deviendrait PARTAGE, et muter
 * l'un muterait l'autre. Elle rendrait de plus la copie elle-meme cyclique,
 * sans un mot, repoussant le probleme au prochain marcheur recursif. La
 * borne, elle, le nomme.
 *
 * Cout mesure de la garde sur une copie NON cyclique : +3 % sur le travail de
 * copie pur (1,6 M noeuds clones, min de 7 passes : 925 ms sans garde,
 * 950 ms avec), soit ~15 ns par noeud — sous le bruit de mesure. */
#define DEEP_CLONE_MAX_DEPTH FRAMES_MAX

static int  deep_clone_depth = 0;
static bool deep_clone_self_containing = false;

/* Vrai si la derniere copie profonde a bute sur la borne. Consomme le
 * drapeau : le caller emet le message et abandonne l'instruction. */
static bool deep_clone_take_self_containing(void) {
    bool hit = deep_clone_self_containing;
    deep_clone_self_containing = false;
    return hit;
}

/* Forward-declared; ObjJson needs to be cloned recursively without going
 * through the serializer. */
static ObjJson* json_deep_clone(ObjJson* src);
static ObjJson* json_deep_clone_body(ObjJson* src);

static ObjJson* json_deep_clone(ObjJson* src) {
    if (src == NULL) return NULL;
    if (deep_clone_depth >= DEEP_CLONE_MAX_DEPTH) {
        deep_clone_self_containing = true;
        return obj_new_json(JSON_T_NULL);
    }
    deep_clone_depth++;
    ObjJson* out = json_deep_clone_body(src);
    deep_clone_depth--;
    return out;
}

static ObjJson* json_deep_clone_body(ObjJson* src) {
    switch (src->jtype) {
        case JSON_T_NULL:   return obj_new_json(JSON_T_NULL);
        case JSON_T_BOOL:   return obj_new_json_bool(src->as.boolVal);
        case JSON_T_INT:    return obj_new_json_int(src->as.intVal);
        case JSON_T_REAL:   return obj_new_json_real(src->as.realVal);
        case JSON_T_CURRENCY: return obj_new_json_currency(src->as.currencyVal);
        case JSON_T_NUMERIC:  return obj_new_json_numeric(OBJ_VAL(src->as.numericVal));
        case JSON_T_STRING: return obj_new_json_string(src->as.strVal);
        case JSON_T_ARRAY: {
            ObjJson* dst = obj_new_json(JSON_T_ARRAY);
            for (int i = 0; i < src->as.arrayVal.count; i++) {
                obj_json_array_push(dst, json_deep_clone(src->as.arrayVal.items[i]));
            }
            return dst;
        }
        case JSON_T_OBJECT: {
            ObjJson* dst = obj_new_json(JSON_T_OBJECT);
            for (int i = 0; i < src->as.objectVal.count; i++) {
                obj_json_object_set(dst, src->as.objectVal.keys[i],
                                    json_deep_clone(src->as.objectVal.values[i]));
            }
            return dst;
        }
    }
    return src;
}

static Value deep_clone_obj(Value v);

/* Deep-clone any value. Primitives are returned as-is (they are copied by
 * value when stored). Heap objects get a fresh instance with their contents
 * cloned recursively. */
static Value value_deep_clone(Value v) {
    /* Remise a zero du drapeau des l'entree la plus externe, y compris pour
     * une valeur non-Obj : le caller doit lire l'etat de SA copie, pas le
     * residu d'une copie precedente dont personne n'a consomme le drapeau. */
    if (deep_clone_depth == 0) deep_clone_self_containing = false;
    if (!IS_OBJ(v)) return v;
    if (deep_clone_depth >= DEEP_CLONE_MAX_DEPTH) {
        /* On ne remonte pas l'erreur d'ici : les etages superieurs doivent
         * derouler leurs vm_pop() pour rendre la stack equilibree. Le caller
         * lit le drapeau une fois la recursion sortie. */
        deep_clone_self_containing = true;
        return NIL_VAL;
    }
    deep_clone_depth++;
    Value cloned = deep_clone_obj(v);
    deep_clone_depth--;
    return cloned;
}

static Value deep_clone_obj(Value v) {
    Obj* o = AS_OBJ(v);
    /*   : push src + dst pendant tous les sub-allocs.
     * Sans ces push, les allocs recursives peuvent GC et collecter src
     * (lu uniquement via C local) ou dst (en construction). Sous
     * OWL_GC_POISON, ecriture dans dst poisonne segfault deterministe. */
    switch (o->type) {
        case OBJ_STRING:
            /* Strings are immutable and interned in OWL. Sharing is safe. */
            return v;
        case OBJ_BUFFER: {
            ObjBuffer* src = (ObjBuffer*)o;
            return OBJ_VAL(obj_new_buffer(src->bytes, src->length));
        }
        case OBJ_ARRAY: {
            vm_push(v); /* root src */
            ObjArray* src = (ObjArray*)o;
            ObjArray* dst = obj_new_array();
            vm_push(OBJ_VAL(dst));
            for (int i = 0; i < src->elements.count; i++) {
                /*  : push cloned avant value_array_write qui peut
                 * grow → GC → cloned fresh collected. */
                Value cloned = value_deep_clone(src->elements.values[i]);
                if (IS_OBJ(cloned)) vm_push(cloned);
                value_array_write(&dst->elements, cloned);
                if (IS_OBJ(cloned)) vm_pop();
            }
            vm_pop(); /* dst */
            vm_pop(); /* src */
            return OBJ_VAL(dst);
        }
        case OBJ_ASSOC_ARRAY: {
            vm_push(v); /* root src */
            ObjAssocArray* src = (ObjAssocArray*)o;
            ObjAssocArray* dst = obj_new_assoc(src->keyType, src->options, src->valueKind);
            dst->valueTypeName = src->valueTypeName;
            vm_push(OBJ_VAL(dst));
            for (int i = 0; i < src->count; i++) {
                Value cloned = value_deep_clone(src->values[i]);
                if (IS_OBJ(cloned)) vm_push(cloned);
                obj_assoc_set(dst, src->keys[i], cloned);
                if (IS_OBJ(cloned)) vm_pop();
            }
            vm_pop(); /* dst */
            vm_pop(); /* src */
            return OBJ_VAL(dst);
        }
        case OBJ_COLLECTION: {
            vm_push(v); /* root src */
            ObjCollection* src = (ObjCollection*)o;
            ObjCollection* dst = obj_new_collection(src->collType);
            vm_push(OBJ_VAL(dst));
            for (int i = 0; i < src->elements.count; i++) {
                Value cloned = value_deep_clone(src->elements.values[i]);
                if (IS_OBJ(cloned)) vm_push(cloned);
                value_array_write(&dst->elements, cloned);
                if (IS_OBJ(cloned)) vm_pop();
            }
            vm_pop(); /* dst */
            vm_pop(); /* src */
            return OBJ_VAL(dst);
        }
        case OBJ_INSTANCE: {
            vm_push(v); /* root src */
            ObjInstance* src = (ObjInstance*)o;
            ObjInstance* dst = obj_new_instance(src->structName);
            vm_push(OBJ_VAL(dst));
            dst->klass = src->klass;
            /* fieldOrder est une metadonnee de declaration, partagee en
             * lecture : la copie doit connaitre l'ordre des champs comme
             * l'original (coercion positionnelle, serialisation, AVEC). */
            dst->fieldOrder = src->fieldOrder;
            for (int i = 0; i < src->fields.capacity; i++) {
                Entry* e = &src->fields.entries[i];
                if (e->key == NULL) continue;
                Value cloned = value_deep_clone(e->value);
                if (IS_OBJ(cloned)) vm_push(cloned);
                table_set(&dst->fields, e->key, cloned);
                if (IS_OBJ(cloned)) vm_pop();
            }
            vm_pop(); /* dst */
            vm_pop(); /* src */
            return OBJ_VAL(dst);
        }
        case OBJ_MULTILINGUE: {
            /* Mutable en place via `ml[tag] = texte` : le partage n'est pas
             * benin. Les textes stockes sont des chaines (immutables), seul
             * le dictionnaire a besoin d'une copie. */
            vm_push(v); /* root src */
            ObjMultilingue* src = (ObjMultilingue*)o;
            ObjMultilingue* dst = obj_new_multilingue();
            vm_push(OBJ_VAL(dst));
            dst->storage = obj_assoc_clone(src->storage);
            vm_pop(); /* dst */
            vm_pop(); /* src */
            return OBJ_VAL(dst);
        }
        case OBJ_JSON:
            return OBJ_VAL(json_deep_clone((ObjJson*)o));
        default:
            /* Partage assume. Deux familles seulement doivent atterrir ici :
             * les objets-valeur remplaces a l'ecriture (date, heure,
             * dateheure, duree, numerique, chaine, motif compile, icone) et
             * les poignees dont la semantique EST la reference (archive zip,
             * mock, connexion SQL, proxys UI/fenetre/etat, namespaces
             * theme/icones, fonction, classe). Un type mutable en place qui
             * arriverait ici serait un partage silencieux entre copies : lui
             * ajouter un case, pas un contournement chez l'appelant. */
            return v;
    }
}

/*  : wrapper public (cf. vm.h). Permet aux stdlib (UI) de deep-cloner
 * une Value sans dupliquer la logique de value_deep_clone (static). */
Value owl_value_deep_clone(Value v) {
    return value_deep_clone(v);
}

/*  — seul un Vecteur natif est un operande "vecteur-like". Un tableau
 * est refuse en amont par vm_vector_arith (distinguer tableau/Vecteur releverait
 * de l'inference de type, bannie) : ces helpers ne voient donc jamais d'array. */
static bool veclike_count(Value v, int* count) {
    if (IS_VECTOR(v)) { *count = AS_VECTOR(v)->count; return true; }
    return false;
}
static bool veclike_elem(Value v, int i, double* out) {
    if (IS_VECTOR(v)) { *out = AS_VECTOR(v)->data[i]; return true; }
    return false;
}

/* Statut arith Vecteur -> OwlErrorCode (renvoye en int, cf. vm.h). Un cas par
 * message : tailles incompatibles et operande tableau ont chacun leur code
 * dedie, plutot que le generique trompeur "les operandes doivent etre des
 * nombres". */
int owl_vector_arith_error_code(VecArithStatus st) {
    switch (st) {
        case VEC_ARITH_DIV_ZERO:      return E_RT_DIV_BY_ZERO;
        case VEC_ARITH_SIZE_MISMATCH: return E_RT_VECTOR_SIZE_MISMATCH;
        case VEC_ARITH_ARRAY_OPERAND: return E_RT_VECTOR_ARRAY_OPERAND;
        case VEC_ARITH_TYPE:          return E_RT_OPERANDS_NUMBERS;
        default:                      return E_RT_OPERANDS_NUMBERS;
    }
}

/*  — coeur arithmetique Vecteur. Cf. vm.h pour le contrat. Pure
 * computation sur double[] : la traduction du statut en exception (et le
 * choix du code d'erreur) reste au caller, qui connait son contexte.
 * L'opcode ne route ici que si au moins un operande est un Vecteur natif :
 * `tableau <op> tableau` ne passe donc jamais par cette fonction (aucune
 * regression sur la semantique des tableaux). */
VecArithStatus vm_vector_arith(Value a, Value b, char op, Value* out) {
    /*  — refus strict de l'operande tableau (litteral nu ou variable
     * typee). Seuls Vecteur op Vecteur et Vecteur op scalaire sont valides ;
     * un tableau doit etre converti explicitement (TableauVersVecteur) ou
     * declare Vecteur. Refus en tete, avant toute lecture veclike. */
    if (IS_ARRAY(a) || IS_ARRAY(b)) return VEC_ARITH_ARRAY_OPERAND;
    int ca = 0, cb = 0;
    bool aVec = veclike_count(a, &ca);
    bool bVec = veclike_count(b, &cb);
    int n;
    if (aVec && bVec) {
        if (ca != cb) return VEC_ARITH_SIZE_MISMATCH;
        n = ca;
    } else if (aVec) {
        /* Vecteur (gauche) op scalaire (droite) : les quatre operateurs. */
        if (!IS_NUMBER(b)) return VEC_ARITH_TYPE;
        n = ca;
    } else {
        /* Scalaire (gauche) op Vecteur : commutatif seulement pour + et *. */
        if (!IS_NUMBER(a)) return VEC_ARITH_TYPE;
        if (op != '+' && op != '*') return VEC_ARITH_TYPE;
        n = cb;
    }

    /* Pre-validation + mur division par zero, AVANT toute allocation, pour ne
     * jamais materialiser de resultat partiel. Une cellule de tableau non
     * numerique -> TYPE ; un diviseur (element ou scalaire) nul -> DIV_ZERO.
     * `x / 0.0` en C brut donnerait Inf/NaN silencieux : divergence a
     * empecher, contrat identique au scalaire reel. */
    for (int i = 0; i < n; i++) {
        if (aVec) { double x; if (!veclike_elem(a, i, &x)) return VEC_ARITH_TYPE; }
        double y;
        if (bVec) { if (!veclike_elem(b, i, &y)) return VEC_ARITH_TYPE; }
        else y = AS_NUMBER(b);
        if (op == '/' && y == 0.0) return VEC_ARITH_DIV_ZERO;
    }

    /* Alloue le resultat operandes rootees : obj_new_vector peut declencher
     * un GC. Apres l'alloc, plus aucune allocation OWL — les lectures des
     * operandes et les ecritures dans res->data (malloc, hors GC) sont sures. */
    vm_push(a);
    vm_push(b);
    ObjVector* res = obj_new_vector(n);
    vm_pop();
    vm_pop();

    double sa = aVec ? 0.0 : AS_NUMBER(a);   /* scalaire coerce entier->reel */
    double sb = bVec ? 0.0 : AS_NUMBER(b);
    for (int i = 0; i < n; i++) {
        double x, y;
        if (aVec) veclike_elem(a, i, &x); else x = sa;
        if (bVec) veclike_elem(b, i, &y); else y = sb;
        double r;
        switch (op) {
            case '+': r = x + y; break;
            case '-': r = x - y; break;
            case '*': r = x * y; break;   /* overflow -> Inf IEEE en silence */
            case '/': r = x / y; break;   /* diviseur nul deja ecarte ci-dessus */
            default:  r = 0.0; break;
        }
        res->data[i] = r;
    }
    *out = OBJ_VAL(res);
    return VEC_ARITH_OK;
}

/*  — pendant Matrice de owl_vector_arith_error_code. */
int owl_matrix_arith_error_code(MatArithStatus st) {
    switch (st) {
        case MAT_ARITH_DIV_ZERO:      return E_RT_DIV_BY_ZERO;
        case MAT_ARITH_SIZE_MISMATCH: return E_RT_MATRIX_SIZE_MISMATCH;
        case MAT_ARITH_ARRAY_OPERAND: return E_RT_MATRIX_ARRAY_OPERAND;
        case MAT_ARITH_TYPE:          return E_RT_OPERANDS_NUMBERS;
        default:                      return E_RT_OPERANDS_NUMBERS;
    }
}

/*  — coeur arithmetique Matrice, calque 2D de vm_vector_arith.
 * Cf. vm.h pour le contrat. Storage row-major contigu : l'iteration lineaire
 * sur rows*cols suffit (element-par-element positionnel). L'opcode ne route
 * ici que si au moins un operande est une Matrice native. */
MatArithStatus vm_matrix_arith(Value a, Value b, char op, Value* out) {
    /* Refus strict de l'operande tableau (litteral imbrique nu ou variable
     * tableau) : pas d'inference, conversion explicite requise. */
    if (IS_ARRAY(a) || IS_ARRAY(b)) return MAT_ARITH_ARRAY_OPERAND;
    bool aMat = IS_MATRIX(a);
    bool bMat = IS_MATRIX(b);
    int rows, cols;
    if (aMat && bMat) {
        ObjMatrix* ma = AS_MATRIX(a);
        ObjMatrix* mb = AS_MATRIX(b);
        if (ma->rows != mb->rows || ma->cols != mb->cols) return MAT_ARITH_SIZE_MISMATCH;
        rows = ma->rows; cols = ma->cols;
    } else if (aMat) {
        /* Matrice (gauche) op scalaire (droite) : les quatre operateurs. */
        if (!IS_NUMBER(b)) return MAT_ARITH_TYPE;
        rows = AS_MATRIX(a)->rows; cols = AS_MATRIX(a)->cols;
    } else {
        /* Scalaire (gauche) op Matrice : commutatif seulement pour + et *. */
        if (!IS_NUMBER(a)) return MAT_ARITH_TYPE;
        if (op != '+' && op != '*') return MAT_ARITH_TYPE;
        rows = AS_MATRIX(b)->rows; cols = AS_MATRIX(b)->cols;
    }
    int n = rows * cols;

    /* Mur division par zero AVANT toute allocation (pas de resultat partiel).
     * `x / 0.0` en C brut donnerait Inf/NaN silencieux : divergence a
     * empecher, contrat identique au scalaire reel et au Vecteur. */
    if (op == '/') {
        if (bMat) {
            const double* bd = AS_MATRIX(b)->data;
            for (int i = 0; i < n; i++) if (bd[i] == 0.0) return MAT_ARITH_DIV_ZERO;
        } else if (AS_NUMBER(b) == 0.0) {
            return MAT_ARITH_DIV_ZERO;
        }
    }

    /* Alloue le resultat operandes rootees : obj_new_matrix peut declencher
     * un GC. Apres l'alloc, plus aucune allocation OWL. */
    vm_push(a);
    vm_push(b);
    ObjMatrix* res = obj_new_matrix(rows, cols);
    vm_pop();
    vm_pop();

    const double* ad = aMat ? AS_MATRIX(a)->data : NULL;
    const double* bd = bMat ? AS_MATRIX(b)->data : NULL;
    double sa = aMat ? 0.0 : AS_NUMBER(a);   /* scalaire coerce entier->reel */
    double sb = bMat ? 0.0 : AS_NUMBER(b);
    for (int i = 0; i < n; i++) {
        double x = aMat ? ad[i] : sa;
        double y = bMat ? bd[i] : sb;
        double r;
        switch (op) {
            case '+': r = x + y; break;
            case '-': r = x - y; break;
            case '*': r = x * y; break;   /* Hadamard ; overflow -> Inf IEEE */
            case '/': r = x / y; break;   /* diviseur nul deja ecarte ci-dessus */
            default:  r = 0.0; break;
        }
        res->data[i] = r;
    }
    *out = OBJ_VAL(res);
    return MAT_ARITH_OK;
}

/* Copie les valeurs par defaut d'une classe dans les champs d'une instance.
 * Les valeurs mutables (tableaux, tableaux associatifs, collections, sous-
 * instances, json) sont deep-clonees : sans ca toutes les instances d'une
 * meme classe partageraient le meme objet de default (un Ajoute sur l'une
 * impacterait les autres). Les scalaires et chaines (immutables, internees)
 * sont partages tels quels. */
static void instance_copy_defaults(Table* from, ObjInstance* instance) {
    for (int i = 0; i < from->capacity; i++) {
        Entry* e = &from->entries[i];
        if (e->key == NULL) continue;
        /*   : push cloned avant table_set qui peut grow
         * → GC → cloned (deep-clone fresh) collecte → instance recoit
         * poison pour ce field (tableau membre vide → poison non-tableau). */
        Value cloned = value_deep_clone(e->value);
        if (IS_OBJ(cloned)) vm_push(cloned);
        table_set(&instance->fields, e->key, cloned);
        if (IS_OBJ(cloned)) vm_pop();
    }
}

/* Flexible string matching used by OP_STRING_MATCH (operators ~=, ~~, [=, =],
 * [=], [~, ~], [~], [~~, ~~]). */
static bool owl_string_match(const char* a, int la, const char* b, int lb,
                             int kind, bool ci, bool ai) {
    char* ba = (char*)a;
    char* bb = (char*)b;
    char* ownA = NULL;
    char* ownB = NULL;
    if (ai) {
        ownA = (char*)malloc((size_t)la + 1);
        ownB = (char*)malloc((size_t)lb + 1);
        la = scanner_strip_accents(a, la, ownA, la + 1);
        lb = scanner_strip_accents(b, lb, ownB, lb + 1);
        ba = ownA;
        bb = ownB;
    }
    bool result = false;
    if (ci) {
        char* la2 = (char*)malloc((size_t)la + 1);
        char* lb2 = (char*)malloc((size_t)lb + 1);
        for (int i = 0; i < la; i++) la2[i] = (char)tolower((unsigned char)ba[i]);
        for (int i = 0; i < lb; i++) lb2[i] = (char)tolower((unsigned char)bb[i]);
        la2[la] = 0; lb2[lb] = 0;
        ba = la2;
        bb = lb2;
    }
    switch (kind) {
        case 0: /* LIKE / equality */
            result = (la == lb) && memcmp(ba, bb, (size_t)la) == 0;
            break;
        case 1: /* STARTS WITH */
            result = (la >= lb) && memcmp(ba, bb, (size_t)lb) == 0;
            break;
        case 2: /* ENDS WITH */
            result = (la >= lb) && memcmp(ba + (la - lb), bb, (size_t)lb) == 0;
            break;
        case 3: { /* CONTAINS */
            if (lb == 0) { result = true; break; }
            if (la < lb) { result = false; break; }
            for (int i = 0; i <= la - lb; i++) {
                if (memcmp(ba + i, bb, (size_t)lb) == 0) { result = true; break; }
            }
            break;
        }
    }
    if (ci) { free((void*)ba); free((void*)bb); }
    if (ai) { free(ownA); free(ownB); }
    return result;
}

ObjString* value_to_string(Value value) {
    char buf[64];
    switch (value.type) {
        case VAL_BOOL:
            return obj_copy_string(AS_BOOL(value) ? "1" : "0", 1);
        case VAL_NIL:
            /* Canon : Null en context chaîne = "". Cf §"Null
             * et les valeurs numériques" + §"Null et les variants". */
            return obj_copy_string("", 0);
        case VAL_INTEGER:
            snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(value));
            return obj_copy_string(buf, (int)strlen(buf));
        case VAL_REAL: {
            /* Canonical shortest round-trippable form. */
            int n = owl_real_to_str(AS_REAL(value), buf, sizeof(buf));
            return obj_copy_string(buf, n);
        }
        case VAL_CURRENCY: {
            char cbuf[48];
            int n = currency_to_str(AS_CURRENCY(value), cbuf, sizeof(cbuf));
            return obj_copy_string(cbuf, n);
        }
        case VAL_OBJ:
            if (IS_NUMERIC(value)) {
                /* Forme décimale plate complète (peut dépasser un buf fixe pour
                 * un bignum) -> allocation mpdecimal puis copie exacte. */
                char* s = numeric_format_alloc(value, "f");
                if (s == NULL) return obj_copy_string("0", 1);
                ObjString* r = obj_copy_string(s, (int)strlen(s));
                owl_numeric_free_str(s);
                return r;
            }
            if (IS_STRING(value)) return AS_STRING(value);
            if (IS_DATE(value)) {
                ObjDate* d = AS_DATE(value);
                snprintf(buf, sizeof(buf), "%04d%02d%02d", d->year, d->month, d->day);
                return obj_copy_string(buf, (int)strlen(buf));
            }
            if (IS_TIME(value)) {
                ObjTime* t = AS_TIME(value);
                snprintf(buf, sizeof(buf), "%02d%02d%02d%02d", t->hour, t->minute, t->second, t->cs);
                return obj_copy_string(buf, (int)strlen(buf));
            }
            if (IS_DATETIME(value)) {
                ObjDateTime* dt = AS_DATETIME(value);
                /* canonical: AAAAMMJJHHMMSSFFF (17 chars, ms sur 3) */
                snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d%03d",
                         dt->year, dt->month, dt->day,
                         dt->hour, dt->minute, dt->second, dt->ms);
                return obj_copy_string(buf, (int)strlen(buf));
            }
            if (IS_DURATION(value)) {
                ObjDuration* dur = AS_DURATION(value);
                int64_t ms = dur->totalMs;
                int neg = ms < 0; if (neg) ms = -ms;
                int64_t secs = ms / 1000;
                int64_t days = secs / 86400;
                int h = (int)((secs % 86400) / 3600);
                int m = (int)((secs % 3600) / 60);
                int s = (int)(secs % 60);
                if (days > 0)
                    snprintf(buf, sizeof(buf), "%s%lld j %02d:%02d:%02d",
                             neg?"-":"", (long long)days, h, m, s);
                else
                    snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d",
                             neg?"-":"", h, m, s);
                return obj_copy_string(buf, (int)strlen(buf));
            }
            if (IS_UI_CONTROL_REF(value)) {
                /*  — symetrise IS_UI_CONTROL ci-dessous pour le
                 * modele proxy. Un nom de widget DSL nu lu en
                 * contexte chaine (concat `SAI & "x"`, coerce chaine,
                 * Trace) resout sa propriete par defaut. On delegue au
                 * resolveur scalaire (resout ..Valeur via window/etat
                 * stack), puis on caste le scalaire obtenu. defaultProperty
                 * NULL / ref non resolu -> resolveur rend NIL ou le ref :
                 * fallback chaine vide pour ne pas boucler. */
                Value resolved = vm_ui_ref_default_resolver(value);
                if (!IS_UI_CONTROL_REF(resolved)) {
                    return value_to_string(resolved);
                }
                return obj_copy_string("", 0);
            }
            if (IS_UI_CONTROL(value)) {
                /* DSL Fenetre v1 (Phase E.X) : coercion implicite canon.
                 * 'User est une chaine = saiUser' lit la propriete par
                 * defaut du champ — pour Saisie = Valeur, pour Libelle/
                 * Bouton = Libelle, pour Fenetre = Titre. C'est
                 * l'equivalent runtime de '..Valeur'. */
                Value v = owl_ui_control_get_prop(AS_UI_CONTROL(value), "Valeur");
                if (IS_STRING(v)) return AS_STRING(v);
                if (IS_NIL(v))    return obj_copy_string("", 0);
                /* Fallback — recurse vers les types primitifs. */
                return value_to_string(v);
            }
            if (IS_BUFFER(value)) {
                /* Canon conformance: a Buffer used in a string context casts
                 * to its byte content (`Trace(b)`,
                 * `s + b`, `"prefix:" + b` all produce the buffer content).
                 * OWL is UTF-8 native -- no transcoding, just copy bytes.
                 * Symetric with __BufferVersChaine (stdlib_string.c:3309)
                 * and print_value_to (stdlib_console.c:40). */
                ObjBuffer* b = AS_BUFFER(value);
                return obj_copy_string((const char*)b->bytes, b->length);
            }
            /*  lot C : ObjMultilingue en contexte chaine resout
             * sur la langue courante runtime (hardcodee "fr-FR" v1). Couvre
             * Trace, concat ("prefix " + m), passage a une native qui
             * traverse value_to_string. La logique de fallback langue
             * source est dans multilingue_resolve. */
            if (IS_MULTILINGUE(value)) {
                Value resolved = multilingue_resolve(AS_MULTILINGUE(value),
                                                     owl_i18n_langue_courante(),
                                                     owl_i18n_langue_source());
                if (IS_STRING(resolved)) return AS_STRING(resolved);
                return obj_copy_string("", 0);
            }
            /* UUID — coercion implicite vers forme canonique 36 chars
             * `xxxxxxxx-xxxx-Vxxx-yxxx-xxxxxxxxxxxx` (V = 4 ou 7 selon version).
             * Délégué à stdlib_uuid pour garder la connaissance du field
             * privé `_bytes` dans le module métier. Concerne Trace, concat
             * "prefix " + u, affectation `s est une chaine = u`, et toute
             * autre coercion implicite via value_to_string. */
            if (owl_value_is_uuid(value)) {
                ObjString* s = owl_uuid_format_canonical(value);
                if (s != NULL) return s;
            }
            /*  — un membre d'énumération en contexte chaîne déballe sa
             * valeur sous-jacente (comme fn_entier_coerce le fait déjà pour le
             * contexte entier). Couvre l'affectation `s est une chaîne =
             * Enum.membre`, la concaténation `"x=" + Enum.membre` et Trace.
             * Détection : instance dont le type global est CLASS_KIND_ENUM.
             * Les accès explicites ..Valeur/..Nom, les variables typées enum et
             * la comparaison de membres passent par d'autres chemins et restent
             * intacts. Combinaisons (CLASS_KIND_COMBINATION) hors-scope, comme
             * pour la coercion entier. */
            if (IS_INSTANCE(value)) {
                ObjInstance* inst = AS_INSTANCE(value);
                if (inst->structName != NULL) {
                    Value classVal;
                    if (table_get(&vm.globals, inst->structName, &classVal)
                        && IS_CLASS(classVal)
                        && AS_CLASS(classVal)->kind == CLASS_KIND_ENUM) {
                        ObjString* valKey = obj_copy_string("Valeur", 6);
                        Value extracted;
                        if (table_get(&inst->fields, valKey, &extracted))
                            return value_to_string(extracted);
                    }
                }
            }
            return obj_copy_string("<object>", 8);
    }
    return obj_copy_string("", 0);
}

/* ================================================================
 * THE MAIN EXECUTION LOOP
 * ================================================================ */

/* g_run_exit_frame / g_run_reentrant_boundary : definis plus haut (avant la
 * section CALL) — la branche native de call_value et exc_top_handler_in_scope()
 * en ont besoin pour statuer sur la frontiere reentrante. */

/* Build a nested N-D array structure for OP_ARRAY_PRESIZE_ND.
 * Recursive: outer has dims[0] sub-arrays, each with dims[1..N-1]. Inner = NIL.
 * GC-safe: each intermediate array is briefly stack-protected. */
static ObjArray* build_nested_array(uint8_t* dims, int dimCount) {
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));  /* GC root */
    if (dimCount == 1) {
        for (int i = 0; i < dims[0]; i++) {
            value_array_write(&arr->elements, NIL_VAL);
        }
    } else {
        for (int i = 0; i < dims[0]; i++) {
            ObjArray* sub = build_nested_array(dims + 1, dimCount - 1);
            obj_array_push(arr, OBJ_VAL(sub));
            vm_pop(); /* sub — was pushed by the recursive call */
        }
    }
    /* arr stays on stack for the caller */
    return arr;
}

/*  — Helpers prop ..Groupe cote champ.
 *
 * Lecture : `champ..Groupe` retourne un nouvel ObjArray (non-group)
 * contenant tous les Groupes globaux (ObjArray is_group=true) dont
 * `elements` referencent ce ObjUiControl (egalite pointeur).
 *
 * Ecriture : `champ..Groupe = [GR_X, GR_Y]` recompose l'appartenance.
 * D'abord on retire le control de tous les Groupes globaux le
 * referencant, puis on l'ajoute (sans doublon) aux Groupes mentionnes
 * dans la valeur (qui doit etre un ObjArray d'ObjArray is_group).
 *
 * Le parcours iterre vm.globals (Table open-addressing) en filtrant
 * les entries non-NULL. Performance OK : N globaux est petit en
 * pratique (dizaines au pire). */
static ObjArray* vm_collect_groups_containing(ObjUiControl* ctrl) {
    ObjArray* result = obj_new_array();
    /* GC root le temps de remplir */
    vm_push(OBJ_VAL(result));
    for (int i = 0; i < vm.globals.capacity; i++) {
        Entry* e = &vm.globals.entries[i];
        if (e->key == NULL) continue;
        if (!IS_ARRAY(e->value)) continue;
        ObjArray* gr = AS_ARRAY(e->value);
        if (!gr->is_group) continue;
        /* Le Groupe contient-il ce control ? Test pointeur. */
        for (int j = 0; j < gr->elements.count; j++) {
            Value m = gr->elements.values[j];
            if (IS_UI_CONTROL(m) && AS_UI_CONTROL(m) == ctrl) {
                obj_array_push(result, OBJ_VAL(gr));
                break;
            }
        }
    }
    vm_pop();
    return result;
}

/*  — pendant pour ObjUiControlRef. Le test pointeur sur ref
 * est stable (le ref est interne au compile-time, jamais recree). */
static ObjArray* vm_collect_groups_containing_ref(ObjUiControlRef* ref) {
    ObjArray* result = obj_new_array();
    vm_push(OBJ_VAL(result));
    for (int i = 0; i < vm.globals.capacity; i++) {
        Entry* e = &vm.globals.entries[i];
        if (e->key == NULL) continue;
        if (!IS_ARRAY(e->value)) continue;
        ObjArray* gr = AS_ARRAY(e->value);
        if (!gr->is_group) continue;
        for (int j = 0; j < gr->elements.count; j++) {
            Value m = gr->elements.values[j];
            if (IS_UI_CONTROL_REF(m) && AS_UI_CONTROL_REF(m) == ref) {
                obj_array_push(result, OBJ_VAL(gr));
                break;
            }
        }
    }
    vm_pop();
    return result;
}

/* Retire toutes les occurrences de `ctrl` des `elements` du Groupe `gr`.
 * Decale les elements pour maintenir compactness. */
static void vm_group_remove_ctrl(ObjArray* gr, ObjUiControl* ctrl) {
    int w = 0;
    for (int r = 0; r < gr->elements.count; r++) {
        Value v = gr->elements.values[r];
        bool drop = IS_UI_CONTROL(v) && AS_UI_CONTROL(v) == ctrl;
        if (!drop) {
            if (w != r) gr->elements.values[w] = v;
            w++;
        }
    }
    gr->elements.count = w;
}

/* Ajoute `ctrl` aux `elements` du Groupe `gr` si absent (idempotent). */
static void vm_group_add_ctrl(ObjArray* gr, ObjUiControl* ctrl) {
    for (int i = 0; i < gr->elements.count; i++) {
        Value v = gr->elements.values[i];
        if (IS_UI_CONTROL(v) && AS_UI_CONTROL(v) == ctrl) return; /* deja la */
    }
    obj_array_push(gr, OBJ_VAL(ctrl));
}

/*  — Modele C : resolution d'un ObjUiControlRef vers
 * son ObjUiControlInstance dans l'instance de fenetre top-of-stack.
 *
 * Cas :
 *   - vm.window_stack vide -> soft error E_RT_UI_FIELD_NO_WINDOW_OPEN (le
 *     programme continue, ErreurDetectee passe a Vrai). Le test user
 *     "Si ErreurDetectee Alors ..." reste fonctionnel.
 *   - instance trouvee mais le champ n'y est pas (decl ne matche aucun
 *     control de l'instance) -> soft error E_RT_UI_FIELD_NOT_IN_WINDOW.
 *   - resolution OK -> retourne l'ObjUiControlInstance*.
 *
 * Lot 1  : aucune instance n'est encore posee par fn_ouvre
 * (build_window cree toujours des ObjUiControl globaux), donc tous les
 * acces resolvent NULL et soft-erreurent. Lot 2 active la materialisation
 * d'instance dans fn_ouvre/build_window pour que la resolution reussisse. */
ObjUiControlInstance* resolve_ui_ref(ObjUiControlRef* ref) {
    /*  lot D — resolution etendue : window_stack puis etat_stack.
     * Un proxy ObjUiControlRef pose au compile (par window_parse_subfield ou
     * par etat_parse_block_label/image) est resolvable PARTOUT a l'compile,
     * mais sa resolution runtime depend du contexte effectif : une fenetre
     * ouverte ou un Etat en cours d'impression. */
    /*  — ref sans nom = proxy "null widget" pose par OP_MOIMEME en
     * cas de stack event_sender vide. L'erreur a deja ete emise par OP_MOIMEME ;
     * on retourne silencieusement NULL pour que OP_STRUCT_GET push NIL sans
     * recouvrir le message d'erreur deja pose. */
    if (ref == NULL || ref->name == NULL) return NULL;
    const char* refName = ref->name->chars;
    int         refLen  = ref->name->length;

    /* (1) Tentative cote fenetre top-of-stack (chemin historique). */
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst != NULL) {
        for (int i = 0; i < inst->control_count; i++) {
            ObjUiControlInstance* uici = inst->controls[i];
            if (!uici || !uici->decl || !uici->decl->name) continue;
            char buf[256];
            int blen = scanner_strip_accents(uici->decl->name,
                                              (int)strlen(uici->decl->name),
                                              buf, sizeof(buf));
            if (blen == refLen
                && strncasecmp(buf, refName, (size_t)blen) == 0) {
                if (owl_trace_ui_enabled()) {
                    const char* wn = (inst->decl && inst->decl->name)
                                     ? inst->decl->name : "?";
                    fprintf(stderr,
                            "[ui-resolve] %s -> %s/%s widget=%p\n",
                            refName, wn, uici->decl->name,
                            (void*)uici->widget);
                    fflush(stderr);
                }
                return uici;
            }
        }
    }

    /* (2) Repli cote Etat top-of-stack. Necessite que iImprimeEtat ait
     * push une ObjEtatInstance avec ses sous-champs materialises. */
    ObjEtatInstance* einst = vm_etat_stack_top();
    if (einst != NULL) {
        for (int i = 0; i < einst->control_count; i++) {
            ObjUiControlInstance* uici = einst->controls[i];
            if (!uici || !uici->decl || !uici->decl->name) continue;
            char buf[256];
            int blen = scanner_strip_accents(uici->decl->name,
                                              (int)strlen(uici->decl->name),
                                              buf, sizeof(buf));
            if (blen == refLen
                && strncasecmp(buf, refName, (size_t)blen) == 0) {
                return uici;
            }
        }
    }

    /* (3) Aucun contexte resolvant : soft-error coherent. Si aucun des deux
     * stacks n'est ouvert, c'est l'erreur "pas de fenetre ouverte" (le
     * test garde-fou `etat_souschamp_hors_etat` tombe ici quand l'acces se
     * fait au niveau projet, hors iImprimeEtat). Si une fenetre est ouverte
     * mais le champ n'y est pas (et pas non plus dans l'Etat) -> erreur
     * "champ pas dans fenetre" pour preserver le message historique. */
    if (inst == NULL && einst == NULL) {
        if (owl_trace_ui_enabled()) {
            fprintf(stderr,
                    "[ui-resolve] %s -> MISS (no window/etat stack)\n",
                    refName);
            fflush(stderr);
        }
        owl_emit_native_error(E_RT_UI_FIELD_NO_WINDOW_OPEN, "AccesChamp",
                              "name", refName, NULL);
        return NULL;
    }
    const char* winName = (inst && inst->decl && inst->decl->name)
                          ? inst->decl->name : "?";
    if (owl_trace_ui_enabled()) {
        fprintf(stderr,
                "[ui-resolve] %s -> %s/MISS (not in window/etat)\n",
                refName, winName);
        fflush(stderr);
    }
    owl_emit_native_error(E_RT_UI_FIELD_NOT_IN_WINDOW, "AccesChamp",
                          "name", refName,
                          "window", winName,
                          NULL);
    return NULL;
}

/*  — resolveur hook pour owl_instance_default_unwrap. Symetrise la
 * traduction d'ECRITURE `WIDGET = X` -> `WIDGET..defaultProperty = X`
 * (compiler.c) dans le sens LECTURE : un nom de widget DSL nu utilise en
 * contexte scalaire (RHS typee, operande arith/compare, coerce de parametre)
 * se deballe vers sa propriete par defaut (`SAI` -> `SAI..Valeur`).
 *
 * Mecanisme : coercion RUNTIME a l'usage (option b du ticket). Le ref
 * circule tel quel partout ; seul le consommateur scalaire (qui appelle
 * owl_instance_default_unwrap) declenche la resolution. Les natives qui
 * veulent le CONTROLE (TestSimuleSouris, TableSelect, dCopieImageChamp...)
 * recoivent le ref intact : elles ne passent jamais par l'unwrap scalaire.
 * Pas d'ambiguite producteur cote compile.
 *
 * defaultProperty NULL (Table/Interrupteur/ZoneRepetee) : pas de prop par
 * defaut scalaire -> ref rendu inchange. S'il atteint ensuite un operateur
 * scalaire, l'erreur "operandes nombres/chaines" historique se produit
 * (pas de resolution magique pour un widget non scalaire).
 *
 * Pas de fenetre ouverte : resolve_ui_ref pose le soft-error standard ;
 * on rend NIL (coherent avec OP_STRUCT_GET..Valeur sur ref non resolu). */
/*  — propriete par defaut d'un kind de control, par lookup dans la
 * table CONTROL_KINDS (source unique de verite). Saisie -> "Valeur",
 * Libelle/Bouton -> "Libelle", Image -> "Image". NULL pour les kinds sans
 * prop par defaut scalaire (Table/ZoneRepetee/Interrupteur). */
static const char* vm_kind_default_property(int kind) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    for (int i = 0; tbl[i].nameFR != NULL; i++) {
        if (!tbl[i].is_widget) continue;
        OwlControlKind k;
        if (owl_control_kind_of_info(&tbl[i], &k) && (int)k == kind) {
            return tbl[i].defaultProperty;
        }
    }
    return NULL;
}

static Value vm_ui_ref_default_resolver(Value v) {
    /*  — cellule ZR headless materialisee (ObjUiControl widget==NULL).
     * En contexte scalaire (concat, =, args natifs dbg*, RENVOYER) elle se
     * deballe vers sa prop par defaut, comme un ObjUiControlRef. Sans ca un
     * ObjUiControl brut fuit dans une comparaison/concat et casse l'egalite
     * (cf RISQUE n1 du plan). On passe par le meme chokepoint
     * owl_instance_default_unwrap que tous les consommateurs scalaires. */
    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uic = AS_UI_CONTROL(v);
        if (uic->widget != NULL) return v;  /* GUI : pas notre cas */
        const char* defProp = vm_kind_default_property(uic->kind);
        if (defProp == NULL) return v;      /* pas de prop scalaire par defaut */
        Value out;
        if (obj_ui_control_headless_get(uic, defProp, &out)) return out;
        /* prop par defaut jamais ecrite : fallback default catalogue (ex
         * Saisie..Valeur jamais set -> chaine vide typee). */
        return headless_default_with_alias(uic, defProp);
    }
    if (!IS_UI_CONTROL_REF(v)) return v;
    ObjUiControlRef* ref = AS_UI_CONTROL_REF(v);
    if (ref == NULL || ref->name == NULL) return v;

    const OwlControlKindInfo* info =
        vm_find_widget_info_by_name(ref->name->chars);
    if (info == NULL || info->defaultProperty == NULL) {
        /* defaultProperty NULL ou widget introuvable : pas de resolution
         * scalaire -> ref inchange (cf doc ci-dessus). */
        return v;
    }

    ObjUiControlInstance* uici = resolve_ui_ref(ref);
    if (uici == NULL) {
        /* soft-error deja emis par resolve_ui_ref (fenetre fermee, champ
         * absent). On rend NIL : symetrique du push NIL d'OP_STRUCT_GET. */
        return NIL_VAL;
    }

    /* Lecture de la prop par defaut : Qt-first (props live) puis store
     * headless, exactement comme la branche IS_UI_CONTROL_REF d'OP_STRUCT_
     * GET. On ne reimplemente pas le default catalog : si rien n'est trouve
     * on rend NIL (les coerces scalaires aval normalisent NIL en 0 / ""). */
    Value out;
    if (try_live_qt_prop_for_uici(uici, info->defaultProperty, &out)) {
        return out;
    }
    if (obj_ui_control_instance_headless_get(uici, info->defaultProperty,
                                             &out)) {
        return out;
    }
    return NIL_VAL;
}

static InterpretResult run(void) {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];
    int exitFrame = g_run_exit_frame;
    bool reentrantBoundary = g_run_reentrant_boundary;
    /*  : nouvelle unite d'execution — aucune fatale-VM en vol. Evite
     * qu'un marqueur laisse arme par un chemin sans pont (invoke methode,
     * vm_call_reentrant qui `return` directement) bloque un rattrapage legitime
     * au prochain OP_CALL. */
    g_rt_fatal_vm = false;

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
/*  — index de constante sur OWL_CONST_IDX_BYTES octets, big-endian,
 * comme les offsets de saut. La largeur est la meme des deux cotes : ce que
 * chunk_opcode_shape() declare, emit_const_op() ecrit et READ_CONST_IDX() lit. */
#define READ_CONST_IDX() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk->constants.values[READ_CONST_IDX()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

#define BINARY_OP(intOp, realOp) \
    do { \
        Value b = vm_pop(); \
        Value a = vm_pop(); \
        if (IS_BOOL(a)) a = INTEGER_VAL(AS_BOOL(a) ? 1 : 0); \
        if (IS_BOOL(b)) b = INTEGER_VAL(AS_BOOL(b) ? 1 : 0); \
        if (IS_NIL(a)) a = INTEGER_VAL(0); \
        if (IS_NIL(b)) b = INTEGER_VAL(0); \
        if (IS_INTEGER(a) && IS_INTEGER(b)) { \
            vm_push(INTEGER_VAL(AS_INTEGER(a) intOp AS_INTEGER(b))); \
        } else if (IS_NUMBER(a) && IS_NUMBER(b)) { \
            vm_push(REAL_VAL(AS_NUMBER(a) realOp AS_NUMBER(b))); \
        } else { \
            owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS, NULL); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while (false)

#define COMPARE_OP(op) \
    do { \
        Value b = vm_pop(); \
        Value a = vm_pop(); \
        /* Null cannot be used with ordered comparisons (language rule) */ \
        if (IS_NIL(a) || IS_NIL(b)) { \
            owl_emit_runtime_error(E_RT_NIL_ORDERED_CMP, NULL); \
            OWL_RT_RECOVER_DISPATCH(); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        /* Implicit conversions: Bool->Int */ \
        if (IS_BOOL(a)) a = INTEGER_VAL(AS_BOOL(a) ? 1 : 0); \
        if (IS_BOOL(b)) b = INTEGER_VAL(AS_BOOL(b) ? 1 : 0); \
        /*  — widget DSL nu en operande de comparaison ordonnee : \
         * deballe vers sa propriete par defaut (resout ..Valeur). */ \
        if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a); \
        if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b); \
        if (IS_INTEGER(a) && IS_INTEGER(b)) { \
            vm_push(BOOL_VAL(AS_INTEGER(a) op AS_INTEGER(b))); \
        } else if ((IS_NUMERIC(a) || IS_NUMERIC(b)) && IS_NUMBER(a) && IS_NUMBER(b)) { \
            /* Bignum : comparaison décimale exacte (jamais via double). */ \
            vm_push(BOOL_VAL(numeric_cmp(a, b) op 0)); \
        } else if (IS_NUMBER(a) && IS_NUMBER(b)) { \
            vm_push(BOOL_VAL(AS_NUMBER(a) op AS_NUMBER(b))); \
        } else if (IS_STRING(a) && IS_STRING(b)) { \
            int cmp = strcmp(AS_CSTRING(a), AS_CSTRING(b)); \
            vm_push(BOOL_VAL(cmp op 0)); \
        } else if ((IS_DATE(a) || IS_DATETIME(a)) && (IS_DATE(b) || IS_DATETIME(b))) { \
            int64_t ma = 0, mb = 0; \
            owl_value_to_absolute_ms(a, &ma); \
            owl_value_to_absolute_ms(b, &mb); \
            vm_push(BOOL_VAL(ma op mb)); \
        } else if (IS_DURATION(a) && IS_DURATION(b)) { \
            vm_push(BOOL_VAL(AS_DURATION(a)->totalMs op AS_DURATION(b)->totalMs)); \
        } else if (IS_DURATION(a) && IS_INTEGER(b)) { \
            vm_push(BOOL_VAL(AS_DURATION(a)->totalMs op AS_INTEGER(b))); \
        } else if (IS_INTEGER(a) && IS_DURATION(b)) { \
            vm_push(BOOL_VAL((int64_t)AS_INTEGER(a) op AS_DURATION(b)->totalMs)); \
        } else { \
            owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS_OR_STRINGS, NULL); \
            OWL_RT_RECOVER_DISPATCH(); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while (false)

/* ================================================================
 * Dispatch macros
 *
 * Mode dual : `switch` classique (debug, MSVC, OWL_DISABLE_COMPUTED_GOTO)
 * vs computed-goto (GCC/clang release). Les corps d'opcodes utilisent
 * les memes macros VM_CASE / DISPATCH dans les deux modes — seul le
 * cablage diffère :
 *
 *   - Mode switch : VM_CASE(OP_X) -> `case OP_X:` ; DISPATCH() -> `break;`
 *     La boucle externe est un `for(;;) { PRE_OPCODE(); switch(...) }`.
 *   - Mode goto   : VM_CASE(OP_X) -> `L_OP_X:` ; DISPATCH() -> saut
 *     direct via `dispatch_table[READ_BYTE()]`. PRE_OPCODE inline dans
 *     chaque DISPATCH. La boucle est remplacee par un saut initial
 *     puis chaque opcode rebondit vers le suivant.
 *
 * `instruction` est declaree hors boucle pour rester accessible aux
 * fall-through BIT_* qui consultent la valeur enum apres le saut. */
#define PRE_OPCODE_TRACE() \
    do { \
        printf("          "); \
        for (Value* _slot = vm.stack; _slot < vm.stackTop; _slot++) { \
            printf("[ "); value_print(*_slot); printf(" ]"); \
        } \
        printf("\n"); \
        debug_disassemble_instruction(frame->function->chunk, \
            (int)(frame->ip - frame->function->chunk->code)); \
    } while (0)

#ifdef DEBUG_TRACE_EXECUTION
#  define PRE_OPCODE_TRACE_MAYBE() PRE_OPCODE_TRACE()
#else
#  define PRE_OPCODE_TRACE_MAYBE() ((void)0)
#endif

#define PRE_OPCODE_HOOK() \
    do { \
        if (owl_debug_mode() != DEBUG_OFF) { \
            int _offset = (int)(frame->ip - frame->function->chunk->code); \
            int _line = frame->function->chunk->lines[_offset]; \
            const char* _file = frame->function->filename \
                ? frame->function->filename->chars : NULL; \
            owl_debug_hook(_file, _line); \
        } \
    } while (0)

#define PRE_OPCODE() do { PRE_OPCODE_TRACE_MAYBE(); PRE_OPCODE_HOOK(); } while (0)

#if (defined(__GNUC__) || defined(__clang__)) && !defined(OWL_DISABLE_COMPUTED_GOTO)
#  define OWL_COMPUTED_GOTO 1
#endif

#ifdef OWL_COMPUTED_GOTO
#  define VM_CASE(op) L_##op
#  define DISPATCH() do { PRE_OPCODE(); instruction = READ_BYTE(); goto *dispatch_table[instruction]; } while (0)
#else
#  define VM_CASE(op) case op
#  define DISPATCH() break
#endif

/*  — pont de rattrapage QUAND EXCEPTION apres l'emission d'une erreur
 * runtime de niveau UTILISATEUR (arithmetique/operandes, index mauvais-type ou
 * hors-borne, propriete inconnue, appel d'un non-appelable...). Si un handler
 * englobe le site, transfere le controle et reprend le dispatch ; sinon on
 * retombe sur la propagation (return INTERPRET_RUNTIME_ERROR du site). Meme
 * contrat que le pont historique DIV_ZERO : a n'employer QUE dans
 * run() (frame / DISPATCH / exceptionCount en scope).
 *
 * Les fatales-VM (E_RT_STACK_OVERFLOW, E_RT_UNKNOWN_OPCODE,
 * E_RT_EXCEPTION_STACK_OVERFLOW, terminus E_RT_UNHANDLED_EXCEPTION) ne passent
 * JAMAIS par ce pont. Seul le debordement de pile pouvait fuiter ici (retour
 * `false` de call_value au site OP_CALL) : la garde `g_rt_fatal_vm` l'exclut
 * explicitement, efface le marqueur et laisse la fatale se propager.
 *
 * PAS de wrapper do{}while(0) : en dispatch switch DISPATCH() vaut
 * `break`, qui doit cibler la boucle de dispatch, pas un do-while parasite. Le
 * pont s'aligne donc sur l'idiome inline preexistant (nested `if`, cf. les
 * sites BINOP dates/vecteurs) et reste correct en computed-goto ET en switch.
 * Toujours employe comme instruction isolee suivie de
 * `return INTERPRET_RUNTIME_ERROR;`, entre accolades. */
/*  — propagation d'une exception en vol a travers une frontiere
 * reentrante. Le handler qui rattrape vit chez un ANCETRE (au-dela d'exitFrame) :
 * on ne le consomme pas ici. On deroule les frames imbriquees jusqu'a la
 * frontiere (vm.frames[exitFrame] = frame du callback pousse par vm_call_reentrant,
 * ses slots = sommet de pile juste avant l'appel reentrant) et on laisse
 * l'exception ARMEE (currentException/exceptionCount intacts) + throwPending, pour
 * que la native appelante rende la main et que le pont OP_CALL du run() englobant
 * consomme (ou re-propage) a son tour. N'imprime rien, ne fatalise pas. Employe
 * uniquement quand exitFrame > 0 (frontiere reentrante active) : au top-level
 * exitFrame == 0 et tout handler est en scope, on ne propage jamais. */
#define OWL_RT_PROPAGATE_REENTRANT() do { \
        vm.stackTop = vm.frames[exitFrame].slots; \
        vm.frameCount = exitFrame; \
        throwPending = true; \
    } while (0)

#define OWL_RT_RECOVER_DISPATCH() \
    if (g_rt_fatal_vm) { \
        g_rt_fatal_vm = false; \
    } else if (exceptionCount > 0 && currentException != NULL) { \
        if (exc_top_handler_in_scope(exitFrame)) { \
            if (handle_runtime_exception()) { \
                frame = &vm.frames[vm.frameCount - 1]; \
                DISPATCH(); \
            } \
        } else { \
            OWL_RT_PROPAGATE_REENTRANT(); \
        } \
    }

    uint8_t instruction;
    (void)instruction; /* mode goto : utilise dans DISPATCH, pas dans le switch */

    /*  — auto-vivification JSON en ecriture : les variantes
     * OP_*_GET_OR_CREATE_* positionnent ce drapeau (0=lecture pure, 1=creer un
     * objet si absent, 2=creer un tableau) puis retombent dans le corps partage
     * d'OP_STRUCT_GET / OP_ARRAY_GET. Declare au scope fonction : les gotos
     * d'entree ne doivent pas sauter par-dessus une initialisation. */
    int opAutoVivKind = 0;
    (void)opAutoVivKind;

#ifdef OWL_COMPUTED_GOTO
    /*  — table dispatch GCC/clang. Indexee par OpCode (uint8_t).
     * &&label : extension GNU "Labels as Values". Slots inutilises pointent
     * vers L_BAD_OP (= E_RT_UNKNOWN_OPCODE). _Static_assert verifie qu'on
     * cable bien OP_COUNT entrees ; le designated-init [OP_X] = ... permet
     * d'ajouter de nouveaux opcodes sans casser l'ordre.
     *
     * Range init `[0 ... OP_COUNT - 1] = &&L_BAD_OP` POIS surcharges
     * `[OP_X] = ...` est intentionnel : clang -Winitializer-overrides
     * hurle (-Werror), on le silence localement. */
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Winitializer-overrides"
#  endif
    static void* const dispatch_table[OP_COUNT] = {
        [0 ... OP_COUNT - 1] = &&L_BAD_OP,
        [OP_ADD] = &&L_OP_ADD,
        [OP_AND_FULL] = &&L_OP_AND_FULL,
        [OP_APPEND_IN_PLACE] = &&L_OP_APPEND_IN_PLACE,
        [OP_APPEND_IN_PLACE_GLOBAL] = &&L_OP_APPEND_IN_PLACE_GLOBAL,
        [OP_ARRAY] = &&L_OP_ARRAY,
        [OP_ARRAY_FILL_DEFAULT] = &&L_OP_ARRAY_FILL_DEFAULT,
        [OP_ARRAY_FILL_STRUCT] = &&L_OP_ARRAY_FILL_STRUCT,
        [OP_ARRAY_GET] = &&L_OP_ARRAY_GET,
        [OP_ARRAY_GET_OR_CREATE_OBJ] = &&L_OP_ARRAY_GET_OR_CREATE_OBJ,
        [OP_ARRAY_GET_OR_CREATE_ARR] = &&L_OP_ARRAY_GET_OR_CREATE_ARR,
        [OP_TABLE_CELL_DESC] = &&L_OP_TABLE_CELL_DESC,
        [OP_ARRAY_GET_2D] = &&L_OP_ARRAY_GET_2D,
        [OP_ARRAY_GROWTH] = &&L_OP_ARRAY_GROWTH,
        [OP_ARRAY_LENGTH] = &&L_OP_ARRAY_LENGTH,
        [OP_ARRAY_MARK_GROUP] = &&L_OP_ARRAY_MARK_GROUP,
        [OP_ARRAY_PACK] = &&L_OP_ARRAY_PACK,
        [OP_ARRAY_PRESIZE] = &&L_OP_ARRAY_PRESIZE,
        [OP_ARRAY_PRESIZE_ND] = &&L_OP_ARRAY_PRESIZE_ND,
        [OP_ARRAY_SET] = &&L_OP_ARRAY_SET,
        [OP_ARRAY_SET_2D] = &&L_OP_ARRAY_SET_2D,
        [OP_ARRAY_TYPE_STRUCT] = &&L_OP_ARRAY_TYPE_STRUCT,
        [OP_ARRAY_UNPACK] = &&L_OP_ARRAY_UNPACK,
        [OP_BIT_AND] = &&L_OP_BIT_AND,
        [OP_BIT_NOT] = &&L_OP_BIT_NOT,
        [OP_BIT_OR] = &&L_OP_BIT_OR,
        [OP_BIT_SHL] = &&L_OP_BIT_SHL,
        [OP_BIT_SHR] = &&L_OP_BIT_SHR,
        [OP_BIT_XOR] = &&L_OP_BIT_XOR,
        [OP_CALL] = &&L_OP_CALL,
        [OP_CLASS] = &&L_OP_CLASS,
        [OP_CLASS_DECLARE_INTERFACE] = &&L_OP_CLASS_DECLARE_INTERFACE,
        [OP_COLLECTION] = &&L_OP_COLLECTION,
        [OP_CONCAT] = &&L_OP_CONCAT,
        [OP_CONSTANT] = &&L_OP_CONSTANT,
        [OP_DECREMENT] = &&L_OP_DECREMENT,
        [OP_DEEP_COPY] = &&L_OP_DEEP_COPY,
        [OP_DEFINE_GLOBAL] = &&L_OP_DEFINE_GLOBAL,
        [OP_DIVIDE] = &&L_OP_DIVIDE,
        [OP_DUP] = &&L_OP_DUP,
        [OP_END_TRY] = &&L_OP_END_TRY,
        [OP_ENUM_VALUE] = &&L_OP_ENUM_VALUE,
        [OP_EQUAL] = &&L_OP_EQUAL,
        [OP_FALSE] = &&L_OP_FALSE,
        [OP_GET_GLOBAL] = &&L_OP_GET_GLOBAL,
        [OP_GET_GLOBAL_CALLEE] = &&L_OP_GET_GLOBAL_CALLEE,
        [OP_GET_ELEMENT_GLOBAL] = &&L_OP_GET_ELEMENT_GLOBAL,
        [OP_GET_TYPE_GLOBAL] = &&L_OP_GET_TYPE_GLOBAL,
        [OP_GET_LOCAL] = &&L_OP_GET_LOCAL,
        [OP_GET_THIS] = &&L_OP_GET_THIS,
        [OP_MOIMEME] = &&L_OP_MOIMEME,
        [OP_MONCHAMP] = &&L_OP_MONCHAMP,
        [OP_MAFENETRE] = &&L_OP_MAFENETRE,
        [OP_GREATER] = &&L_OP_GREATER,
        [OP_GREATER_EQUAL] = &&L_OP_GREATER_EQUAL,
        [OP_INCREMENT] = &&L_OP_INCREMENT,
        [OP_INDIRECT_FIELD_GET] = &&L_OP_INDIRECT_FIELD_GET,
        [OP_INDIRECT_FIELD_SET] = &&L_OP_INDIRECT_FIELD_SET,
        [OP_INDIRECT_GLOBAL_GET] = &&L_OP_INDIRECT_GLOBAL_GET,
        [OP_INDIRECT_GLOBAL_SET] = &&L_OP_INDIRECT_GLOBAL_SET,
        [OP_INHERIT] = &&L_OP_INHERIT,
        [OP_INVOKE] = &&L_OP_INVOKE,
        [OP_INVOKE_DIRECT] = &&L_OP_INVOKE_DIRECT,
        [OP_INVOKE_SAFE] = &&L_OP_INVOKE_SAFE,
        [OP_INVOKE_SUPER] = &&L_OP_INVOKE_SUPER,
        [OP_IS_CLASS] = &&L_OP_IS_CLASS,
        [OP_ITER_BEGIN] = &&L_OP_ITER_BEGIN,
        [OP_ITER_END] = &&L_OP_ITER_END,
        [OP_ITER_KEY] = &&L_OP_ITER_KEY,
        [OP_ITER_TRACK_KEY] = &&L_OP_ITER_TRACK_KEY,
        [OP_JUMP] = &&L_OP_JUMP,
        [OP_JUMP_IF_FALSE] = &&L_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_NIL] = &&L_OP_JUMP_IF_NIL,
        [OP_JUMP_IF_NOT_NIL] = &&L_OP_JUMP_IF_NOT_NIL,
        [OP_LESS] = &&L_OP_LESS,
        [OP_LESS_EQUAL] = &&L_OP_LESS_EQUAL,
        [OP_LOOP] = &&L_OP_LOOP,
        [OP_MARK_CLASS_KIND] = &&L_OP_MARK_CLASS_KIND,
        [OP_METHOD] = &&L_OP_METHOD,
        [OP_MODULO] = &&L_OP_MODULO,
        [OP_MULTIPLY] = &&L_OP_MULTIPLY,
        [OP_NEGATE] = &&L_OP_NEGATE,
        [OP_NIL] = &&L_OP_NIL,
        [OP_NOT] = &&L_OP_NOT,
        [OP_NOT_EQUAL] = &&L_OP_NOT_EQUAL,
        [OP_OR_FULL] = &&L_OP_OR_FULL,
        [OP_OUT_PARAM_TO_GLOBAL] = &&L_OP_OUT_PARAM_TO_GLOBAL,
        [OP_OUT_PARAM_TO_LOCAL] = &&L_OP_OUT_PARAM_TO_LOCAL,
        [OP_POP] = &&L_OP_POP,
        [OP_POPN] = &&L_OP_POPN,
        [OP_POWER] = &&L_OP_POWER,
        [OP_PRINT] = &&L_OP_PRINT,
        [OP_PROC_SET_HOOK] = &&L_OP_PROC_SET_HOOK,
        [OP_RERAISE] = &&L_OP_RERAISE,
        [OP_EXC_RESTORE] = &&L_OP_EXC_RESTORE,
        [OP_RETURN] = &&L_OP_RETURN,
        [OP_SET_FIELD_THIS] = &&L_OP_SET_FIELD_THIS,
        [OP_SET_GLOBAL] = &&L_OP_SET_GLOBAL,
        [OP_SET_LOCAL] = &&L_OP_SET_LOCAL,
        [OP_STATIC_FIELD] = &&L_OP_STATIC_FIELD,
        [OP_STRING_CHAR_AT] = &&L_OP_STRING_CHAR_AT,
        [OP_BUFFER_BYTE_SET] = &&L_OP_BUFFER_BYTE_SET,
        [OP_STRING_MATCH] = &&L_OP_STRING_MATCH,
        [OP_STRING_SLICE] = &&L_OP_STRING_SLICE,
        [OP_STRING_SLICE_LEN] = &&L_OP_STRING_SLICE_LEN,
        [OP_STRING_SLICE_TO_END] = &&L_OP_STRING_SLICE_TO_END,
        [OP_STRUCT_FIELD_ORDER] = &&L_OP_STRUCT_FIELD_ORDER,
        [OP_STRUCT_GET] = &&L_OP_STRUCT_GET,
        [OP_STRUCT_GET_OR_CREATE_OBJ] = &&L_OP_STRUCT_GET_OR_CREATE_OBJ,
        [OP_STRUCT_GET_OR_CREATE_ARR] = &&L_OP_STRUCT_GET_OR_CREATE_ARR,
        [OP_STRUCT_SET] = &&L_OP_STRUCT_SET,
        [OP_SUBTRACT] = &&L_OP_SUBTRACT,
        [OP_SWAP] = &&L_OP_SWAP,
        [OP_THROW] = &&L_OP_THROW,
        [OP_TO_UNDETECTABLE] = &&L_OP_TO_UNDETECTABLE,
        [OP_TRUE] = &&L_OP_TRUE,
        [OP_TRY] = &&L_OP_TRY,
        [OP_VERIFY_BOOL_COND] = &&L_OP_VERIFY_BOOL_COND,
    };
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  endif
    _Static_assert(sizeof dispatch_table / sizeof dispatch_table[0] == OP_COUNT,
                   "dispatch_table size must equal OP_COUNT");

    DISPATCH(); /* lance la chaine ; chaque opcode rebondit via DISPATCH() */
#else
    for (;;) {
        PRE_OPCODE();
        instruction = READ_BYTE();
        switch (instruction) {
#endif

            /* === Corps des opcodes (commun mode switch / mode goto) ===
             * VM_CASE(OP_X) se resout en `case OP_X:` ou `L_OP_X:` selon
             * OWL_COMPUTED_GOTO ; DISPATCH() en `break;` ou saut indirect.
             * Les labels L_OP_X sont accessibles a l'interieur du switch
             * en mode goto (extension GNU) — gcc autorise un goto qui
             * atterrit dans un switch. */

            VM_CASE(OP_CONSTANT): {
                Value constant = READ_CONSTANT();
                vm_push(constant);
                DISPATCH();
            }

            VM_CASE(OP_NIL):   vm_push(NIL_VAL); DISPATCH();
            VM_CASE(OP_TRUE):  vm_push(BOOL_VAL(true)); DISPATCH();
            VM_CASE(OP_FALSE): vm_push(BOOL_VAL(false)); DISPATCH();

            VM_CASE(OP_ADD): {
                /* IMPORTANT: peek (don't pop) so operands stay rooted on
                 * the VM stack during any intermediate allocation that may
                 * trigger a GC cycle (obj_concat_strings, obj_new_*, etc.).
                 * The string interning table owl_strings is NOT a GC root
                 * (it's swept via table_remove_white), so an operand only
                 * referenced as a C local could be collected mid-op and
                 * cause a use-after-free segfault.
                 * Caller pops the 2 operands and pushes the result via
                 * BINOP_RESULT below. */
                Value a = vm_peek(1);
                Value b = vm_peek(0);
                #define BINOP_RESULT(v) do { \
                    Value _r = (v); vm_pop(); vm_pop(); vm_push(_r); \
                } while (0)
                #define BINOP_ERROR_CODE(code) do { \
                    vm_pop(); vm_pop(); \
                    owl_emit_runtime_error((code), NULL); \
                    OWL_RT_RECOVER_DISPATCH(); \
                    return INTERPRET_RUNTIME_ERROR; \
                } while (0)

                if (IS_CALLABLE(b) && (IS_CALLABLE(a) || IS_NIL(a))) {
                    ObjProcChain* chain = obj_new_proc_chain();
                    if (IS_PROC_CHAIN(a)) {
                        ObjProcChain* src = AS_PROC_CHAIN(a);
                        chain->avant = src->avant;
                        chain->apres = src->apres;
                        for (int i = 0; i < src->count; i++) {
                            obj_proc_chain_add(chain, src->procs[i]);
                        }
                    } else if (!IS_NIL(a)) {
                        obj_proc_chain_add(chain, a);
                    }
                    obj_proc_chain_add(chain, b);
                    BINOP_RESULT(OBJ_VAL(chain));
                    DISPATCH();
                }
                /* Implicit Bool->Int conversion for arithmetic (on C locals
                 * only -- stack values are untouched, still rooting the
                 * originals). */
                if (IS_BOOL(a)) a = INTEGER_VAL(AS_BOOL(a) ? 1 : 0);
                if (IS_BOOL(b)) b = INTEGER_VAL(AS_BOOL(b) ? 1 : 0);
                /*  — scalar unwrap : instance composite avec
                 * defaultProperty (Couleur ...) en arithmetique. Si une
                 * seule operande est composite-with-defaultProperty et
                 * l'autre n'est pas instance, deballer la composite. Le
                 * cas instance + instance reste traite plus bas (Combo
                 * enums bitwise OR). */
                if (IS_INSTANCE(a) && !IS_INSTANCE(b)) {
                    a = owl_instance_default_unwrap(a);
                }
                if (IS_INSTANCE(b) && !IS_INSTANCE(a)) {
                    b = owl_instance_default_unwrap(b);
                }
                /*  — widget DSL nu en operande de `+` (somme ou
                 * concat) : deballe vers sa prop par defaut. Place avant le
                 * dispatch string/numerique pour que `SAI_Num + 1` resolve
                 * 42 et `SAI_Mot + "x"` resolve "ok". */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                /*  — addition differentiable Tenseur : construit un
                 * nouveau Tenseur c = a + b dont le noeud memorise op + parents.
                 * a et b restent peeked (rootes) pendant obj_tensor_binop qui
                 * alloue. Resolue avant le path Vecteur (un Tenseur n'est ni
                 * Vecteur ni scalaire). */
                if (IS_TENSOR(a) || IS_TENSOR(b)) {
                    if (IS_TENSOR(a) && IS_TENSOR(b)) {
                        ObjTensor* tc = obj_tensor_binop(TENSOR_OP_ADD,
                                                         AS_TENSOR(a), AS_TENSOR(b));
                        if (tc != NULL) { BINOP_RESULT(OBJ_VAL(tc)); DISPATCH(); }
                    }
                    vm_pop(); vm_pop();
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS_OR_DATES, NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Vecteur : element-par-element ou
                 * broadcast scalaire. Resolue avant le path string (un Vecteur
                 * n'est ni chaine ni scalaire). */
                if (IS_VECTOR(a) || IS_VECTOR(b)) {
                    Value vout;
                    VecArithStatus vst = vm_vector_arith(a, b, '+', &vout);
                    if (vst == VEC_ARITH_OK) { BINOP_RESULT(vout); DISPATCH(); }
                    vm_pop(); vm_pop();
                    owl_emit_runtime_error(owl_vector_arith_error_code(vst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Matrice (pendant 2D du Vecteur). */
                if (IS_MATRIX(a) || IS_MATRIX(b)) {
                    Value mout;
                    MatArithStatus mst = vm_matrix_arith(a, b, '+', &mout);
                    if (mst == MAT_ARITH_OK) { BINOP_RESULT(mout); DISPATCH(); }
                    vm_pop(); vm_pop();
                    owl_emit_runtime_error(owl_matrix_arith_error_code(mst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /* String path d'abord : NIL passe par value_to_string (= "")
                 * pour respecter le canon. Si on castait NIL -> 0 ici,
                 * "x" + Null produirait "x0" au lieu de "x". */
                if (IS_STRING(a) || IS_STRING(b) ||
                    (IS_BUFFER(a) && IS_BUFFER(b))) {
                    /* String/Buffer concat path. canon : si au moins un
                     * operande est string OU si les deux sont Buffer, la
                     * concat passe par value_to_string (qui caste Buffer ->
                     * bytes UTF-8 depuis le fix Buffer-vers-chaine).
                     *
                     * Volontairement restreint : Buffer + int garde la
                     * semantique numerique canon (l'entier est un byte ajoute
                     * en binaire, pas la string "42"). On ne peut pas
                     * reproduire ca sans aller toucher au layout buffer
                     * mutable -- hors scope, laisse aux paths existants. */
                    /*  : sb push avant value_to_string(a) qui peut
                     * allouer → GC → sb collected (Obj fresh non-rooted).
                     * Idem sa push avant obj_concat_strings qui alloue. */
                    ObjString* sb = value_to_string(b);
                    vm_push(OBJ_VAL(sb));
                    ObjString* sa = value_to_string(a);
                    vm_push(OBJ_VAL(sa));
                    ObjString* concat = obj_concat_strings(sa, sb);
                    vm_pop(); /* sa */
                    vm_pop(); /* sb */
                    BINOP_RESULT(OBJ_VAL(concat));
                    DISPATCH();
                }
                /* Sortis du path string : NIL devient 0 numérique pour les
                 * branches arith ci-dessous (canon : Null = 0). */
                if (IS_NIL(a)) a = INTEGER_VAL(0);
                if (IS_NIL(b)) b = INTEGER_VAL(0);
                if (IS_NUMERIC(a) || IS_NUMERIC(b)) {
                    /* Bignum décimal exact (mpdecimal). a/b restent rootés sur
                     * la stack pendant numeric_add (qui alloue). */
                    BINOP_RESULT(numeric_add(a, b));
                } else if (IS_CURRENCY(a) || IS_CURRENCY(b)) {
                    __int128 ca = IS_CURRENCY(a) ? AS_CURRENCY(a) :
                                 IS_INTEGER(a) ? (__int128)AS_INTEGER(a) * CURRENCY_SCALE :
                                 (__int128)(AS_REAL(a) * CURRENCY_SCALE + (AS_REAL(a) >= 0 ? 0.5 : -0.5));
                    __int128 cb = IS_CURRENCY(b) ? AS_CURRENCY(b) :
                                 IS_INTEGER(b) ? (__int128)AS_INTEGER(b) * CURRENCY_SCALE :
                                 (__int128)(AS_REAL(b) * CURRENCY_SCALE + (AS_REAL(b) >= 0 ? 0.5 : -0.5));
                    BINOP_RESULT(CURRENCY_VAL(ca + cb));
                } else if (IS_INTEGER(a) && IS_INTEGER(b)) {
                    int64_t result;
                    if (__builtin_add_overflow(AS_INTEGER(a), AS_INTEGER(b), &result)) {
                        __int128 big = (__int128)AS_INTEGER(a) + (__int128)AS_INTEGER(b);
                        BINOP_RESULT(numeric_from_i128(big));  /* montée auto entier->numérique */
                    } else {
                        BINOP_RESULT(INTEGER_VAL(result));
                    }
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    BINOP_RESULT(REAL_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
                } else if (IS_INSTANCE(a) && IS_INSTANCE(b)) {
                    /* Combination: instance + instance -> bitwise OR of Valeur.
                     *   : push tous les Obj alloues (valKey,
                     * nomKey, result) avant les allocs suivantes qui peuvent
                     * GC. Sans ces push, valKey/nomKey/result sont collectes
                     * et leur usage suivant lit du poison. */
                    ObjInstance* ia = AS_INSTANCE(a);
                    ObjInstance* ib = AS_INSTANCE(b);
                    ObjString* valKey = obj_copy_string("Valeur", 6);
                    vm_push(OBJ_VAL(valKey));
                    Value va, vb;
                    if (table_get(&ia->fields, valKey, &va) && IS_INTEGER(va) &&
                        table_get(&ib->fields, valKey, &vb) && IS_INTEGER(vb)) {
                        int64_t combined = AS_INTEGER(va) | AS_INTEGER(vb);
                        /* Create a new instance with combined value */
                        ObjString* nomKey = obj_copy_string("Nom", 3);
                        vm_push(OBJ_VAL(nomKey));
                        Value naVal, nbVal;
                        const char* nameA = "";
                        const char* nameB = "";
                        if (table_get(&ia->fields, nomKey, &naVal) && IS_STRING(naVal))
                            nameA = AS_CSTRING(naVal);
                        if (table_get(&ib->fields, nomKey, &nbVal) && IS_STRING(nbVal))
                            nameB = AS_CSTRING(nbVal);
                        char combinedName[256];
                        snprintf(combinedName, sizeof(combinedName), "%s+%s", nameA, nameB);
                        ObjInstance* result = obj_new_instance(ia->structName);
                        vm_push(OBJ_VAL(result));
                        ObjString* combinedStr = obj_copy_string(combinedName, (int)strlen(combinedName));
                        table_set(&result->fields, nomKey, OBJ_VAL(combinedStr));
                        table_set(&result->fields, valKey, INTEGER_VAL(combined));
                        vm_pop(); /* result */
                        vm_pop(); /* nomKey */
                        vm_pop(); /* valKey */
                        BINOP_RESULT(OBJ_VAL(result));
                    } else {
                        vm_pop(); /* valKey */
                        BINOP_ERROR_CODE(E_RT_OPERANDS_NUMBERS_OR_STRINGS);
                    }
                } else if ((IS_DATE(a) || IS_DATETIME(a)) && IS_INTEGER(b)) {
                    BINOP_RESULT(owl_date_add_days(a, AS_INTEGER(b)));
                } else if (IS_INTEGER(a) && (IS_DATE(b) || IS_DATETIME(b))) {
                    BINOP_RESULT(owl_date_add_days(b, AS_INTEGER(a)));
                } else if (IS_DURATION(a) && IS_DURATION(b)) {
                    BINOP_RESULT(OBJ_VAL(obj_new_duration(
                        AS_DURATION(a)->totalMs + AS_DURATION(b)->totalMs)));
                } else if ((IS_DATE(a) || IS_DATETIME(a)) && IS_DURATION(b)) {
                    int64_t days = AS_DURATION(b)->totalMs / 86400000LL;
                    BINOP_RESULT(owl_date_add_days(a, days));
                } else if (IS_DURATION(a) && (IS_DATE(b) || IS_DATETIME(b))) {
                    int64_t days = AS_DURATION(a)->totalMs / 86400000LL;
                    BINOP_RESULT(owl_date_add_days(b, days));
                } else if (IS_INSTANCE(a) && IS_INTEGER(b)) {
                    /*   : push valKey/nomKey/result avant
                     * allocs suivantes. nameA C ptr lu apres push naVal-bearing
                     * field reste valide tant que l'instance ia est sur stack
                     * (rooted via peek). */
                    ObjInstance* ia = AS_INSTANCE(a);
                    ObjString* valKey = obj_copy_string("Valeur", 6);
                    vm_push(OBJ_VAL(valKey));
                    Value va;
                    if (table_get(&ia->fields, valKey, &va) && IS_INTEGER(va)) {
                        int64_t combined = AS_INTEGER(va) | AS_INTEGER(b);
                        ObjString* nomKey = obj_copy_string("Nom", 3);
                        vm_push(OBJ_VAL(nomKey));
                        ObjInstance* result = obj_new_instance(ia->structName);
                        vm_push(OBJ_VAL(result));
                        Value naVal;
                        const char* nameA = "";
                        if (table_get(&ia->fields, nomKey, &naVal) && IS_STRING(naVal))
                            nameA = AS_CSTRING(naVal);
                        ObjString* nameCopy = obj_copy_string(nameA, (int)strlen(nameA));
                        table_set(&result->fields, nomKey, OBJ_VAL(nameCopy));
                        table_set(&result->fields, valKey, INTEGER_VAL(combined));
                        vm_pop(); /* result */
                        vm_pop(); /* nomKey */
                        vm_pop(); /* valKey */
                        BINOP_RESULT(OBJ_VAL(result));
                    } else {
                        vm_pop(); /* valKey */
                        BINOP_ERROR_CODE(E_RT_OPERANDS_NUMBERS_OR_STRINGS);
                    }
                } else {
                    BINOP_ERROR_CODE(E_RT_OPERANDS_NUMBERS_OR_STRINGS);
                }
                #undef BINOP_RESULT
                #undef BINOP_ERROR_CODE
                DISPATCH();
            }

            VM_CASE(OP_SUBTRACT): {
                /* Same GC-safety pattern as OP_ADD: peek, alloc, then pop-pop-push. */
                Value a = vm_peek(1);
                Value b = vm_peek(0);
                #define BINOP_RESULT(v) do { \
                    Value _r = (v); vm_pop(); vm_pop(); vm_push(_r); \
                } while (0)
                #define BINOP_ERROR_CODE(code) do { \
                    vm_pop(); vm_pop(); \
                    owl_emit_runtime_error((code), NULL); \
                    OWL_RT_RECOVER_DISPATCH(); \
                    return INTERPRET_RUNTIME_ERROR; \
                } while (0)

                if (IS_CALLABLE(a) && IS_CALLABLE(b)) {
                    ObjProcChain* src = IS_PROC_CHAIN(a) ? AS_PROC_CHAIN(a) : obj_proc_chain_of(a);
                    ObjProcChain* clone = obj_new_proc_chain();
                    clone->avant = src->avant;
                    clone->apres = src->apres;
                    for (int i = 0; i < src->count; i++) {
                        obj_proc_chain_add(clone, src->procs[i]);
                    }
                    obj_proc_chain_remove(clone, b);
                    BINOP_RESULT(OBJ_VAL(clone));
                    DISPATCH();
                }
                if (IS_BOOL(a)) a = INTEGER_VAL(AS_BOOL(a) ? 1 : 0);
                if (IS_BOOL(b)) b = INTEGER_VAL(AS_BOOL(b) ? 1 : 0);
                if (IS_NIL(a)) a = INTEGER_VAL(0);
                if (IS_NIL(b)) b = INTEGER_VAL(0);
                /*  — scalar unwrap composite. Cf OP_ADD. */
                if (IS_INSTANCE(a) && !IS_INSTANCE(b)) a = owl_instance_default_unwrap(a);
                if (IS_INSTANCE(b) && !IS_INSTANCE(a)) b = owl_instance_default_unwrap(b);
                /*  — widget DSL nu en operande arithmetique. */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                /*  — arithmetique Vecteur (cf. OP_ADD). */
                if (IS_VECTOR(a) || IS_VECTOR(b)) {
                    Value vout;
                    VecArithStatus vst = vm_vector_arith(a, b, '-', &vout);
                    if (vst == VEC_ARITH_OK) { BINOP_RESULT(vout); DISPATCH(); }
                    vm_pop(); vm_pop();
                    owl_emit_runtime_error(owl_vector_arith_error_code(vst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Matrice (cf. OP_ADD). */
                if (IS_MATRIX(a) || IS_MATRIX(b)) {
                    Value mout;
                    MatArithStatus mst = vm_matrix_arith(a, b, '-', &mout);
                    if (mst == MAT_ARITH_OK) { BINOP_RESULT(mout); DISPATCH(); }
                    vm_pop(); vm_pop();
                    owl_emit_runtime_error(owl_matrix_arith_error_code(mst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                if ((IS_DATE(a) || IS_DATETIME(a)) && (IS_DATE(b) || IS_DATETIME(b))) {
                    BINOP_RESULT(owl_date_subtract(a, b));
                } else if ((IS_DATE(a) || IS_DATETIME(a)) && IS_INTEGER(b)) {
                    BINOP_RESULT(owl_date_add_days(a, -AS_INTEGER(b)));
                } else if (IS_TIME(a) && IS_TIME(b)) {
                    /* Heure - Heure = Durée (ms) — doc 1514068 */
                    ObjTime* ta = AS_TIME(a);
                    ObjTime* tb = AS_TIME(b);
                    int64_t ma = (int64_t)ta->hour*3600000 + (int64_t)ta->minute*60000 +
                                 (int64_t)ta->second*1000 + (int64_t)ta->cs*10;
                    int64_t mb = (int64_t)tb->hour*3600000 + (int64_t)tb->minute*60000 +
                                 (int64_t)tb->second*1000 + (int64_t)tb->cs*10;
                    BINOP_RESULT(OBJ_VAL(obj_new_duration(ma - mb)));
                } else if (IS_DURATION(a) && IS_DURATION(b)) {
                    BINOP_RESULT(OBJ_VAL(obj_new_duration(
                        AS_DURATION(a)->totalMs - AS_DURATION(b)->totalMs)));
                } else if (IS_NUMERIC(a) || IS_NUMERIC(b)) {
                    BINOP_RESULT(numeric_sub(a, b));
                } else if (IS_CURRENCY(a) || IS_CURRENCY(b)) {
                    __int128 ca = IS_CURRENCY(a) ? AS_CURRENCY(a) :
                                 IS_INTEGER(a) ? (__int128)AS_INTEGER(a) * CURRENCY_SCALE :
                                 (__int128)(AS_REAL(a) * CURRENCY_SCALE + (AS_REAL(a) >= 0 ? 0.5 : -0.5));
                    __int128 cb = IS_CURRENCY(b) ? AS_CURRENCY(b) :
                                 IS_INTEGER(b) ? (__int128)AS_INTEGER(b) * CURRENCY_SCALE :
                                 (__int128)(AS_REAL(b) * CURRENCY_SCALE + (AS_REAL(b) >= 0 ? 0.5 : -0.5));
                    BINOP_RESULT(CURRENCY_VAL(ca - cb));
                } else if (IS_INTEGER(a) && IS_INTEGER(b)) {
                    int64_t result;
                    if (__builtin_sub_overflow(AS_INTEGER(a), AS_INTEGER(b), &result)) {
                        __int128 big = (__int128)AS_INTEGER(a) - (__int128)AS_INTEGER(b);
                        BINOP_RESULT(numeric_from_i128(big));  /* montée auto entier->numérique */
                    } else {
                        BINOP_RESULT(INTEGER_VAL(result));
                    }
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    BINOP_RESULT(REAL_VAL(AS_NUMBER(a) - AS_NUMBER(b)));
                } else {
                    BINOP_ERROR_CODE(E_RT_OPERANDS_NUMBERS_OR_DATES);
                }
                #undef BINOP_RESULT
                #undef BINOP_ERROR_CODE
                DISPATCH();
            }
            VM_CASE(OP_MULTIPLY): {
                Value b = vm_pop();
                Value a = vm_pop();
                if (IS_BOOL(a)) a = INTEGER_VAL(AS_BOOL(a) ? 1 : 0);
                if (IS_BOOL(b)) b = INTEGER_VAL(AS_BOOL(b) ? 1 : 0);
                if (IS_NIL(a)) a = INTEGER_VAL(0);
                if (IS_NIL(b)) b = INTEGER_VAL(0);
                /*  — scalar unwrap composite. */
                if (IS_INSTANCE(a) && !IS_INSTANCE(b)) a = owl_instance_default_unwrap(a);
                if (IS_INSTANCE(b) && !IS_INSTANCE(a)) b = owl_instance_default_unwrap(b);
                /* TANDEM CANON — tout opcode scalaire qui deballe IS_INSTANCE
                 * doit aussi deballer IS_UI_CONTROL_REF
                 * vers ..defaultProperty : meme chokepoint owl_instance_default
                 * _unwrap. Concerne OP_ADD/OP_SUB/OP_MULTIPLY/OP_DIVIDE/
                 * OP_MODULO/OP_POWER/OP_NEGATE + COMPARE_OP/OP_EQUAL/
                 * OP_NOT_EQUAL. Qui ajoute un opcode scalaire sans ce guard
                 * recree le crash "operandes doivent etre des nombres" sur un
                 * nom de widget DSL nu (`SAI_Num * 2`). */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                /*  — multiplication differentiable Tenseur. Operandes
                 * deja depilees (C locals non-rootes) : on les REPOUSSE pour
                 * les rooter pendant obj_tensor_binop (qui alloue), puis on les
                 * depile avant de pousser le resultat. Le cas a*a (meme objet
                 * parent des deux cotes) est correct : la regle backward
                 * accumule les deux contributions. */
                if (IS_TENSOR(a) || IS_TENSOR(b)) {
                    if (IS_TENSOR(a) && IS_TENSOR(b)) {
                        vm_push(a); vm_push(b);   /* re-root pendant l'alloc */
                        ObjTensor* tc = obj_tensor_binop(TENSOR_OP_MUL,
                                                         AS_TENSOR(a), AS_TENSOR(b));
                        vm_pop(); vm_pop();
                        if (tc != NULL) { vm_push(OBJ_VAL(tc)); DISPATCH(); }
                    }
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS_OR_DATES, NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Vecteur. Operandes deja depilees :
                 * pas de vm_pop avant la levee. */
                if (IS_VECTOR(a) || IS_VECTOR(b)) {
                    Value vout;
                    VecArithStatus vst = vm_vector_arith(a, b, '*', &vout);
                    if (vst == VEC_ARITH_OK) { vm_push(vout); DISPATCH(); }
                    owl_emit_runtime_error(owl_vector_arith_error_code(vst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Matrice. `*` = Hadamard (element-
                 * par-element), PAS le produit matriciel. Operandes deja
                 * depilees : pas de vm_pop avant la levee. */
                if (IS_MATRIX(a) || IS_MATRIX(b)) {
                    Value mout;
                    MatArithStatus mst = vm_matrix_arith(a, b, '*', &mout);
                    if (mst == MAT_ARITH_OK) { vm_push(mout); DISPATCH(); }
                    owl_emit_runtime_error(owl_matrix_arith_error_code(mst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (IS_NUMERIC(a) || IS_NUMERIC(b)) {
                    /* numeric_mul snapshote a/b en mpd avant toute alloc :
                     * GC-safe même si a/b sont déjà dépilés (C locals). */
                    vm_push(numeric_mul(a, b));
                } else if (IS_CURRENCY(a) && IS_INTEGER(b)) {
                    vm_push(CURRENCY_VAL(AS_CURRENCY(a) * AS_INTEGER(b)));
                } else if (IS_INTEGER(a) && IS_CURRENCY(b)) {
                    vm_push(CURRENCY_VAL(AS_INTEGER(a) * AS_CURRENCY(b)));
                } else if (IS_CURRENCY(a) && IS_REAL(b)) {
                    vm_push(CURRENCY_VAL((__int128)((double)AS_CURRENCY(a) * AS_REAL(b) + 0.5)));
                } else if (IS_REAL(a) && IS_CURRENCY(b)) {
                    vm_push(CURRENCY_VAL((__int128)(AS_REAL(a) * (double)AS_CURRENCY(b) + 0.5)));
                } else if (IS_INTEGER(a) && IS_INTEGER(b)) {
                    int64_t result;
                    if (__builtin_mul_overflow(AS_INTEGER(a), AS_INTEGER(b), &result)) {
                        __int128 big = (__int128)AS_INTEGER(a) * (__int128)AS_INTEGER(b);
                        vm_push(numeric_from_i128(big));  /* montée auto entier->numérique */
                    } else {
                        vm_push(INTEGER_VAL(result));
                    }
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    vm_push(REAL_VAL(AS_NUMBER(a) * AS_NUMBER(b)));
                } else {
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }
            VM_CASE(OP_DIVIDE): {
                Value b = vm_pop();
                Value a = vm_pop();
                /*  — scalar unwrap composite. */
                if (IS_INSTANCE(a) && !IS_INSTANCE(b)) a = owl_instance_default_unwrap(a);
                if (IS_INSTANCE(b) && !IS_INSTANCE(a)) b = owl_instance_default_unwrap(b);
                /*  — tandem IS_UI_CONTROL_REF (cf. note OP_MULTIPLY). */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                /*  — arithmetique Vecteur. Division par un element/
                 * scalaire nul -> E_RT_DIV_BY_ZERO, exactement comme le scalaire
                 * reel (pas de Inf/NaN silencieux). */
                if (IS_VECTOR(a) || IS_VECTOR(b)) {
                    Value vout;
                    VecArithStatus vst = vm_vector_arith(a, b, '/', &vout);
                    if (vst == VEC_ARITH_OK) { vm_push(vout); DISPATCH(); }
                    owl_emit_runtime_error(owl_vector_arith_error_code(vst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — arithmetique Matrice. Division par un element/
                 * scalaire nul -> E_RT_DIV_BY_ZERO (pas de Inf/NaN silencieux). */
                if (IS_MATRIX(a) || IS_MATRIX(b)) {
                    Value mout;
                    MatArithStatus mst = vm_matrix_arith(a, b, '/', &mout);
                    if (mst == MAT_ARITH_OK) { vm_push(mout); DISPATCH(); }
                    owl_emit_runtime_error(owl_matrix_arith_error_code(mst), NULL);
                    if (exceptionCount > 0 && currentException != NULL &&
                        handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /* String / String = path join (canon 2024, doc ComplèteRep).
                 * Construit dynamiquement avec le séparateur de la plateforme.
                 * Implémenté via ComplèteRep(a) + b. */
                if (IS_STRING(a) && IS_STRING(b)) {
                    Value left = owl_complete_rep(a);
                    ObjString* l = AS_STRING(left);
                    ObjString* r = AS_STRING(b);
                    int total = l->length + r->length;
                    char* buf = (char*)malloc((size_t)total + 1);
                    memcpy(buf, l->chars, (size_t)l->length);
                    memcpy(buf + l->length, r->chars, (size_t)r->length);
                    buf[total] = '\0';
                    vm_push(OBJ_VAL(obj_copy_string(buf, total)));
                    free(buf);
                    DISPATCH();
                }
                /*  — DIV_BY_ZERO doit etre rattrapable par QUAND
                 * EXCEPTION (consultation exceptionStack avant return, comme
                 * les 12 autres sites VM ; cf. pattern OP_GET_GLOBAL).
                 *  — a une frontiere reentrante, un handler ancetre
                 * (hors scope) est propage en vol, pas consomme ici. */
                #define DIV_ZERO_RAISE() do { \
                    owl_emit_runtime_error(E_RT_DIV_BY_ZERO, NULL); \
                    if (exceptionCount > 0 && currentException != NULL) { \
                        if (exc_top_handler_in_scope(exitFrame)) { \
                            if (handle_runtime_exception()) { \
                                frame = &vm.frames[vm.frameCount - 1]; \
                                DISPATCH(); \
                            } \
                        } else { \
                            OWL_RT_PROPAGATE_REENTRANT(); \
                        } \
                    } \
                    return INTERPRET_RUNTIME_ERROR; \
                } while (0)
                if (IS_NUMERIC(a) || IS_NUMERIC(b)) {
                    /* Division bignum : 50 chiffres significatifs, demi-pair.
                     * div-par-zéro -> exception runtime (mur). */
                    bool dz = false;
                    Value res = numeric_div(a, b, &dz);
                    if (dz) DIV_ZERO_RAISE();
                    vm_push(res);
                } else if (IS_CURRENCY(a) && IS_INTEGER(b)) {
                    if (AS_INTEGER(b) == 0) DIV_ZERO_RAISE();
                    vm_push(CURRENCY_VAL(AS_CURRENCY(a) / AS_INTEGER(b)));
                } else if (IS_CURRENCY(a) && IS_CURRENCY(b)) {
                    if (AS_CURRENCY(b) == 0) DIV_ZERO_RAISE();
                    /* Currency / Currency = Real */
                    vm_push(REAL_VAL((double)AS_CURRENCY(a) / (double)AS_CURRENCY(b)));
                } else if (IS_CURRENCY(a) && IS_REAL(b)) {
                    if (AS_REAL(b) == 0.0) DIV_ZERO_RAISE();
                    vm_push(CURRENCY_VAL((__int128)((double)AS_CURRENCY(a) / AS_REAL(b) + 0.5)));
                } else if (IS_INTEGER(a) && IS_INTEGER(b)) {
                    if (AS_INTEGER(b) == 0) DIV_ZERO_RAISE();
                    /* Language semantics: / always promotes to real (like Python3/JS).
                     * For integer division, use PartieEntiere(a / b). */
                    vm_push(REAL_VAL((double)AS_INTEGER(a) / (double)AS_INTEGER(b)));
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    if (AS_NUMBER(b) == 0.0) DIV_ZERO_RAISE();
                    vm_push(REAL_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
                } else {
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                #undef DIV_ZERO_RAISE
                DISPATCH();
            }

            VM_CASE(OP_MODULO): {
                Value b = vm_pop();
                Value a = vm_pop();
                /*  — scalar unwrap composite. */
                if (IS_INSTANCE(a) && !IS_INSTANCE(b)) a = owl_instance_default_unwrap(a);
                if (IS_INSTANCE(b) && !IS_INSTANCE(a)) b = owl_instance_default_unwrap(b);
                /*  — tandem IS_UI_CONTROL_REF (cf. note OP_MULTIPLY). */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                /*  — meme pont que OP_DIVIDE pour rattrapage par
                 * QUAND EXCEPTION (consultation exceptionStack avant return).
                 *  — frontiere reentrante : handler ancetre propage. */
                #define DIV_ZERO_RAISE() do { \
                    owl_emit_runtime_error(E_RT_DIV_BY_ZERO, NULL); \
                    if (exceptionCount > 0 && currentException != NULL) { \
                        if (exc_top_handler_in_scope(exitFrame)) { \
                            if (handle_runtime_exception()) { \
                                frame = &vm.frames[vm.frameCount - 1]; \
                                DISPATCH(); \
                            } \
                        } else { \
                            OWL_RT_PROPAGATE_REENTRANT(); \
                        } \
                    } \
                    return INTERPRET_RUNTIME_ERROR; \
                } while (0)
                if (IS_INTEGER(a) && IS_INTEGER(b)) {
                    if (AS_INTEGER(b) == 0) DIV_ZERO_RAISE();
                    vm_push(INTEGER_VAL(AS_INTEGER(a) % AS_INTEGER(b)));
                } else if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    /*  — real % 0.0 doit lever, comme integer % 0
                     * (fmod renverrait NaN silencieusement sinon). */
                    if (AS_NUMBER(b) == 0.0) DIV_ZERO_RAISE();
                    vm_push(REAL_VAL(fmod(AS_NUMBER(a), AS_NUMBER(b))));
                } else {
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                #undef DIV_ZERO_RAISE
                DISPATCH();
            }

            VM_CASE(OP_POWER): {
                Value b = vm_pop();
                Value a = vm_pop();
                /*  — tandem IS_UI_CONTROL_REF (cf. note OP_MULTIPLY). */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                if (IS_NUMBER(a) && IS_NUMBER(b)) {
                    vm_push(REAL_VAL(pow(AS_NUMBER(a), AS_NUMBER(b))));
                } else {
                    owl_emit_runtime_error(E_RT_OPERANDS_NUMBERS, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }

            VM_CASE(OP_NEGATE): {
                Value v = vm_pop();
                /*  — tandem IS_UI_CONTROL_REF (cf. note OP_MULTIPLY). */
                if (IS_UI_CONTROL_REF(v)) v = owl_instance_default_unwrap(v);
                if (IS_INTEGER(v)) {
                    /* -INT64_MIN overflows, promote to numerique */
                    if (AS_INTEGER(v) == INT64_MIN) {
                        __int128 big = -(__int128)AS_INTEGER(v);
                        vm_push(numeric_from_i128(big));
                    } else {
                        vm_push(INTEGER_VAL(-AS_INTEGER(v)));
                    }
                } else if (IS_REAL(v)) {
                    vm_push(REAL_VAL(-AS_REAL(v)));
                } else if (IS_CURRENCY(v)) {
                    vm_push(CURRENCY_VAL(-AS_CURRENCY(v)));
                } else if (IS_NUMERIC(v)) {
                    vm_push(numeric_neg(v));
                } else if (IS_DURATION(v)) {
                    vm_push(OBJ_VAL(obj_new_duration(-AS_DURATION(v)->totalMs)));
                } else if (IS_VECTOR(v)) {
                    /*  — negation element-par-element, nouveau Vecteur.
                     * v rootee pendant obj_new_vector (peut GC) ; aucune autre
                     * alloc ensuite. */
                    vm_push(v);
                    ObjVector* res = obj_new_vector(AS_VECTOR(v)->count);
                    vm_pop();
                    ObjVector* src = AS_VECTOR(v);
                    for (int i = 0; i < src->count; i++) res->data[i] = -src->data[i];
                    vm_push(OBJ_VAL(res));
                } else if (IS_MATRIX(v)) {
                    /*  — negation element-par-element, nouvelle Matrice. */
                    vm_push(v);
                    ObjMatrix* res = obj_new_matrix(AS_MATRIX(v)->rows, AS_MATRIX(v)->cols);
                    vm_pop();
                    ObjMatrix* src = AS_MATRIX(v);
                    int n = src->rows * src->cols;
                    for (int i = 0; i < n; i++) res->data[i] = -src->data[i];
                    vm_push(OBJ_VAL(res));
                } else {
                    owl_emit_runtime_error(E_RT_OPERAND_NUMBER, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }

            VM_CASE(OP_INCREMENT): {
                Value v = vm_pop();
                if (IS_INTEGER(v)) vm_push(INTEGER_VAL(AS_INTEGER(v) + 1));
                else if (IS_REAL(v)) vm_push(REAL_VAL(AS_REAL(v) + 1.0));
                else { owl_emit_runtime_error(E_RT_OPERAND_NUMBER, NULL); OWL_RT_RECOVER_DISPATCH(); return INTERPRET_RUNTIME_ERROR; }
                DISPATCH();
            }

            VM_CASE(OP_DECREMENT): {
                Value v = vm_pop();
                if (IS_INTEGER(v)) vm_push(INTEGER_VAL(AS_INTEGER(v) - 1));
                else if (IS_REAL(v)) vm_push(REAL_VAL(AS_REAL(v) - 1.0));
                else { owl_emit_runtime_error(E_RT_OPERAND_NUMBER, NULL); OWL_RT_RECOVER_DISPATCH(); return INTERPRET_RUNTIME_ERROR; }
                DISPATCH();
            }

            VM_CASE(OP_EQUAL): {
                Value b = vm_pop();
                Value a = vm_pop();
                /*  — widget DSL nu compare a une valeur scalaire :
                 * deballe le(s) ref(s) vers leur prop par defaut avant le
                 * test. `Si SAI = "ok"` compare la valeur, pas le controle. */
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                vm_push(BOOL_VAL(values_equal(a, b)));
                DISPATCH();
            }

            VM_CASE(OP_NOT_EQUAL): {
                Value b = vm_pop();
                Value a = vm_pop();
                /*  +  — deballe un widget DSL nu (ObjUiControlRef)
                 * OU une cellule ZR headless materialisee (ObjUiControl
                 * widget==NULL) vers sa prop par defaut. Le resolver rend la
                 * valeur inchangee si non deballable (widget!=NULL, sans prop
                 * defaut). */
                if (IS_UI_CONTROL_REF(a) || IS_UI_CONTROL(a)) a = owl_instance_default_unwrap(a);
                if (IS_UI_CONTROL_REF(b) || IS_UI_CONTROL(b)) b = owl_instance_default_unwrap(b);
                vm_push(BOOL_VAL(!values_equal(a, b)));
                DISPATCH();
            }

            VM_CASE(OP_GREATER):       COMPARE_OP(>); DISPATCH();
            VM_CASE(OP_GREATER_EQUAL): COMPARE_OP(>=); DISPATCH();
            VM_CASE(OP_LESS):          COMPARE_OP(<); DISPATCH();
            VM_CASE(OP_LESS_EQUAL):    COMPARE_OP(<=); DISPATCH();

            VM_CASE(OP_NOT):
                vm_push(BOOL_VAL(is_falsy(vm_pop())));
                DISPATCH();

            /* OP_AND_FULL / OP_OR_FULL — full-eval booléen, canon.
             * Refuse les types non convertibles en condition (string, buffer,
             * tableau, instance, etc.) — aligné OP_VERIFY_BOOL_COND. Les types
             * numériques + nil + booléen sont acceptés. */
            VM_CASE(OP_AND_FULL):
            VM_CASE(OP_OR_FULL): {
                Value b = vm_peek(0);
                Value a = vm_peek(1);
                if (IS_STRING(a) || IS_BUFFER(a) ||
                    IS_ARRAY(a) || IS_ASSOC_ARRAY(a) || IS_COLLECTION(a) ||
                    IS_INSTANCE(a) || IS_JSON(a) ||
                    IS_DATE(a) || IS_TIME(a) || IS_DATETIME(a) || IS_DURATION(a) ||
                    IS_STRING(b) || IS_BUFFER(b) ||
                    IS_ARRAY(b) || IS_ASSOC_ARRAY(b) || IS_COLLECTION(b) ||
                    IS_INSTANCE(b) || IS_JSON(b) ||
                    IS_DATE(b) || IS_TIME(b) || IS_DATETIME(b) || IS_DURATION(b)) {
                    owl_emit_runtime_error(E_RT_LOGIC_OPERAND_TYPE, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm_pop(); vm_pop();
                bool ba = !is_falsy(a);
                bool bb = !is_falsy(b);
                bool r = (instruction == OP_AND_FULL) ? (ba && bb) : (ba || bb);
                vm_push(BOOL_VAL(r));
                DISPATCH();
            }

            /* --- Bitwise operators (canon Langage/Operateurs/1512009) ---
             * Coerce les opérandes vers int64. entier/réel/booléen acceptés ;
             * sinon 0 (cohérent avec stdlib_bitwise.c::to_int64). */
            VM_CASE(OP_BIT_AND):
            VM_CASE(OP_BIT_OR):
            VM_CASE(OP_BIT_XOR):
            VM_CASE(OP_BIT_SHL):
            VM_CASE(OP_BIT_SHR): {
                OpCode op = (OpCode)instruction;
                Value b = vm_pop();
                Value a = vm_pop();
                int64_t ia = IS_INTEGER(a) ? AS_INTEGER(a)
                           : IS_REAL(a)    ? (int64_t)AS_REAL(a)
                           : IS_BOOL(a)    ? (AS_BOOL(a) ? 1 : 0)
                           : 0;
                int64_t ib = IS_INTEGER(b) ? AS_INTEGER(b)
                           : IS_REAL(b)    ? (int64_t)AS_REAL(b)
                           : IS_BOOL(b)    ? (AS_BOOL(b) ? 1 : 0)
                           : 0;
                int64_t r = 0;
                switch (op) {
                    case OP_BIT_AND: r = ia & ib; break;
                    case OP_BIT_OR:  r = ia | ib; break;
                    case OP_BIT_XOR: r = ia ^ ib; break;
                    case OP_BIT_SHL: {
                        if (ib < 0)  ib = 0;
                        if (ib > 63) ib = 63;
                        r = (int64_t)((uint64_t)ia << ib);
                        break;
                    }
                    case OP_BIT_SHR: {
                        if (ib < 0)  ib = 0;
                        if (ib > 63) ib = 63;
                        r = (int64_t)((uint64_t)ia >> ib);
                        break;
                    }
                    default: break;
                }
                vm_push(INTEGER_VAL(r));
                DISPATCH();
            }
            VM_CASE(OP_BIT_NOT): {
                Value a = vm_pop();
                int64_t ia = IS_INTEGER(a) ? AS_INTEGER(a)
                           : IS_REAL(a)    ? (int64_t)AS_REAL(a)
                           : IS_BOOL(a)    ? (AS_BOOL(a) ? 1 : 0)
                           : 0;
                vm_push(INTEGER_VAL(~ia));
                DISPATCH();
            }

            VM_CASE(OP_CONCAT): {
                /*  : keep BOTH original operands AND fresh ObjString
                 * a/b rooted during all allocations. Original peek(0)/(1) sont
                 * rooted via vm.stack mais les fresh a/b retournes par
                 * value_to_string sont des Obj fresh non-rooted — l'alloc
                 * intermediaire (value_to_string(a) apres b ; obj_concat_strings
                 * apres a) peut GC et collecter les fresh. */
                ObjString* b = value_to_string(vm_peek(0));
                vm_push(OBJ_VAL(b));
                ObjString* a = value_to_string(vm_peek(2)); /* peek(2) car push(b) a decale */
                vm_push(OBJ_VAL(a));
                ObjString* r = obj_concat_strings(a, b);
                vm_pop(); /* a */
                vm_pop(); /* b */
                vm_pop(); /* operande original op2 */
                vm_pop(); /* operande original op1 */
                vm_push(OBJ_VAL(r));
                DISPATCH();
            }

            VM_CASE(OP_DEEP_COPY): {
                /*  : peek au lieu de pop pour garder v rooted sur
                 * la stack pendant value_deep_clone qui alloue. */
                Value v = vm_peek(0);
                Value cloned = value_deep_clone(v);
                /*  : la valeur se contient elle-meme. La copie est
                 * abandonnee — la source, elle, reste intacte. */
                if (deep_clone_take_self_containing()) {
                    vm_pop();
                    owl_emit_runtime_error(E_RT_DEEP_COPY_SELF_CONTAINING, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) {
                            frame = &vm.frames[vm.frameCount - 1];
                            DISPATCH();
                        }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm_pop();
                vm_push(cloned);
                DISPATCH();
            }

            VM_CASE(OP_TO_UNDETECTABLE): {
#ifdef OWL_INDETECTABLE
                /* Convert the top-of-stack to an undetectable string if it's
                 * a non-undetectable string. Other values are left untouched
                 * (Nil, integers, etc. -- declaration of `chaîne <indétectable>`
                 * with a non-string initializer keeps its native value). */
                Value v = vm_peek(0);
                if (IS_STRING(v) && !AS_STRING(v)->undetectable) {
                    ObjString* src = AS_STRING(v);
                    ObjString* prot = obj_copy_string_undetectable(src->chars, src->length);
                    vm_pop();
                    vm_push(OBJ_VAL(prot));
                }
#endif
                /* When OWL_INDETECTABLE is not set: opcode is a silent no-op.
                 * Bundles compiled by a default `owl.exe` may contain this
                 * opcode (harmless), and bundles compiled by `owl-secure.exe`
                 * running on a default VM would just skip the masking. */
                DISPATCH();
            }

            VM_CASE(OP_STRING_CHAR_AT): {
                int64_t idx = AS_INTEGER(vm_pop());
                Value srcV = vm_pop();
                /* Buffer : acces octet brut. buf[[i]] -> entier 0-255, hors du
                 * codec texte (sinon les octets >=128 forment des demi-sequences
                 * UTF-8 corrompues). Aligne sur BufferLitOctet : index hors
                 * buffer = soft-error ErreurDetectee, pas d'exception. */
                if (IS_BUFFER(srcV)) {
                    ObjBuffer* buf = AS_BUFFER(srcV);
                    int64_t pos = idx; /* 1-based */
                    if (pos < 1 || pos > buf->length) {
                        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE,
                                              "BufferLitOctet",
                                              "arg", "position", NULL);
                        vm_push(INTEGER_VAL(0));
                        DISPATCH();
                    }
                    vm_push(INTEGER_VAL((int64_t)buf->bytes[pos - 1]));
                    DISPATCH();
                }
                const char* chars = NULL; int len = 0;
                bool srcUndetectable = false;
                if (IS_STRING(srcV)) {
                    chars = AS_STRING(srcV)->chars;
                    len = AS_STRING(srcV)->length;
                    srcUndetectable = AS_STRING(srcV)->undetectable;
                } else {
                    owl_emit_runtime_error(E_RT_SUBSCRIPT_NEEDS_STRING_OR_BUFFER, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int i = (int)idx - 1; /* base-1 */
                if (i < 0 || i >= len) { vm_push(OBJ_VAL(obj_copy_string("", 0))); DISPATCH(); }
                if (srcUndetectable) {
                    /* Unmask the source string to extract the cleartext byte,
                     * then hand it to the undetectable allocator (which
                     * re-masks at offset 0 in its own buffer). */
                    char* clear = obj_string_unmask_dup(AS_STRING(srcV));
                    ObjString* result = obj_copy_string_undetectable(clear + i, 1);
                    obj_string_secure_free(clear, len);
                    vm_push(OBJ_VAL(result));
                } else {
                    vm_push(OBJ_VAL(obj_copy_string(chars + i, 1)));
                }
                DISPATCH();
            }

            VM_CASE(OP_BUFFER_BYTE_SET): {
                /* buf[[i]] = v : ecrit exactement 1 octet (base 1) en mode
                 * strict, jumeau de BufferEcritOctet :
                 *   - valeur hors [0,255]      -> exception (vm_throw_from_erreur)
                 *   - index hors buffer        -> soft-error ErreurDetectee
                 * Sur une chaine (immutable) ou tout autre type : no-op,
                 * comportement historique inchange. Laisse v en sommet de pile
                 * (resultat de l'expression d'affectation). */
                Value valV = vm_pop();
                int64_t idx = AS_INTEGER(vm_pop());
                Value tgtV = vm_pop();
                if (IS_BUFFER(tgtV)) {
                    ObjBuffer* buf = AS_BUFFER(tgtV);
                    if (idx < 1 || idx > buf->length) {
                        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE,
                                              "BufferÉcritOctet",
                                              "arg", "position", NULL);
                        vm_push(valV);
                        DISPATCH();
                    }
                    int64_t v = IS_INTEGER(valV) ? AS_INTEGER(valV)
                              : IS_NUMBER(valV)  ? (int64_t)AS_NUMBER(valV)
                                                 : -1; /* non-numerique -> hors plage */
                    if (v < 0 || v > 255) {
                        vm_push(valV);
                        vm_throw_from_erreur(
                            "Écriture d'octet : valeur hors de la plage 0-255.", 0);
                        if (throwPending) {
                            throwPending = false;
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                            owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION,
                                "message",
                                currentException ? currentException->chars : "unknown",
                                NULL);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        DISPATCH();
                    }
                    buf->bytes[idx - 1] = (unsigned char)(v & 0xFF);
                }
                vm_push(valV);
                DISPATCH();
            }

            VM_CASE(OP_INDIRECT_GLOBAL_GET): {
                Value nameV = vm_pop();
                if (!IS_STRING(nameV)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_NAME_STRING, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjString* name = AS_STRING(nameV);
                Value out;
                /*  phase 2b : lookup canon (insensible casse+accents).
                 *  : un nom qui ne resout AUCUN element erre au lieu de
                 * rendre NIL en silence (canon + determinisme, pas d'echec
                 * masque). */
                if (!vm_globals_canon_get(name, &out, NULL)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_NAME_UNKNOWN, "nom", name->chars, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm_push(out);
                DISPATCH();
            }

            VM_CASE(OP_INDIRECT_GLOBAL_SET): {
                /* Stack (bottom to top): [name, value]. Leave value on stack
                 * as the expression result. */
                Value value = vm_pop();
                Value nameV = vm_pop();
                if (!IS_STRING(nameV)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_NAME_STRING, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjString* name = AS_STRING(nameV);
                /*  : ecrire via indirection sur un nom inconnu erre
                 * (canon confirme a l'oracle), au lieu de creer une globale
                 * a la volee (non deterministe). */
                Value existing;
                if (!vm_globals_canon_get(name, &existing, NULL)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_NAME_UNKNOWN, "nom", name->chars, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  phase 2b : route via obj_globals_set_kv pour
                 * peupler vm.globals_canon_idx + table_set principale. */
                obj_globals_set_kv(name, value);
                vm_push(value);
                DISPATCH();
            }

            VM_CASE(OP_INDIRECT_FIELD_GET): {
                Value nameV = vm_pop();
                Value instV = vm_pop();
                if (!IS_INSTANCE(instV) || !IS_STRING(nameV)) {
                    owl_emit_runtime_error(E_RT_OBJ_DOT_BRACE_NEEDS_INSTANCE, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* inst = AS_INSTANCE(instV);
                Value out;
                /*  : champ absent -> erreur (coherent avec le global). */
                if (!table_get(&inst->fields, AS_STRING(nameV), &out)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_FIELD_UNKNOWN, "nom", AS_STRING(nameV)->chars, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm_push(out);
                DISPATCH();
            }

            VM_CASE(OP_INDIRECT_FIELD_SET): {
                /* Stack order (bottom to top): instance, nameStr, value.
                 * Leave value on the stack as the expression result. */
                Value value = vm_pop();
                Value nameV = vm_pop();
                Value instV = vm_pop();
                if (!IS_INSTANCE(instV) || !IS_STRING(nameV)) {
                    owl_emit_runtime_error(E_RT_OBJ_DOT_BRACE_NEEDS_INSTANCE, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* inst = AS_INSTANCE(instV);
                /*  : ecrire un champ inconnu erre (coherent avec le
                 * global ; champs de structure fixes -> pas de creation a la
                 * volee). */
                Value existing;
                if (!table_get(&inst->fields, AS_STRING(nameV), &existing)) {
                    owl_emit_runtime_error(E_RT_INDIRECTION_FIELD_UNKNOWN, "nom", AS_STRING(nameV)->chars, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) { frame = &vm.frames[vm.frameCount - 1]; DISPATCH(); }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                table_set(&inst->fields, AS_STRING(nameV), value);
                vm_push(value);
                DISPATCH();
            }

            VM_CASE(OP_STRING_SLICE):
            VM_CASE(OP_STRING_SLICE_TO_END):
            VM_CASE(OP_STRING_SLICE_LEN): {
                /* Stack layout selon variante :
                 *   OP_STRING_SLICE         : [s, i, j]    -> s[i..j] (j absolu, inclusif)
                 *   OP_STRING_SLICE_TO_END  : [s, i]       -> s[i..Taille(s)]
                 *   OP_STRING_SLICE_LEN     : [s, i, n]    -> n chars depuis i
                 * Toutes les variantes convergent sur (si, ei) demi-ouvert. */
                int64_t arg2 = 0, i_arg = 0;
                if (instruction == OP_STRING_SLICE_TO_END) {
                    i_arg = AS_INTEGER(vm_pop());
                } else {
                    arg2  = AS_INTEGER(vm_pop());
                    i_arg = AS_INTEGER(vm_pop());
                }
                Value srcV = vm_pop();
                const char* chars = NULL; int len = 0;
                bool isBuf = false;
                bool srcUndetectable = false;
                if (IS_STRING(srcV)) {
                    chars = AS_STRING(srcV)->chars;
                    len = AS_STRING(srcV)->length;
                    srcUndetectable = AS_STRING(srcV)->undetectable;
                } else if (IS_BUFFER(srcV)) {
                    chars = (const char*)AS_BUFFER(srcV)->bytes;
                    len = AS_BUFFER(srcV)->length;
                    isBuf = true;
                } else {
                    owl_emit_runtime_error(E_RT_SLICE_NEEDS_STRING_OR_BUFFER, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int si = (int)i_arg - 1;
                int ei;
                if (instruction == OP_STRING_SLICE_TO_END) {
                    ei = len;
                } else if (instruction == OP_STRING_SLICE_LEN) {
                    /* arg2 = nombre de chars demandé. ei exclusif. */
                    ei = si + (int)arg2;
                } else {
                    /* OP_STRING_SLICE : arg2 = borne fin inclusive (canon). */
                    ei = (int)arg2;
                }
                if (si < 0) si = 0;
                if (ei > len) ei = len;
                if (ei < si) ei = si;
                int n = ei - si;
                if (isBuf) vm_push(OBJ_VAL(obj_new_buffer(chars + si, n)));
                else if (srcUndetectable) {
                    char* clear = obj_string_unmask_dup(AS_STRING(srcV));
                    ObjString* result = obj_copy_string_undetectable(clear + si, n);
                    obj_string_secure_free(clear, len);
                    vm_push(OBJ_VAL(result));
                } else {
                    vm_push(OBJ_VAL(obj_copy_string(chars + si, n)));
                }
                DISPATCH();
            }

            VM_CASE(OP_STRING_MATCH): {
                uint8_t spec = READ_BYTE();
                int kind  = spec & 0x7;
                bool ci   = (spec & 0x08) != 0;
                bool ai   = (spec & 0x10) != 0;
                ObjString* pat = value_to_string(vm_pop());
                ObjString* s   = value_to_string(vm_pop());
                vm_push(BOOL_VAL(owl_string_match(s->chars, s->length,
                                                  pat->chars, pat->length,
                                                  kind, ci, ai)));
                DISPATCH();
            }

            VM_CASE(OP_DEFINE_GLOBAL): {
                /* Pas de RAZ-auto ici : si le RHS est un OP_CALL, ce call a
                 * déjà fait son propre RAZ pré-call ; une erreur posée par la
                 * native pendant son exécution DOIT survivre jusqu'à l'opcode
                 * suivant pour que le pattern `c = fNative(...) ; Si
                 * ErreurDétectée Alors ...` détecte l'échec. Cf.  +
                 *  ligne 17 (contrat user) : « 1 instruction = 1
                 * RAZ », pas « 1 opcode = 1 RAZ ». Le RAZ effectif est porté
                 * par OP_CALL / OP_INVOKE / OP_INVOKE_SAFE. */
                ObjString* name = READ_STRING();
                /*  phase 2b : route via obj_globals_set_kv pour
                 * peupler aussi vm.globals_canon_idx. */
                obj_globals_set_kv(name, vm_peek(0));
                vm_pop();
                DISPATCH();
            }

            VM_CASE(OP_GET_TYPE_GLOBAL): {
                /*  : lookup type-only dans vm.types. Emis par le
                 * compilo au site `est un X` / `__StructClone <name>`. Miss
                 * remonte la meme E_RT_UNDEFINED_VARIABLE que GET_GLOBAL —
                 * un type inconnu en runtime est tres rare (validation
                 * compile-time pour la plupart des cas), garder un seul
                 * code d'erreur permet de ne pas multiplier la surface.
                 * Phase 2b : lookup via vm.types_canon_idx (tolerance
                 * casse+accents). */
                ObjString* name = READ_STRING();
                Value value;
                if (!vm_types_canon_get(name, &value)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_VARIABLE,
                                           "name", name->chars, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) {
                            frame = &vm.frames[vm.frameCount - 1];
                            DISPATCH();
                        }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm_push(value);
                DISPATCH();
            }

            VM_CASE(OP_GET_GLOBAL):
            VM_CASE(OP_GET_GLOBAL_CALLEE): {
                bool asCallee = (instruction == OP_GET_GLOBAL_CALLEE);
                ObjString* name = READ_STRING();
                Value value;
                /*  phase 2b : lookup canon (insensible casse+accents)
                 * via vm.globals_canon_idx → vm.globals. */
                if (!vm_globals_canon_get(name, &value, NULL)) {
                    if (asCallee) {
                        owl_emit_runtime_error(E_RT_UNDEFINED_FUNCTION,
                                               "name", name->chars, NULL);
                    } else {
                        owl_emit_runtime_error(E_RT_UNDEFINED_VARIABLE,
                                               "name", name->chars, NULL);
                    }
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) {
                            frame = &vm.frames[vm.frameCount - 1];
                            DISPATCH();
                        }
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                /* Freeze a builder-mode string before exposing it on the
                 * stack, so any read of a global string sees an immutable
                 * snapshot. The buffer itself stays in the globals table
                 * for the next OP_APPEND_IN_PLACE_GLOBAL to mutate.
                 * Undetectable strings are also `interned=false` but must
                 * NOT be re-interned (that would defeat the masking) -- skip
                 * the freeze entirely; they are already immutable storage. */
                if (IS_STRING(value) && !AS_STRING(value)->interned
                        && !AS_STRING(value)->undetectable) {
                    ObjString* buf = AS_STRING(value);
                    /* Lazy freeze cache: only re-copy when the buffer was
                     * mutated since the last read. OP_APPEND_IN_PLACE_*
                     * sets frozen_cache=NULL on every mutation. Cuts the
                     * O(n) copy down to O(1) on read-heavy patterns. */
                    if (buf->frozen_cache == NULL) {
                        buf->frozen_cache = obj_copy_string(buf->chars, buf->length);
                    }
                    value = OBJ_VAL(buf->frozen_cache);
                }
                vm_push(value);
                DISPATCH();
            }

            VM_CASE(OP_GET_ELEMENT_GLOBAL): {
                /*  — lecture qualifiee `FEN_X.gNom` / `ETAT_X.gCode`.
                 * La cle `<ELEM>$<var>` est definie dans vm.globals quand le
                 * top-level de l'element a tourne (Ouvre pour fenetre, 1re
                 * impression pour etat). Avant ca, l'element est endormi : la
                 * cle est absente -> on push NIL (pas d'erreur). C'est le
                 * contrat NIL-tolerant de l'opcode ( : NIL avant que
                 * l'element ait vecu). Le crash sur symbole vraiment inconnu
                 * reste reserve a OP_GET_GLOBAL (procedures cross-file `$`). */
                ObjString* name = READ_STRING();
                /*  — acces qualifie a une variable de FENETRE
                 * (`FEN_X.gVar`) : instance-based strict. La cle mangled est
                 * `<ELEM>$<var>` ; on extrait `<ELEM>`. Si c'est une fenetre
                 * declaree SANS instance ouverte -> exception (plus de valeur/
                 * Null muet : le slot statique `FEN$var` n'est plus un fallback
                 * silencieux). Les elements NON-fenetre (Etat, globales cross-
                 * file) gardent le contrat NIL-tolerant historique. En multi-
                 * instance, la valeur lue est celle du slot mangled partage
                 * (derniere ouverture gagnante). */
                const char* dollar = strchr(name->chars, '$');
                if (dollar != NULL) {
                    int elemLen = (int)(dollar - name->chars);
                    if (elemLen > 0 && elemLen < 256) {
                        char elemName[256];
                        memcpy(elemName, name->chars, (size_t)elemLen);
                        elemName[elemLen] = '\0';
                        if (vm_window_lookup(elemName) != NULL
                                && !vm_window_instance_is_open(elemName)) {
                            owl_emit_runtime_error(E_RT_WINDOW_NOT_OPEN,
                                                   "window", elemName, NULL);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                }
                Value value;
                if (vm_globals_canon_get(name, &value, NULL)) {
                    vm_push(value);
                } else {
                    vm_push(NIL_VAL);
                }
                DISPATCH();
            }

            VM_CASE(OP_SET_GLOBAL): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ).
                 * Phase 2b : lookup canon d'abord, puis ecriture sur la cle
                 * verbatim canonique (preserve la graphie de declaration).
                 * Si la variable n'existe pas, erreur sans insertion. */
                ObjString* name = READ_STRING();
                Value dummy;
                ObjString* verbatim = NULL;
                if (!vm_globals_canon_get(name, &dummy, &verbatim)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_VARIABLE,
                                           "name", name->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                /* Ecriture directe sur le verbatim (deja dans la table et
                 * dans l'index canon — pas d'insertion bilaterale a refaire). */
                table_set(&vm.globals, verbatim, vm_peek(0));
                DISPATCH();
            }

            VM_CASE(OP_GET_LOCAL): {
                uint8_t slot = READ_BYTE();
                Value v = frame->slots[slot];
                /* If the slot holds a string in builder mode (interned=false),
                 * freeze a stable copy before exposing it on the stack. This
                 * prevents aliasing: any read of `s` in user code (e.g.
                 * `t = s` or `Trace(s)`) sees a snapshot, immune to later
                 * OP_APPEND_IN_PLACE mutations on `s`. The slot itself keeps
                 * its mutable buffer so subsequent appends remain O(1).
                 * frozen_cache makes the snapshot pay-once: read-heavy
                 * patterns (`POUR i .. : Milieu(seq, i, 1)`) drop from
                 * O(n) per read to O(1). Invalidated by OP_APPEND_IN_PLACE. */
                if (IS_STRING(v) && !AS_STRING(v)->interned
                        && !AS_STRING(v)->undetectable) {
                    ObjString* buf = AS_STRING(v);
                    if (buf->frozen_cache == NULL) {
                        buf->frozen_cache = obj_copy_string(buf->chars, buf->length);
                    }
                    v = OBJ_VAL(buf->frozen_cache);
                }
                vm_push(v);
                DISPATCH();
            }

            VM_CASE(OP_SET_LOCAL): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ). */
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = vm_peek(0);
                DISPATCH();
            }

            VM_CASE(OP_APPEND_IN_PLACE_GLOBAL): {
                ObjString* name = READ_STRING();
                Value rhs = vm_pop();
                Value lhs;
                /*  phase 2b : lookup canon, recupere verbatim pour
                 * les ecritures en aval. */
                ObjString* verbatim = NULL;
                if (!vm_globals_canon_get(name, &lhs, &verbatim)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_VARIABLE,
                                           "name", name->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_STRING(lhs) || !IS_BYTES(rhs)) {
                    /*  : push sa/sb avant prochaine alloc. */
                    ObjString* sa = value_to_string(lhs);
                    vm_push(OBJ_VAL(sa));
                    ObjString* sb = value_to_string(rhs);
                    vm_push(OBJ_VAL(sb));
                    Value result = OBJ_VAL(obj_concat_strings(sa, sb));
                    vm_pop(); vm_pop();
                    table_set(&vm.globals, verbatim, result);
                    vm_push(result);
                    DISPATCH();
                }

                ObjString* dest = AS_STRING(lhs);
                ObjString* src  = IS_STRING(rhs) ? AS_STRING(rhs) : NULL;

                if (src == NULL) {
                    /*  : push sb avant obj_concat_strings. */
                    ObjString* sb = value_to_string(rhs);
                    vm_push(OBJ_VAL(sb));
                    Value result = OBJ_VAL(obj_concat_strings(dest, sb));
                    vm_pop();
                    table_set(&vm.globals, verbatim, result);
                    vm_push(result);
                    DISPATCH();
                }

                if (dest->interned) {
                    dest = obj_string_to_buffer(dest);
                    /* Root the freshly promoted buffer before the next
                     * allocation so the GC can trace it. */
                    table_set(&vm.globals, verbatim, OBJ_VAL(dest));
                }

                int new_len = dest->length + src->length;
                if (new_len + 1 > dest->capacity) {
                    int new_cap = dest->capacity > 0 ? dest->capacity * 2 : 16;
                    while (new_cap < new_len + 1) new_cap *= 2;
                    dest->chars = (char*)owl_realloc(
                        dest->chars,
                        (size_t)dest->capacity,
                        (size_t)new_cap);
                    dest->capacity = new_cap;
                }

                memcpy(dest->chars + dest->length, src->chars, (size_t)src->length);
                dest->length = new_len;
                dest->chars[new_len] = '\0';
                dest->hash = 0;
                /* Invalidate UTF-8 codepoint caches: bytes changed, all
                 * cached scans are now stale. */
                dest->ascii_state = 0;
                dest->cp_count = -1;
                dest->last_cp_index = -1;
                dest->last_byte_offset = 0;
                dest->frozen_cache = NULL;

                Value result = OBJ_VAL(dest);
                table_set(&vm.globals, verbatim, result);
                vm_push(result);
                DISPATCH();
            }

            VM_CASE(OP_APPEND_IN_PLACE): {
                uint8_t slot = READ_BYTE();
                Value rhs   = vm_pop();
                Value lhs   = frame->slots[slot];

                /* Type fallback: if either side isn't a string-ish value,
                 * behave like a normal OP_ADD on (lhs, rhs) and write the
                 * result into the slot. We re-push and goto the standard
                 * concat path via the helper. */
                if (!IS_STRING(lhs) || !IS_BYTES(rhs)) {
                    /* Coerce per the existing OP_ADD rules and concat.
                     *  : push sa/sb avant prochaine alloc. */
                    ObjString* sa = value_to_string(lhs);
                    vm_push(OBJ_VAL(sa));
                    ObjString* sb = value_to_string(rhs);
                    vm_push(OBJ_VAL(sb));
                    Value result = OBJ_VAL(obj_concat_strings(sa, sb));
                    vm_pop(); vm_pop();
                    frame->slots[slot] = result;
                    vm_push(result);
                    DISPATCH();
                }

                ObjString* dest = AS_STRING(lhs);
                ObjString* src  = IS_STRING(rhs) ? AS_STRING(rhs) : NULL;

                /* Buffer mode: src must be a STRING here (rhs validated above
                 * with IS_BYTES; if it's a Buffer, fall back to value_to_string). */
                if (src == NULL) {
                    /*  : push sb avant obj_concat_strings. */
                    ObjString* sa = dest;
                    ObjString* sb = value_to_string(rhs);
                    vm_push(OBJ_VAL(sb));
                    Value result = OBJ_VAL(obj_concat_strings(sa, sb));
                    vm_pop();
                    frame->slots[slot] = result;
                    vm_push(result);
                    DISPATCH();
                }

                /* First-time entry into builder mode: copy the interned
                 * string into a fresh mutable buffer. Subsequent iterations
                 * skip this and mutate the existing buffer directly.
                 * Critical: write the new buffer into the slot BEFORE the
                 * next allocation (grow) so the GC can see it as a root. */
                if (dest->interned) {
                    dest = obj_string_to_buffer(dest);
                    frame->slots[slot] = OBJ_VAL(dest);
                }

                int new_len = dest->length + src->length;

                /* Amortized x2 growth so total cost across N appends is O(N). */
                if (new_len + 1 > dest->capacity) {
                    int new_cap = dest->capacity > 0 ? dest->capacity * 2 : 16;
                    while (new_cap < new_len + 1) new_cap *= 2;
                    dest->chars = (char*)owl_realloc(
                        dest->chars,
                        (size_t)dest->capacity,
                        (size_t)new_cap);
                    dest->capacity = new_cap;
                }

                memcpy(dest->chars + dest->length, src->chars, (size_t)src->length);
                dest->length = new_len;
                dest->chars[new_len] = '\0';
                dest->hash = 0;  /* invalid; recomputed if/when frozen */
                /* Invalidate UTF-8 codepoint caches (see note above). */
                dest->ascii_state = 0;
                dest->cp_count = -1;
                dest->last_cp_index = -1;
                dest->last_byte_offset = 0;
                dest->frozen_cache = NULL;

                Value result = OBJ_VAL(dest);
                frame->slots[slot] = result;
                vm_push(result);
                DISPATCH();
            }

            VM_CASE(OP_JUMP): {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                DISPATCH();
            }

            VM_CASE(OP_JUMP_IF_FALSE): {
                uint16_t offset = READ_SHORT();
                if (is_falsy(vm_peek(0))) frame->ip += offset;
                DISPATCH();
            }

            VM_CASE(OP_VERIFY_BOOL_COND): {
                /* Canon strict : refuse les types non-booléens/numériques
                 * comme condition de SI / TANTQUE / POUR. Aligne avec le canon
                 * "expression non booléenne". Pour tester "chaîne non
                 * vide", utiliser `SI Taille(s) > 0` ou `SI s <> ""`. */
                Value v = vm_peek(0);
                if (IS_STRING(v) || IS_BUFFER(v) ||
                    IS_ARRAY(v) || IS_ASSOC_ARRAY(v) || IS_COLLECTION(v) ||
                    IS_INSTANCE(v) || IS_JSON(v) ||
                    IS_DATE(v) || IS_TIME(v) || IS_DATETIME(v) || IS_DURATION(v)) {
                    owl_emit_runtime_error(E_RT_NON_BOOL_CONDITION, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }

            VM_CASE(OP_JUMP_IF_NIL): {
                uint16_t offset = READ_SHORT();
                if (IS_NIL(vm_peek(0))) frame->ip += offset;
                DISPATCH();
            }

            VM_CASE(OP_JUMP_IF_NOT_NIL): {
                uint16_t offset = READ_SHORT();
                if (!IS_NIL(vm_peek(0))) frame->ip += offset;
                DISPATCH();
            }

            VM_CASE(OP_PROC_SET_HOOK): {
                uint8_t which = READ_BYTE();
                Value val = vm_pop();
                Value recv = vm_pop();
                ObjProcChain* chain;
                if (IS_PROC_CHAIN(recv)) {
                    chain = AS_PROC_CHAIN(recv);
                } else if (IS_FUNCTION(recv) || IS_NATIVE(recv)) {
                    chain = obj_proc_chain_of(recv);
                } else {
                    chain = obj_new_proc_chain();
                }
                if (which == 0)      chain->avant = val;
                else                 chain->apres = val;
                vm_push(OBJ_VAL(chain));
                DISPATCH();
            }

            VM_CASE(OP_LOOP): {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                /* Ctrl-C poll: at every loop back-edge we're at a safe point
                 * (between instructions). Raising here produces a catchable
                 * runtime exception — user code in QUAND EXCEPTION can
                 * intercept it; otherwise the REPL drops back to the prompt
                 * with the session intact instead of being SIGKILL'd. */
                if (g_interrupt_requested) {
                    g_interrupt_requested = 0;
                    owl_emit_runtime_error(E_RT_INTERRUPTED, NULL);
                    if (handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }

            VM_CASE(OP_CALL): {
                int argCount = READ_BYTE();
                /* RAZ-auto dispatch PRÉ-CALL : c'est le seul vrai point de
                 * reset du flag ErreurDétectée. Une native qui pose une
                 * erreur via owl_set_error() survit jusqu'au PROCHAIN
                 * OP_CALL / OP_INVOKE (ou ErreurRAZ() user-explicit). Les
                 * opcodes d'assignation (DEFINE_GLOBAL, SET_*, ARRAY_SET*,
                 * STRUCT_SET, SET_FIELD_THIS) ne reset PAS, pour que le
                 * pattern `c = fNative(...) ; Si ErreurDétectée Alors ...`
                 * fonctionne. Cf.  +  ligne 17 (contrat
                 * user) +  (correction du RAZ trop large). */
                owl_clear_error();
                if (!call_value(vm_peek(argCount), argCount)) {
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_RETURN): {
                Value result = vm_pop();
                /* Pop any exception handlers installed during this frame's
                 * execution (general-form QUAND EXCEPTION without a matching
                 * OP_END_TRY at end of block). */
                while (exceptionCount > frame->exceptionBase) exceptionCount--;
                /*  : rembobine la pile de sauvegarde du contexte au
                 * niveau d'entree de la frame. La forme generale de handler
                 * (QUAND EXCEPTION ... FIN) installe un OP_TRY mais termine la
                 * frame via OP_RETURN sans OP_END_TRY ni OP_EXC_RESTORE : sans
                 * ce rembobinage le snapshot fuiterait et le contexte resterait
                 * stale apres retour. Restaure le contexte englobant du niveau
                 * d'entree (contexte du top-level → vide, sinon handler appelant). */
                if (g_exec_ctx_cur->excSaveDepth > frame->excSaveBase) {
                    g_exec_ctx_cur->excSaveDepth = frame->excSaveBase;
                    exc_context_apply(&g_exec_ctx_cur->excCtxSaveStack[g_exec_ctx_cur->excSaveDepth]);
                }
                vm.frameCount--;
                if (vm.frameCount <= exitFrame) {
                    /* Top-level script exit (exitFrame == 0 ET PAS une frontiere
                     * reentrante) pop le sentinel ObjFunction du script et jette
                     * la valeur de retour. Un retour a une frontiere reentrante
                     * (vm_call_reentrant) doit toujours PUSH le resultat pour la
                     * native appelante, meme quand la frontiere est a frame 0 :
                     * une native lancee top-level (vm.frameCount == 0, ex. Ouvre
                     * qui resout une prop dynamique) ne doit pas etre confondue
                     * avec l'exit du script. */
                    if (exitFrame == 0 && !reentrantBoundary) {
                        vm_pop(); /* pop the script function */
                    } else {
                        vm.stackTop = frame->slots;
                        vm_push(result);
                    }
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                vm_push(result);
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_POP):
                vm_pop();
                DISPATCH();

            VM_CASE(OP_POPN): {
                uint8_t n = READ_BYTE();
                vm.stackTop -= n;
                DISPATCH();
            }

            VM_CASE(OP_DUP):
                vm_push(vm_peek(0));
                DISPATCH();

            VM_CASE(OP_SWAP): {
                /* Échange les deux valeurs au sommet de la pile.
                 * Pile [a, b] -> [b, a]. */
                Value b = vm_pop();
                Value a = vm_pop();
                vm_push(b);
                vm_push(a);
                DISPATCH();
            }

            VM_CASE(OP_PRINT): {
                /* Auto-print opcode (REPL `--repl` mode). Skip NIL so that
                 * statements whose expression is a void-returning call
                 * (Trace, Info, etc. — they return NIL_VAL) don't produce
                 * an extra empty/zero line after their own output. A user
                 * typing `Null` bare on a line gets no output either, an
                 * accepted trade-off. */
                Value v = vm_pop();
                if (!IS_NIL(v)) {
                    value_print(v);
                    printf("\n");
                    fflush(stdout);
                }
                DISPATCH();
            }

            /* --- Array operations --- */

            VM_CASE(OP_ARRAY): {
                ObjArray* array = obj_new_array();
                vm_push(OBJ_VAL(array));
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_PRESIZE): {
                /* Create an array pre-filled with N NIL values (canon-style fixed-size decl).
                 * Per-type zero-initialisation (0 for int, 0.0 for real, "" for string...)
                 * is handled lazily: NIL coerces to the zero value of the target type on read
                 * via value_to_real/value_to_bool etc., matching OWL's Null=zero-value rule.
                 *   : push array AVANT les value_array_write
                 * qui peuvent grow → GC. Sans ce push array est unrooted
                 * pendant les grows et peut etre collecte. */
                uint8_t n = READ_BYTE();
                ObjArray* array = obj_new_array();
                vm_push(OBJ_VAL(array));
                for (int i = 0; i < n; i++) {
                    value_array_write(&array->elements, NIL_VAL);
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_PRESIZE_ND): {
                /* Nested N-D array: array-of-arrays-of-...-of-NIL (option B).
                 * build_nested_array() laisse déjà arr sur la stack (GC root).
                 * On ne re-push pas — sinon stack imbalance qui pollue les
                 * opcodes suivants (POUR, comparaisons…). */
                uint8_t dimCount = READ_BYTE();
                uint8_t dims[10] = {0};
                for (int k = 0; k < dimCount && k < 10; k++) {
                    dims[k] = READ_BYTE();
                }
                (void)build_nested_array(dims, dimCount);
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_GROWTH): {
                uint8_t mode = READ_BYTE();
                if (IS_ARRAY(vm_peek(0))) {
                    AS_ARRAY(vm_peek(0))->growthMode = (int)mode;
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_TYPE_STRUCT): {
                /* Tag top-of-stack array with its declared struct element type.
                 * Operand: constant index of the struct name string. Used by Ajoute
                 * to coerce `[v1, v2, ...]` literals into struct instances. */
                Value nameVal = READ_CONSTANT();
                if (IS_STRING(nameVal) && IS_ARRAY(vm_peek(0))) {
                    ObjArray* arr = AS_ARRAY(vm_peek(0));
                    arr->elementStructType = AS_STRING(nameVal);
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_MARK_GROUP): {
                /*  — tag top-of-stack ObjArray comme Groupe.
                 * Emis apres l'init d'un `GR_X est un Groupe [= [...]]`.
                 * Le tag est_group active le dispatch broadcast dans
                 * OP_STRUCT_SET (et lecture de `champ..Groupe` cote
                 * ObjUiControl). owner_window est rempli plus tard quand
                 * c'est utile (lecture .Groupe parcourt vm.windows). */
                if (IS_ARRAY(vm_peek(0))) {
                    AS_ARRAY(vm_peek(0))->is_group = true;
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_FILL_DEFAULT): {
                /* Tag l'array avec son elementPrimKind (persistant pour les
                 * extensions futures) ET fill les cells NIL_VAL existantes
                 * avec la zero-value du type (canon : tableau de N <type>
                 * -> cells = valeur par défaut, jamais Null). */
                uint8_t kind = READ_BYTE();
                if (IS_ARRAY(vm_peek(0))) {
                    ObjArray* arr = AS_ARRAY(vm_peek(0));
                    arr->elementPrimKind = (int)kind;
                    Value defaultVal = obj_array_default_for_kind((int)kind);
                    if (!IS_NIL(defaultVal)) {
                        for (int i = 0; i < arr->elements.count; i++) {
                            if (IS_NIL(arr->elements.values[i])) {
                                arr->elements.values[i] = defaultVal;
                            }
                        }
                    }
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_FILL_STRUCT): {
                /* `tableau de N <Structure>` / `tableau de N <TypeObjet>` :
                 * canon — les N cells sont des instances CONSTRUITES (champs
                 * a leur zero-value), jamais Null. Chaque cell recoit un clone
                 * DISTINCT du template : un clone unique partage ferait
                 * apparaitre l'ecriture de t[1] dans t[2].
                 *
                 * Template : vm.types (types du registry, httpRequete, Couleur)
                 * puis vm.globals (structures utilisateur). Une resolution qui
                 * ne rend pas un ObjInstance -> no-op, l'array garde ses cells
                 * Null. Un nom inconnu n'arrive jamais ici : la declaration est
                 * refusee des la compilation. Inventaire des cas au site
                 * d'emission (compiler.c, emission de OP_ARRAY_FILL_STRUCT). */
                ObjString* typeName = READ_STRING();
                Value tpl;
                if (IS_ARRAY(vm_peek(0))
                    && (vm_types_canon_get(typeName, &tpl)
                        || vm_globals_canon_get(typeName, &tpl, NULL))
                    && IS_INSTANCE(tpl)) {
                    ObjArray* arr = AS_ARRAY(vm_peek(0));
                    /*  — la boucle alloue N instances ; l'array reste
                     * roote par la stack (peek 0) et le template par le push
                     * explicite. Chaque clone est stocke dans l'array (donc
                     * atteignable) avant l'allocation suivante. */
                    vm_push(tpl);
                    for (int i = 0; i < arr->elements.count; i++) {
                        if (!IS_NIL(arr->elements.values[i])) continue;
                        Value cloneArg = tpl;
                        Value cell = native_struct_clone(1, &cloneArg);
                        arr->elements.values[i] = cell;
                    }
                    vm_pop();
                }
                DISPATCH();
            }

            VM_CASE(OP_STRUCT_FIELD_ORDER): {
                /* Append a declared field name to top-of-stack ObjInstance.fieldOrder.
                 * Used during struct template construction to preserve field order. */
                Value nameVal = READ_CONSTANT();
                if (IS_STRING(nameVal) && IS_INSTANCE(vm_peek(0))) {
                    ObjInstance* inst = AS_INSTANCE(vm_peek(0));
                    if (inst->fieldOrder == NULL) {
                        inst->fieldOrder = obj_new_array();
                    }
                    value_array_write(&inst->fieldOrder->elements, nameVal);
                }
                DISPATCH();
            }

            /*  — meme corps que OP_ARRAY_GET : seule la branche
             * proxy-colonne diverge, en rendant le descripteur de
             * cellule plutot que sa valeur. `instruction` porte l'opcode
             * courant dans les deux modes de dispatch (goto calcule et
             * switch), c'est lui qui tranche. */
            VM_CASE(OP_TABLE_CELL_DESC):   /* FALLTHROUGH */
            VM_CASE(OP_ARRAY_GET_OR_CREATE_OBJ): opAutoVivKind = 1; goto L_array_get_entry;
            VM_CASE(OP_ARRAY_GET_OR_CREATE_ARR): opAutoVivKind = 2; goto L_array_get_entry;
            VM_CASE(OP_ARRAY_GET): opAutoVivKind = 0;
            L_array_get_entry: {
                /* Stack: [array_or_combi] [index_or_flag].
                 * Peek to keep operands rooted during any intermediate alloc
                 * (obj_copy_string below can trigger GC). Pop only at each
                 * vm_push/error exit. */
                Value arrVal   = vm_peek(1);
                Value indexVal = vm_peek(0);
                #define IDX_RESULT(v) do { Value _r=(v); vm_pop(); vm_pop(); vm_push(_r); } while(0)
                /*  — aucun pop manuel des operandes sur le chemin d'erreur,
                 * meme raison que IDX_THROW_RECOVER : owl_emit_runtime_error
                 * reamorce entierement la pile sur une erreur NON rattrapee
                 * (stackTop = vm.stack, frameCount = 0). Popper apres ce reset ferait
                 * passer stackTop SOUS la base -> underflow qui corrompt le prochain
                 * usage de la VM (segfault a la prochaine collecte, exhibe par la
                 * recuperation d'erreur de l'executeur HTTP sous poison). Sur le
                 * chemin rattrape, handle_runtime_exception reamorce stackTop au
                 * handler et DISPATCH (les pops n'etaient jamais atteints). */
                #define IDX_ERROR_RET() do { \
                    OWL_RT_RECOVER_DISPATCH(); \
                    return INTERPRET_RUNTIME_ERROR; \
                } while(0)
                /*  — throw rattrapable propre depuis un opcode (pont
                 * vm_throw_from_erreur, jumeau de OP_BUFFER_BYTE_SET) : capture le
                 * contexte d'exception PUIS route vers le handler QUAND EXCEPTION
                 * en scope (handle_runtime_exception reamorce stackTop, pas de
                 * pop manuel) ou, non capturee, emet E_RT_UNHANDLED_EXCEPTION et
                 * sort. Aucun pop manuel des operandes sur le chemin non capture :
                 * owl_emit_runtime_error reamorce entierement la pile
                 * (stackTop = vm.stack, frameCount = 0). Un vm_pop() apres ce reset
                 * ferait passer stackTop SOUS la base — underflow silencieux qui
                 * corrompt le scenario suivant dans le runner (segfault). Aligne
                 * sur la branche native de call_value (uncaught -> return sans pop). */
                #define IDX_THROW_RECOVER() do { \
                    if (throwPending) { \
                        throwPending = false; \
                        if (handle_runtime_exception()) { \
                            frame = &vm.frames[vm.frameCount - 1]; \
                            DISPATCH(); \
                        } \
                        owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION, \
                            "message", \
                            currentException ? currentException->chars : "unknown", \
                            NULL); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                } while(0)

                /*  — reference ligne/colonne : doc..Ligne[l] / doc..Colonne[c]. */
                if (stdlib_xls_is_ref(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    int idx = IS_INTEGER(indexVal) ? (int)AS_INTEGER(indexVal)
                                                   : (int)AS_REAL(indexVal);
                    Value bound = stdlib_xls_ref_bind_index(arrVal, idx);
                    IDX_RESULT(bound);
                    DISPATCH();
                }

                /* Indexation widget : widget[i] dispatche via le hook
                 * owl_ui_control_get_subitem (registered par stdlib_ui). En
                 * V1, supporte le Champ Selecteur (retourne un proxy d'option
                 * avec back-ref + fields snapshot). NIL_VAL si pas d'indexation
                 * pour ce kind. */
                if (IS_UI_CONTROL(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    int idx = IS_INTEGER(indexVal) ? (int)AS_INTEGER(indexVal)
                                                   : (int)AS_REAL(indexVal);
                    Value out = owl_ui_control_get_subitem(AS_UI_CONTROL(arrVal), idx);
                    if (!IS_NIL(out)) {
                        IDX_RESULT(out);
                        DISPATCH();
                    }
                    /* sinon, fall through aux autres types — qui rejetteront. */
                }

                /*  — indexation ZR[i] /  — Table[ligne] quand
                 * le receveur est un proxy global ObjUiControlRef (cas DSL
                 * Fenetre : `ZR`/`TBL` global resout vers son
                 * ObjUiControlInstance dans la fenetre courante). Le hook
                 * retourne un proxy de rep (CONTAINER en GUI ZR, ObjInstance
                 * rep de ligne en headless / pour Table) sur lequel STRUCT_GET
                 * route les cellules par nom de sous-controle (ZR) ou de
                 * colonne (Table). Hors-bornes : erreur claire alignee sur
                 * l'indexation tableau (pas le fall-through generique
                 * "indexable" qui masquerait la vraie cause). */
                if (IS_UI_CONTROL_REF(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    ObjUiControlInstance* uici =
                        resolve_ui_ref(AS_UI_CONTROL_REF(arrVal));
                    if (uici != NULL && uici->decl != NULL &&
                        (uici->decl->kind == OWL_CONTROL_ZONE_REPETEE ||
                         uici->decl->kind == OWL_CONTROL_TABLE ||
                         uici->decl->kind == OWL_CONTROL_SELECTEUR ||
                         uici->decl->kind == OWL_CONTROL_INTERRUPTEUR)) {
                        int idx = IS_INTEGER(indexVal) ? (int)AS_INTEGER(indexVal)
                                                       : (int)AS_REAL(indexVal);
                        int count = (uici->widget != NULL)
                            ? owl_ui_widget_items_count((int)uici->decl->kind,
                                                        uici->widget)
                            : owl_ui_headless_widget_items_count_uici(uici);
                        /*  — Selecteur / Interrupteur : meme routage que
                         * ZR/Table, mais le hors-plage a son propre message. Le
                         * vocabulaire « ligne » est faux pour une option, et le
                         * libelle porte le nom declare : dans une fenetre a trois
                         * selecteurs, c'est la seule information qui dit lequel a
                         * deborde. */
                        bool isOpt =
                            (uici->decl->kind == OWL_CONTROL_SELECTEUR ||
                             uici->decl->kind == OWL_CONTROL_INTERRUPTEUR);
                        if (isOpt && (idx < 1 || idx > count)) {
                            char idxBuf[32], maxBuf[16], ctrlBuf[160];
                            snprintf(idxBuf, sizeof idxBuf, "%d", idx);
                            snprintf(maxBuf, sizeof maxBuf, "%d", count);
                            const char* kindFr = owl_control_kind_name_fr(
                                (OwlControlKind)uici->decl->kind);
                            if (uici->decl->name != NULL)
                                snprintf(ctrlBuf, sizeof ctrlBuf, "%s %s",
                                         kindFr, uici->decl->name);
                            else
                                snprintf(ctrlBuf, sizeof ctrlBuf, "%s", kindFr);
                            owl_emit_native_error(E_RT_UI_OPTION_OOB,
                                                  ctrlBuf,
                                                  "control", ctrlBuf,
                                                  "index", idxBuf,
                                                  "max", maxBuf, NULL);
                            Value oob = owl_ui_control_instance_get_subitem(
                                uici, idx);
                            IDX_RESULT(oob);
                            DISPATCH();
                        }
                        if (idx < 1 || idx > count) {
                            char idxBuf[32], maxBuf[16];
                            snprintf(idxBuf, sizeof idxBuf, "%d", idx);
                            snprintf(maxBuf, sizeof maxBuf, "%d", count);
                            /*  (Table) +  (ZR) — lecture cellule
                             * hors plage = soft-error recuperable ( /
                             * canon : lecture hors-plage ne crashe pas). Pose
                             * ErreurDetectee et renvoie une rep de ligne vide
                             * typee (TBL[oob].COL / ZR[oob].champ reste lisible
                             * = chaine vide), la VM continue. Harmonise ZR sur
                             * Table : la divergence / est
                             * resorbee, plus de hard-stop E_RT_ARRAY_INDEX_OOB. */
                            const char* ctrl =
                                (uici->decl->kind == OWL_CONTROL_TABLE)
                                    ? "Table" : "ZoneRepetee";
                            owl_emit_native_error(E_RT_UI_ROW_OOB,
                                                  ctrl,
                                                  "control", ctrl,
                                                  "index", idxBuf,
                                                  "max", maxBuf, NULL);
                            Value oob = owl_ui_control_instance_get_subitem(
                                uici, idx);
                            IDX_RESULT(oob);
                            DISPATCH();
                        }
                        Value out = owl_ui_control_instance_get_subitem(uici, idx);
                        IDX_RESULT(out);
                        DISPATCH();
                    }
                    /* uici NULL (pas de fenetre / champ absent) : resolve_ui_ref
                     * a deja emis l'erreur. On laisse tomber au reject final. */
                }

                /*  — col-first `TBL.COL_Nom[ligne]` : arrVal est le
                 * proxy-colonne (ObjInstance porteur des marqueurs poses par
                 * OP_STRUCT_GET). On re-resout la Table via la ref, on applique
                 * la MEME borne + soft-error que la forme ligne-first (,
                 * branche IS_UI_CONTROL_REF ci-dessus) puis on lit la cellule
                 * <COL_Nom> de la rep de ligne -> symetrie stricte avec
                 * TBL[ligne].COL_Nom. Intercept avant le test combi (qui
                 * matcherait IS_INSTANCE). */
                if (IS_INSTANCE(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    ObjInstance* px = AS_INSTANCE(arrVal);
                    ObjString* refKey = table_find_key_case_insensitive(
                        &px->fields, OWL_TABLE_COLPROXY_REF_KEY);
                    Value refV, colV;
                    if (refKey != NULL
                        && table_get(&px->fields, refKey, &refV)
                        && IS_UI_CONTROL_REF(refV)) {
                        ObjString* colKey = table_find_key_case_insensitive(
                            &px->fields, OWL_TABLE_COLPROXY_COL_KEY);
                        if (colKey != NULL
                            && table_get(&px->fields, colKey, &colV)
                            && IS_STRING(colV)) {
                            ObjUiControlInstance* uici =
                                resolve_ui_ref(AS_UI_CONTROL_REF(refV));
                            if (uici != NULL && uici->decl != NULL &&
                                uici->decl->kind == OWL_CONTROL_TABLE) {
                                int idx = IS_INTEGER(indexVal)
                                    ? (int)AS_INTEGER(indexVal)
                                    : (int)AS_REAL(indexVal);
                                /*  — la borne de ligne se valide contre
                                 * le store ligne-first, deja peuple
                                 * par TableAjouteLigne. Pendant FinInitialisation
                                 * le widget Qt existe mais n'est synchronise
                                 * qu'apres le retour du handler (0 ligne) : s'y
                                 * fier levait une erreur « hors plage » parasite
                                 * sur un override pourtant valide. On prend le
                                 * max -> le store couvre le desync, le widget
                                 * reste un plancher s'il devance. */
                                int storeCount =
                                    owl_ui_headless_widget_items_count_uici(uici);
                                int widgetCount = (uici->widget != NULL)
                                    ? owl_ui_widget_items_count(
                                        (int)uici->decl->kind, uici->widget)
                                    : 0;
                                int count = widgetCount > storeCount
                                    ? widgetCount : storeCount;
                                if (idx < 1 || idx > count) {
                                    char idxBuf[32], maxBuf[16];
                                    snprintf(idxBuf, sizeof idxBuf, "%d", idx);
                                    snprintf(maxBuf, sizeof maxBuf, "%d", count);
                                    owl_emit_native_error(E_RT_UI_ROW_OOB,
                                                          "Table",
                                                          "control", "Table",
                                                          "index", idxBuf,
                                                          "max", maxBuf, NULL);
                                }
                                /* get_subitem rend la rep de ligne (vide typee
                                 * si OOB, cf. ) ; on lit la colonne. */
                                Value row = owl_ui_control_instance_get_subitem(
                                    uici, idx);
                                /*  — `TABLE_X.COL_Y[ligne]..Couleur` :
                                 * le compilateur a vu la propriete derriere le
                                 * `]` et demande le DESCRIPTEUR. Meme rep, meme
                                 * cle canon de colonne que
                                 * `TABLE_X[ligne][colonne]` : les deux graphies
                                 * ecrivent et relisent le meme casier. */
                                if (instruction == OP_TABLE_CELL_DESC) {
                                    vm_push(row);   /* root : alloc dessous */
                                    if (!IS_INSTANCE(row)) {
                                        vm_pop();
                                        vm_push(OBJ_VAL(obj_new_instance(NULL)));
                                    }
                                    ObjInstance* cellDesc =
                                        vm_table_cell_descriptor(vm_peek(0),
                                                                 AS_CSTRING(colV));
                                    vm_pop();
                                    IDX_RESULT(OBJ_VAL(cellDesc));
                                    DISPATCH();
                                }
                                Value cell = NIL_VAL;
                                if (IS_INSTANCE(row)) {
                                    ObjString* ck =
                                        table_find_key_case_insensitive(
                                            &AS_INSTANCE(row)->fields,
                                            AS_CSTRING(colV));
                                    if (ck != NULL)
                                        table_get(&AS_INSTANCE(row)->fields,
                                                  ck, &cell);
                                }
                                IDX_RESULT(cell);
                                DISPATCH();
                            }
                        }
                    }
                }

                /*  — `TABLE_X[ligne][colonne]` : la rep de ligne (taguee
                 * `$tblsrc`) indexee par un NUMERO de colonne rend un
                 * DESCRIPTEUR de cellule, receveur des proprietes de mise en
                 * forme. La VALEUR de la cellule reste lue par
                 * `TABLE_X[ligne].COL_Nom` ou `TABLE_X.COL_Nom[ligne]`
                 * : un ObjInstance ne peut pas etre a la fois un
                 * descripteur et un scalaire.
                 *
                 * C'est le seul endroit ou l'uici de la Table et l'indice de
                 * colonne coexistent — d'ou la resolution du nom de colonne ici
                 * plutot qu'a l'ecriture. Intercept apres le proxy-colonne et
                 * avant le test combinaison (qui matcherait IS_INSTANCE).
                 *
                 * Rendre un DESIGNATEUR (et non une valeur) est la lecture canon
                 * de cette forme, pas une divergence : la doc ne la mentionne
                 * que sur les deux fiches de proprietes (2510071 Couleur,
                 * 2510022 CouleurFond), et pour « designer la cellule » — « il
                 * est necessaire d'utiliser la syntaxe
                 * NomTable[NumLigne][NumColonne] pour designer la cellule ».
                 * Elle n'est nulle part documentee comme lecteur de valeur
                 * autonome. Ne pas « reparer » ce chemin en le faisant rendre le
                 * contenu de la cellule : ce serait casser la mise en forme. */
                if (IS_INSTANCE(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    ObjInstance* rep = AS_INSTANCE(arrVal);
                    ObjString* srcKey = table_find_key_case_insensitive(
                        &rep->fields, OWL_TABLE_REP_SRC_KEY);
                    Value srcV;
                    if (srcKey != NULL
                        && table_get(&rep->fields, srcKey, &srcV)
                        && IS_UI_CONTROL_INSTANCE(srcV)) {
                        ObjUiControlInstance* uici =
                            AS_UI_CONTROL_INSTANCE(srcV);
                        int ncol = (uici->decl != NULL)
                                       ? uici->decl->column_count : 0;
                        int cidx = IS_INTEGER(indexVal)
                                       ? (int)AS_INTEGER(indexVal)
                                       : (int)AS_REAL(indexVal);
                        const char* cname =
                            (cidx >= 1 && cidx <= ncol)
                                ? uici->decl->columns[cidx - 1].name : NULL;
                        if (cname == NULL) {
                            /* Soft-error alignee sur la ligne hors plage
                             * (, ) : ErreurDetectee posee, la VM
                             * continue. Le retour est un descripteur BRANCHE SUR
                             * LE VIDE — pas Null : sur Null, le `..Couleur` qui
                             * suit mourrait sur « acces a un champ sur une
                             * valeur Null », le message meme qui a egare
                             * l'utilisateur au depart. Ici la lecture rend Null
                             * et l'ecriture se perd, sans second diagnostic. */
                            char idxBuf[32], maxBuf[16];
                            snprintf(idxBuf, sizeof idxBuf, "%d", cidx);
                            snprintf(maxBuf, sizeof maxBuf, "%d", ncol);
                            owl_emit_native_error(E_RT_UI_TABLE_COLUMN_OOB,
                                                  "Table",
                                                  "index", idxBuf,
                                                  "max", maxBuf, NULL);
                            if (instruction != OP_TABLE_CELL_DESC) {
                                /*  — lecture nue hors colonne : valeur
                                 * vide, pas de descripteur orphelin. */
                                IDX_RESULT(NIL_VAL);
                                DISPATCH();
                            }
                            ObjInstance* vide = obj_new_instance(NULL);
                            vm_push(OBJ_VAL(vide));
                            ObjInstance* orphan =
                                vm_table_cell_descriptor(OBJ_VAL(vide), "");
                            vm_pop();
                            IDX_RESULT(OBJ_VAL(orphan));
                            DISPATCH();
                        }
                        /* Cle canon (accents strippes) : la meme que celle posee
                         * par la construction de la rep. */
                        char colKey[256];
                        int klen = scanner_strip_accents(cname,
                                                        (int)strlen(cname),
                                                        colKey,
                                                        (int)sizeof(colKey));
                        if (klen > 0 && klen < (int)sizeof(colKey)) {
                            colKey[klen] = '\0';
                            if (instruction == OP_TABLE_CELL_DESC) {
                                /* arrVal est un operande peeke : deja root. */
                                ObjInstance* cell =
                                    vm_table_cell_descriptor(arrVal, colKey);
                                IDX_RESULT(OBJ_VAL(cell));
                                DISPATCH();
                            }
                            /*  — forme NUE (aucun `..Prop` derriere) : la
                             * VALEUR scalaire de la cellule, symetrie stricte avec
                             * TABLE[l].COL_nom. Le descripteur ne sert
                             * qu'a la mise en forme (OP_TABLE_CELL_DESC). */
                            Value cellVal = NIL_VAL;
                            ObjString* ck = table_find_key_case_insensitive(
                                &rep->fields, colKey);
                            if (ck != NULL)
                                table_get(&rep->fields, ck, &cellVal);
                            IDX_RESULT(cellVal);
                            DISPATCH();
                        }
                    }
                }

                /* Combination flag test: combi[Flag] -> boolean */
                if (IS_INSTANCE(arrVal)) {
                    ObjInstance* inst = AS_INSTANCE(arrVal);
                    ObjString* valKey = obj_copy_string("Valeur", 6);
                    Value instValeur;
                    if (table_get(&inst->fields, valKey, &instValeur)) {
                        int64_t flagBits = 0;
                        if (IS_INTEGER(indexVal)) {
                            flagBits = AS_INTEGER(indexVal);
                        } else if (IS_INSTANCE(indexVal)) {
                            ObjInstance* flagInst = AS_INSTANCE(indexVal);
                            Value flagVal;
                            if (table_get(&flagInst->fields, valKey, &flagVal) && IS_INTEGER(flagVal)) {
                                flagBits = AS_INTEGER(flagVal);
                            }
                        }
                        int64_t combiVal = 0;
                        if (IS_INTEGER(instValeur)) combiVal = AS_INTEGER(instValeur);
                        IDX_RESULT(BOOL_VAL((combiVal & flagBits) == flagBits && flagBits != 0));
                        DISPATCH();
                    }
                }

                if (IS_JSON(arrVal)) {
                    ObjJson* json = AS_JSON(arrVal);
                    Value out = NIL_VAL;
                    if (json->jtype == JSON_T_ARRAY && (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                        int64_t idx = IS_INTEGER(indexVal) ? AS_INTEGER(indexVal) : (int64_t)AS_REAL(indexVal);
                        if (idx >= 1 && idx <= json->as.arrayVal.count) {
                            out = obj_json_to_value(json->as.arrayVal.items[idx - 1]);
                        } else if (opAutoVivKind != 0) {
                            /*  — descente LVALUE : l'element demande n'existe
                             * pas. Croissance d'un seul element a la fois (regle
                             * tableau WL) : idx == count+1 cree le conteneur du type
                             * requis par le segment suivant ; tout saut leve une
                             * exception rattrapable (le tableau reste tel quel). */
                            if (idx == json->as.arrayVal.count + 1) {
                                ObjJson* child = obj_new_json(
                                    opAutoVivKind == 2 ? JSON_T_ARRAY : JSON_T_OBJECT);
                                vm_push(OBJ_VAL(child)); /* root avant push (realloc pur, mais canon) */
                                obj_json_array_push(json, child);
                                vm_pop();
                                IDX_RESULT(OBJ_VAL(child));
                                DISPATCH();
                            }
                            char idxBuf[32], maxBuf[16];
                            snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                            snprintf(maxBuf, sizeof maxBuf, "%d", json->as.arrayVal.count);
                            owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                                   "index", idxBuf,
                                                   "max", maxBuf, NULL);
                            IDX_ERROR_RET();
                        }
                    } else if (json->jtype == JSON_T_OBJECT && IS_STRING(indexVal)) {
                        ObjJson* child = obj_json_object_get(json, AS_STRING(indexVal));
                        if (child) out = obj_json_to_value(child);
                    } else if (json->jtype == JSON_T_OBJECT && (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                        /*  — indexation positionnelle des membres d'un
                         * objet JSON (base 1), utilisee par POUR TOUT pour
                         * iterer les valeurs des membres. Aligne l'objet JSON
                         * sur le tableau associatif, ou obj[i] rend la i-eme
                         * valeur. La cle correspondante est exposee par
                         * OP_ITER_KEY. */
                        int64_t idx = IS_INTEGER(indexVal) ? AS_INTEGER(indexVal) : (int64_t)AS_REAL(indexVal);
                        if (idx >= 1 && idx <= json->as.objectVal.count) {
                            out = obj_json_to_value(json->as.objectVal.values[idx - 1]);
                        }
                    }
                    IDX_RESULT(out);
                    DISPATCH();
                }

                if (IS_COLLECTION(arrVal)) {
                    ObjCollection* coll = AS_COLLECTION(arrVal);
                    int64_t idx = 0;
                    if (IS_INTEGER(indexVal)) idx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) idx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_COLLECTION_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (idx < 1 || idx > coll->elements.count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", coll->elements.count);
                        owl_emit_runtime_error(E_RT_COLLECTION_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        IDX_ERROR_RET();
                    }
                    IDX_RESULT(coll->elements.values[idx - 1]);
                    DISPATCH();
                }

                if (IS_ASSOC_ARRAY(arrVal)) {
                    ObjAssocArray* a = AS_ASSOC_ARRAY(arrVal);
                    if (IS_STRING(indexVal)) {
                        int assocExists = 0;
                        Value got = obj_assoc_get(a, AS_STRING(indexVal),
                                                  &assocExists);
                        /*  — auto-vivification objet : lire une clé
                         * absente d'un tableau associatif de type NON primitif
                         * crée l'entrée (canon, symétrique à l'écriture ;
                         * ..Occurrence passe à 1). Le template est résolu par
                         * nom (vm.types puis vm.globals) et deep-cloné —
                         * cellules indépendantes, même mécanique que .
                         * Un nom qui ne résout pas en ObjInstance -> pas de
                         * vivification (défaut rendu tel quel). Les scalaires
                         * (valueTypeName == NULL) tombent gratuitement ici : la
                         * lecture rend le défaut SANS créer (asymétrie canon). */
                        if (!assocExists && a->valueTypeName != NULL) {
                            Value tpl;
                            if ((vm_types_canon_get(a->valueTypeName, &tpl)
                                 || vm_globals_canon_get(a->valueTypeName, &tpl,
                                                         NULL))
                                && IS_INSTANCE(tpl)) {
                                Value cloneArg = tpl;
                                Value cell = native_struct_clone(1, &cloneArg);
                                /*  : cell fresh non-rooté ; push avant
                                 * obj_assoc_set (peut grow -> GC). arrVal /
                                 * indexVal restent rootés via peek stack. */
                                if (IS_OBJ(cell)) vm_push(cell);
                                obj_assoc_set(a, AS_STRING(indexVal), cell);
                                if (IS_OBJ(cell)) vm_pop();
                                IDX_RESULT(cell);
                                DISPATCH();
                            }
                        }
                        IDX_RESULT(got);
                        DISPATCH();
                    }
                    if (IS_INTEGER(indexVal) || IS_REAL(indexVal)) {
                        int64_t idx = IS_INTEGER(indexVal) ? AS_INTEGER(indexVal)
                                                           : (int64_t)AS_REAL(indexVal);
                        if (idx < 1 || idx > a->count) {
                            IDX_RESULT(obj_assoc_default_for_kind(a->valueKind));
                        } else {
                            IDX_RESULT(a->values[idx - 1]);
                        }
                        DISPATCH();
                    }
                    owl_emit_runtime_error(E_RT_ASSOC_KEY_STRING_V1, NULL);
                    IDX_ERROR_RET();
                }

                /*  lot C : chaineMultilingue indexable par
                 * string (tag BCP 47 libre ou valeur d'une constante
                 * langueXxx — qui resout en string "fr-FR" via vm.globals
                 * cote lot A). Lookup via multilingue_resolve qui ajoute
                 * le fallback langue source (decision v1 : pas de cascade
                 * hierarchique BCP 47, juste fallback direct). */
                if (IS_MULTILINGUE(arrVal)) {
                    ObjMultilingue* m = AS_MULTILINGUE(arrVal);
                    if (!IS_STRING(indexVal)) {
                        owl_emit_runtime_error(E_RT_ASSOC_KEY_STRING_V1, NULL);
                        IDX_ERROR_RET();
                    }
                    IDX_RESULT(multilingue_resolve(m, AS_STRING(indexVal)->chars,
                                                   owl_i18n_langue_source()));
                    DISPATCH();
                }

                /*  — lecture indexee d'un Vecteur : base 1, element
                 * = reel. Hors borne = erreur recuperable (meme code + meme
                 * routage QUAND EXCEPTION que le tableau, aucune corruption). */
                if (IS_VECTOR(arrVal)) {
                    ObjVector* vec = AS_VECTOR(arrVal);
                    int64_t vidx = 0;
                    if (IS_INTEGER(indexVal)) vidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) vidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (vidx < 1 || vidx > vec->count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)vidx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", vec->count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        IDX_ERROR_RET();
                    }
                    IDX_RESULT(REAL_VAL(vec->data[vidx - 1]));
                    DISPATCH();
                }

                /*  — premier crochet d'une Matrice : `m[i]` produit
                 * une vue de ligne (base 1) qui portera le second crochet
                 * `[j]`. Hors borne de ligne = erreur recuperable (meme code +
                 * routage QUAND EXCEPTION que le tableau/Vecteur). `arrVal` reste
                 * peeke (rooted) pendant obj_new_matrix_row -> alloc GC-sure. */
                if (IS_MATRIX(arrVal)) {
                    ObjMatrix* mat = AS_MATRIX(arrVal);
                    int64_t ridx = 0;
                    if (IS_INTEGER(indexVal)) ridx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) ridx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (ridx < 1 || ridx > mat->rows) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)ridx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", mat->rows);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        IDX_ERROR_RET();
                    }
                    ObjMatrixRow* rv = obj_new_matrix_row(mat, (int)ridx);
                    IDX_RESULT(OBJ_VAL(rv));
                    DISPATCH();
                }

                /*  — second crochet : `m[i][j]` lit l'element (base 1)
                 * via la vue de ligne. Hors borne de colonne = erreur
                 * recuperable, matrice intacte. */
                if (IS_MATRIX_ROW(arrVal)) {
                    ObjMatrixRow* rv = AS_MATRIX_ROW(arrVal);
                    ObjMatrix* mat = rv->mat;
                    int64_t cidx = 0;
                    if (IS_INTEGER(indexVal)) cidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) cidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (cidx < 1 || cidx > mat->cols) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)cidx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", mat->cols);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        IDX_ERROR_RET();
                    }
                    IDX_RESULT(REAL_VAL(
                        mat->data[(rv->row - 1) * mat->cols + (cidx - 1)]));
                    DISPATCH();
                }

                /*  — crochet sur une vue de Tenseur N-D (rang >= 3) :
                 * `t..Valeur[i]` descend d'un axe. Une vue de rang 1 rend le
                 * reel ; une vue de rang k > 1 rend une sous-vue de rang k-1
                 * (offset avance de (i-1)*stride[0], premier axe consomme). Hors
                 * borne = erreur recuperable, meme routage QUAND EXCEPTION que
                 * le Vecteur/la Matrice. `arrVal` reste peeke (rooted) pendant
                 * obj_new_tensor_view -> alloc GC-sure (backing trace via la vue). */
                if (IS_TENSOR_VIEW(arrVal)) {
                    ObjTensorView* vw = AS_TENSOR_VIEW(arrVal);
                    int64_t tidx = 0;
                    if (IS_INTEGER(indexVal)) tidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) tidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    int dim0 = vw->shape[0];
                    if (tidx < 1 || tidx > dim0) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)tidx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", dim0);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        IDX_ERROR_RET();
                    }
                    int newOffset = vw->offset + (int)(tidx - 1) * vw->strides[0];
                    if (vw->ndim == 1) {
                        double* cells = tensor_cells(vw->backing);
                        IDX_RESULT(REAL_VAL(cells[newOffset]));
                        DISPATCH();
                    }
                    int subShape[OWL_TENSOR_MAX_RANK], subStrides[OWL_TENSOR_MAX_RANK];
                    for (int i = 1; i < vw->ndim; i++) {
                        subShape[i - 1]   = vw->shape[i];
                        subStrides[i - 1] = vw->strides[i];
                    }
                    ObjTensorView* sub = obj_new_tensor_view(
                        vw->backing, newOffset, vw->ndim - 1, subShape, subStrides);
                    IDX_RESULT(OBJ_VAL(sub));
                    DISPATCH();
                }

                /*  — indexation ds[i] : rend l'ImageÉtiquetée a la
                 * position i (base 1). Hors borne = erreur recuperable, routee
                 * QUAND EXCEPTION comme l'indexation tableau/Tenseur. */
                if (IS_DATASET(arrVal)) {
                    ObjDataset* ds = AS_DATASET(arrVal);
                    int64_t didx = 0;
                    if (IS_INTEGER(indexVal)) didx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) didx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (didx < 1 || didx > ds->count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)didx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", ds->count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        IDX_ERROR_RET();
                    }
                    IDX_RESULT(OBJ_VAL(ds->items[didx - 1]));
                    DISPATCH();
                }

                /*  — `mat[i]` sur un Tenseur : sous-Tenseur first-class de
                 * rang N-1, selection le long de l'axe 0 (base 1). Idiome
                 * numpy/torch, DISTINCT de l'acces brut `..Valeur[i]` (vue
                 * transitoire, IS_TENSOR_VIEW ci-dessus, inchange). Rang 1 = pas de
                 * sous-Tenseur, bornes hors [1, forme[0]] : exception RATTRAPABLE
                 * via le pont vm_throw_from_erreur (recuperable proprement dans le
                 * runner). `arrVal` reste peeke (rooted) pendant obj_tensor_select
                 * -> alloc GC-sure. */
                if (IS_TENSOR(arrVal)) {
                    ObjTensor* src = AS_TENSOR(arrVal);
                    int64_t tidx = 0;
                    if (IS_INTEGER(indexVal)) tidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) tidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        IDX_ERROR_RET();
                    }
                    if (src->ndim < 2) {
                        vm_throw_from_erreur(
                            "Un Tenseur de rang 1 n'a pas de sous-Tenseur ; "
                            "utiliser ..Valeur[i] pour l'élément scalaire.", 0);
                        IDX_THROW_RECOVER();
                    }
                    if (tidx < 1 || tidx > src->shape[0]) {
                        char msg[128];
                        snprintf(msg, sizeof msg,
                                 "Index de sous-Tenseur %lld hors limites "
                                 "(1..%d).", (long long)tidx, src->shape[0]);
                        vm_throw_from_erreur(msg, 0);
                        IDX_THROW_RECOVER();
                    }
                    ObjTensor* sub = obj_tensor_select(src, (int)tidx);
                    IDX_RESULT(OBJ_VAL(sub));
                    DISPATCH();
                }

                if (!IS_ARRAY(arrVal)) {
                    owl_emit_runtime_error(E_RT_INDEX_NEEDS_ARRAY_OR_COMBI, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) {
                            frame = &vm.frames[vm.frameCount - 1];
                            DISPATCH();
                        }
                    }
                    IDX_ERROR_RET();
                }
                ObjArray* array = AS_ARRAY(arrVal);
                int64_t idx = 0;
                if (IS_INTEGER(indexVal)) idx = AS_INTEGER(indexVal);
                else if (IS_REAL(indexVal)) idx = (int64_t)AS_REAL(indexVal);
                else {
                    owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                    IDX_ERROR_RET();
                }
                if (idx < 1 || idx > array->elements.count) {
                    char idxBuf[32], maxBuf[16];
                    snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                    snprintf(maxBuf, sizeof maxBuf, "%d", array->elements.count);
                    owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                           "index", idxBuf,
                                           "max", maxBuf, NULL);
                    if (exceptionCount > 0 && currentException != NULL) {
                        if (handle_runtime_exception()) {
                            frame = &vm.frames[vm.frameCount - 1];
                            DISPATCH();
                        }
                    }
                    IDX_ERROR_RET();
                }
                IDX_RESULT(array->elements.values[idx - 1]);
                #undef IDX_RESULT
                #undef IDX_ERROR_RET
                #undef IDX_THROW_RECOVER
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_SET): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ). */
                /* Stack: [array_or_combi] [index_or_flag] [value].
                 * Peek to keep operands rooted during any intermediate alloc
                 * (obj_copy_string, obj_new_json_*, etc.). */
                Value arrVal   = vm_peek(2);
                Value indexVal = vm_peek(1);
                Value val      = vm_peek(0);
                #define SET_RESULT(v) do { Value _r=(v); vm_pop(); vm_pop(); vm_pop(); vm_push(_r); } while(0)
                /*  — aucun pop manuel des operandes sur le chemin
                 * d'erreur, meme raison que IDX_ERROR_RET : owl_emit_runtime_error
                 * reamorce entierement la pile sur une erreur NON rattrapee
                 * (stackTop = vm.stack, frameCount = 0). Popper apres ce reset
                 * ferait passer stackTop SOUS la base -> underflow qui corrompt
                 * le prochain usage de la VM (segfault a la prochaine collecte).
                 * Sur le chemin rattrape, OWL_RT_RECOVER_DISPATCH reamorce au
                 * handler et DISPATCH (les pops n'etaient jamais atteints). */
                #define SET_ERROR_RET() do { \
                    OWL_RT_RECOVER_DISPATCH(); \
                    return INTERPRET_RUNTIME_ERROR; \
                } while(0)
                #define SET_TRY_CATCH() do { \
                    if (exceptionCount > 0 && currentException != NULL) { \
                        if (handle_runtime_exception()) { \
                            frame = &vm.frames[vm.frameCount - 1]; \
                            goto op_array_set_done; \
                        } \
                    } \
                } while(0)

                /*  — ecriture col-first `TBL.COL_Nom[ligne] = v` : arrVal
                 * est le proxy-colonne (, pose par OP_STRUCT_GET). Miroir
                 * STRICT de la lecture (OP_ARRAY_GET, meme proxy) et de l'ecriture
                 * ligne-first `TBL[ligne].COL_Nom = v` (, OP_STRUCT_SET) :
                 * meme borne + soft-error, meme rep de ligne du store, meme casier
                 * de cellule -> les deux graphies ecrivent la MEME cellule. Sans
                 * cette branche le proxy (un ObjInstance) tombait au rejet
                 * generique E_RT_INDEX_NEEDS_ARRAY_OR_COMBI, un message qui accuse
                 * une fausse cause (« seuls les tableaux... »). Intercept en tete :
                 * le combi-set plus bas matcherait aussi IS_INSTANCE. */
                if (IS_INSTANCE(arrVal) &&
                    (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                    ObjInstance* px = AS_INSTANCE(arrVal);
                    ObjString* refKey = table_find_key_case_insensitive(
                        &px->fields, OWL_TABLE_COLPROXY_REF_KEY);
                    Value refV, colV;
                    if (refKey != NULL
                        && table_get(&px->fields, refKey, &refV)
                        && IS_UI_CONTROL_REF(refV)) {
                        ObjString* colKey = table_find_key_case_insensitive(
                            &px->fields, OWL_TABLE_COLPROXY_COL_KEY);
                        if (colKey != NULL
                            && table_get(&px->fields, colKey, &colV)
                            && IS_STRING(colV)) {
                            ObjUiControlInstance* uici =
                                resolve_ui_ref(AS_UI_CONTROL_REF(refV));
                            if (uici != NULL && uici->decl != NULL &&
                                uici->decl->kind == OWL_CONTROL_TABLE) {
                                int idx = IS_INTEGER(indexVal)
                                    ? (int)AS_INTEGER(indexVal)
                                    : (int)AS_REAL(indexVal);
                                /*  — meme borne que la lecture : source
                                 * de verite = store ligne-first, pas
                                 * le compteur widget en retard pendant
                                 * FinInitialisation. max -> store si desync,
                                 * widget en plancher. */
                                int storeCount =
                                    owl_ui_headless_widget_items_count_uici(
                                        uici);
                                int widgetCount = (uici->widget != NULL)
                                    ? owl_ui_widget_items_count(
                                        (int)uici->decl->kind, uici->widget)
                                    : 0;
                                int count = widgetCount > storeCount
                                    ? widgetCount : storeCount;
                                if (idx < 1 || idx > count) {
                                    char idxBuf[32], maxBuf[16];
                                    snprintf(idxBuf, sizeof idxBuf, "%d", idx);
                                    snprintf(maxBuf, sizeof maxBuf, "%d", count);
                                    owl_emit_native_error(E_RT_UI_ROW_OOB,
                                                          "Table",
                                                          "control", "Table",
                                                          "index", idxBuf,
                                                          "max", maxBuf, NULL);
                                }
                                /* get_subitem rend la rep de ligne du store (ou
                                 * une rep vide typee hors plage : l'ecriture s'y
                                 * perd, symetrie ). On coerce comme la
                                 * forme ligne-first (identite pour une rep Table),
                                 * on ecrit la cellule <COL_Nom> sous sa cle canon
                                 * puis on reflete au widget monte. */
                                Value row =
                                    owl_ui_control_instance_get_subitem(uici, idx);
                                if (IS_INSTANCE(row)) {
                                    ObjInstance* rep = AS_INSTANCE(row);
                                    vm_push(row);  /* root : table_set peut grow */
                                    Value cellVal = owl_ui_zr_rep_coerce_cell(
                                        rep, AS_CSTRING(colV), val);
                                    if (IS_OBJ(cellVal)) vm_push(cellVal);
                                    ObjString* slot =
                                        table_find_key_case_insensitive(
                                            &rep->fields, AS_CSTRING(colV));
                                    ObjString* ckey = slot != NULL
                                        ? slot
                                        : obj_copy_string(
                                            AS_CSTRING(colV),
                                            (int)strlen(AS_CSTRING(colV)));
                                    table_set(&rep->fields, ckey, cellVal);
                                    owl_ui_on_instance_modified(rep);
                                    if (IS_OBJ(cellVal)) vm_pop();
                                    vm_pop();  /* row */
                                }
                                SET_RESULT(val);
                                DISPATCH();
                            }
                        }
                    }
                }

                /* Combination flag set: combi[Flag] = Vrai/Faux */
                if (IS_INSTANCE(arrVal)) {
                    ObjInstance* inst = AS_INSTANCE(arrVal);
                    ObjString* valKey = obj_copy_string("Valeur", 6);
                    Value instValeur;
                    if (table_get(&inst->fields, valKey, &instValeur) && IS_INTEGER(instValeur)) {
                        int64_t flagBits = 0;
                        if (IS_INTEGER(indexVal)) {
                            flagBits = AS_INTEGER(indexVal);
                        } else if (IS_INSTANCE(indexVal)) {
                            ObjInstance* flagInst = AS_INSTANCE(indexVal);
                            Value flagVal;
                            if (table_get(&flagInst->fields, valKey, &flagVal) && IS_INTEGER(flagVal)) {
                                flagBits = AS_INTEGER(flagVal);
                            }
                        }
                        int64_t combiVal = AS_INTEGER(instValeur);
                        bool setFlag = false;
                        if (IS_BOOL(val)) setFlag = AS_BOOL(val);
                        else if (IS_INTEGER(val)) setFlag = (AS_INTEGER(val) != 0);

                        if (setFlag) {
                            combiVal |= flagBits;  /* set flag */
                        } else {
                            combiVal &= ~flagBits; /* clear flag */
                        }
                        table_set(&inst->fields, valKey, INTEGER_VAL(combiVal));
                        SET_RESULT(val);
                        DISPATCH();
                    }
                }

                /* Associative array set: tab["cle"] = val — creates entry if absent */
                if (IS_ASSOC_ARRAY(arrVal)) {
                    ObjAssocArray* a = AS_ASSOC_ARRAY(arrVal);
                    if (!IS_STRING(indexVal)) {
                        owl_emit_runtime_error(E_RT_ASSOC_KEY_STRING_V1, NULL);
                        SET_ERROR_RET();
                    }
                    obj_assoc_set(a, AS_STRING(indexVal), val);
                    SET_RESULT(val);
                    DISPATCH();
                }

                /*  lot B : chaineMultilingue[tag] = "texte".
                 * Insertion ou update via le storage assoc. Aucune coerce
                 * de la valeur : si le user pose un entier, il sera stocke
                 * tel quel (les scenarios ne testent pas ce cas, et le
                 * type "chaineMultilingue" implique chaine cote spec). */
                if (IS_MULTILINGUE(arrVal)) {
                    ObjMultilingue* m = AS_MULTILINGUE(arrVal);
                    if (!IS_STRING(indexVal)) {
                        owl_emit_runtime_error(E_RT_ASSOC_KEY_STRING_V1, NULL);
                        SET_ERROR_RET();
                    }
                    /*  — cle canonicalisee BCP 47 (case-insensible). */
                    multilingue_store(m, AS_STRING(indexVal)->chars, val);
                    SET_RESULT(val);
                    DISPATCH();
                }

                /* JSON object/array set: jDoc[i] = x or jDoc["k"] = x.
                 * Critical: the freshly-allocated ObjJson `n` is not rooted
                 * by itself. Push it on the stack before further operations
                 * that can trigger GC (obj_json_array_push, object_set both
                 * grow tables). */
                if (IS_JSON(arrVal)) {
                    ObjJson* j = AS_JSON(arrVal);
                    if (j->jtype == JSON_T_ARRAY && (IS_INTEGER(indexVal) || IS_REAL(indexVal))) {
                        int64_t idx = IS_INTEGER(indexVal) ? AS_INTEGER(indexVal) : (int64_t)AS_REAL(indexVal);
                        /*  — croissance d'un seul element a la fois (regle
                         * tableau WL) : idx dans [1..count] ecrase, idx == count+1
                         * ajoute, tout saut leve une exception rattrapable sans
                         * muter le tableau (`j.a[3] = x` sur un tableau vide laisse
                         * `a` materialise a `[]`). */
                        if (idx < 1 || idx > j->as.arrayVal.count + 1) {
                            char idxBuf[32], maxBuf[16];
                            snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                            snprintf(maxBuf, sizeof maxBuf, "%d", j->as.arrayVal.count);
                            owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                                   "index", idxBuf,
                                                   "max", maxBuf, NULL);
                            SET_TRY_CATCH();
                            SET_ERROR_RET();
                        }
                        ObjJson* n = NULL;
                        if (IS_INTEGER(val))      n = obj_new_json_int(AS_INTEGER(val));
                        else if (IS_REAL(val))    n = obj_new_json_real(AS_REAL(val));
                        else if (IS_CURRENCY(val)) n = obj_new_json_currency(AS_CURRENCY(val));
                        else if (IS_NUMERIC(val)) n = obj_new_json_numeric(val);
                        else if (IS_BOOL(val))    n = obj_new_json_bool(AS_BOOL(val));
                        else if (IS_STRING(val))  n = obj_new_json_string(AS_STRING(val));
                        else                      n = obj_new_json(JSON_T_NULL);
                        vm_push(OBJ_VAL(n));  /* root n before the grow */
                        if (idx <= j->as.arrayVal.count) {
                            j->as.arrayVal.items[idx - 1] = n;
                        } else {
                            obj_json_array_push(j, n);
                        }
                        vm_pop();
                        SET_RESULT(val);
                        DISPATCH();
                    }
                    if (j->jtype == JSON_T_OBJECT && IS_STRING(indexVal)) {
                        ObjJson* n = NULL;
                        if (IS_INTEGER(val))      n = obj_new_json_int(AS_INTEGER(val));
                        else if (IS_REAL(val))    n = obj_new_json_real(AS_REAL(val));
                        else if (IS_CURRENCY(val)) n = obj_new_json_currency(AS_CURRENCY(val));
                        else if (IS_NUMERIC(val)) n = obj_new_json_numeric(val);
                        else if (IS_BOOL(val))    n = obj_new_json_bool(AS_BOOL(val));
                        else if (IS_STRING(val))  n = obj_new_json_string(AS_STRING(val));
                        else                      n = obj_new_json(JSON_T_NULL);
                        vm_push(OBJ_VAL(n));  /* root n before object_set may grow */
                        obj_json_object_set(j, AS_STRING(indexVal), n);
                        vm_pop();
                        SET_RESULT(val);
                        DISPATCH();
                    }
                }

                /*  — ecriture indexee d'un Vecteur : base 1, taille
                 * figee (pas d'agrandissement). Hors borne = erreur recuperable,
                 * vecteur intact. La valeur ecrite est coercee en reel
                 * (mecanisme entier->reel), coherent avec element = reel. */
                if (IS_VECTOR(arrVal)) {
                    ObjVector* vec = AS_VECTOR(arrVal);
                    int64_t vidx = 0;
                    if (IS_INTEGER(indexVal)) vidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) vidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    if (vidx < 1 || vidx > vec->count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)vidx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", vec->count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    if (!IS_NUMBER(val)) {
                        owl_emit_runtime_error(E_RT_OPERAND_NUMBER, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    vec->data[vidx - 1] = AS_NUMBER(val);
                    SET_RESULT(val);
                    DISPATCH();
                }

                /*  — ecriture indexee `m[i][j] = v` : la cible du
                 * second crochet est une vue de ligne, l'ecriture atteint le
                 * stockage de la matrice. Base 1, dimensions figees. Hors borne
                 * de colonne = erreur recuperable, matrice intacte. La valeur
                 * est coercee en reel (element = reel). Note : `m[i]` seul en
                 * cible tombe sur la matrice nue (rejet generique plus bas), et
                 * un index de ligne hors borne leve deja au premier crochet. */
                if (IS_MATRIX_ROW(arrVal)) {
                    ObjMatrixRow* rv = AS_MATRIX_ROW(arrVal);
                    ObjMatrix* mat = rv->mat;
                    int64_t cidx = 0;
                    if (IS_INTEGER(indexVal)) cidx = AS_INTEGER(indexVal);
                    else if (IS_REAL(indexVal)) cidx = (int64_t)AS_REAL(indexVal);
                    else {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    if (cidx < 1 || cidx > mat->cols) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)cidx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", mat->cols);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    if (!IS_NUMBER(val)) {
                        owl_emit_runtime_error(E_RT_OPERAND_NUMBER, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    mat->data[(rv->row - 1) * mat->cols + (cidx - 1)] = AS_NUMBER(val);
                    SET_RESULT(val);
                    DISPATCH();
                }

                if (!IS_ARRAY(arrVal)) {
                    owl_emit_runtime_error(E_RT_INDEX_NEEDS_ARRAY_OR_COMBI, NULL);
                    SET_TRY_CATCH();
                    SET_ERROR_RET();
                }
                ObjArray* array = AS_ARRAY(arrVal);
                int64_t idx = 0;
                if (IS_INTEGER(indexVal)) idx = AS_INTEGER(indexVal);
                else if (IS_REAL(indexVal)) idx = (int64_t)AS_REAL(indexVal);
                else {
                    owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                    SET_TRY_CATCH();
                    SET_ERROR_RET();
                }
                if (idx < 1) {
                    char idxBuf[32];
                    snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                    owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB_MIN,
                                           "index", idxBuf, NULL);
                    SET_TRY_CATCH();
                    SET_ERROR_RET();
                }
                if (idx > array->elements.count) {
                    /* Check growthMode: 0=error, 1=grow-by-1, 2=grow-to-N */
                    int gm = array->growthMode;
                    bool ok = false;
                    if (gm == 1 && idx == array->elements.count + 1) ok = true;
                    if (gm == 2) ok = true;
                    if (!ok) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)idx);
                        snprintf(maxBuf, sizeof maxBuf, "%d", array->elements.count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        SET_TRY_CATCH();
                        SET_ERROR_RET();
                    }
                    /* Grow to idx, fill intermediates avec la zero-value du
                     * type déclaré (jamais Null sur tableau typé). */
                    Value defaultVal = obj_array_default_for_kind(array->elementPrimKind);
                    while ((int64_t)array->elements.count < idx) {
                        value_array_write(&array->elements, defaultVal);
                    }
                }
                array->elements.values[idx - 1] = val;
                SET_RESULT(val);
                op_array_set_done:;
                #undef SET_RESULT
                #undef SET_ERROR_RET
                #undef SET_TRY_CATCH
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_GET_2D): {
                /* Stack: [recv] [x] [y] -> [val].
                 *
                 * Émis par le compiler (cf subscript) quand on a exactement
                 * 2 indices entre crochets. Dispatch runtime selon le type :
                 *
                 *   1) ObjInstance Image : lit le pixel (x, y), retourne
                 *      l'entier canon Couleur (BGR 24-bit). Doc :
                 *      Multimedia/Images/1000019650 §"Accès aux pixels".
                 *
                 *   2) ObjArray (tableau 2D nested) : équivalent à
                 *      recv[x][y] — le 1er index descend dans le sous-
                 *      tableau, le 2e lit la valeur. Préserve l'usage
                 *      classique des tableaux 2D OWL.
                 *
                 *   3) Autre : erreur runtime.
                 */
                Value yVal = vm_peek(0);
                Value xVal = vm_peek(1);
                Value recv = vm_peek(2);

                /* === ) reference cellule : doc..Cellule[ligne, colonne] === */
                if (stdlib_xls_is_ref(recv)) {
                    if ((!IS_INTEGER(xVal) && !IS_REAL(xVal)) ||
                        (!IS_INTEGER(yVal) && !IS_REAL(yVal))) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int row = IS_INTEGER(xVal) ? (int)AS_INTEGER(xVal) : (int)AS_REAL(xVal);
                    int col = IS_INTEGER(yVal) ? (int)AS_INTEGER(yVal) : (int)AS_REAL(yVal);
                    Value bound = stdlib_xls_ref_bind_cell(recv, row, col);
                    vm_pop(); vm_pop(); vm_pop();
                    vm_push(bound);
                    DISPATCH();
                }

                /* === 0) xlsDocument : doc[ligne, colonne] === */
                if (stdlib_xls_is_document(recv)) {
                    if (!IS_INTEGER(xVal) && !IS_REAL(xVal)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int row = IS_INTEGER(xVal) ? (int)AS_INTEGER(xVal) : (int)AS_REAL(xVal);
                    int col = 0;
                    if (!stdlib_xls_resolve_col(yVal, &col)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    Value cell = stdlib_xls_get_cell(recv, row, col);
                    vm_pop(); vm_pop(); vm_pop();
                    vm_push(cell);
                    DISPATCH();
                }

                /* === 1) Image === */
                if (stdlib_image_is_instance(recv)) {
                    if (!IS_INTEGER(xVal) || !IS_INTEGER(yVal)) {
                        owl_emit_runtime_error(E_RT_IMAGE_INDEX_INT_XY, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int x = (int)AS_INTEGER(xVal);
                    int y = (int)AS_INTEGER(yVal);
                    int64_t color = stdlib_image_get_pixel(recv, x, y);
                    vm_pop(); vm_pop(); vm_pop();
                    vm_push(INTEGER_VAL(color));
                    DISPATCH();
                }

                /* === 2) Tableau 2D nested : recv[x][y] === */
                if (IS_ARRAY(recv)) {
                    if (!IS_INTEGER(xVal) && !IS_REAL(xVal)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjArray* outer = AS_ARRAY(recv);
                    int64_t i = IS_INTEGER(xVal) ? AS_INTEGER(xVal)
                                                 : (int64_t)AS_REAL(xVal);
                    if (i < 1 || i > outer->elements.count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)i);
                        snprintf(maxBuf, sizeof maxBuf, "%d", outer->elements.count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    Value inner = outer->elements.values[i - 1];
                    if (!IS_ARRAY(inner)) {
                        owl_emit_runtime_error(E_RT_2D_INDEX_NEEDS_NESTED_OR_IMAGE, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjArray* innerArr = AS_ARRAY(inner);
                    if (!IS_INTEGER(yVal) && !IS_REAL(yVal)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int64_t j = IS_INTEGER(yVal) ? AS_INTEGER(yVal)
                                                 : (int64_t)AS_REAL(yVal);
                    if (j < 1 || j > innerArr->elements.count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)j);
                        snprintf(maxBuf, sizeof maxBuf, "%d", innerArr->elements.count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    Value out = innerArr->elements.values[j - 1];
                    vm_pop(); vm_pop(); vm_pop();
                    vm_push(out);
                    DISPATCH();
                }

                owl_emit_runtime_error(E_RT_2D_TYPE_UNSUPPORTED, NULL);
                OWL_RT_RECOVER_DISPATCH();
                return INTERPRET_RUNTIME_ERROR;
            }

            VM_CASE(OP_ARRAY_SET_2D): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ). */
                /* Stack: [recv] [x] [y] [val] -> [val].
                 *
                 * Symétrique de OP_ARRAY_GET_2D :
                 *   1) ObjInstance Image : écrit le pixel (x, y) avec
                 *      la couleur fournie ; alpha conservé si déjà posé,
                 *      sinon 255 (opaque).
                 *   2) ObjArray nested : équivalent à recv[x][y] = val.
                 *   3) Autre : erreur.
                 */
                Value val  = vm_peek(0);
                Value yVal = vm_peek(1);
                Value xVal = vm_peek(2);
                Value recv = vm_peek(3);

                /* === 0) xlsDocument : doc[ligne, colonne] = valeur === */
                if (stdlib_xls_is_document(recv)) {
                    int col = 0;
                    if ((!IS_INTEGER(xVal) && !IS_REAL(xVal)) ||
                        !stdlib_xls_resolve_col(yVal, &col)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int row = IS_INTEGER(xVal) ? (int)AS_INTEGER(xVal) : (int)AS_REAL(xVal);
                    stdlib_xls_set_cell(recv, row, col, val);
                    vm_pop(); vm_pop(); vm_pop(); vm_pop();
                    vm_push(val);
                    DISPATCH();
                }

                /* === 1) Image === */
                if (stdlib_image_is_instance(recv)) {
                    if (!IS_INTEGER(xVal) || !IS_INTEGER(yVal) ||
                        !IS_INTEGER(val)) {
                        owl_emit_runtime_error(E_RT_IMAGE_SET_TYPES, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int x = (int)AS_INTEGER(xVal);
                    int y = (int)AS_INTEGER(yVal);
                    int64_t color = AS_INTEGER(val);
                    stdlib_image_set_pixel(recv, x, y, color);
                    vm_pop(); vm_pop(); vm_pop(); vm_pop();
                    vm_push(val);  /* l'expression d'affectation renvoie la valeur */
                    DISPATCH();
                }

                /* === 2) Tableau 2D nested : recv[x][y] = val === */
                if (IS_ARRAY(recv)) {
                    if (!IS_INTEGER(xVal) && !IS_REAL(xVal)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjArray* outer = AS_ARRAY(recv);
                    int64_t i = IS_INTEGER(xVal) ? AS_INTEGER(xVal)
                                                 : (int64_t)AS_REAL(xVal);
                    if (i < 1 || i > outer->elements.count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)i);
                        snprintf(maxBuf, sizeof maxBuf, "%d", outer->elements.count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    Value inner = outer->elements.values[i - 1];
                    if (!IS_ARRAY(inner)) {
                        owl_emit_runtime_error(E_RT_2D_SET_NEEDS_NESTED_OR_IMAGE, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjArray* innerArr = AS_ARRAY(inner);
                    if (!IS_INTEGER(yVal) && !IS_REAL(yVal)) {
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_NUMBER, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int64_t j = IS_INTEGER(yVal) ? AS_INTEGER(yVal)
                                                 : (int64_t)AS_REAL(yVal);
                    if (j < 1 || j > innerArr->elements.count) {
                        char idxBuf[32], maxBuf[16];
                        snprintf(idxBuf, sizeof idxBuf, "%lld", (long long)j);
                        snprintf(maxBuf, sizeof maxBuf, "%d", innerArr->elements.count);
                        owl_emit_runtime_error(E_RT_ARRAY_INDEX_OOB,
                                               "index", idxBuf,
                                               "max", maxBuf, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    innerArr->elements.values[j - 1] = val;
                    vm_pop(); vm_pop(); vm_pop(); vm_pop();
                    vm_push(val);
                    DISPATCH();
                }

                owl_emit_runtime_error(E_RT_2D_SET_TYPE_UNSUPPORTED, NULL);
                OWL_RT_RECOVER_DISPATCH();
                return INTERPRET_RUNTIME_ERROR;
            }

            VM_CASE(OP_ITER_KEY): {
                /* Stack: [iterable] [1-based counter] -> [key] */
                Value idxVal = vm_pop();
                Value iterVal = vm_pop();
                int64_t idx = IS_INTEGER(idxVal) ? AS_INTEGER(idxVal) : (int64_t)AS_REAL(idxVal);
                if (IS_ASSOC_ARRAY(iterVal)) {
                    ObjAssocArray* a = AS_ASSOC_ARRAY(iterVal);
                    if (idx >= 1 && idx <= a->count) {
                        vm_push(OBJ_VAL(a->keys[idx - 1]));
                    } else {
                        vm_push(NIL_VAL);
                    }
                } else if (IS_JSON(iterVal) && AS_JSON(iterVal)->jtype == JSON_T_OBJECT) {
                    /*  — POUR TOUT sur un objet JSON : la cle secondaire
                     * recoit le nom du membre (comme un tableau associatif), pas
                     * un index numerique. Un tableau JSON garde l'index
                     * positionnel via la branche else. */
                    ObjJson* j = AS_JSON(iterVal);
                    if (idx >= 1 && idx <= j->as.objectVal.count) {
                        vm_push(OBJ_VAL(j->as.objectVal.keys[idx - 1]));
                    } else {
                        vm_push(NIL_VAL);
                    }
                } else {
                    /* Arrays/collections/others: the "key" is just the counter. */
                    vm_push(INTEGER_VAL(idx));
                }
                DISPATCH();
            }

            VM_CASE(OP_ITER_BEGIN): {
                /* Stack: [iterable] -> [iterable] (peek). Push contexte
                 * pour ÉlémentCourant. */
                if (vm.iterDepth < 16) {
                    vm.iterTarget[vm.iterDepth]     = vm_peek(0);
                    vm.iterCurrentKey[vm.iterDepth] = NIL_VAL;
                    vm.iterDepth++;
                }
                DISPATCH();
            }

            VM_CASE(OP_ITER_TRACK_KEY): {
                /* Stack: [key] -> [key]. Update top of iter context. */
                if (vm.iterDepth > 0) {
                    vm.iterCurrentKey[vm.iterDepth - 1] = vm_peek(0);
                }
                DISPATCH();
            }

            VM_CASE(OP_ITER_END): {
                /* Pop le contexte d'itération. */
                if (vm.iterDepth > 0) vm.iterDepth--;
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_LENGTH): {
                Value arrVal = vm_pop();
                if (IS_ARRAY(arrVal)) {
                    vm_push(INTEGER_VAL(AS_ARRAY(arrVal)->elements.count));
                } else if (IS_ASSOC_ARRAY(arrVal)) {
                    vm_push(INTEGER_VAL(AS_ASSOC_ARRAY(arrVal)->count));
                } else if (IS_COLLECTION(arrVal)) {
                    vm_push(INTEGER_VAL(AS_COLLECTION(arrVal)->elements.count));
                } else if (IS_JSON(arrVal)) {
                    ObjJson* j = AS_JSON(arrVal);
                    int count = 0;
                    if (j->jtype == JSON_T_ARRAY) count = j->as.arrayVal.count;
                    else if (j->jtype == JSON_T_OBJECT) count = j->as.objectVal.count;
                    vm_push(INTEGER_VAL(count));
                } else {
                    owl_emit_runtime_error(E_RT_LENGTH_NEEDS_ARRAY_OR_COLLECTION, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }

            /* --- Struct operations --- */

            VM_CASE(OP_STRUCT_GET_OR_CREATE_OBJ): opAutoVivKind = 1; goto L_struct_get_entry;
            VM_CASE(OP_STRUCT_GET_OR_CREATE_ARR): opAutoVivKind = 2; goto L_struct_get_entry;
            VM_CASE(OP_STRUCT_GET): opAutoVivKind = 0;
            L_struct_get_entry: {
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* fieldName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value instVal = vm_pop();

                /*  lot B — ObjWindowRef..Prop : proxy global de
                 * fenetre pose au compile-time par window_declaration. La
                 * resolution runtime tombe sur vm_window_stack_top()
                 * (l'instance fenetre courante). Stack vide = soft error
                 * E_RT_UI_FIELD_NO_WINDOW_OPEN (le programme continue,
                 * ErreurDetectee = Vrai). On push NIL/val pour preserver
                 * l'invariant pile. Lecture : casier d'instance d'abord
                 * (modifications imperatives), sinon les valeurs declarees
                 * du decl ont deja ete seeded dans le store par
                 * open_window_instance — donc en pratique tout est dans le
                 * store. Garde defensif sur le miss (fallback NIL). */
                if (IS_WINDOW_REF(instVal)) {
                    ObjWindowRef* wref = AS_WINDOW_REF(instVal);
                    /*  — ..Nom/..Name lecture seule : identifiant
                     * declare de la fenetre. Intercepte AVANT le lookup
                     * catalogue/store (Nom n'est pas une prop scalaire). */
                    if (owl_ui_prop_is_universal_readonly(fieldName->chars)
                        && wref->decl != NULL && wref->decl->name != NULL) {
                        vm_push(OBJ_VAL(obj_copy_string(
                            wref->decl->name, (int)strlen(wref->decl->name))));
                        DISPATCH();
                    }
                    /*  — fallback "sous-control" : si fieldName matche
                     * un OwlControlDecl du decl fenetre, retourner le
                     * ObjUiControlRef global pose par window_parse_subfield
                     * (vm.globals[strip(name)]). Permet le path qualifie
                     *   FEN_X.CTRL_Y.<methode|prop>
                     * d'arriver a la meme valeur que le raccourci global
                     * CTRL_Y.<methode|prop> (qui marche deja). Match
                     * accent-insensible + case-insensible (invariants 1 & 2).
                     * Intercept en tete de branche : sinon les paths
                     * headless_store / fallback_store / props_decl renvoient
                     * NIL puisque "CTRL_Y" n'est pas une prop fenetre. */
                    if (wref->decl != NULL) {
                        char qstr[256];
                        int qlen = scanner_strip_accents(fieldName->chars,
                                                         fieldName->length,
                                                         qstr, sizeof(qstr));
                        for (int i = 0; i < wref->decl->control_count; i++) {
                            const char* cn = wref->decl->controls[i].name;
                            if (!cn) continue;
                            char cstr[256];
                            int clen = scanner_strip_accents(cn,
                                                             (int)strlen(cn),
                                                             cstr, sizeof(cstr));
                            if (clen == qlen
                                && strncasecmp(cstr, qstr, (size_t)qlen) == 0) {
                                /* Lookup vm.globals[stripped]. */
                                ObjString* gk = obj_copy_string(cstr, clen);
                                Value gv;
                                if (table_get(&vm.globals, gk, &gv)) {
                                    vm_push(gv);
                                    DISPATCH();
                                }
                                /* Sinon : tombe sur les paths suivants
                                 * (cas atypique : control declare mais pas
                                 * encore globalise — defensive only). */
                                break;
                            }
                        }
                    }
                    ObjWindowInstance* wi = vm_window_stack_top();
                    if (wi != NULL) {
                        /*  — Qt-first pour la geometrie quand la
                         * fenetre porte un widget monte : sa taille reelle
                         * vient de la disposition, qui peut refuser aussi
                         * bien la taille declaree qu'une taille ecrite. Hors
                         * geometrie ou fenetre non montee, le hook rend false
                         * et le flow store ci-dessous est preserve. */
                        Value liveW;
                        if (owl_ui_window_live_get(wi, fieldName->chars,
                                                   &liveW)) {
                            vm_push(liveW);
                            DISPATCH();
                        }
                        /* Fenetre ouverte : lit le casier d'instance. Seed
                         * declaratif effectue par open_window_instance, donc
                         * la prop demandee est presente sauf si jamais
                         * declaree ni ecrite. Sur ce miss, si la prop existe
                         * au catalogue fenetre, on rend son defaut type (ex
                         * Note -> chaine vide) — symetrique des sous-controles
                         * (owl_ui_headless_default). Prop hors catalogue -> NIL
                         * defensif inchange. */
                        Value v;
                        if (obj_window_instance_headless_get(wi,
                                                             fieldName->chars,
                                                             &v)) {
                            vm_push(v);
                        } else if (owl_window_catalog_find(fieldName->chars)
                                   != NULL) {
                            ObjUiControl proxy;
                            proxy.obj.type = OBJ_UI_CONTROL;
                            proxy.kind     = OWL_CONTROL_WINDOW;
                            proxy.name     = NULL;
                            proxy.widget   = NULL;
                            proxy.headless_store = NULL;
                            vm_push(owl_ui_headless_default(&proxy,
                                                            fieldName->chars));
                        } else {
                            vm_push(NIL_VAL);
                        }
                        DISPATCH();
                    }
                    /* Pas d'instance : fallback_store (ecrit par
                     * `FEN_X..Prop = val` avant Ouvre). Preserve la
                     * semantique historique de l'ancien ObjUiControl global. */
                    if (wref->fallback_store != NULL) {
                        char keybuf[128];
                        const OwlControlPropDef* pdef =
                            owl_window_catalog_find(fieldName->chars);
                        const char* key = pdef
                            ? (pdef->nameAscii ? pdef->nameAscii : pdef->nameFR)
                            : fieldName->chars;
                        int klen = 0;
                        while (key[klen] != '\0' && klen < (int)sizeof(keybuf)-1) {
                            keybuf[klen] = (char)tolower((unsigned char)key[klen]);
                            klen++;
                        }
                        keybuf[klen] = '\0';
                        ObjString* ks = obj_copy_string(keybuf, klen);
                        Value v;
                        if (table_get(wref->fallback_store, ks, &v)) {
                            vm_push(v);
                            DISPATCH();
                        }
                    }
                    /* Defaut : lit la valeur declarative depuis le decl. */
                    if (wref->decl != NULL) {
                        for (int i = 0; i < wref->decl->property_count; i++) {
                            OwlPropertyInit* p = &wref->decl->properties[i];
                            if (p->is_constant
                                && strcasecmp(p->name, fieldName->chars) == 0) {
                                vm_push(p->const_value);
                                goto window_ref_get_done;
                            }
                        }
                    }
                    vm_push(NIL_VAL);
                window_ref_get_done:
                    DISPATCH();
                }

                /*  — Modele C : ObjUiControlRef..Prop.
                 * Le ref est un proxy global pose au compile-time par le
                 * parser de sous-champ. Il delegue toujours a l'instance
                 * top-of-stack (vm.window_stack). Stack vide ou champ hors
                 * fenetre courante = soft error (le programme continue,
                 * ErreurDetectee = Vrai). On push NIL en valeur de retour
                 * pour preserver l'invariant pile (la cible d'affectation
                 * recoit NIL si lecture, ignoree si dans une expression). */
                if (IS_UI_CONTROL_REF(instVal)) {
                    ObjUiControlRef* ref = AS_UI_CONTROL_REF(instVal);
                    /*  — intercept ..Groupe AVANT resolution :
                     * c'est une prop introspectee, calculee depuis le
                     * referentiel global (vm.globals) — pas un membre de
                     * l'instance. Le ref est suffisant car le contenu des
                     * groupes utilise leur identite ObjUiControlRef
                     * (passes par compile-time dans le init de Groupe). */
                    if ((fieldName->length == 6 &&
                         strncasecmp(fieldName->chars, "groupe", 6) == 0) ||
                        (fieldName->length == 5 &&
                         strncasecmp(fieldName->chars, "group", 5) == 0)) {
                        ObjArray* groups = vm_collect_groups_containing_ref(ref);
                        vm_push(OBJ_VAL(groups));
                        DISPATCH();
                    }
                    ObjUiControlInstance* uici = resolve_ui_ref(ref);
                    if (uici == NULL) {
                        vm_push(NIL_VAL);
                        DISPATCH();
                    }
                    /*  — ..Nom/..Name lecture seule : identifiant
                     * declare du controle. Intercept AVANT le catalogue (Nom
                     * n'est pas une prop scalaire per-kind). */
                    if (owl_ui_prop_is_universal_readonly(fieldName->chars)
                        && uici->decl != NULL && uici->decl->name != NULL) {
                        vm_push(OBJ_VAL(obj_copy_string(
                            uici->decl->name, (int)strlen(uici->decl->name))));
                        DISPATCH();
                    }
                    /*  — col-first `TBL.COL_Nom[ligne]` : si le ref est
                     * une Table et fieldName matche une colonne declaree
                     * (accent/case-insensible, invariants 1 & 2), on resout
                     * `TBL.COL_Nom` en un proxy-colonne indexable. L'OP_ARRAY_GET
                     * qui suit lit la cellule depuis le MEME store que la forme
                     * ligne-first -> symetrie stricte
                     *   TBL.COL_Nom[L] == TBL[L].COL_Nom.
                     * Le proxy est un ObjInstance leger portant 2 marqueurs
                     * internes (cle ref de la Table + nom canon de colonne) ;
                     * aucun nouveau type GC. Intercept en tete : une colonne
                     * n'est pas une prop catalogue, sinon elle tomberait sur le
                     * soft-error E_RT_UI_PROP_UNKNOWN_FOR_KIND. */
                    if (uici->decl != NULL &&
                        uici->decl->kind == OWL_CONTROL_TABLE &&
                        uici->decl->column_count > 0) {
                        char qstr[256];
                        int qlen = scanner_strip_accents(fieldName->chars,
                                                         fieldName->length,
                                                         qstr, sizeof(qstr));
                        for (int ci = 0; ci < uici->decl->column_count; ci++) {
                            const char* cn = uici->decl->columns[ci].name;
                            if (!cn) continue;
                            char cstr[256];
                            int clen = scanner_strip_accents(cn,
                                                             (int)strlen(cn),
                                                             cstr, sizeof(cstr));
                            if (clen == qlen
                                && clen < (int)sizeof(cstr)
                                && strncasecmp(cstr, qstr, (size_t)qlen) == 0) {
                                cstr[clen] = '\0';
                                ObjInstance* proxy = obj_new_instance(NULL);
                                vm_push(OBJ_VAL(proxy));  /* root pendant les set */
                                /* ref de la Table (re-resolution dans ARRAY_GET) */
                                inst_field_set_value(proxy,
                                                     OWL_TABLE_COLPROXY_REF_KEY,
                                                     instVal);
                                /*  — cle de cellule sous sa forme canon
                                 * (accents strippes), la meme que celle posee
                                 * par table_build_headless_rep. On passait ici
                                 * la graphie brute : inoffensif tant que la rep
                                 * etait elle aussi indexee brut, faux des
                                 * qu'elle passe au canon. */
                                inst_field_set_str(proxy,
                                                   OWL_TABLE_COLPROXY_COL_KEY,
                                                   cstr);
                                vm_pop();
                                vm_push(OBJ_VAL(proxy));
                                DISPATCH();
                            }
                        }
                    }
                    /*  — Qt-first pour les props "live" (Valeur,
                     * ValeurAffichee, ValeurMemorisee) quand
                     * widget != NULL. Symetrise apply_instance_scalar_impl
                     * (ecriture). Hors whitelist ou widget == NULL ->
                     * helper retourne false, flow standard preserve
                     * (path REF + store testé par les sentinelles
                     * tests/runtime/ui_saisie_lecture_live_gui/). */
                    {
                        Value liveV;
                        if (try_live_qt_prop_for_uici(uici, fieldName->chars,
                                                       &liveV)) {
                            vm_push(liveV);
                            DISPATCH();
                        }
                    }
                    /*  — raccourci d'apparence plat Saisie : lit le
                     * sous-champ de la ZoneSaisie de l'instance Style (source de
                     * verite unique). Gate INPUT : Libelle/Bouton gardent le
                     * chemin plat direct (couleur_police_raccourci). */
                    if (uici->decl
                        && (OwlControlKind)uici->decl->kind == OWL_CONTROL_INPUT) {
                        const char* zsSub =
                            saisie_style_shortcut_to_subfield(fieldName->chars);
                        if (zsSub != NULL) {
                            ObjInstance* zone =
                                vm_style_zonesaisie_for_uici(uici);
                            if (zone != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &zone->fields, zsSub);
                                Value sv;
                                if (sk != NULL
                                    && table_get(&zone->fields, sk, &sv)) {
                                    vm_push(sv);
                                } else {
                                    vm_push(NIL_VAL);
                                }
                            } else {
                                vm_push(NIL_VAL);
                            }
                            DISPATCH();
                        }
                    }
                    /*  — raccourci d'apparence plat mono (Libellé/Bouton) :
                     * lit le sous-champ de la sous-brique de l'instance Style
                     * (verite unique). CouleurFond->CadreExtérieur..CouleurFond,
                     * PoliceCouleur->Libellé..Couleur. */
                    if (uici->decl) {
                        const char* mBrick = NULL;
                        const char* mSub   = NULL;
                        if (mono_style_shortcut_bricksub_vm(
                                uici->decl->kind, fieldName->chars,
                                &mBrick, &mSub)) {
                            ObjInstance* brick = vm_style_mono_brick_for_uici(
                                uici, uici->decl->kind, mBrick);
                            if (brick != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &brick->fields, mSub);
                                Value sv;
                                if (sk != NULL
                                    && table_get(&brick->fields, sk, &sv)) {
                                    vm_push(sv);
                                } else {
                                    vm_push(NIL_VAL);
                                }
                            } else {
                                vm_push(NIL_VAL);
                            }
                            DISPATCH();
                        }
                    }
                    /*  — raccourci Police plat : lit le sous-champ
                     * de l'instance Police materialisee. */
                    const char* polSub =
                        police_shortcut_to_subfield(fieldName->chars);
                    if (polSub != NULL) {
                        ObjInstance* pol =
                            vm_headless_police_instance_for_uici(uici);
                        if (pol != NULL) {
                            ObjString* sk = table_find_key_case_insensitive(
                                &pol->fields, polSub);
                            Value sv;
                            if (sk != NULL && table_get(&pol->fields, sk, &sv)) {
                                vm_push(sv);
                            } else {
                                vm_push(NIL_VAL);
                            }
                        } else {
                            vm_push(NIL_VAL);
                        }
                        DISPATCH();
                    }
                    /*  — Liste/Combo : ..Occurrence / ..Vide doivent
                     * lire le count effectif. En GUI : Qt widget (via function
                     * pointer registered par stdlib_ui_init). En headless :
                     * items store rooted dans le headless_store de l'uici. Sans
                     * cette branche, le headless_get sous "Occurrence" est faux
                     * (aucune ecriture n'aboutit jamais sous cette cle) et le
                     * fallback default renvoie 0.
                     *  — etendu Selecteur/Interrupteur/ZoneRepetee. */
                    {
                        int ukind_lc = uici->decl ? (int)uici->decl->kind : 0;
                        if (ukind_lc == OWL_CONTROL_LISTE
                            || ukind_lc == OWL_CONTROL_COMBO
                            || ukind_lc == OWL_CONTROL_SELECTEUR
                            || ukind_lc == OWL_CONTROL_INTERRUPTEUR
                            || ukind_lc == OWL_CONTROL_ZONE_REPETEE) {
                            bool isOcc =
                                strcasecmp(fieldName->chars, "Occurrence") == 0
                                || strcasecmp(fieldName->chars, "Count") == 0;
                            bool isVide =
                                strcasecmp(fieldName->chars, "Vide") == 0
                                || strcasecmp(fieldName->chars, "Empty") == 0;
                            if (isOcc || isVide) {
                                int count;
                                if (uici->widget != NULL) {
                                    count = owl_ui_widget_items_count(
                                        ukind_lc, uici->widget);
                                } else {
                                    count = owl_ui_headless_widget_items_count_uici(uici);
                                }
                                vm_push(isVide ? BOOL_VAL(count == 0)
                                               : INTEGER_VAL(count));
                                DISPATCH();
                            }
                        }
                    }
                    /*  +  — Liste/Combo : ..Contenu / ..Content
                     * reflete la liste VIVANTE d'items (meme source que
                     * ListeOccurrence), pas un store de propriete separe que seul
                     * le setter ..Contenu alimentait. En GUI : labels du widget
                     * joints par LF. En headless pur (widget == NULL) : items du
                     * store headless joints par LF ( — meme liste que
                     * ListeXxx). Intercept AVANT headless_get, miroir de la branche
                     * Occurrence/Vide. */
                    {
                        int ukind_ct = uici->decl ? (int)uici->decl->kind : 0;
                        if ((ukind_ct == OWL_CONTROL_LISTE
                             || ukind_ct == OWL_CONTROL_COMBO)
                            && (strcasecmp(fieldName->chars, "Contenu") == 0
                                || strcasecmp(fieldName->chars, "Content") == 0)) {
                            const char* lf = uici->widget != NULL
                                ? owl_ui_widget_content_lf(ukind_ct, uici->widget)
                                : owl_ui_headless_content_lf_uici(uici);
                            if (!lf) lf = "";
                            vm_push(OBJ_VAL(obj_copy_string(lf, (int)strlen(lf))));
                            DISPATCH();
                        }
                    }
                    Value v;
                    if (obj_ui_control_instance_headless_get(uici,
                                                              fieldName->chars,
                                                              &v)) {
                        if (owl_trace_ui_enabled()) {
                            const char* cn = (uici->decl && uici->decl->name)
                                              ? uici->decl->name : "?";
                            owl_trace_ui_value(cn, fieldName->chars,
                                                "store", v);
                        }
                        vm_push(v);
                    } else {
                        /*  — prop composee (OWL_PROP_TYPE_OBJECT) en
                         * mode headless : materialise l'instance a la demande
                         * et la stocke dans headless_store sous la cle canon. */
                        int ukind = uici->decl ? (int)uici->decl->kind : 0;
                        const OwlControlPropDef* pdef =
                            owl_control_catalog_find(
                                (OwlControlKind)ukind, fieldName->chars);
                        if (pdef && pdef->type == OWL_PROP_TYPE_OBJECT
                            && pdef->object_type) {
                            const char* canonKey = pdef->nameAscii
                                ? pdef->nameAscii : pdef->nameFR;
                            Value existing;
                            if (obj_ui_control_instance_headless_get(
                                    uici, canonKey, &existing)
                                && IS_INSTANCE(existing)) {
                                vm_push(existing);
                                DISPATCH();
                            }
                            Value inst_v =
                                owl_clone_global_template(pdef->object_type);
                            if (IS_INSTANCE(inst_v)) {
                                vm_push(inst_v);  /* root pendant le set */
                                obj_ui_control_instance_headless_set(
                                    uici, canonKey, inst_v);
                                /*  lot 2 — relie le composite materialise
                                 * (Cadre/Police) a son widget Qt monte pour que le
                                 * chained set suivant (..Epaisseur = N) soit re-
                                 * applique. No-op en headless (widget == NULL). */
                                if (uici->widget != NULL) {
                                    owl_ui_register_instance_composite(
                                        uici, AS_INSTANCE(inst_v));
                                }
                                vm_pop();
                                vm_push(inst_v);
                            } else {
                                vm_push(NIL_VAL);
                            }
                        } else if (pdef == NULL
                                   && !owl_ui_prop_is_universal(
                                          fieldName->chars)) {
                            /*  — prop hors catalogue lue via reference
                             * qualifiee (non verifiable a la compile) : soft
                             * error au lieu du default fantome silencieux.
                             * Les props catalogues non encore ecrites gardent
                             * leur default typé (pdef != NULL ci-dessus). */
                            owl_emit_native_error(
                                E_RT_UI_PROP_UNKNOWN_FOR_KIND,
                                uici->decl ? uici->decl->name : "?",
                                "prop", fieldName->chars,
                                "kind", owl_control_kind_name_fr(
                                            (OwlControlKind)ukind),
                                NULL);
                            vm_push(NIL_VAL);
                        } else {
                            ObjUiControl proxy;
                            proxy.obj.type = OBJ_UI_CONTROL;
                            proxy.kind     = ukind;
                            proxy.name     = uici->decl ? uici->decl->name : "?";
                            proxy.widget   = NULL;
                            proxy.headless_store = NULL;
                            vm_push(headless_default_with_alias(
                                &proxy, fieldName->chars));
                        }
                    }
                    DISPATCH();
                }

                /* DSL Fenetre v1 (Phase E.X) : ObjUiControl..Prop.
                 *  — dispatch :
                 *   widget != NULL : hook Qt (mode GUI attach actif).
                 *   widget == NULL : storage virtuel (headless_store).
                 *     - Si la prop a deja ete ecrite : retourne sa valeur.
                 *     - Sinon : default sensible par nom/type (Visible=Vrai,
                 *       Texte/Libelle/Titre="", entiers/couleur=0). */
                if (IS_UI_CONTROL(instVal)) {
                    ObjUiControl* uic = AS_UI_CONTROL(instVal);
                    /*  — ..Nom/..Name lecture seule : identifiant
                     * declare du controle. Intercept AVANT le fallback
                     * get_prop (Nom n'est pas une prop scalaire per-kind). */
                    if (owl_ui_prop_is_universal_readonly(fieldName->chars)
                        && uic->name != NULL) {
                        vm_push(OBJ_VAL(obj_copy_string(
                            uic->name, (int)strlen(uic->name))));
                        DISPATCH();
                    }
                    /*  — intercept ..Groupe AVANT le fallback get_prop.
                     * Retourne un tableau de Groupe (ObjArray non-group)
                     * collectant tous les Groupes globaux qui referencent
                     * ce control. */
                    if ((fieldName->length == 6 &&
                         strncasecmp(fieldName->chars, "groupe", 6) == 0) ||
                        (fieldName->length == 5 &&
                         strncasecmp(fieldName->chars, "group", 5) == 0)) {
                        ObjArray* groups = vm_collect_groups_containing(uic);
                        vm_push(OBJ_VAL(groups));
                        DISPATCH();
                    }
                    /*  — Liste/Combo : ..Occurrence / ..Vide. Symetrique
                     * de la branche IS_UI_CONTROL_REF. En GUI : Qt widget via
                     * function pointer. En headless : items store. Sans cette
                     * interception, le path 'widget != NULL' delegue a
                     * owl_ui_control_get_prop -> real_ui_get_prop qui lit
                     * owl_ui_listcombo_count(w) — OK ; le path 'widget == NULL'
                     * tombe sur headless_get + default = 0 (bug). Centraliser
                     * ici aligne les deux modes.
                     *  — etendu Selecteur/Interrupteur/ZoneRepetee. */
                    if (uic->kind == OWL_CONTROL_LISTE
                        || uic->kind == OWL_CONTROL_COMBO
                        || uic->kind == OWL_CONTROL_SELECTEUR
                        || uic->kind == OWL_CONTROL_INTERRUPTEUR
                        || uic->kind == OWL_CONTROL_ZONE_REPETEE) {
                        bool isOcc =
                            strcasecmp(fieldName->chars, "Occurrence") == 0
                            || strcasecmp(fieldName->chars, "Count") == 0;
                        bool isVide =
                            strcasecmp(fieldName->chars, "Vide") == 0
                            || strcasecmp(fieldName->chars, "Empty") == 0;
                        if (isOcc || isVide) {
                            int count;
                            if (uic->widget != NULL) {
                                count = owl_ui_widget_items_count(uic->kind, uic->widget);
                            } else {
                                count = owl_ui_headless_widget_items_count_uic(uic);
                            }
                            vm_push(isVide ? BOOL_VAL(count == 0)
                                           : INTEGER_VAL(count));
                            DISPATCH();
                        }
                    }
                    /*  +  — miroir de la branche REF :
                     * ..Contenu/..Content d'un Liste/Combo reflete la liste
                     * vivante d'items (GUI : labels du widget ; headless pur :
                     * store items), joints par LF, pas un store separe. */
                    if ((uic->kind == OWL_CONTROL_LISTE
                         || uic->kind == OWL_CONTROL_COMBO)
                        && (strcasecmp(fieldName->chars, "Contenu") == 0
                            || strcasecmp(fieldName->chars, "Content") == 0)) {
                        const char* lf = uic->widget != NULL
                            ? owl_ui_widget_content_lf(uic->kind, uic->widget)
                            : owl_ui_headless_content_lf_uic(uic);
                        if (!lf) lf = "";
                        vm_push(OBJ_VAL(obj_copy_string(lf, (int)strlen(lf))));
                        DISPATCH();
                    }
                    if (uic->widget != NULL) {
                        vm_push(owl_ui_control_get_prop(uic, fieldName->chars));
                    } else {
                        /*  — raccourci d'apparence plat Saisie : lit le
                         * sous-champ de la ZoneSaisie de l'instance Style. */
                        if ((OwlControlKind)uic->kind == OWL_CONTROL_INPUT) {
                            const char* zsSub =
                                saisie_style_shortcut_to_subfield(fieldName->chars);
                            if (zsSub != NULL) {
                                ObjInstance* zone =
                                    vm_style_zonesaisie_for_uic(uic);
                                if (zone != NULL) {
                                    ObjString* sk = table_find_key_case_insensitive(
                                        &zone->fields, zsSub);
                                    Value sv;
                                    if (sk != NULL
                                        && table_get(&zone->fields, sk, &sv)) {
                                        vm_push(sv);
                                    } else {
                                        vm_push(NIL_VAL);
                                    }
                                } else {
                                    vm_push(NIL_VAL);
                                }
                                DISPATCH();
                            }
                        }
                        /*  — raccourci d'apparence plat mono (Libellé/
                         * Bouton) : lit le sous-champ de la sous-brique de
                         * l'instance Style. CouleurFond->CadreExtérieur..CouleurFond,
                         * PoliceCouleur->Libellé..Couleur. */
                        {
                            const char* mBrick = NULL;
                            const char* mSub   = NULL;
                            if (mono_style_shortcut_bricksub_vm(
                                    (int)uic->kind, fieldName->chars,
                                    &mBrick, &mSub)) {
                                ObjInstance* brick = vm_style_mono_brick_for_uic(
                                    uic, (int)uic->kind, mBrick);
                                if (brick != NULL) {
                                    ObjString* sk =
                                        table_find_key_case_insensitive(
                                            &brick->fields, mSub);
                                    Value sv;
                                    if (sk != NULL
                                        && table_get(&brick->fields, sk, &sv)) {
                                        vm_push(sv);
                                    } else {
                                        vm_push(NIL_VAL);
                                    }
                                } else {
                                    vm_push(NIL_VAL);
                                }
                                DISPATCH();
                            }
                        }
                        /*  — raccourci Police plat : lit le sous-champ
                         * de l'instance Police materialisee (source de verite
                         * unique), pas une cle plate deconnectee. */
                        const char* polSub =
                            police_shortcut_to_subfield(fieldName->chars);
                        if (polSub != NULL) {
                            ObjInstance* pol = vm_headless_police_instance(uic);
                            if (pol != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &pol->fields, polSub);
                                Value sv;
                                if (sk != NULL
                                    && table_get(&pol->fields, sk, &sv)) {
                                    vm_push(sv);
                                } else {
                                    vm_push(NIL_VAL);
                                }
                            } else {
                                vm_push(NIL_VAL);
                            }
                            DISPATCH();
                        }
                        Value v;
                        if (obj_ui_control_headless_get(uic, fieldName->chars, &v)) {
                            vm_push(v);
                        } else {
                            /*  : prop composee (OWL_PROP_TYPE_OBJECT,
                             * ex Cadre) en mode headless. Materialise l'instance
                             * a la demande depuis le template global, la stocke
                             * dans le headless_store (persistance + round-trip)
                             * et la retourne. Sans ca la cascade top-level
                             * `WIDGET..Cadre..SousChamp` n'a pas d'instance sur
                             * laquelle OP_STRUCT_GET/SET pourrait enchainer. */
                            const OwlControlPropDef* pdef =
                                owl_control_catalog_find(
                                    (OwlControlKind)uic->kind, fieldName->chars);
                            if (pdef && pdef->type == OWL_PROP_TYPE_OBJECT
                                && pdef->object_type) {
                                /*  — stocke l'instance sous la cle
                                 * canon de la prop (nameAscii/nameFR), PAS sous
                                 * le nom tape : `..Police..` et `..Font..`
                                 * (forme EN) doivent partager la meme instance.
                                 * Sans ca deux stores divergents. Re-lit sous la
                                 * cle canon avant de cloner (cas EN apres un
                                 * acces FR prealable). */
                                const char* canonKey = pdef->nameAscii
                                    ? pdef->nameAscii : pdef->nameFR;
                                Value existing;
                                if (obj_ui_control_headless_get(
                                        uic, canonKey, &existing)
                                    && IS_INSTANCE(existing)) {
                                    vm_push(existing);
                                    DISPATCH();
                                }
                                Value inst =
                                    owl_clone_global_template(pdef->object_type);
                                if (IS_INSTANCE(inst)) {
                                    vm_push(inst); /* root pendant le set */
                                    obj_ui_control_headless_set(
                                        uic, canonKey, inst);
                                    vm_pop();
                                    vm_push(inst);
                                } else {
                                    vm_push(NIL_VAL);
                                }
                            } else {
                                vm_push(headless_default_with_alias(
                                    uic, fieldName->chars));
                            }
                        }
                    }
                    DISPATCH();
                }

                /*  — propriete inconnue sur une collection (tableau,
                 * Vecteur, Matrice, tableau associatif, collection) = erreur
                 * RECUPERABLE, routee vers QUAND EXCEPTION, au lieu du Null
                 * muet qui masquait les fautes de frappe (..Occurence pour
                 * ..Occurrence). Pattern rattrapable calque sur
                 * E_RT_ARRAY_INDEX_OOB (consultation exceptionStack avant
                 * return) — PAS le `return INTERPRET_RUNTIME_ERROR` direct de
                 * E_RT_STRING_HAS_NO_PROP, qui bypasse handle_runtime_exception
                 * et sort en exit 70. La resolution JSON (membre absent -> Null
                 * par design) vit dans sa propre branche IS_JSON, non touchee. */
                /*  — la liste des proprietes valides est PER-TYPE :
                 * chaque branche passe SA liste, derivee de ses propres tests
                 * d'acceptation ci-dessous (une whitelist globale listait
                 * ..Occurrence comme valide sur un Vecteur qui l'a perdue via
                 *  -> boucle de confusion). frList/enList selectionnees
                 * selon la langue de rendu active pour ne regresser aucune des
                 * deux surfaces. Les formes affichees = orthographes reellement
                 * acceptees par la branche (pas d'alias EN advertise qui ne
                 * matcherait pas, ex ..Count non reconnu sur un tableau). */
                #define COLL_NO_PROP(frList, enList) do { \
                    char nameBuf[128]; \
                    int nl = fieldName->length < (int)sizeof(nameBuf) - 1 \
                                 ? fieldName->length : (int)sizeof(nameBuf) - 1; \
                    memcpy(nameBuf, fieldName->chars, (size_t)nl); \
                    nameBuf[nl] = '\0'; \
                    const char* validList__ = owl_error_render_lang_is_en() \
                                                  ? (enList) : (frList); \
                    owl_emit_runtime_error(E_RT_COLLECTION_HAS_NO_PROP, \
                                           "name", nameBuf, \
                                           "valid", validList__, NULL); \
                    if (exceptionCount > 0 && currentException != NULL) { \
                        if (handle_runtime_exception()) { \
                            frame = &vm.frames[vm.frameCount - 1]; \
                            DISPATCH(); \
                        } \
                    } \
                    return INTERPRET_RUNTIME_ERROR; \
                } while (0)

                /* Associative array property access: .Occurrence, .Vide */
                if (IS_ASSOC_ARRAY(instVal)) {
                    ObjAssocArray* a = AS_ASSOC_ARRAY(instVal);
                    if (fieldName->length == 10 && strncasecmp(fieldName->chars, "occurrence", 10) == 0) {
                        vm_push(INTEGER_VAL(a->count));
                    } else if ((fieldName->length == 4 && strncasecmp(fieldName->chars, "vide", 4) == 0) ||
                               (fieldName->length == 5 && strncasecmp(fieldName->chars, "empty", 5) == 0)) {
                        vm_push(BOOL_VAL(a->count == 0));
                    } else {
                        COLL_NO_PROP("..Occurrence, ..Vide",
                                     "..Occurrence, ..Empty");
                    }
                    DISPATCH();
                }

                /*  +  — Tenseur : ..Valeur/..Value et
                 * ..Gradient rendent la donnee/gradient sous une forme
                 * re-indexable, ..Forme/..Shape la sequence des tailles d'axes
                 * (tableau 1-based), ..Rang/..Rank le nombre d'axes.
                 *  - rang 1 : l'ObjVector interne (indexable [i]) ;
                 *  - rang 2 : l'ObjMatrix interne (indexable [i][j]) ;
                 *  - rang >= 3 : une vue plein-rang (ObjTensorView) qui descend
                 *    d'un axe a chaque [i] jusqu'a l'element (cf. OP_ARRAY_GET). */
                if (IS_TENSOR(instVal)) {
                    ObjTensor* t = AS_TENSOR(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    bool isValeur = (fl == 6 && strncasecmp(fn, "valeur", 6) == 0)
                                 || (fl == 5 && strncasecmp(fn, "value", 5) == 0);
                    bool isGradient = (fl == 8 && strncasecmp(fn, "gradient", 8) == 0);
                    if (isValeur || isGradient) {
                        Obj* buf = isValeur ? t->data : t->grad;
                        if (t->ndim <= 2) {
                            /* Aucune allocation : on pousse un objet deja vivant. */
                            vm_push(OBJ_VAL(buf));
                        } else {
                            /* Strides row-major contigus deduits de la forme
                             * (dernier axe le plus rapide). La vue est allouee :
                             * instVal re-pousse pour garder `t` (et son buffer)
                             * roote pendant l'unique alloc. */
                            int strides[OWL_TENSOR_MAX_RANK];
                            int acc = 1;
                            for (int i = t->ndim - 1; i >= 0; i--) {
                                strides[i] = acc;
                                acc *= t->shape[i];
                            }
                            vm_push(instVal);
                            ObjTensorView* vw = obj_new_tensor_view(
                                buf, 0, t->ndim, t->shape, strides);
                            vm_pop();
                            vm_push(OBJ_VAL(vw));
                        }
                        DISPATCH();
                    }
                    if ((fl == 5 && strncasecmp(fn, "forme", 5) == 0)
                        || (fl == 5 && strncasecmp(fn, "shape", 5) == 0)) {
                        vm_push(instVal);   /* root t pendant l'alloc du tableau */
                        ObjArray* arr = obj_new_array();
                        vm_push(OBJ_VAL(arr));
                        for (int i = 0; i < t->ndim; i++) {
                            value_array_write(&arr->elements,
                                              INTEGER_VAL(t->shape[i]));
                        }
                        vm_pop();   /* arr */
                        vm_pop();   /* instVal */
                        vm_push(OBJ_VAL(arr));
                        DISPATCH();
                    }
                    if ((fl == 4 && strncasecmp(fn, "rang", 4) == 0)
                        || (fl == 4 && strncasecmp(fn, "rank", 4) == 0)) {
                        vm_push(INTEGER_VAL(t->ndim));
                        DISPATCH();
                    }
                    COLL_NO_PROP("..Valeur, ..Gradient, ..Forme, ..Rang",
                                 "..Value, ..Gradient, ..Shape, ..Rank");
                    DISPATCH();
                }

                /*  — IAModèle : ..NbCouches / ..LayerCount = nombre
                 * total de couches empilees (denses + activations). Propriete
                 * (double point), cablee cote VM comme ..Taille du Vecteur. */
                if (IS_IAMODELE(instVal)) {
                    ObjIAModele* mdl = AS_IAMODELE(instVal);
                    if ((fieldName->length == 9
                         && strncasecmp(fieldName->chars, "nbcouches", 9) == 0)
                        || (fieldName->length == 10
                            && strncasecmp(fieldName->chars, "layercount", 10) == 0)) {
                        vm_push(INTEGER_VAL(mdl->count));
                    } else {
                        COLL_NO_PROP("..NbCouches", "..LayerCount");
                    }
                    DISPATCH();
                }

                /*  — MLArbreDécision : lecture des hyperparamètres et
                 * de l'objectif (double point). Noms accent-strippés par le
                 * scanner (..Objectif -> "objectif", ..MinÉchantillonsSéparation
                 * -> "minechantillonsseparation"). Chaque propriété se relit
                 * exactement comme écrite (branche OP_STRUCT_SET symétrique). */
                if (IS_MLARBRE(instVal)) {
                    ObjMLArbre* a = AS_MLARBRE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        vm_push(INTEGER_VAL(a->objective));
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        vm_push(INTEGER_VAL(a->maxDepth));
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        vm_push(INTEGER_VAL(a->minSamplesSplit));
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        vm_push(INTEGER_VAL(a->minSamplesLeaf));
                    } else {
                        COLL_NO_PROP(
                            "..Objectif, ..ProfondeurMax, "
                            "..MinÉchantillonsSéparation, ..MinÉchantillonsFeuille",
                            "..Objective, ..MaxDepth, "
                            "..MinSamplesSplit, ..MinSamplesLeaf");
                    }
                    DISPATCH();
                }

                /*  — MLForêtAléatoire : lecture des hyperparamètres et
                 * de l'objectif (double point). Noms accent-strippés par le
                 * scanner. Symétrique de la branche OP_STRUCT_SET. */
                if (IS_MLFORET(instVal)) {
                    ObjMLForet* f = AS_MLFORET(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        vm_push(INTEGER_VAL(f->objective));
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        vm_push(INTEGER_VAL(f->maxDepth));
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        vm_push(INTEGER_VAL(f->minSamplesSplit));
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        vm_push(INTEGER_VAL(f->minSamplesLeaf));
                    } else if ((fl == 8 && strncasecmp(fn, "nbarbres", 8) == 0)
                        || (fl == 8 && strncasecmp(fn, "numtrees", 8) == 0)) {
                        vm_push(INTEGER_VAL(f->numTrees));
                    } else if ((fl == 22 && strncasecmp(fn, "variablesparseparation", 22) == 0)
                        || (fl == 11 && strncasecmp(fn, "maxfeatures", 11) == 0)) {
                        vm_push(INTEGER_VAL(f->maxFeatures));
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        vm_push(INTEGER_VAL(f->seed));
                    } else {
                        COLL_NO_PROP(
                            "..Objectif, ..NbArbres, ..VariablesParSéparation, "
                            "..Graine, ..ProfondeurMax, "
                            "..MinÉchantillonsSéparation, ..MinÉchantillonsFeuille",
                            "..Objective, ..NumTrees, ..MaxFeatures, ..Seed, "
                            "..MaxDepth, ..MinSamplesSplit, ..MinSamplesLeaf");
                    }
                    DISPATCH();
                }

                /*  — MLGradientBoosting : lecture des hyperparamètres et
                 * de l'objectif (double point). Noms accent-strippés par le
                 * scanner. ..TauxApprentissage est un réel (les autres entiers).
                 * Symétrique de la branche OP_STRUCT_SET. */
                if (IS_MLBOOSTING(instVal)) {
                    ObjMLBoosting* b = AS_MLBOOSTING(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        vm_push(INTEGER_VAL(b->objective));
                    } else if ((fl == 8 && strncasecmp(fn, "nbarbres", 8) == 0)
                        || (fl == 8 && strncasecmp(fn, "numtrees", 8) == 0)) {
                        vm_push(INTEGER_VAL(b->numTrees));
                    } else if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        vm_push(REAL_VAL(b->learningRate));
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        vm_push(INTEGER_VAL(b->maxDepth));
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        vm_push(INTEGER_VAL(b->minSamplesSplit));
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        vm_push(INTEGER_VAL(b->minSamplesLeaf));
                    } else {
                        COLL_NO_PROP(
                            "..Objectif, ..NbArbres, ..TauxApprentissage, "
                            "..ProfondeurMax, ..MinÉchantillonsSéparation, "
                            "..MinÉchantillonsFeuille",
                            "..Objective, ..NumTrees, ..LearningRate, "
                            "..MaxDepth, ..MinSamplesSplit, ..MinSamplesLeaf");
                    }
                    DISPATCH();
                }

                /*  — MLRégressionLinéaire : lecture des poids appris
                 * (double point, lecture seule). ..Coefficients rend un Vecteur
                 * frais des poids w1..wn ; ..Ordonnée/..Intercept rend le biais
                 * (réel). Noms accent-strippés par le scanner. */
                if (IS_MLLINEAIRE(instVal)) {
                    ObjMLLineaire* r = AS_MLLINEAIRE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if (fl == 12 && strncasecmp(fn, "coefficients", 12) == 0) {
                        /* Vecteur frais : alloc AVANT remplissage (peut déclencher
                         * un GC ; r reste rooté via le slot local). Aucune alloc
                         * entre obj_new_vector et le push -> vecteur sain. */
                        int n = r->trained ? r->nCoef : 0;
                        ObjVector* v = obj_new_vector(n);
                        for (int j = 0; j < n; j++) v->data[j] = r->coef[j];
                        vm_push(OBJ_VAL(v));
                    } else if ((fl == 8 && strncasecmp(fn, "ordonnee", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "intercept", 9) == 0)) {
                        vm_push(REAL_VAL(r->intercept));
                    } else {
                        COLL_NO_PROP(
                            "..Coefficients, ..Ordonnée",
                            "..Coefficients, ..Intercept");
                    }
                    DISPATCH();
                }

                /*  — MLRégressionLogistique : lecture des hyperparamètres
                 * (double point). ..TauxApprentissage est un réel, ..NbItérations
                 * un entier. Noms accent-strippés par le scanner. Symétrique de la
                 * branche OP_STRUCT_SET. */
                if (IS_MLLOGISTIQUE(instVal)) {
                    ObjMLLogistique* r = AS_MLLOGISTIQUE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        vm_push(REAL_VAL(r->learningRate));
                    } else if ((fl == 12 && strncasecmp(fn, "nbiterations", 12) == 0)
                        || (fl == 13 && strncasecmp(fn, "numiterations", 13) == 0)) {
                        vm_push(INTEGER_VAL(r->numIterations));
                    } else {
                        COLL_NO_PROP(
                            "..TauxApprentissage, ..NbItérations",
                            "..LearningRate, ..NumIterations");
                    }
                    DISPATCH();
                }

                /*  — MLKMeans : lecture des hyperparamètres et des
                 * centroïdes appris (double point). ..NbClusters/..NbItérations/
                 * ..Graine sont des entiers ; ..Centroïdes rend une Matrice
                 * k×variables (frais). Noms accent-strippés par le scanner
                 * (..Centroïdes -> "centroides", ..Graine -> "graine"). Symétrique
                 * de la branche OP_STRUCT_SET. */
                if (IS_MLKMEANS(instVal)) {
                    ObjMLKMeans* k = AS_MLKMEANS(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 10 && strncasecmp(fn, "nbclusters", 10) == 0)
                        || (fl == 11 && strncasecmp(fn, "numclusters", 11) == 0)) {
                        vm_push(INTEGER_VAL(k->numClusters));
                    } else if ((fl == 12 && strncasecmp(fn, "nbiterations", 12) == 0)
                        || (fl == 13 && strncasecmp(fn, "numiterations", 13) == 0)) {
                        vm_push(INTEGER_VAL(k->numIterations));
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        vm_push(INTEGER_VAL(k->seed));
                    } else if ((fl == 10 && strncasecmp(fn, "centroides", 10) == 0)
                        || (fl == 9 && strncasecmp(fn, "centroids", 9) == 0)) {
                        /* Matrice fraiche k×variables : alloc AVANT remplissage
                         * (peut declencher un GC ; k reste roote via le slot
                         * local). Aucune alloc entre obj_new_matrix et le push
                         * -> matrice saine. Avant entrainement, matrice zero. */
                        int rows = k->numClusters > 0 ? k->numClusters : 1;
                        int cols = k->nFeatures > 0 ? k->nFeatures : 1;
                        ObjMatrix* mat = obj_new_matrix(rows, cols);
                        if (k->trained && k->centroids != NULL) {
                            for (int i = 0; i < rows * cols; i++)
                                mat->data[i] = k->centroids[i];
                        }
                        vm_push(OBJ_VAL(mat));
                    } else {
                        COLL_NO_PROP(
                            "..NbClusters, ..NbItérations, ..Graine, ..Centroïdes",
                            "..NumClusters, ..NumIterations, ..Seed, ..Centroids");
                    }
                    DISPATCH();
                }

                /*  — ImageÉtiquetée : ..Chemin/..Path (chaine),
                 * ..Étiquette/..Label (chaine) et ..Image/..Image (Image
                 * MATERIALISEE PARESSEUSEMENT au premier acces). Noms
                 * accent-strippes par le scanner (..Étiquette -> "etiquette").
                 * Une image absente/illisible leve une exception rattrapable
                 * (jamais de plantage) — routage QUAND EXCEPTION identique a
                 * l'indexation hors-plage. */
                if (IS_IMAGE_ETIQUETEE(instVal)) {
                    ObjImageEtiquetee* ie = AS_IMAGE_ETIQUETEE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 6 && strncasecmp(fn, "chemin", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "path", 4) == 0)) {
                        vm_push(ie->path != NULL ? OBJ_VAL(ie->path)
                                                 : OBJ_VAL(obj_copy_string("", 0)));
                        DISPATCH();
                    }
                    if ((fl == 9 && strncasecmp(fn, "etiquette", 9) == 0)
                        || (fl == 5 && strncasecmp(fn, "label", 5) == 0)) {
                        vm_push(ie->label != NULL ? OBJ_VAL(ie->label)
                                                  : OBJ_VAL(obj_copy_string("", 0)));
                        DISPATCH();
                    }
                    if (fl == 5 && strncasecmp(fn, "image", 5) == 0) {
                        /* Root ie (instVal a ete pop) pendant la
                         * materialisation : image_make_instance alloue. */
                        vm_push(instVal);
                        ObjInstance* img = obj_image_etiquetee_materialize(ie);
                        vm_pop();
                        if (img == NULL) {
                            char pbuf[256];
                            const char* p = ie->path != NULL ? ie->path->chars : "";
                            int pl = (int)strlen(p);
                            if (pl > (int)sizeof(pbuf) - 1) pl = (int)sizeof(pbuf) - 1;
                            memcpy(pbuf, p, (size_t)pl);
                            pbuf[pl] = '\0';
                            owl_emit_runtime_error(E_NATIVE_IMAGE_LOAD_FAILURE,
                                                   "native", "Image\xc3\x89tiquet\xc3\xa9""e",
                                                   "path", pbuf,
                                                   "detail",
                                                   "fichier absent ou format non reconnu",
                                                   NULL);
                            if (exceptionCount > 0 && currentException != NULL) {
                                if (handle_runtime_exception()) {
                                    frame = &vm.frames[vm.frameCount - 1];
                                    DISPATCH();
                                }
                            }
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        vm_push(OBJ_VAL(img));
                        DISPATCH();
                    }
                    COLL_NO_PROP("..Chemin, ..Étiquette, ..Image",
                                 "..Path, ..Label, ..Image");
                    DISPATCH();
                }

                /*  — ImageAnnotée : ..Chemin/..Path (chaine),
                 * ..Boîtes/..Boxes (tableau de boites, chacune tableau de
                 * reels [classe, cx, cy, l, h]) et ..Image/..Image (Image
                 * MATERIALISEE PARESSEUSEMENT au premier acces). Noms
                 * accent-strippes par le scanner (..Boîtes -> "boites"). */
                if (IS_IMAGE_ANNOTEE(instVal)) {
                    ObjImageAnnotee* ia = AS_IMAGE_ANNOTEE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 6 && strncasecmp(fn, "chemin", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "path", 4) == 0)) {
                        vm_push(ia->path != NULL ? OBJ_VAL(ia->path)
                                                 : OBJ_VAL(obj_copy_string("", 0)));
                        DISPATCH();
                    }
                    if ((fl == 6 && strncasecmp(fn, "boites", 6) == 0)
                        || (fl == 5 && strncasecmp(fn, "boxes", 5) == 0)) {
                        /* Le tableau de boites est deja materialise a l'import ;
                         * on rend la reference directe (partagee). */
                        vm_push(ia->boxes != NULL ? OBJ_VAL(ia->boxes)
                                                  : OBJ_VAL(obj_new_array()));
                        DISPATCH();
                    }
                    if (fl == 5 && strncasecmp(fn, "image", 5) == 0) {
                        /* Root ia (instVal a ete pop) pendant la
                         * materialisation : image_make_instance alloue. */
                        vm_push(instVal);
                        ObjInstance* img = obj_image_annotee_materialize(ia);
                        vm_pop();
                        if (img == NULL) {
                            char pbuf[256];
                            const char* p = ia->path != NULL ? ia->path->chars : "";
                            int pl = (int)strlen(p);
                            if (pl > (int)sizeof(pbuf) - 1) pl = (int)sizeof(pbuf) - 1;
                            memcpy(pbuf, p, (size_t)pl);
                            pbuf[pl] = '\0';
                            owl_emit_runtime_error(E_NATIVE_IMAGE_LOAD_FAILURE,
                                                   "native", "ImageAnnot\xc3\xa9""e",
                                                   "path", pbuf,
                                                   "detail",
                                                   "fichier absent ou format non reconnu",
                                                   NULL);
                            if (exceptionCount > 0 && currentException != NULL) {
                                if (handle_runtime_exception()) {
                                    frame = &vm.frames[vm.frameCount - 1];
                                    DISPATCH();
                                }
                            }
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        vm_push(OBJ_VAL(img));
                        DISPATCH();
                    }
                    COLL_NO_PROP("..Chemin, ..Boîtes, ..Image",
                                 "..Path, ..Boxes, ..Image");
                    DISPATCH();
                }

                /*  — Dataset : ..Taille/..Count (entier),
                 * ..NbClasses/..ClassCount (entier), ..Classes/..Classes
                 * (tableau de chaines, ordre = index de classe) et
                 * ..Source/..Source (chaine, chemin passe au chargeur). */
                if (IS_DATASET(instVal)) {
                    ObjDataset* ds = AS_DATASET(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 6 && strncasecmp(fn, "taille", 6) == 0)
                        || (fl == 5 && strncasecmp(fn, "count", 5) == 0)) {
                        vm_push(INTEGER_VAL(ds->count));
                        DISPATCH();
                    }
                    if ((fl == 9 && strncasecmp(fn, "nbclasses", 9) == 0)
                        || (fl == 10 && strncasecmp(fn, "classcount", 10) == 0)) {
                        vm_push(INTEGER_VAL(ds->classCount));
                        DISPATCH();
                    }
                    if (fl == 7 && strncasecmp(fn, "classes", 7) == 0) {
                        /* Root ds pendant l'alloc du tableau + des chaines. */
                        vm_push(instVal);
                        ObjArray* arr = obj_new_array();
                        vm_push(OBJ_VAL(arr));
                        for (int i = 0; i < ds->classCount; i++) {
                            value_array_write(&arr->elements,
                                              OBJ_VAL(ds->classes[i]));
                        }
                        vm_pop();   /* arr */
                        vm_pop();   /* instVal */
                        vm_push(OBJ_VAL(arr));
                        DISPATCH();
                    }
                    if (fl == 6 && strncasecmp(fn, "source", 6) == 0) {
                        vm_push(ds->source != NULL ? OBJ_VAL(ds->source)
                                                   : OBJ_VAL(obj_copy_string("", 0)));
                        DISPATCH();
                    }
                    COLL_NO_PROP("..Taille, ..NbClasses, ..Classes, ..Source",
                                 "..Count, ..ClassCount, ..Classes, ..Source");
                    DISPATCH();
                }

                /*  — OptionsEntraînement : lecture des proprietes de
                 * config (double point). Noms accent-strippes par le scanner :
                 * ..Époques -> "epoques". Chaque propriete se relit exactement
                 * comme ecrite (cf. branche OP_STRUCT_SET symetrique). */
                if (IS_OPTIONS_ENTRAINEMENT(instVal)) {
                    ObjOptionsEntrainement* o = AS_OPTIONS_ENTRAINEMENT(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 7 && strncasecmp(fn, "epoques", 7) == 0)
                        || (fl == 6 && strncasecmp(fn, "epochs", 6) == 0)) {
                        vm_push(INTEGER_VAL(o->epochs));
                    } else if ((fl == 9 && strncasecmp(fn, "taillelot", 9) == 0)
                        || (fl == 9 && strncasecmp(fn, "batchsize", 9) == 0)) {
                        vm_push(INTEGER_VAL(o->batchSize));
                    } else if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        vm_push(REAL_VAL(o->learningRate));
                    } else if ((fl == 10 && strncasecmp(fn, "optimiseur", 10) == 0)
                        || (fl == 9 && strncasecmp(fn, "optimizer", 9) == 0)) {
                        vm_push(INTEGER_VAL(o->optimizer));
                    } else if ((fl == 5 && strncasecmp(fn, "perte", 5) == 0)
                        || (fl == 4 && strncasecmp(fn, "loss", 4) == 0)) {
                        vm_push(INTEGER_VAL(o->loss));
                    } else if ((fl == 8 && strncasecmp(fn, "plantaux", 8) == 0)
                        || (fl == 10 && strncasecmp(fn, "lrschedule", 10) == 0)) {
                        /*  — plan de decroissance du taux (sentinel plan*). */
                        vm_push(INTEGER_VAL(o->lrSchedule));
                    } else if ((fl == 17 && strncasecmp(fn, "decroissancepoids", 17) == 0)
                        || (fl == 11 && strncasecmp(fn, "weightdecay", 11) == 0)) {
                        /*  — coefficient de regularisation L2 (0.0 = off). */
                        vm_push(REAL_VAL(o->weightDecay));
                    } else if ((fl == 18 && strncasecmp(fn, "fractionvalidation", 18) == 0)
                        || (fl == 18 && strncasecmp(fn, "validationfraction", 18) == 0)) {
                        /*  — fraction du jeu reservee a la validation (0.0 = off). */
                        vm_push(REAL_VAL(o->validationFraction));
                    } else if ((fl == 19 && strncasecmp(fn, "gardemeilleurmodele", 19) == 0)
                        || (fl == 13 && strncasecmp(fn, "keepbestmodel", 13) == 0)) {
                        /*  — garde les poids de la meilleure epoque validation. */
                        vm_push(BOOL_VAL(o->keepBestModel != 0));
                    } else if ((fl == 13 && strncasecmp(fn, "patiencearret", 13) == 0)
                        || (fl == 12 && strncasecmp(fn, "stoppatience", 12) == 0)) {
                        /*  — patience d'arret anticipe en epoques (0 = off). */
                        vm_push(INTEGER_VAL(o->stopPatience));
                    } else if ((fl == 9 && strncasecmp(fn, "surepoque", 9) == 0)
                        || (fl == 7 && strncasecmp(fn, "onepoch", 7) == 0)) {
                        /*  — reference de procedure rappelee par
                         * .Entraîne a chaque fin d'epoque (NIL si non affectee). */
                        vm_push(o->onEpoch);
                    } else {
                        COLL_NO_PROP(
                            "..Époques, ..TailleLot, ..TauxApprentissage, "
                            "..Optimiseur, ..Perte, ..PlanTaux, "
                            "..DécroissancePoids, ..FractionValidation, "
                            "..GardeMeilleurModèle, ..PatienceArrêt, ..SurÉpoque",
                            "..Epochs, ..BatchSize, ..LearningRate, "
                            "..Optimizer, ..Loss, ..LRSchedule, "
                            "..WeightDecay, ..ValidationFraction, "
                            "..KeepBestModel, ..StopPatience, ..OnEpoch");
                    }
                    DISPATCH();
                }

                /*  — OptionsGénération : lecture des reglages de
                 * generation (double point). Noms accent-strippes par le
                 * scanner (..Température -> "temperature"). Chaque propriete se
                 * relit exactement comme ecrite (branche OP_STRUCT_SET symetrique). */
                if (IS_OPTIONS_GENERATION(instVal)) {
                    ObjOptionsGeneration* o = AS_OPTIONS_GENERATION(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if (fl == 11 && strncasecmp(fn, "temperature", 11) == 0) {
                        vm_push(REAL_VAL(o->temperature));
                    } else if (fl == 4 && strncasecmp(fn, "topk", 4) == 0) {
                        vm_push(INTEGER_VAL(o->topK));
                    } else if (fl == 4 && strncasecmp(fn, "topp", 4) == 0) {
                        vm_push(REAL_VAL(o->topP));
                    } else if (fl == 9 && strncasecmp(fn, "maxtokens", 9) == 0) {
                        vm_push(INTEGER_VAL(o->maxTokens));
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        vm_push(INTEGER_VAL(o->seed));
                    } else if ((fl == 8 && strncasecmp(fn, "surtoken", 8) == 0)
                        || (fl == 7 && strncasecmp(fn, "ontoken", 7) == 0)) {
                        /*  — reference de procedure rappelee par
                         * .Génère a chaque token (NIL si non affectee). */
                        vm_push(o->onToken);
                    } else if ((fl == 18 && strncasecmp(fn, "contraintedesortie", 18) == 0)
                        || (fl == 16 && strncasecmp(fn, "outputconstraint", 16) == 0)) {
                        /*  — contrainte de sortie open-enum (chaine, vide
                         * par defaut). NIL_VAL interne = relu comme chaine vide
                         * (contrainteLibre). */
                        vm_push(IS_STRING(o->constraint) ? o->constraint
                                                         : OBJ_VAL(obj_copy_string("", 0)));
                    } else {
                        COLL_NO_PROP(
                            "..Température, ..TopK, ..TopP, ..MaxTokens, "
                            "..Graine, ..SurToken, ..ContrainteDeSortie",
                            "..Temperature, ..TopK, ..TopP, ..MaxTokens, "
                            "..Seed, ..OnToken, ..OutputConstraint");
                    }
                    DISPATCH();
                }

                /*  — MessageLLM : lecture des proprietes ..Rôle /
                 * ..Contenu (double point). Noms accent-strippes par le scanner
                 * (..Rôle -> "role"). Une propriete jamais affectee relit ""
                 * (chaine vide). Symetrique de la branche OP_STRUCT_SET. */
                if (IS_MESSAGE_LLM(instVal)) {
                    ObjMessageLLM* m = AS_MESSAGE_LLM(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if (fl == 4 && strncasecmp(fn, "role", 4) == 0) {
                        vm_push(IS_STRING(m->role) ? m->role
                                                   : OBJ_VAL(obj_copy_string("", 0)));
                    } else if ((fl == 7 && strncasecmp(fn, "contenu", 7) == 0)
                        || (fl == 7 && strncasecmp(fn, "content", 7) == 0)) {
                        vm_push(IS_STRING(m->content) ? m->content
                                                      : OBJ_VAL(obj_copy_string("", 0)));
                    } else {
                        COLL_NO_PROP("..Rôle, ..Contenu", "..Role, ..Content");
                    }
                    DISPATCH();
                }

                /*  — RéponseLLM : lecture des 4 proprietes (sortie
                 * moteur, lecture seule). ..Contenu, ..Raison (enum stricte
                 * E_raisonStop), ..JetonsEntrée, ..JetonsSortie. Noms
                 * accent-strippes par le scanner. */
                if (IS_RESPONSE_LLM(instVal)) {
                    ObjResponseLLM* r = AS_RESPONSE_LLM(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 7 && strncasecmp(fn, "contenu", 7) == 0)
                        || (fl == 7 && strncasecmp(fn, "content", 7) == 0)) {
                        vm_push(IS_STRING(r->content) ? r->content
                                                      : OBJ_VAL(obj_copy_string("", 0)));
                    } else if ((fl == 6 && strncasecmp(fn, "raison", 6) == 0)
                        || (fl == 6 && strncasecmp(fn, "reason", 6) == 0)) {
                        vm_push(INTEGER_VAL(r->reason));
                    } else if ((fl == 12 && strncasecmp(fn, "jetonsentree", 12) == 0)
                        || (fl == 11 && strncasecmp(fn, "inputtokens", 11) == 0)) {
                        vm_push(INTEGER_VAL(r->inputTokens));
                    } else if ((fl == 12 && strncasecmp(fn, "jetonssortie", 12) == 0)
                        || (fl == 12 && strncasecmp(fn, "outputtokens", 12) == 0)) {
                        vm_push(INTEGER_VAL(r->outputTokens));
                    } else {
                        COLL_NO_PROP(
                            "..Contenu, ..Raison, ..JetonsEntrée, ..JetonsSortie",
                            "..Content, ..Reason, ..InputTokens, ..OutputTokens");
                    }
                    DISPATCH();
                }

                /*  — Détection : lecture des 3 proprietes (sortie de
                 * .Détecte, lecture seule). ..Boîte (tableau de 4 reels [x, y,
                 * largeur, hauteur]), ..Classe (entier), ..Score (reel). Noms
                 * accent-strippes par le scanner (..Boîte -> "boite"). */
                if (IS_DETECTION(instVal)) {
                    ObjDetection* det = AS_DETECTION(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if ((fl == 5 && strncasecmp(fn, "boite", 5) == 0)
                        || (fl == 3 && strncasecmp(fn, "box", 3) == 0)) {
                        vm_push(IS_ARRAY(det->box) ? det->box
                                                   : OBJ_VAL(obj_new_array()));
                    } else if ((fl == 6 && strncasecmp(fn, "classe", 6) == 0)
                        || (fl == 5 && strncasecmp(fn, "class", 5) == 0)) {
                        vm_push(INTEGER_VAL(det->classIndex));
                    } else if (fl == 5 && strncasecmp(fn, "score", 5) == 0) {
                        vm_push(REAL_VAL(det->score));
                    } else {
                        COLL_NO_PROP("..Boîte, ..Classe, ..Score",
                                     "..Box, ..Class, ..Score");
                    }
                    DISPATCH();
                }

                /*  — Vecteur : ..Taille / ..Length = nombre
                 * d'elements (propriete, double point). ..Vide/..Empty =
                 * booleen.  : ..Occurrence retiree (redondante avec
                 * ..Taille) — ne s'applique qu'au Vecteur, le tableau garde
                 * ..Occurrence via sa propre branche IS_ARRAY plus bas. */
                /*  — xlsDocument : proprietes lecture seule
                 * ..NombreFeuille / ..NombreLigne / ..NombreColonne. Cablees
                 * cote VM (valeurs calculees a la volee), comme ..Taille du
                 * Vecteur. */
                if (IS_XLS_DOCUMENT(instVal)) {
                    Value out;
                    if (stdlib_xls_get_property(instVal, fieldName->chars, &out)) {
                        vm_push(out);
                    } else if (stdlib_xls_document_subref(instVal, fieldName->chars, &out)) {
                        /*  — ..Cellule / ..Colonne / ..Ligne : ref non liee. */
                        vm_push(out);
                    } else {
                        COLL_NO_PROP(
                            "..NombreFeuille, ..NombreLigne, ..NombreColonne, ..Cellule, ..Ligne, ..Colonne",
                            "..SheetCount, ..RowCount, ..ColumnCount, ..Cell, ..Row, ..Column");
                    }
                    DISPATCH();
                }

                /*  — reference de style liee : `...[l,c]..CouleurFond`. */
                if (IS_XLS_REF(instVal)) {
                    vm_push(instVal);   /* root : la lecture d'un composite alloue (clone) */
                    Value out;
                    bool ok = stdlib_xls_ref_get_property(instVal, fieldName->chars, &out);
                    vm_pop();
                    if (ok) {
                        vm_push(out);
                    } else {
                        COLL_NO_PROP(
                            "..CouleurFond, ..Police, ..Cadre, ..AlignementH, ..AlignementV, ..Multiligne, ..Masque, ..Largeur, ..Hauteur",
                            "..BackgroundColor, ..Font, ..Border, ..AlignmentH, ..AlignmentV, ..Multiline, ..Mask, ..Width, ..Height");
                    }
                    DISPATCH();
                }

                if (IS_VECTOR(instVal)) {
                    ObjVector* vec = AS_VECTOR(instVal);
                    if (fieldName->length == 6
                        && (strncasecmp(fieldName->chars, "taille", 6) == 0
                            || strncasecmp(fieldName->chars, "length", 6) == 0)) {
                        vm_push(INTEGER_VAL(vec->count));
                    } else if ((fieldName->length == 4
                                && strncasecmp(fieldName->chars, "vide", 4) == 0)
                               || (fieldName->length == 5
                                   && strncasecmp(fieldName->chars, "empty", 5) == 0)) {
                        vm_push(BOOL_VAL(vec->count == 0));
                    } else {
                        COLL_NO_PROP("..Taille, ..Vide", "..Length, ..Empty");
                    }
                    DISPATCH();
                }

                /*  — Matrice : ..Lignes/..Rows = nombre de lignes,
                 * ..Colonnes/..Columns = nombre de colonnes (proprietes, double
                 * point). Attributs structurels intrinseques, cables cote VM
                 * comme ..Taille du Vecteur. */
                if (IS_MATRIX(instVal)) {
                    ObjMatrix* mat = AS_MATRIX(instVal);
                    if ((fieldName->length == 6
                         && strncasecmp(fieldName->chars, "lignes", 6) == 0)
                        || (fieldName->length == 4
                            && strncasecmp(fieldName->chars, "rows", 4) == 0)) {
                        vm_push(INTEGER_VAL(mat->rows));
                    } else if ((fieldName->length == 8
                                && strncasecmp(fieldName->chars, "colonnes", 8) == 0)
                               || (fieldName->length == 7
                                   && strncasecmp(fieldName->chars, "columns", 7) == 0)) {
                        vm_push(INTEGER_VAL(mat->cols));
                    } else {
                        COLL_NO_PROP("..Lignes, ..Colonnes",
                                     "..Rows, ..Columns");
                    }
                    DISPATCH();
                }

                /* Array property access: .Occurrence, .Vide */
                if (IS_ARRAY(instVal)) {
                    ObjArray* arr = AS_ARRAY(instVal);
                    if (fieldName->length == 10 && strncasecmp(fieldName->chars, "occurrence", 10) == 0) {
                        vm_push(INTEGER_VAL(arr->elements.count));
                    } else if (fieldName->length == 4 && strncasecmp(fieldName->chars, "vide", 4) == 0) {
                        vm_push(BOOL_VAL(arr->elements.count == 0));
                    } else if (fieldName->length == 5 && strncasecmp(fieldName->chars, "empty", 5) == 0) {
                        vm_push(BOOL_VAL(arr->elements.count == 0));
                    } else {
                        COLL_NO_PROP("..Occurrence, ..Vide",
                                     "..Occurrence, ..Empty");
                    }
                    DISPATCH();
                }

                /* Collection property access: .Occurrence, .Vide */
                if (IS_COLLECTION(instVal)) {
                    ObjCollection* coll = AS_COLLECTION(instVal);
                    if (fieldName->length == 10 && strncasecmp(fieldName->chars, "occurrence", 10) == 0) {
                        vm_push(INTEGER_VAL(coll->elements.count));
                    } else if (fieldName->length == 4 && strncasecmp(fieldName->chars, "vide", 4) == 0) {
                        vm_push(BOOL_VAL(coll->elements.count == 0));
                    } else if (fieldName->length == 5 && strncasecmp(fieldName->chars, "empty", 5) == 0) {
                        vm_push(BOOL_VAL(coll->elements.count == 0));
                    } else {
                        COLL_NO_PROP("..Occurrence, ..Vide",
                                     "..Occurrence, ..Empty");
                    }
                    DISPATCH();
                }
                #undef COLL_NO_PROP

                /* JSON property access: jDoc.nom, jDoc.Occurrence, etc. */
                if (IS_JSON(instVal)) {
                    ObjJson* json = AS_JSON(instVal);
                    /* Built-in properties */
                    if (strncasecmp(fieldName->chars, "occurrence", 10) == 0 && fieldName->length == 10) {
                        int count = 0;
                        if (json->jtype == JSON_T_ARRAY) count = json->as.arrayVal.count;
                        else if (json->jtype == JSON_T_OBJECT) count = json->as.objectVal.count;
                        vm_push(INTEGER_VAL(count));
                        DISPATCH();
                    }
                    if ((strncasecmp(fieldName->chars, "vide", 4) == 0 && fieldName->length == 4) ||
                        (strncasecmp(fieldName->chars, "empty", 5) == 0 && fieldName->length == 5)) {
                        bool empty = true;
                        if (json->jtype == JSON_T_ARRAY) empty = json->as.arrayVal.count == 0;
                        else if (json->jtype == JSON_T_OBJECT) empty = json->as.objectVal.count == 0;
                        else if (json->jtype != JSON_T_NULL) empty = false;
                        vm_push(BOOL_VAL(empty));
                        DISPATCH();
                    }
                    if (strncasecmp(fieldName->chars, "type", 4) == 0 && fieldName->length == 4) {
                        const char* t = "null";
                        switch (json->jtype) {
                            case JSON_T_NULL: t = "null"; break;
                            case JSON_T_BOOL: t = "booleen"; break;
                            case JSON_T_INT: t = "entier"; break;
                            case JSON_T_REAL: t = "reel"; break;
                            case JSON_T_CURRENCY: t = "monetaire"; break;
                            case JSON_T_NUMERIC: t = "numerique"; break;
                            case JSON_T_STRING: t = "chaine"; break;
                            case JSON_T_ARRAY: t = "tableau"; break;
                            case JSON_T_OBJECT: t = "objet"; break;
                        }
                        vm_push(OBJ_VAL(obj_copy_string(t, (int)strlen(t))));
                        DISPATCH();
                    }
                    /* Field access on JSON object */
                    if (json->jtype == JSON_T_OBJECT) {
                        ObjJson* child = obj_json_object_get(json, fieldName);
                        if (child) {
                            vm_push(obj_json_to_value(child));
                        } else if (opAutoVivKind != 0) {
                            /*  — descente LVALUE : membre absent d'un objet
                             * JSON. On materialise le conteneur du type requis par
                             * le segment suivant (_OBJ -> objet, _ARR -> tableau) et
                             * on le relie a son parent (reference partagee : la
                             * mutation du segment plus profond remonte). La lecture
                             * pure (opAutoVivKind == 0) reste NIL, sans creation. */
                            ObjJson* child2 = obj_new_json(
                                opAutoVivKind == 2 ? JSON_T_ARRAY : JSON_T_OBJECT);
                            vm_push(OBJ_VAL(child2)); /* root avant object_set */
                            obj_json_object_set(json, fieldName, child2);
                            vm_pop();
                            vm_push(OBJ_VAL(child2));
                        } else {
                            vm_push(NIL_VAL);
                        }
                    } else {
                        vm_push(NIL_VAL);
                    }
                    DISPATCH();
                }

                /* Date property access: .Annee, .Mois, .Jour */
                if (IS_DATE(instVal)) {
                    ObjDate* d = AS_DATE(instVal);
                    if (strncasecmp(fieldName->chars, "annee", 5) == 0 ||
                        strncasecmp(fieldName->chars, "year", 4) == 0)
                        vm_push(INTEGER_VAL(d->year));
                    else if (strncasecmp(fieldName->chars, "mois", 4) == 0 ||
                             strncasecmp(fieldName->chars, "month", 5) == 0)
                        vm_push(INTEGER_VAL(d->month));
                    else if (strncasecmp(fieldName->chars, "jour", 4) == 0 ||
                             strncasecmp(fieldName->chars, "day", 3) == 0)
                        vm_push(INTEGER_VAL(d->day));
                    else vm_push(NIL_VAL);
                    DISPATCH();
                }

                /* Time property access: .Heure, .Minute, .Seconde, .Milliseconde */
                if (IS_TIME(instVal)) {
                    ObjTime* t = AS_TIME(instVal);
                    if (strncasecmp(fieldName->chars, "heure", 5) == 0 ||
                        strncasecmp(fieldName->chars, "hour", 4) == 0)
                        vm_push(INTEGER_VAL(t->hour));
                    else if (strncasecmp(fieldName->chars, "minute", 6) == 0)
                        vm_push(INTEGER_VAL(t->minute));
                    else if (strncasecmp(fieldName->chars, "seconde", 7) == 0 ||
                             strncasecmp(fieldName->chars, "second", 6) == 0)
                        vm_push(INTEGER_VAL(t->second));
                    else if (strncasecmp(fieldName->chars, "milliseconde", 12) == 0 ||
                             strncasecmp(fieldName->chars, "millisecond", 11) == 0)
                        vm_push(INTEGER_VAL(t->ms));
                    else vm_push(NIL_VAL);
                    DISPATCH();
                }

                /* DateTime property access: all date + time properties + PartieDate/PartieHeure */
                if (IS_DATETIME(instVal)) {
                    ObjDateTime* dt = AS_DATETIME(instVal);
                    if (strncasecmp(fieldName->chars, "annee", 5) == 0 ||
                        strncasecmp(fieldName->chars, "year", 4) == 0)
                        vm_push(INTEGER_VAL(dt->year));
                    else if (strncasecmp(fieldName->chars, "mois", 4) == 0 ||
                             strncasecmp(fieldName->chars, "month", 5) == 0)
                        vm_push(INTEGER_VAL(dt->month));
                    else if (strncasecmp(fieldName->chars, "jour", 4) == 0 ||
                             strncasecmp(fieldName->chars, "day", 3) == 0)
                        vm_push(INTEGER_VAL(dt->day));
                    else if (strncasecmp(fieldName->chars, "heure", 5) == 0 ||
                             strncasecmp(fieldName->chars, "hour", 4) == 0)
                        vm_push(INTEGER_VAL(dt->hour));
                    else if (strncasecmp(fieldName->chars, "minute", 6) == 0)
                        vm_push(INTEGER_VAL(dt->minute));
                    else if (strncasecmp(fieldName->chars, "seconde", 7) == 0 ||
                             strncasecmp(fieldName->chars, "second", 6) == 0)
                        vm_push(INTEGER_VAL(dt->second));
                    else if (strncasecmp(fieldName->chars, "partiedate", 10) == 0 ||
                             strncasecmp(fieldName->chars, "datepart", 8) == 0)
                        vm_push(OBJ_VAL(obj_new_date(dt->year, dt->month, dt->day)));
                    else if (strncasecmp(fieldName->chars, "partieheure", 11) == 0 ||
                             strncasecmp(fieldName->chars, "timepart", 8) == 0)
                        vm_push(OBJ_VAL(obj_new_time(dt->hour, dt->minute, dt->second, dt->ms)));
                    else vm_push(NIL_VAL);
                    DISPATCH();
                }

                /* Duration property access:
                 *   Totaux (réels, extension OWL alignée canon):
                 *     EnJours, EnHeures, EnMinutes, EnSecondes, EnMillisecondes
                 *   Composantes décomposées (entiers, canoniques):
                 *     Jour, Heure, Minute, Seconde, Milliseconde
                 * Pour une durée négative, Jour est négatif et les autres composantes
                 * sont aussi négatives (ou 0), cohérent avec le signe. */
                if (IS_DURATION(instVal)) {
                    ObjDuration* dur = AS_DURATION(instVal);
                    int64_t ms = dur->totalMs;
                    const char* fn = fieldName->chars;
                    /* Totaux (ordre longueurs décroissantes pour éviter shadowing) */
                    if (strcasecmp(fn, "enmillisecondes") == 0 ||
                        strcasecmp(fn, "inmilliseconds") == 0)
                        vm_push(INTEGER_VAL(ms));
                    else if (strcasecmp(fn, "ensecondes") == 0 ||
                             strcasecmp(fn, "inseconds") == 0)
                        vm_push(REAL_VAL((double)ms / 1000.0));
                    else if (strcasecmp(fn, "enminutes") == 0 ||
                             strcasecmp(fn, "inminutes") == 0)
                        vm_push(REAL_VAL((double)ms / 60000.0));
                    else if (strcasecmp(fn, "enheures") == 0 ||
                             strcasecmp(fn, "inhours") == 0)
                        vm_push(REAL_VAL((double)ms / 3600000.0));
                    else if (strcasecmp(fn, "enjours") == 0 ||
                             strcasecmp(fn, "indays") == 0)
                        vm_push(REAL_VAL((double)ms / 86400000.0));
                    /* Composantes décomposées (canoniques) */
                    else if (strcasecmp(fn, "milliseconde") == 0 ||
                             strcasecmp(fn, "millisecond") == 0)
                        vm_push(INTEGER_VAL(ms % 1000));
                    else if (strcasecmp(fn, "seconde") == 0 ||
                             strcasecmp(fn, "second") == 0)
                        vm_push(INTEGER_VAL((ms / 1000) % 60));
                    else if (strcasecmp(fn, "minute") == 0)
                        vm_push(INTEGER_VAL((ms / 60000) % 60));
                    else if (strcasecmp(fn, "heure") == 0 ||
                             strcasecmp(fn, "hour") == 0)
                        vm_push(INTEGER_VAL((ms / 3600000) % 24));
                    else if (strcasecmp(fn, "jour") == 0 ||
                             strcasecmp(fn, "day") == 0)
                        vm_push(INTEGER_VAL(ms / 86400000LL));
                    else vm_push(NIL_VAL);
                    DISPATCH();
                }

                /*  Lot 2e-1 — Namespace catalogue `icones.<nom>`.
                 * fieldName arrive sans accents (parsing canon), on resout
                 * via la stdlib. Hit : push l'ObjIcone materialise. Miss :
                 * la stdlib emet une erreur native + push un ObjIcone
                 * invalide. L'invariant pile reste preserve dans les deux
                 * cas. */
                if (IS_ICONES_NAMESPACE(instVal)) {
                    Value icn = owl_icones_namespace_lookup(fieldName->chars);
                    vm_push(icn);
                    DISPATCH();
                }

                /*  Lot A — Namespace styles `theme.<nom>`. Resout vers
                 * l'instance de style nommee declaree dans le bloc `est un
                 * Theme` du fichier designe par Projet..Theme. Les styles du
                 * bloc sont mangles `theme$<nom>` au compile (scoping strict :
                 * un style global nu declare hors bloc N'EST PAS resolu ici).
                 * On reconstruit la cle manglee et on la resout via le lookup
                 * canon (casse+accents), on valide que la cible est une
                 * instance de style typee, puis on push l'instance elle-meme
                 * (semantique reference identique a un read global nu — pas de
                 * semantique neuve). Miss (nom absent ou mauvais type) : erreur
                 * soft (ErreurDetectee) + push une sentinelle navigable
                 * (null-object) au lieu de NIL, pour que le chainage
                 * `theme.miss..X..Y` ne hard-crashe pas au second sous-acces.
                 * Contrat calque sur `icones.<nom>` (miss = objet navigable
                 * invalide, pas NIL). */
                if (IS_THEME_NAMESPACE(instVal)) {
                    Value styleVal;
                    char mangled[320];
                    int ml = snprintf(mangled, sizeof(mangled), "theme$%.*s",
                                      fieldName->length, fieldName->chars);
                    ObjString* mangledKey = NULL;
                    if (ml > 0 && ml < (int)sizeof(mangled)) {
                        mangledKey = obj_copy_string(mangled, ml);
                    }
                    if (mangledKey != NULL) vm_push(OBJ_VAL(mangledKey));
                    bool hit = mangledKey != NULL
                            && vm_globals_canon_get(mangledKey, &styleVal, NULL)
                            && owl_theme_is_style_instance(styleVal);
                    if (mangledKey != NULL) vm_pop(); /* mangledKey */
                    if (hit) {
                        vm_push(styleVal);
                    } else {
                        owl_theme_namespace_emit_miss(fieldName->chars);
                        vm_push(owl_theme_make_miss_sentinel(fieldName->chars));
                    }
                    DISPATCH();
                }

                /*  Lot 2e-1 — Props d'une ObjIcone. Slots typees
                 * directs (pas de table_get) : dispatch O(len) sur le nom de
                 * champ canonique. Lookup case+accent-insensible via le
                 * scanner strip + strcasecmp. Accepte les variantes EN
                 * (Color/Rotation/Shape/Valid/ColorEditable/Icon).
                 *
                 * ..Type est traite ici aussi : retourne "Icone" (chaine
                 * litterale) pour symetrie avec d'autres types reifies. */
                if (IS_ICONE(instVal)) {
                    ObjIcone* icn = AS_ICONE(instVal);
                    char fn[64];
                    int fnlen = scanner_strip_accents(fieldName->chars,
                                                     fieldName->length,
                                                     fn, sizeof(fn));
                    (void)fnlen;
                    if (strcasecmp(fn, "Couleur") == 0
                        || strcasecmp(fn, "Color") == 0) {
                        /* couleur == -1 = unset -> retourne 0 (noir),
                         * semantique canon "couleur par defaut". */
                        vm_push(INTEGER_VAL(icn->couleur == -1 ? 0 : icn->couleur));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Rotation") == 0) {
                        vm_push(INTEGER_VAL(icn->rotation));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Forme") == 0
                        || strcasecmp(fn, "Shape") == 0) {
                        vm_push(INTEGER_VAL(icn->forme));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "CouleurFond") == 0
                        || strcasecmp(fn, "BackgroundColor") == 0) {
                        /*  : -1 = transparent, retourne 0 (semantique
                         * coherente avec ..Couleur unset). */
                        vm_push(INTEGER_VAL(icn->couleur_fond == -1 ? 0 : icn->couleur_fond));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Valide") == 0
                        || strcasecmp(fn, "Valid") == 0) {
                        vm_push(BOOL_VAL(icn->valide));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "CouleurModifiable") == 0
                        || strcasecmp(fn, "ColorEditable") == 0) {
                        vm_push(BOOL_VAL(icn->couleur_modifiable));
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Icone") == 0
                        || strcasecmp(fn, "Icon") == 0) {
                        /* GET ..Icone : retourne le SVG raw. Si null,
                         * chaine vide. */
                        if (icn->svg_raw != NULL) {
                            vm_push(OBJ_VAL(icn->svg_raw));
                        } else {
                            vm_push(OBJ_VAL(obj_copy_string("", 0)));
                        }
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Type") == 0) {
                        vm_push(OBJ_VAL(obj_copy_string("Icone", 5)));
                        DISPATCH();
                    }
                    /* Prop inconnue : NIL (cf. comportement ObjInstance miss). */
                    vm_push(NIL_VAL);
                    DISPATCH();
                }

                /* Support class-level field access (e.g. enum values, GLOBAL
                 * members ). Ordre lookup : staticFields (GLOBAL) →
                 * defaults (enum values + defaults exposes au niveau classe). */
                if (IS_OBJ(instVal) && OBJ_TYPE(instVal) == OBJ_CLASS) {
                    ObjClass* klass = AS_CLASS(instVal);
                    Value value;
                    if (table_get(&klass->staticFields, fieldName, &value)) {
                        vm_push(value);
                    } else if (table_get(&klass->defaults, fieldName, &value)) {
                        vm_push(value);
                    } else {
                        /*  : peut etre une methode globale referencee
                         * sans appel (cMath.Carre stocke dans une variable).
                         * Cherche aussi dans methods. */
                        Value method;
                        if (table_get(&klass->methods, fieldName, &method)) {
                            vm_push(method);
                        } else {
                            /*  : invariant 1 — acces membre
                             * case-insensitive sur les noms utilisateur.
                             * Fallback scan sur miss exact (constantes de
                             * classe, GLOBAL, methodes). */
                            ObjString* k;
                            if ((k = table_find_key_case_insensitive(
                                     &klass->staticFields, fieldName->chars))
                                    != NULL
                                && table_get(&klass->staticFields, k, &value)) {
                                vm_push(value);
                            } else if ((k = table_find_key_case_insensitive(
                                            &klass->defaults, fieldName->chars))
                                           != NULL
                                       && table_get(&klass->defaults, k,
                                                    &value)) {
                                vm_push(value);
                            } else if ((k = table_find_key_case_insensitive(
                                            &klass->methods, fieldName->chars))
                                           != NULL
                                       && table_get(&klass->methods, k,
                                                    &value)) {
                                vm_push(value);
                            } else {
                                vm_push(NIL_VAL);
                            }
                        }
                    }
                    DISPATCH();
                }

                /*  — propriete sur chaine (canon `v..Taille`).
                 * Sans cette branche, `v..Taille` tombait sur le fallback
                 * !IS_INSTANCE et emettait "valeur Null" trompeur. Seules
                 * ..Taille / ..Length sont supportees ici (scope ticket
                 * 0331). Toute autre propriete chaine emet une erreur
                 * explicite, pas le message Null generique. cp_count est
                 * memoize sur ObjString (compute lazy si -1). */
                if (IS_STRING(instVal)) {
                    ObjString* sval = AS_STRING(instVal);
                    bool isTaille =
                        (fieldName->length == 6
                         && (strncasecmp(fieldName->chars, "taille", 6) == 0
                             || strncasecmp(fieldName->chars, "length", 6) == 0));
                    if (isTaille) {
                        int cp = sval->cp_count;
                        if (cp < 0) {
                            /* Lazy compute UTF-8 code points (coherent avec
                             * str_cp_count dans stdlib_string.c). Continuation
                             * byte = 0x80..0xBF, on compte les autres. */
                            cp = 0;
                            for (int i = 0; i < sval->length; i++) {
                                if (((unsigned char)sval->chars[i] & 0xC0) != 0x80) cp++;
                            }
                            sval->cp_count = cp;
                        }
                        vm_push(INTEGER_VAL(cp));
                        DISPATCH();
                    }
                    char nameBuf[128];
                    int nl = fieldName->length < (int)sizeof(nameBuf) - 1
                                 ? fieldName->length
                                 : (int)sizeof(nameBuf) - 1;
                    memcpy(nameBuf, fieldName->chars, nl);
                    nameBuf[nl] = '\0';
                    owl_emit_runtime_error(E_RT_STRING_HAS_NO_PROP,
                                           "name", nameBuf, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!IS_INSTANCE(instVal)) {
                    owl_emit_runtime_error(E_RT_FIELDS_NEED_INSTANCE, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(instVal);
                /*  — sentinelle de miss `theme.<nom>` : null-object
                 * navigable. Tout sous-acces se replie sur une sentinelle (au
                 * lieu du NIL de fin de fallback), donc `theme.miss..X..Y`
                 * reste tolere a toute profondeur sans second crash.
                 * Branche haute : pas de champs reels a lire sur ce vehicule.
                 *
                 *  — la sentinelle rendue est FRAICHE, jamais `instVal`.
                 * Rendre le meme objet permettait au write-back d'une cascade
                 * (`..Style..Brique..Feuille = v` reecrit la brique lue dans son
                 * parent) de poser `S.fields[Brique] = S` : graphe cyclique, et
                 * value_deep_clone (semantique COPIE du set `..Style = inst`)
                 * recursait a l'infini -> debordement de pile, SIGSEGV muet.
                 * Une instance fraiche par niveau rend le cycle inconstructible :
                 * la profondeur du graphe reste bornee par celle de la cascade. */
                if (owl_theme_is_miss_sentinel(instVal)) {
                    vm_push(owl_theme_make_miss_sentinel(NULL));
                    DISPATCH();
                }
                /*  — cellule de rep ZoneRepetee : `ZR[i].champ` doit
                 * materialiser la cellule en ObjUiControl (store de props par
                 * cellule) AVANT le table_get generique, pour que le `..Prop`
                 * suivant tombe dans la branche IS_UI_CONTROL. Garde stricte
                 * cote hook (rep taggee $zrsrc + kind ZR + field = sous-
                 * controle) : NIL_VAL pour toute autre instance (struct user,
                 * rep Table) -> comportement actuel inchange. */
                {
                    Value cell = owl_ui_zr_rep_cell_subcontrol(
                        instance, fieldName->chars);
                    if (!IS_NIL(cell)) {
                        vm_push(cell);
                        DISPATCH();
                    }
                }
                /*  — relecture de la mise en forme d'une cellule ou
                 * d'une ligne de Table. Null tant qu'aucune couleur n'a ete
                 * posee. Aucune allocation : pas de rooting a prevoir. */
                {
                    Value fmtV;
                    if (vm_table_fmt_get(instance, fieldName->chars, &fmtV)) {
                        vm_push(fmtV);
                        DISPATCH();
                    }
                }
                /*  — receveur colonne `COL_X..MasqueSaisie` (prop COLONNE,
                 * hors catalogue cellule). Store OWL-side + fallback declare. */
                {
                    Value maskV;
                    if (vm_table_colmask_get(instance, fieldName->chars, &maskV)) {
                        vm_push(maskV);
                        DISPATCH();
                    }
                }
                /*  — receveur colonne `COL_X..MasqueAffichage` (prop
                 * COLONNE, canal distinct de MasqueSaisie). Store OWL-side +
                 * fallback declare. */
                {
                    Value dmaskV;
                    if (vm_table_coldispmask_get(instance, fieldName->chars,
                                                 &dmaskV)) {
                        vm_push(dmaskV);
                        DISPATCH();
                    }
                }
                /*  — propriete calculee : si le type du recepteur
                 * declare un OwlField avec computeGetter pour ce nom, on
                 * delegue au getter (etat live) au lieu de lire le storage
                 * (snapshot pose au dernier sync). Match accent/case-insensible
                 * sur name / nameAccented / nameEN, comme la branche compile-time
                 * dans compiler.c (cf OwlTypeDef::fields). */
                if (instance->structName != NULL) {
                    const OwlTypeDef* td = owl_registry_type_find(
                        instance->structName->chars);
                    if (td != NULL && td->fields != NULL) {
                        for (const OwlField* f = td->fields; f->name; f++) {
                            if (f->computeGetter == NULL) continue;
                            bool match =
                                (strcasecmp(fieldName->chars, f->name) == 0)
                                || (f->nameAccented
                                    && strcasecmp(fieldName->chars,
                                                  f->nameAccented) == 0)
                                || (f->nameEN
                                    && strcasecmp(fieldName->chars,
                                                  f->nameEN) == 0);
                            if (match) {
                                Value arg = instVal;
                                Value result = f->computeGetter(1, &arg);
                                vm_push(result);
                                DISPATCH();
                            }
                        }
                    }
                }
                Value value;
                if (table_get(&instance->fields, fieldName, &value)) {
                    vm_push(value);
                } else {
                    /* Fallback bilingue EN->FR : si requestedName matche le
                     * nameEN d'un OwlField du type, on relit le storage canon.
                     * Permet `req.Method` -> `Methode` (cf invariant 3). */
                    const char* canon = NULL;
                    if (instance->structName != NULL) {
                        canon = owl_registry_resolve_field_en_to_canon(
                            instance->structName->chars, fieldName->chars);
                    }
                    if (canon != NULL) {
                        ObjString* canonKey = obj_copy_string(canon, (int)strlen(canon));
                        if (table_get(&instance->fields, canonKey, &value)) {
                            vm_push(value);
                            DISPATCH();
                        }
                    }
                    /*  : invariant 1 — acces champ d'instance
                     * case-insensitive sur les noms utilisateur. Scan sur
                     * miss exact (et apres le fallback bilingue registry,
                     * qu'on ne regresse pas). */
                    {
                        ObjString* k = table_find_key_case_insensitive(
                            &instance->fields, fieldName->chars);
                        if (k != NULL && table_get(&instance->fields, k,
                                                   &value)) {
                            vm_push(value);
                            DISPATCH();
                        }
                    }
                    if (instance->klass != NULL) {
                        /* Check class methods */
                        Value method;
                        if (class_lookup_method(instance->klass, fieldName, &method)) {
                            vm_push(method);
                        } else {
                            vm_push(NIL_VAL);
                        }
                    } else {
                        vm_push(NIL_VAL);
                    }
                }
                DISPATCH();
            }

            VM_CASE(OP_STRUCT_SET): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ). */
                /* Stack: [instance] [value]; operand: field name constant idx */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* fieldName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value val = vm_pop();
                Value instVal = vm_pop();

                /*  — style de cellule/ligne/colonne : `...[l,c]..CouleurFond = X`. */
                if (IS_XLS_REF(instVal)) {
                    const char* xlsExpected__ = NULL;
                    XlsSetResult xlsSet__ = stdlib_xls_ref_set_property(
                        instVal, fieldName->chars, val, &xlsExpected__);
                    if (xlsSet__ != XLS_SET_OK) {
                        char nameBuf[128];
                        int nl = fieldName->length < (int)sizeof(nameBuf) - 1
                                     ? fieldName->length : (int)sizeof(nameBuf) - 1;
                        memcpy(nameBuf, fieldName->chars, (size_t)nl);
                        nameBuf[nl] = '\0';
                        if (xlsSet__ == XLS_SET_BAD_TYPE) {
                            owl_emit_runtime_error(E_RT_XLS_STYLE_VALUE_TYPE,
                                                   "prop", nameBuf,
                                                   "expected", xlsExpected__, NULL);
                        } else {
                            const char* validList__ = owl_error_render_lang_is_en()
                                ? "..BackgroundColor, ..Font, ..Border, ..AlignmentH, ..AlignmentV, ..Multiline, ..Mask, ..Width, ..Height"
                                : "..CouleurFond, ..Police, ..Cadre, ..AlignementH, ..AlignementV, ..Multiligne, ..Masque, ..Largeur, ..Hauteur";
                            owl_emit_runtime_error(E_RT_COLLECTION_HAS_NO_PROP,
                                                   "name", nameBuf,
                                                   "valid", validList__, NULL);
                        }
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vm_push(val);   /* l'expression d'affectation renvoie la valeur */
                    DISPATCH();
                }

                /*  — Broadcast Groupe : GR_X..Prop = val.
                 * Quand instVal est un ObjArray taggue is_group, on dispatch
                 * le set sur chaque membre ObjUiControl qui expose la prop
                 * dans son catalogue (no-op silencieux sur les autres ou sur
                 * les membres non-controle). Le check is_group passe AVANT
                 * IS_UI_CONTROL : un Groupe est techniquement un ObjArray. */
                if (IS_ARRAY(instVal) && AS_ARRAY(instVal)->is_group) {
                    ObjArray* gr = AS_ARRAY(instVal);
                    for (int i = 0; i < gr->elements.count; i++) {
                        Value member = gr->elements.values[i];
                        /*  — Modele C : les groupes contiennent
                         * desormais des ObjUiControlRef (poses au
                         * compile-time dans vm.globals). On resout chacun
                         * vers son ObjUiControlInstance via la stack puis
                         * dispatch comme avant. ObjUiControl historique
                         * reste accepte pour la fenetre top-level. */
                        if (IS_UI_CONTROL_REF(member)) {
                            ObjUiControlRef* ref = AS_UI_CONTROL_REF(member);
                            if (owl_control_catalog_find(
                                    (OwlControlKind)ref->kind, fieldName->chars) == NULL) {
                                continue;
                            }
                            ObjUiControlInstance* uici = resolve_ui_ref(ref);
                            if (uici == NULL) continue;
                            obj_ui_control_instance_headless_set(
                                uici, fieldName->chars, val);
                            /*  — broadcast Groupe = ecriture imperative :
                             * override sticky sur chaque membre (cf. site
                             * `..prop =` direct). */
                            obj_ui_control_instance_mark_override(
                                uici, fieldName->chars);
                            /*  lot 2 — reflet GUI du broadcast. */
                            if (uici->widget != NULL) {
                                owl_ui_apply_instance_scalar(
                                    uici, fieldName->chars, val);
                            }
                        } else if (IS_UI_CONTROL(member)) {
                            ObjUiControl* uc = AS_UI_CONTROL(member);
                            if (owl_control_catalog_find(
                                    (OwlControlKind)uc->kind, fieldName->chars) == NULL) {
                                continue;
                            }
                            if (uc->widget != NULL) {
                                owl_ui_control_set_prop(uc, fieldName->chars, val);
                            } else {
                                obj_ui_control_headless_set(uc, fieldName->chars, val);
                            }
                        }
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  lot B — ObjWindowRef..Prop = val : ecrit dans le
                 * casier de l'instance fenetre courante. Cf. commentaire
                 * symetrique dans OP_STRUCT_GET.
                 *
                 *  lot C — reflet Qt cable via hook
                 * owl_ui_apply_window_scalar (default no-op en CLI, vraie
                 * impl dans stdlib_ui.c). Symetrique strict de la branche
                 * IS_UI_CONTROL_REF plus bas (owl_ui_apply_instance_scalar).
                 * En headless wi->widget reste NULL : pas d'appel Qt. */
                if (IS_WINDOW_REF(instVal)) {
                    ObjWindowRef* wref = AS_WINDOW_REF(instVal);
                    /*  — ..Nom/..Name lecture seule : le nom de la
                     * fenetre est son identite declaree, non reassignable. */
                    if (owl_ui_prop_is_universal_readonly(fieldName->chars)) {
                        owl_emit_native_error(
                            E_SEM_DSL_PROP_IDENTITY_READONLY,
                            wref->decl ? wref->decl->name : "?",
                            "prop", fieldName->chars,
                            NULL);
                        vm_push(val);
                        DISPATCH();
                    }
                    ObjWindowInstance* wi = vm_window_stack_top();
                    if (wi != NULL) {
                        /*  — sémantique COPIE pour `FEN..Style =
                         * <instance>` (parite widget, ) : la fenetre
                         * recoit SA PROPRE copie profonde du style, isolee de la
                         * source theme.X / STY_X. Clone stocke ET rejoue au
                         * panneau pour que watch/racine ciblent la copie. */
                        Value storeVal = val;
                        bool styleCloned = false;
                        if ((strcasecmp(fieldName->chars, "Style") == 0)
                            && IS_INSTANCE(val)) {
                            storeVal = owl_value_deep_clone(val);
                            styleCloned = true;
                        }
                        if (styleCloned && deep_clone_take_self_containing()) {
                            owl_emit_runtime_error(E_RT_DEEP_COPY_SELF_CONTAINING, NULL);
                            if (exceptionCount > 0 && currentException != NULL) {
                                if (handle_runtime_exception()) {
                                    frame = &vm.frames[vm.frameCount - 1];
                                    DISPATCH();
                                }
                            }
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        if (styleCloned) vm_push(storeVal);
                        obj_window_instance_headless_set(wi, fieldName->chars,
                                                          storeVal);
                        /*  — MenuPrincipal passe par le hook MEME sans
                         * widget monte : la bascule doit resoudre le nom de
                         * l'arbre vise (et rouspeter s'il n'existe pas) aussi
                         * bien en headless qu'en GUI. */
                        bool isMainMenu =
                            (strcasecmp(fieldName->chars, "MenuPrincipal") == 0
                             || strcasecmp(fieldName->chars, "MainMenu") == 0);
                        if (wi->widget != NULL || isMainMenu) {
                            owl_ui_apply_window_scalar(wi, fieldName->chars,
                                                       storeVal);
                        }
                        if (styleCloned) vm_pop();
                        vm_push(val);
                        DISPATCH();
                    }
                    /* Pas d'instance ouverte : ecrit dans le fallback_store
                     * du ref (cf. commentaire OP_STRUCT_GET window-ref).
                     * Preserve `FEN_X..Prop = val` AVANT Ouvre, semantique
                     * historique de l'ancien ObjUiControl global.
                     *
                     *   : val et wref non rootes car popped
                     * en debut d'opcode. Push pendant obj_copy_string +
                     * table_set qui peuvent grow → GC. Sans ca, val (si Obj)
                     * collected → poison ecrit dans la table. */
                    vm_push(instVal); /* root wref */
                    if (IS_OBJ(val)) vm_push(val);
                    if (wref->fallback_store == NULL) {
                        wref->fallback_store = (Table*)malloc(sizeof(Table));
                        table_init(wref->fallback_store);
                    }
                    char keybuf[128];
                    const OwlControlPropDef* pdef =
                        owl_window_catalog_find(fieldName->chars);
                    const char* key = pdef
                        ? (pdef->nameAscii ? pdef->nameAscii : pdef->nameFR)
                        : fieldName->chars;
                    int klen = 0;
                    while (key[klen] != '\0' && klen < (int)sizeof(keybuf)-1) {
                        keybuf[klen] = (char)tolower((unsigned char)key[klen]);
                        klen++;
                    }
                    keybuf[klen] = '\0';
                    ObjString* ks = obj_copy_string(keybuf, klen);
                    vm_push(OBJ_VAL(ks)); /* root ks */
                    table_set(wref->fallback_store, ks, val);
                    vm_pop(); /* ks */
                    if (IS_OBJ(val)) vm_pop();
                    vm_pop(); /* wref */
                    vm_push(val);
                    DISPATCH();
                }

                /*  — Modele C : ObjUiControlRef..Prop = val.
                 * Cf. commentaire dans OP_STRUCT_GET pour le rationale.
                 * Stack vide / champ hors fenetre = soft error + valeur
                 * pushee sur la pile pour preserver l'invariant. */
                if (IS_UI_CONTROL_REF(instVal)) {
                    /*   : val + instVal popped en haut de
                     * l'opcode → C locals unrooted. La branche UI_CONTROL_REF
                     * alloue (resolve, helpers, Qt apply) avant et pendant le
                     * headless_set → val peut etre GC entre la pop et le set
                     * → multilingue store recoit poison → relecture indexee
                     * echoue 'Can only index'. Push val + instVal en debut
                     * de branche, pop en fin (commun aux 4 sous-branches via
                     * goto cleanup). */
                    vm_push(instVal);
                    if (IS_OBJ(val)) vm_push(val);
                    ObjUiControlRef* ref = AS_UI_CONTROL_REF(instVal);
                    /*  — intercept ..Groupe = [...] AVANT
                     * resolution : ecrit l'appartenance dans les Groupes
                     * globaux (vm.globals). Test pointeur sur le ref (stable). */
                    if ((fieldName->length == 6 &&
                         strncasecmp(fieldName->chars, "groupe", 6) == 0) ||
                        (fieldName->length == 5 &&
                         strncasecmp(fieldName->chars, "group", 5) == 0)) {
                        /* Etape 1 : retrait global du ref. */
                        for (int i = 0; i < vm.globals.capacity; i++) {
                            Entry* e = &vm.globals.entries[i];
                            if (e->key == NULL) continue;
                            if (!IS_ARRAY(e->value)) continue;
                            ObjArray* gr = AS_ARRAY(e->value);
                            if (!gr->is_group) continue;
                            int w = 0;
                            for (int r = 0; r < gr->elements.count; r++) {
                                Value vv = gr->elements.values[r];
                                bool drop = IS_UI_CONTROL_REF(vv)
                                    && AS_UI_CONTROL_REF(vv) == ref;
                                if (!drop) {
                                    if (w != r) gr->elements.values[w] = vv;
                                    w++;
                                }
                            }
                            gr->elements.count = w;
                        }
                        /* Etape 2 : ajout aux Groupes mentionnes dans val. */
                        if (IS_ARRAY(val)) {
                            ObjArray* targetList = AS_ARRAY(val);
                            for (int i = 0; i < targetList->elements.count; i++) {
                                Value v = targetList->elements.values[i];
                                if (!IS_ARRAY(v)) continue;
                                ObjArray* gr = AS_ARRAY(v);
                                if (!gr->is_group) continue;
                                bool already = false;
                                for (int j = 0; j < gr->elements.count; j++) {
                                    Value mv = gr->elements.values[j];
                                    if (IS_UI_CONTROL_REF(mv)
                                        && AS_UI_CONTROL_REF(mv) == ref) {
                                        already = true;
                                        break;
                                    }
                                }
                                if (!already) {
                                    obj_array_push(gr, OBJ_VAL(ref));
                                }
                            }
                        }
                        /*  : cleanup push entry de branche */
                        if (IS_OBJ(val)) vm_pop();
                        vm_pop(); /* instVal */
                        vm_push(val);
                        DISPATCH();
                    }
                    ObjUiControlInstance* uici = resolve_ui_ref(ref);
                    if (uici == NULL) {
                        if (IS_OBJ(val)) vm_pop();
                        vm_pop(); /* instVal */
                        vm_push(val);
                        DISPATCH();
                    }
                    /*  — raccourci d'apparence plat Saisie : ecrit dans
                     * la ZoneSaisie de l'instance Style (verite unique), puis
                     * rejoue la feuille via owl_ui_apply_instance_scalar quand un
                     * widget est monte. Gate INPUT. */
                    if (uici->decl
                        && (OwlControlKind)uici->decl->kind == OWL_CONTROL_INPUT) {
                        const char* zsSubSet =
                            saisie_style_shortcut_to_subfield(fieldName->chars);
                        if (zsSubSet != NULL) {
                            ObjInstance* zone =
                                vm_style_zonesaisie_for_uici(uici);
                            if (zone != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &zone->fields, zsSubSet);
                                ObjString* key = sk != NULL
                                    ? sk
                                    : obj_copy_string(zsSubSet,
                                                      (int)strlen(zsSubSet));
                                table_set(&zone->fields, key, val);
                            }
                            if (uici->widget != NULL) {
                                owl_ui_apply_instance_scalar(uici,
                                                             fieldName->chars, val);
                            }
                            if (IS_OBJ(val)) vm_pop();
                            vm_pop(); /* instVal */
                            vm_push(val);
                            DISPATCH();
                        }
                    }
                    /*  — raccourci d'apparence plat mono (Libellé/Bouton) :
                     * ecrit dans la sous-brique correspondante de l'instance Style
                     * (verite unique). CouleurFond->CadreExtérieur..CouleurFond,
                     * PoliceCouleur->Libellé..Couleur. Rejoue la feuille via
                     * owl_ui_apply_instance_scalar quand un widget est monte. */
                    if (uici->decl) {
                        const char* mBrick = NULL;
                        const char* mSub   = NULL;
                        if (mono_style_shortcut_bricksub_vm(
                                uici->decl->kind, fieldName->chars,
                                &mBrick, &mSub)) {
                            ObjInstance* brick = vm_style_mono_brick_for_uici(
                                uici, uici->decl->kind, mBrick);
                            if (brick != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &brick->fields, mSub);
                                ObjString* key = sk != NULL
                                    ? sk
                                    : obj_copy_string(mSub, (int)strlen(mSub));
                                table_set(&brick->fields, key, val);
                            }
                            if (uici->widget != NULL) {
                                owl_ui_apply_instance_scalar(uici,
                                                             fieldName->chars, val);
                            }
                            if (IS_OBJ(val)) vm_pop();
                            vm_pop(); /* instVal */
                            vm_push(val);
                            DISPATCH();
                        }
                    }
                    /*  — raccourci Police plat : ecrit dans le
                     * sous-champ de l'instance Police materialisee. */
                    const char* polSubSet =
                        police_shortcut_to_subfield(fieldName->chars);
                    if (polSubSet != NULL) {
                        ObjInstance* pol =
                            vm_headless_police_instance_for_uici(uici);
                        if (pol != NULL) {
                            ObjString* sk = table_find_key_case_insensitive(
                                &pol->fields, polSubSet);
                            ObjString* key = sk != NULL
                                ? sk
                                : obj_copy_string(polSubSet,
                                                  (int)strlen(polSubSet));
                            table_set(&pol->fields, key, val);
                        }
                        /*  /  — le store est a jour mais le
                         * QWidget monte ne l'etait pas : ce raccourci returnait
                         * sans rejouer la fonte sur le widget (contrairement au
                         * chemin scalaire generique plus bas). owl_ui_apply_
                         * instance_scalar -> real_ui_set_prop gere les raccourcis
                         * PoliceXXX et applique apply_police_instance_to_widget.
                         * No-op en CLI/headless. val + instVal encore rootes. */
                        if (uici->widget != NULL) {
                            owl_ui_apply_instance_scalar(uici,
                                                         fieldName->chars, val);
                        }
                        if (IS_OBJ(val)) vm_pop();
                        vm_pop(); /* instVal */
                        vm_push(val);
                        DISPATCH();
                    }
                    /*  — ..Nom/..Name lecture seule : le nom est
                     * l'identite declaree, non reassignable. Le compilateur
                     * rejette deja `CHAMP..Nom = x` direct ; ce garde couvre
                     * les refs qualifiees (non verifiables a la compile). */
                    if (owl_ui_prop_is_universal_readonly(fieldName->chars)) {
                        owl_emit_native_error(
                            E_SEM_DSL_PROP_IDENTITY_READONLY,
                            uici->decl ? uici->decl->name : "?",
                            "prop", fieldName->chars,
                            NULL);
                        if (IS_OBJ(val)) vm_pop();
                        vm_pop(); /* instVal */
                        vm_push(val);
                        DISPATCH();
                    }
                    /*  — ecriture d'une prop hors catalogue via
                     * reference qualifiee (non verifiable a la compile) : soft
                     * error au lieu du store fantome silencieux. Groupe et
                     * raccourcis Police sont interceptes plus haut ; Style et
                     * les props catalogues restent legitimes. */
                    {
                        int ukind_set = uici->decl ? (int)uici->decl->kind : 0;
                        if (owl_control_catalog_find(
                                (OwlControlKind)ukind_set, fieldName->chars)
                                == NULL
                            && !owl_ui_prop_is_universal(fieldName->chars)) {
                            owl_emit_native_error(
                                E_RT_UI_PROP_UNKNOWN_FOR_KIND,
                                uici->decl ? uici->decl->name : "?",
                                "prop", fieldName->chars,
                                "kind", owl_control_kind_name_fr(
                                            (OwlControlKind)ukind_set),
                                NULL);
                            if (IS_OBJ(val)) vm_pop();
                            vm_pop(); /* instVal */
                            vm_push(val);
                            DISPATCH();
                        }
                    }
                    /*  — `..Contenu =` headless pur d'un Liste/Combo :
                     * pose la liste dans le store items (split LF), PAS dans une
                     * cle "Contenu" separee. Unifie setter + getter + ListeXxx
                     * sur UNE liste (le getter headless relit ce meme store, cf.
                     * OP_STRUCT_GET ). En GUI (widget != NULL) : chemin
                     * generique inchange (headless_set + apply Qt set_content,
                     * ). Intercept AVANT le set generique. */
                    {
                        int ukind_ct = uici->decl ? (int)uici->decl->kind : 0;
                        if ((ukind_ct == OWL_CONTROL_LISTE
                             || ukind_ct == OWL_CONTROL_COMBO)
                            && uici->widget == NULL
                            && (strcasecmp(fieldName->chars, "Contenu") == 0
                                || strcasecmp(fieldName->chars, "Content") == 0)) {
                            ObjString* cs = value_to_string(val);
                            vm_push(cs ? OBJ_VAL(cs) : NIL_VAL);  /* root : fill alloue */
                            owl_ui_headless_set_content_uici(uici, cs ? cs->chars : "");
                            vm_pop();
                            obj_ui_control_instance_mark_override(uici,
                                                                  fieldName->chars);
                            if (IS_OBJ(val)) vm_pop();
                            vm_pop(); /* instVal */
                            vm_push(val);
                            DISPATCH();
                        }
                    }
                    /*  — sémantique COPIE : `X..Style = <instance>`
                     * donne au widget SA PROPRE copie profonde du style. Sans
                     * ça, deux widgets recevant la même instance (theme.X /
                     * STY_X) la partagent ; une surcharge par widget
                     * (..Style..sous..feuille = ...) fuiterait sur les autres
                     * et sur la source. Deep-clone => instance + sous-éléments
                     * imbriqués isolés. Le clone (et non `val`) est stocké ET
                     * rejoué au widget pour que watch/racine ciblent la copie. */
                    /*  — capture de l'ancienne valeur AVANT le set
                     * pour la logique Onglet/Volet (changement reel de ..Valeur,
                     * report au masquage). old n'est lu que comme entier ->
                     * jamais un Obj pour les props concernees, pas de risque GC. */
                    Value ongOldVal = NIL_VAL;
                    obj_ui_control_instance_headless_get(uici, fieldName->chars,
                                                         &ongOldVal);
                    Value storeVal = val;
                    bool styleCloned = false;
                    /*  — Cadre/Police suivent la meme semantique COPIE
                     * que Style ( ci-dessus). Affecter la MEME instance
                     * composite a deux controles (`LIB_B..Cadre = LIB_A..Cadre`)
                     * la partagerait par reference ; une surcharge de sous-champ
                     * sur l'un (`LIB_A..Cadre..Couleur = ...`) fuiterait sur
                     * l'autre via la back-ref last-writer (ui_owner). Deep-clone
                     * a l'affectation => chaque controle a SA copie, dont la
                     * back-ref repart a NULL (obj_new_instance) et se re-tag sur
                     * le bon uici au seed (obj_ui_control_instance_headless_set).
                     * Formes FR/EN canoniques : Style, Cadre/Frame, Police/Font. */
                    if (IS_INSTANCE(val)
                        && (strcasecmp(fieldName->chars, "Style") == 0
                            || strcasecmp(fieldName->chars, "Cadre") == 0
                            || strcasecmp(fieldName->chars, "Frame") == 0
                            || strcasecmp(fieldName->chars, "Police") == 0
                            || strcasecmp(fieldName->chars, "Font") == 0)) {
                        storeVal = owl_value_deep_clone(val);
                        styleCloned = true;
                    }
                    if (styleCloned && deep_clone_take_self_containing()) {
                        owl_emit_runtime_error(E_RT_DEEP_COPY_SELF_CONTAINING, NULL);
                        if (exceptionCount > 0 && currentException != NULL) {
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (styleCloned) vm_push(storeVal);
                    obj_ui_control_instance_headless_set(uici,
                                                          fieldName->chars,
                                                          storeVal);
                    /*  — ecriture imperative `..prop =` : marque la prop
                     * comme override sticky. Le reseed d'expression PROPRIETES
                     * par rendu de bloc d'un Etat (stdlib_pdf.c) ne re-seedera
                     * plus cette prop, quelle que soit sa valeur (fin du temoin
                     * par valeur, qui perdait l'override en cas de coincidence). */
                    obj_ui_control_instance_mark_override(uici,
                                                          fieldName->chars);
                    /*  lot 2 — si l'instance porte un widget Qt monte,
                     * rejoue le scalaire sur le QWidget (imperatif reflete en
                     * GUI comme il l'est en headless). No-op en CLI/headless. */
                    if (uici->widget != NULL) {
                        owl_ui_apply_instance_scalar(uici, fieldName->chars,
                                                     storeVal);
                    }
                    /*  — logique metier Onglet/Volet apres le set
                     * (inconditionnel : headless ET GUI). No-op pour tout autre
                     * kind/prop. val encore root (pop plus bas). */
                    owl_ui_control_logic_after_set(uici, fieldName->chars,
                                                   ongOldVal, storeVal);
                    if (styleCloned) vm_pop();
                    if (IS_OBJ(val)) vm_pop();
                    vm_pop(); /* instVal */
                    vm_push(val);
                    DISPATCH();
                }

                /* DSL Fenetre v1 (Phase E.X) : ctrl..Prop = val.
                 *  — dispatch :
                 *   widget != NULL : hook Qt (mode GUI attach actif).
                 *   widget == NULL : ecrit dans headless_store (lazy alloc).
                 * En mode GUI, le set virtuel est aussi conserve pour qu'un
                 * attach posterieur (fn_ouvre) puisse replayer les pending
                 * sur le widget (lot 3). */
                if (IS_UI_CONTROL(instVal)) {
                    ObjUiControl* uic = AS_UI_CONTROL(instVal);
                    /*  — intercept ..Groupe = [GR_X, GR_Y, ...].
                     * Recompose l'appartenance : retire ctrl de tous les
                     * Groupes globaux le referencant, puis ajoute ctrl aux
                     * Groupes mentionnes dans val. */
                    if ((fieldName->length == 6 &&
                         strncasecmp(fieldName->chars, "groupe", 6) == 0) ||
                        (fieldName->length == 5 &&
                         strncasecmp(fieldName->chars, "group", 5) == 0)) {
                        /* Etape 1 : retrait global. Parcourt vm.globals,
                         * pour chaque ObjArray is_group, supprime ctrl. */
                        for (int i = 0; i < vm.globals.capacity; i++) {
                            Entry* e = &vm.globals.entries[i];
                            if (e->key == NULL) continue;
                            if (!IS_ARRAY(e->value)) continue;
                            ObjArray* gr = AS_ARRAY(e->value);
                            if (!gr->is_group) continue;
                            vm_group_remove_ctrl(gr, uic);
                        }
                        /* Etape 2 : ajout dans les Groupes de val. */
                        if (IS_ARRAY(val)) {
                            ObjArray* targetList = AS_ARRAY(val);
                            for (int i = 0; i < targetList->elements.count; i++) {
                                Value v = targetList->elements.values[i];
                                if (!IS_ARRAY(v)) continue;
                                ObjArray* gr = AS_ARRAY(v);
                                if (!gr->is_group) continue;
                                vm_group_add_ctrl(gr, uic);
                            }
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    if (uic->widget != NULL) {
                        owl_ui_control_set_prop(uic, fieldName->chars, val);
                    } else {
                        /*  — miroir de la branche REF : `..Contenu =`
                         * headless pur d'un Liste/Combo pose la liste dans le
                         * store items (split LF), source unique relue par le
                         * getter et ListeXxx. */
                        if ((uic->kind == OWL_CONTROL_LISTE
                             || uic->kind == OWL_CONTROL_COMBO)
                            && (strcasecmp(fieldName->chars, "Contenu") == 0
                                || strcasecmp(fieldName->chars, "Content") == 0)) {
                            vm_push(val);  /* root pendant value_to_string + fill */
                            ObjString* cs = value_to_string(val);
                            vm_push(cs ? OBJ_VAL(cs) : NIL_VAL);
                            owl_ui_headless_set_content_uic(uic, cs ? cs->chars : "");
                            vm_pop();
                            vm_pop();
                            vm_push(val);
                            DISPATCH();
                        }
                        /*  — raccourci d'apparence plat Saisie : ecrit
                         * dans la ZoneSaisie de l'instance Style (verite unique).
                         * Gate INPUT. */
                        const char* zsSub =
                            ((OwlControlKind)uic->kind == OWL_CONTROL_INPUT)
                                ? saisie_style_shortcut_to_subfield(
                                      fieldName->chars)
                                : NULL;
                        if (zsSub != NULL) {
                            vm_push(val); /* root val pendant un eventuel clone */
                            ObjInstance* zone = vm_style_zonesaisie_for_uic(uic);
                            vm_pop();
                            if (zone != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &zone->fields, zsSub);
                                ObjString* key = sk != NULL
                                    ? sk
                                    : obj_copy_string(zsSub,
                                                      (int)strlen(zsSub));
                                table_set(&zone->fields, key, val);
                            }
                            vm_push(val);
                            DISPATCH();
                        }
                        /*  — raccourci d'apparence plat mono (Libellé/
                         * Bouton) : ecrit dans la sous-brique correspondante de
                         * l'instance Style (verite unique).
                         * CouleurFond->CadreExtérieur..CouleurFond,
                         * PoliceCouleur->Libellé..Couleur. */
                        {
                            const char* mBrick = NULL;
                            const char* mSub   = NULL;
                            if (mono_style_shortcut_bricksub_vm(
                                    (int)uic->kind, fieldName->chars,
                                    &mBrick, &mSub)) {
                                vm_push(val); /* root pendant un eventuel clone */
                                ObjInstance* brick = vm_style_mono_brick_for_uic(
                                    uic, (int)uic->kind, mBrick);
                                vm_pop();
                                if (brick != NULL) {
                                    ObjString* sk =
                                        table_find_key_case_insensitive(
                                            &brick->fields, mSub);
                                    ObjString* key = sk != NULL
                                        ? sk
                                        : obj_copy_string(mSub,
                                                          (int)strlen(mSub));
                                    table_set(&brick->fields, key, val);
                                }
                                vm_push(val);
                                DISPATCH();
                            }
                        }
                        /*  — raccourci Police plat : ecrit dans le
                         * sous-champ de l'instance Police materialisee (source
                         * de verite unique), pour que la cascade composee voie
                         * la meme valeur (equivalence bidirectionnelle). */
                        const char* polSub =
                            police_shortcut_to_subfield(fieldName->chars);
                        if (polSub != NULL) {
                            vm_push(val); /* root val pendant un eventuel clone */
                            ObjInstance* pol = vm_headless_police_instance(uic);
                            vm_pop();
                            if (pol != NULL) {
                                ObjString* sk = table_find_key_case_insensitive(
                                    &pol->fields, polSub);
                                ObjString* key = sk != NULL
                                    ? sk
                                    : obj_copy_string(polSub,
                                                      (int)strlen(polSub));
                                table_set(&pol->fields, key, val);
                            }
                        } else {
                            obj_ui_control_headless_set(uic, fieldName->chars, val);
                        }
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /* JSON field set: jDoc.nom = "valeur" */
                if (IS_JSON(instVal)) {
                    ObjJson* json = AS_JSON(instVal);
                    if (json->jtype != JSON_T_OBJECT) {
                        owl_emit_runtime_error(E_RT_FIELD_SET_NEEDS_JSON_OBJECT, NULL);
                        OWL_RT_RECOVER_DISPATCH();
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    /* Convert OWL value to ObjJson node. Root the freshly
                     * allocated jval before obj_json_object_set which may
                     * grow the underlying table and trigger GC. */
                    ObjJson* jval;
                    if (IS_NIL(val))          jval = obj_new_json(JSON_T_NULL);
                    else if (IS_BOOL(val))    jval = obj_new_json_bool(AS_BOOL(val));
                    else if (IS_INTEGER(val)) jval = obj_new_json_int(AS_INTEGER(val));
                    else if (IS_REAL(val))    jval = obj_new_json_real(AS_REAL(val));
                    else if (IS_CURRENCY(val)) jval = obj_new_json_currency(AS_CURRENCY(val));
                    else if (IS_NUMERIC(val)) jval = obj_new_json_numeric(val);
                    else if (IS_STRING(val))  jval = obj_new_json_string(AS_STRING(val));
                    else if (IS_JSON(val))    jval = AS_JSON(val);
                    else                      jval = obj_new_json(JSON_T_NULL);
                    vm_push(OBJ_VAL(jval));
                    obj_json_object_set(json, fieldName, jval);
                    vm_pop();
                    vm_push(val);
                    DISPATCH();
                }

                /*  — OptionsEntraînement : ecriture d'une propriete de
                 * config (double point). Feuille scalaire, aucun rooting requis
                 * (val stocke par valeur dans un champ int/double). Noms
                 * accent-strippes par le scanner. Symetrique de la branche
                 * OP_STRUCT_GET. */
                if (IS_OPTIONS_ENTRAINEMENT(instVal)) {
                    ObjOptionsEntrainement* o = AS_OPTIONS_ENTRAINEMENT(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    double dv = IS_REAL(val)    ? AS_REAL(val)
                              : IS_INTEGER(val) ? (double)AS_INTEGER(val) : 0.0;
                    if ((fl == 7 && strncasecmp(fn, "epoques", 7) == 0)
                        || (fl == 6 && strncasecmp(fn, "epochs", 6) == 0)) {
                        o->epochs = iv;
                    } else if ((fl == 9 && strncasecmp(fn, "taillelot", 9) == 0)
                        || (fl == 9 && strncasecmp(fn, "batchsize", 9) == 0)) {
                        o->batchSize = iv;
                    } else if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        o->learningRate = dv;
                    } else if ((fl == 10 && strncasecmp(fn, "optimiseur", 10) == 0)
                        || (fl == 9 && strncasecmp(fn, "optimizer", 9) == 0)) {
                        o->optimizer = iv;
                    } else if ((fl == 5 && strncasecmp(fn, "perte", 5) == 0)
                        || (fl == 4 && strncasecmp(fn, "loss", 4) == 0)) {
                        o->loss = iv;
                    } else if ((fl == 8 && strncasecmp(fn, "plantaux", 8) == 0)
                        || (fl == 10 && strncasecmp(fn, "lrschedule", 10) == 0)) {
                        o->lrSchedule = iv;   /*  */
                    } else if ((fl == 17 && strncasecmp(fn, "decroissancepoids", 17) == 0)
                        || (fl == 11 && strncasecmp(fn, "weightdecay", 11) == 0)) {
                        o->weightDecay = dv;  /*  */
                    } else if ((fl == 18 && strncasecmp(fn, "fractionvalidation", 18) == 0)
                        || (fl == 18 && strncasecmp(fn, "validationfraction", 18) == 0)) {
                        o->validationFraction = dv;  /*  */
                    } else if ((fl == 19 && strncasecmp(fn, "gardemeilleurmodele", 19) == 0)
                        || (fl == 13 && strncasecmp(fn, "keepbestmodel", 13) == 0)) {
                        /*  — booleen : Vrai/Faux, tolere aussi un entier non nul. */
                        o->keepBestModel = IS_BOOL(val) ? (AS_BOOL(val) ? 1 : 0)
                                                        : (iv != 0);
                    } else if ((fl == 13 && strncasecmp(fn, "patiencearret", 13) == 0)
                        || (fl == 12 && strncasecmp(fn, "stoppatience", 12) == 0)) {
                        o->stopPatience = iv;  /*  */
                    } else if ((fl == 9 && strncasecmp(fn, "surepoque", 9) == 0)
                        || (fl == 7 && strncasecmp(fn, "onepoch", 7) == 0)) {
                        /*  — stocke la reference de procedure (nom nu
                         * resolu en callable par le compilateur). Ce champ
                         * devient une racine GC vivante : tracee dans gc.c. */
                        o->onEpoch = val;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLArbreDécision : écriture des hyperparamètres et
                 * de l'objectif (double point). Feuille scalaire (val stocké par
                 * valeur dans un champ int). Noms accent-strippés par le
                 * scanner. Symétrique de la branche OP_STRUCT_GET. */
                if (IS_MLARBRE(instVal)) {
                    ObjMLArbre* a = AS_MLARBRE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        a->objective = iv;
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        a->maxDepth = iv;
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        a->minSamplesSplit = iv;
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        a->minSamplesLeaf = iv;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLForêtAléatoire : écriture des hyperparamètres et
                 * de l'objectif (double point). Feuille scalaire. Noms
                 * accent-strippés par le scanner. Symétrique de OP_STRUCT_GET. */
                if (IS_MLFORET(instVal)) {
                    ObjMLForet* f = AS_MLFORET(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    int64_t sv = IS_INTEGER(val) ? AS_INTEGER(val)
                               : IS_REAL(val)    ? (int64_t)AS_REAL(val) : 0;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        f->objective = iv;
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        f->maxDepth = iv;
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        f->minSamplesSplit = iv;
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        f->minSamplesLeaf = iv;
                    } else if ((fl == 8 && strncasecmp(fn, "nbarbres", 8) == 0)
                        || (fl == 8 && strncasecmp(fn, "numtrees", 8) == 0)) {
                        f->numTrees = iv;
                    } else if ((fl == 22 && strncasecmp(fn, "variablesparseparation", 22) == 0)
                        || (fl == 11 && strncasecmp(fn, "maxfeatures", 11) == 0)) {
                        f->maxFeatures = iv;
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        f->seed = sv;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLGradientBoosting : écriture des hyperparamètres et
                 * de l'objectif (double point). ..TauxApprentissage est un réel
                 * (feuille scalaire double) ; les autres sont des entiers. Noms
                 * accent-strippés par le scanner. Symétrique de OP_STRUCT_GET. */
                if (IS_MLBOOSTING(instVal)) {
                    ObjMLBoosting* b = AS_MLBOOSTING(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    double dv = IS_REAL(val)    ? AS_REAL(val)
                              : IS_INTEGER(val) ? (double)AS_INTEGER(val) : 0.0;
                    if ((fl == 8 && strncasecmp(fn, "objectif", 8) == 0)
                        || (fl == 9 && strncasecmp(fn, "objective", 9) == 0)) {
                        b->objective = iv;
                    } else if ((fl == 8 && strncasecmp(fn, "nbarbres", 8) == 0)
                        || (fl == 8 && strncasecmp(fn, "numtrees", 8) == 0)) {
                        b->numTrees = iv;
                    } else if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        b->learningRate = dv;
                    } else if ((fl == 13 && strncasecmp(fn, "profondeurmax", 13) == 0)
                        || (fl == 8 && strncasecmp(fn, "maxdepth", 8) == 0)) {
                        b->maxDepth = iv;
                    } else if ((fl == 25 && strncasecmp(fn, "minechantillonsseparation", 25) == 0)
                        || (fl == 15 && strncasecmp(fn, "minsamplessplit", 15) == 0)) {
                        b->minSamplesSplit = iv;
                    } else if ((fl == 22 && strncasecmp(fn, "minechantillonsfeuille", 22) == 0)
                        || (fl == 14 && strncasecmp(fn, "minsamplesleaf", 14) == 0)) {
                        b->minSamplesLeaf = iv;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLRégressionLinéaire : aucun hyperparamètre réglable
                 * (OLS = zéro hyperparamètre). ..Coefficients/..Ordonnée sont en
                 * lecture seule. Toute écriture est silencieusement ignorée (on
                 * préserve l'invariant de pile), branche présente pour empêcher la
                 * chute vers le handler d'instance générique. */
                if (IS_MLLINEAIRE(instVal)) {
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLRégressionLogistique : écriture des hyperparamètres
                 * (double point). ..TauxApprentissage est un réel (feuille scalaire
                 * double) ; ..NbItérations un entier. Noms accent-strippés par le
                 * scanner. Symétrique de la branche OP_STRUCT_GET. */
                if (IS_MLLOGISTIQUE(instVal)) {
                    ObjMLLogistique* r = AS_MLLOGISTIQUE(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    double dv = IS_REAL(val)    ? AS_REAL(val)
                              : IS_INTEGER(val) ? (double)AS_INTEGER(val) : 0.0;
                    if ((fl == 17 && strncasecmp(fn, "tauxapprentissage", 17) == 0)
                        || (fl == 12 && strncasecmp(fn, "learningrate", 12) == 0)) {
                        r->learningRate = dv;
                    } else if ((fl == 12 && strncasecmp(fn, "nbiterations", 12) == 0)
                        || (fl == 13 && strncasecmp(fn, "numiterations", 13) == 0)) {
                        r->numIterations = iv;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MLKMeans : écriture des hyperparamètres (double
                 * point). ..NbClusters/..NbItérations/..Graine sont des entiers.
                 * ..Centroïdes est en lecture seule (écriture ignorée). Noms
                 * accent-strippés par le scanner. Symétrique de la branche
                 * OP_STRUCT_GET. */
                if (IS_MLKMEANS(instVal)) {
                    ObjMLKMeans* k = AS_MLKMEANS(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    int64_t sv = IS_INTEGER(val) ? AS_INTEGER(val)
                               : IS_REAL(val)    ? (int64_t)AS_REAL(val) : 0;
                    if ((fl == 10 && strncasecmp(fn, "nbclusters", 10) == 0)
                        || (fl == 11 && strncasecmp(fn, "numclusters", 11) == 0)) {
                        k->numClusters = iv;
                    } else if ((fl == 12 && strncasecmp(fn, "nbiterations", 12) == 0)
                        || (fl == 13 && strncasecmp(fn, "numiterations", 13) == 0)) {
                        k->numIterations = iv;
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        k->seed = sv;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — OptionsGénération : ecriture d'un reglage de
                 * generation (double point). Feuille scalaire + callback
                 * ..SurToken (reference de procedure). Symetrique de la branche
                 * OP_STRUCT_GET. Noms accent-strippes par le scanner. */
                if (IS_OPTIONS_GENERATION(instVal)) {
                    ObjOptionsGeneration* o = AS_OPTIONS_GENERATION(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    int iv = IS_INTEGER(val) ? (int)AS_INTEGER(val)
                           : IS_REAL(val)    ? (int)AS_REAL(val) : 0;
                    double dv = IS_REAL(val)    ? AS_REAL(val)
                              : IS_INTEGER(val) ? (double)AS_INTEGER(val) : 0.0;
                    int64_t sv = IS_INTEGER(val) ? AS_INTEGER(val)
                               : IS_REAL(val)    ? (int64_t)AS_REAL(val) : 0;
                    if (fl == 11 && strncasecmp(fn, "temperature", 11) == 0) {
                        o->temperature = dv;
                    } else if (fl == 4 && strncasecmp(fn, "topk", 4) == 0) {
                        o->topK = iv;
                    } else if (fl == 4 && strncasecmp(fn, "topp", 4) == 0) {
                        o->topP = dv;
                    } else if (fl == 9 && strncasecmp(fn, "maxtokens", 9) == 0) {
                        o->maxTokens = iv;
                    } else if ((fl == 6 && strncasecmp(fn, "graine", 6) == 0)
                        || (fl == 4 && strncasecmp(fn, "seed", 4) == 0)) {
                        o->seed = sv;
                    } else if ((fl == 8 && strncasecmp(fn, "surtoken", 8) == 0)
                        || (fl == 7 && strncasecmp(fn, "ontoken", 7) == 0)) {
                        /*  — stocke la reference de procedure (nom nu
                         * resolu en callable par le compilateur). Ce champ
                         * devient une racine GC vivante : tracee dans gc.c. */
                        o->onToken = val;
                    } else if ((fl == 18 && strncasecmp(fn, "contraintedesortie", 18) == 0)
                        || (fl == 16 && strncasecmp(fn, "outputconstraint", 16) == 0)) {
                        /*  — contrainte de sortie open-enum. Devient racine
                         * GC vivante des qu'une chaine y est posee : tracee dans gc.c. */
                        o->constraint = IS_STRING(val) ? val : NIL_VAL;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /*  — MessageLLM : ecriture de ..Rôle / ..Contenu (double
                 * point). Enum ouverte E_rôleLLM cote ..Rôle : une constante
                 * (rôleUtilisateur = "user") comme une chaine libre ("developer")
                 * sont acceptees telles quelles. Les deux champs deviennent racines
                 * GC vivantes des qu'une chaine y est posee : tracees dans gc.c.
                 * Symetrique de la branche OP_STRUCT_GET. */
                if (IS_MESSAGE_LLM(instVal)) {
                    ObjMessageLLM* m = AS_MESSAGE_LLM(instVal);
                    int fl = fieldName->length;
                    const char* fn = fieldName->chars;
                    if (fl == 4 && strncasecmp(fn, "role", 4) == 0) {
                        m->role = IS_STRING(val) ? val : NIL_VAL;
                    } else if ((fl == 7 && strncasecmp(fn, "contenu", 7) == 0)
                        || (fl == 7 && strncasecmp(fn, "content", 7) == 0)) {
                        m->content = IS_STRING(val) ? val : NIL_VAL;
                    }
                    vm_push(val);
                    DISPATCH();
                }

                /* Date property set: dDate..Jour = 5 */
                if (IS_DATE(instVal)) {
                    ObjDate* d = AS_DATE(instVal);
                    int v = IS_INTEGER(val) ? (int)AS_INTEGER(val) :
                            IS_REAL(val) ? (int)AS_REAL(val) : 0;
                    if (strncasecmp(fieldName->chars, "jour", 4) == 0 ||
                        strncasecmp(fieldName->chars, "day", 3) == 0)
                        d->day = v;
                    else if (strncasecmp(fieldName->chars, "mois", 4) == 0 ||
                             strncasecmp(fieldName->chars, "month", 5) == 0)
                        d->month = v;
                    else if (strncasecmp(fieldName->chars, "annee", 5) == 0 ||
                             strncasecmp(fieldName->chars, "year", 4) == 0)
                        d->year = v;
                    owl_normalize_date(d);
                    vm_push(val);
                    DISPATCH();
                }

                /* Time property set */
                if (IS_TIME(instVal)) {
                    ObjTime* t = AS_TIME(instVal);
                    int v = IS_INTEGER(val) ? (int)AS_INTEGER(val) :
                            IS_REAL(val) ? (int)AS_REAL(val) : 0;
                    if (strncasecmp(fieldName->chars, "heure", 5) == 0 ||
                        strncasecmp(fieldName->chars, "hour", 4) == 0)
                        t->hour = v;
                    else if (strncasecmp(fieldName->chars, "minute", 6) == 0)
                        t->minute = v;
                    else if (strncasecmp(fieldName->chars, "seconde", 7) == 0 ||
                             strncasecmp(fieldName->chars, "second", 6) == 0)
                        t->second = v;
                    owl_normalize_time(t);
                    vm_push(val);
                    DISPATCH();
                }

                /* DateTime property set */
                if (IS_DATETIME(instVal)) {
                    ObjDateTime* dt = AS_DATETIME(instVal);
                    int v = IS_INTEGER(val) ? (int)AS_INTEGER(val) :
                            IS_REAL(val) ? (int)AS_REAL(val) : 0;
                    if (strncasecmp(fieldName->chars, "jour", 4) == 0 ||
                        strncasecmp(fieldName->chars, "day", 3) == 0)
                        dt->day = v;
                    else if (strncasecmp(fieldName->chars, "mois", 4) == 0 ||
                             strncasecmp(fieldName->chars, "month", 5) == 0)
                        dt->month = v;
                    else if (strncasecmp(fieldName->chars, "annee", 5) == 0 ||
                             strncasecmp(fieldName->chars, "year", 4) == 0)
                        dt->year = v;
                    else if (strncasecmp(fieldName->chars, "heure", 5) == 0 ||
                             strncasecmp(fieldName->chars, "hour", 4) == 0)
                        dt->hour = v;
                    else if (strncasecmp(fieldName->chars, "minute", 6) == 0)
                        dt->minute = v;
                    else if (strncasecmp(fieldName->chars, "seconde", 7) == 0 ||
                             strncasecmp(fieldName->chars, "second", 6) == 0)
                        dt->second = v;
                    owl_normalize_datetime(dt);
                    vm_push(val);
                    DISPATCH();
                }

                /*  : ecriture membre GLOBAL via cClasse.nMembre = val
                 * — stockage dans klass->staticFields. */
                if (IS_OBJ(instVal) && OBJ_TYPE(instVal) == OBJ_CLASS) {
                    ObjClass* klass = AS_CLASS(instVal);
                    /*  : si un slot existe deja sous une autre casse,
                     * on ecrit dedans (pas de slot duplique). */
                    ObjString* slot = table_find_key_case_insensitive(
                        &klass->staticFields, fieldName->chars);
                    table_set(&klass->staticFields,
                              slot != NULL ? slot : fieldName, val);
                    vm_push(val);
                    DISPATCH();
                }

                /*  Lot 2e-1 — Ecriture sur ObjIcone. Props RW :
                 * Couleur, Rotation, Forme. Read-only (silencieusement
                 * ignore, pour ne pas casser un test d'edge case qui set
                 * une prop derivee) : Valide, CouleurModifiable.
                 *
                 * ..Icone SET = reassignation polymorphique : appelle
                 * parse_to_icone sur la valeur RHS et reabsorbe ses
                 * champs en preservant Couleur/Rotation/Forme du recepteur
                 * (Cas F : `ICN_Save = icones.Delete` preserve les
                 * customisations).
                 *
                 * ..Type : ignore en SET (read-only de fait). */
                if (IS_ICONE(instVal)) {
                    ObjIcone* icn = AS_ICONE(instVal);
                    char fn[64];
                    int fnlen = scanner_strip_accents(fieldName->chars,
                                                     fieldName->length,
                                                     fn, sizeof(fn));
                    (void)fnlen;
                    if (strcasecmp(fn, "Couleur") == 0
                        || strcasecmp(fn, "Color") == 0) {
                        if (IS_INTEGER(val)) {
                            icn->couleur = AS_INTEGER(val);
                        } else if (IS_REAL(val)) {
                            icn->couleur = (int64_t)AS_REAL(val);
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Rotation") == 0) {
                        if (IS_INTEGER(val)) {
                            icn->rotation = (int)AS_INTEGER(val);
                        } else if (IS_REAL(val)) {
                            icn->rotation = (int)AS_REAL(val);
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Forme") == 0
                        || strcasecmp(fn, "Shape") == 0) {
                        if (IS_INTEGER(val)) {
                            icn->forme = (int)AS_INTEGER(val);
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "CouleurFond") == 0
                        || strcasecmp(fn, "BackgroundColor") == 0) {
                        /*  : couleur de fond visible si forme != formeAucune. */
                        if (IS_INTEGER(val)) {
                            icn->couleur_fond = AS_INTEGER(val);
                        } else if (IS_REAL(val)) {
                            icn->couleur_fond = (int64_t)AS_REAL(val);
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    if (strcasecmp(fn, "Icone") == 0
                        || strcasecmp(fn, "Icon") == 0) {
                        /* Reassignation polymorphique : parse val vers
                         * une nouvelle Icone, puis copie ses champs
                         * "source" (svg_raw + valide + couleur_modifiable
                         * + catalog_hash) dans le recepteur, en preservant
                         * les props utilisateur (couleur/rotation/forme). */
                        Value parsed = owl_icones_parse_value(val);
                        if (IS_ICONE(parsed)) {
                            ObjIcone* p = AS_ICONE(parsed);
                            icn->catalog_hash       = p->catalog_hash;
                            icn->svg_raw            = p->svg_raw;
                            icn->valide             = p->valide;
                            icn->couleur_modifiable = p->couleur_modifiable;
                        }
                        vm_push(val);
                        DISPATCH();
                    }
                    /* RO ou ..Type : no-op silencieux, push val pour
                     * preserver l'invariant pile. */
                    vm_push(val);
                    DISPATCH();
                }

                if (!IS_INSTANCE(instVal)) {
                    owl_emit_runtime_error(E_RT_FIELDS_NEED_INSTANCE, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(instVal);

                /*  — cascade imperative sur un sous-champ composite
                 * d'un controle d'Etat (`LIB..Cadre..Couleur =`) : l'ecriture
                 * cible cette sous-instance, pas l'ObjUiControlInstance. Via la
                 * back-ref posee au seed du store, on marque la prop composite
                 * (Cadre/Police) override cote controle, pour que le reseed par
                 * bloc ne re-pointe plus le store sur une instance
                 * fraiche et n'orpheline plus cette ecriture. */
                if (instance->ui_owner != NULL
                    && instance->ui_owner_key != NULL) {
                    vm_push(instVal); /* root pendant l'alloc de mark_override */
                    obj_ui_control_instance_mark_override(
                        instance->ui_owner, instance->ui_owner_key->chars);
                    vm_pop();
                }

                /*  — mise en forme d'une cellule ou d'une ligne de
                 * Table. instVal a deja ete depile : le stockage peut grow et
                 * declencher un GC, donc on re-root instVal (donc la rep, qui
                 * n'est joignable que par lui) et la valeur ecrite le temps de
                 * l'appel — pop en LIFO strict. */
                {
                    vm_push(instVal);
                    bool fmtValIsObj = IS_OBJ(val);
                    if (fmtValIsObj) vm_push(val);
                    bool fmtDone = vm_table_fmt_set(instance, fieldName->chars,
                                                    val);
                    if (!fmtDone)
                        fmtDone = vm_table_colmask_set(instance,
                                                       fieldName->chars, val);
                    if (!fmtDone)
                        fmtDone = vm_table_coldispmask_set(instance,
                                                           fieldName->chars, val);
                    if (fmtValIsObj) vm_pop();
                    vm_pop();
                    if (fmtDone) {
                        vm_push(val);
                        DISPATCH();
                    }
                }

                /*  — cellule de rep ZoneRepetee : coerce selon le
                 * contrat de valeur du sous-controle (Saisie -> chaine, parite
                 * SAI..Valeur ). No-op (identite) pour tout autre
                 * instance : struct user, rep Table typee, sous-controle non
                 * Saisie. La coercion peut produire une ObjString fraiche
                 * (value_to_string) ; le storage qui suit (obj_copy_string +
                 * table_set) peut grow → GC. On root instVal + le val coerce
                 * pendant toute la phase de stockage, pop juste avant le
                 * push final. */
                val = owl_ui_zr_rep_coerce_cell(instance, fieldName->chars,
                                                val);
                vm_push(instVal);
                bool zrValIsObj = IS_OBJ(val);
                if (zrValIsObj) vm_push(val);

                /*  : plus de dispatch __set_Prop ici. Le sucre
                 * `obj.Prop = val` est rejeté en tant que setter implicite.
                 * Pour assigner via méthode, écrire `obj.DefinirProp(val)`. */

                /* Fallback bilingue EN->FR : si fieldName matche le nameEN
                 * d'un OwlField du type, on ecrit dans le canon ASCII pour
                 * preserver l'unicite du storage (eviter Method + Methode
                 * comme deux fields distincts). Cf invariant 3 + rule 11. */
                const char* writtenFieldCanon = NULL;
                {
                    const char* canon = NULL;
                    if (instance->structName != NULL) {
                        canon = owl_registry_resolve_field_en_to_canon(
                            instance->structName->chars, fieldName->chars);
                    }
                    if (canon != NULL) {
                        ObjString* canonKey = obj_copy_string(canon, (int)strlen(canon));
                        table_set(&instance->fields, canonKey, val);
                        writtenFieldCanon = canon;
                    } else {
                        /*  : invariant 1 — l'ecriture via une variante
                         * de casse cible le slot existant (celui de la decl),
                         * sans creer de doublon. Slot absent => nouveau champ
                         * sous la casse ecrite. */
                        ObjString* slot = table_find_key_case_insensitive(
                            &instance->fields, fieldName->chars);
                        ObjString* key = slot != NULL ? slot : fieldName;
                        table_set(&instance->fields, key, val);
                        writtenFieldCanon = key->chars;
                    }
                }
                /*  — Hook onFieldSet du type (rebalance post-write).
                 * Couleur l'utilise pour recalculer RGB <-> packed <-> HSL.
                 * Le hook recoit le nom canon ASCII FR du field ecrit. */
                if (instance->structName != NULL) {
                    const OwlTypeDef* td = owl_registry_type_find(
                        instance->structName->chars);
                    if (td != NULL && td->onFieldSet != NULL
                        && writtenFieldCanon != NULL) {
                        td->onFieldSet(instance, writtenFieldCanon);
                    }
                }
                /* V1.5-D : hook watcher Cadre. Si l'instance ou un de
                 * ses ancetres est attache a un widget, re-apply Qt6. */
                owl_ui_on_instance_modified(instance);
                /*  — release des roots poses avant le storage. */
                if (zrValIsObj) vm_pop();
                vm_pop(); /* instVal */
                vm_push(val);
                DISPATCH();
            }

            /* --- Exception handling --- */

            VM_CASE(OP_TRY): {
                uint16_t offset = READ_SHORT();
                if (exceptionCount >= EXCEPTION_STACK_MAX) {
                    owl_emit_runtime_error(E_RT_EXCEPTION_STACK_OVERFLOW, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ExceptionHandler* handler = &exceptionStack[exceptionCount++];
                handler->ip = frame->ip + offset; /* point to handler code */
                handler->stackTop = vm.stackTop;
                handler->frameCount = vm.frameCount;
                handler->frame = *frame;
                /*  : snapshot du contexte d'exception englobant, a
                 * restaurer en sortie du handler. Cape sur EXCEPTION_STACK_MAX
                 * (meme borne que exceptionStack : push apparie). */
                if (g_exec_ctx_cur->excSaveDepth < EXCEPTION_STACK_MAX) {
                    exc_context_capture(&g_exec_ctx_cur->excCtxSaveStack[g_exec_ctx_cur->excSaveDepth]);
                    g_exec_ctx_cur->excSaveDepth++;
                }
                handler->savedCtxDepth = g_exec_ctx_cur->excSaveDepth;
                DISPATCH();
            }

            VM_CASE(OP_END_TRY): {
                if (exceptionCount > 0) {
                    exceptionCount--;
                }
                /*  : chemin SANS exception — depop le snapshot pose par
                 * l'OP_TRY correspondant. Pas de restauration : le contexte n'a
                 * pas ete modifie (aucune exception rattrapee ici). */
                if (g_exec_ctx_cur->excSaveDepth > 0) {
                    g_exec_ctx_cur->excSaveDepth--;
                }
                DISPATCH();
            }

            VM_CASE(OP_THROW): {
                Value msg = vm_pop();
                if (IS_STRING(msg)) {
                    capture_exception_context(AS_CSTRING(msg), NULL);
                } else {
                    capture_exception_context("Exception", NULL);
                }
                /*  — a une frontiere reentrante, un throw user vers un
                 * handler ancetre (hors scope) est propage en vol, pas consomme
                 * ici (symetrique du chemin div-par-zero / pont OP_CALL). */
                if (exc_top_handler_in_scope(exitFrame)) {
                    if (handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                } else if (exceptionCount > 0) {
                    OWL_RT_PROPAGATE_REENTRANT();
                    return INTERPRET_RUNTIME_ERROR;
                }
                owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION,
                                       "message", currentException->chars, NULL);
                return INTERPRET_RUNTIME_ERROR;
            }

            VM_CASE(OP_RERAISE): {
                /* Re-raise the current exception — message/code/line etc.
                 * are preserved from when it was first captured. */
                if (currentException == NULL) {
                    owl_emit_runtime_error(E_RT_NO_PENDING_EXCEPTION, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                /*  — cf. OP_THROW : handler ancetre propage a la
                 * frontiere reentrante. */
                if (exc_top_handler_in_scope(exitFrame)) {
                    if (handle_runtime_exception()) {
                        frame = &vm.frames[vm.frameCount - 1];
                        DISPATCH();
                    }
                } else if (exceptionCount > 0) {
                    OWL_RT_PROPAGATE_REENTRANT();
                    return INTERPRET_RUNTIME_ERROR;
                }
                owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION,
                                       "message", currentException->chars, NULL);
                return INTERPRET_RUNTIME_ERROR;
            }

            VM_CASE(OP_EXC_RESTORE): {
                /*  : sortie NORMALE d'un handler. Restaure le contexte
                 * d'exception englobant snapshotte a l'OP_TRY correspondant.
                 * Emis par le compilateur en fin de corps FAIRE uniquement (pas
                 * sur le chemin de propagation OP_RERAISE/outer_unwind). Au
                 * niveau externe, restaure le contexte vide du top-level → hors
                 * handler, ExceptionInfo renvoie vide et ExceptionPropage est un
                 * no-op. */
                if (g_exec_ctx_cur->excSaveDepth > 0) {
                    g_exec_ctx_cur->excSaveDepth--;
                    exc_context_apply(&g_exec_ctx_cur->excCtxSaveStack[g_exec_ctx_cur->excSaveDepth]);
                }
                DISPATCH();
            }

            /* --- OOP operations --- */

            VM_CASE(OP_CLASS): {
                ObjString* name = READ_STRING();
                ObjClass* klass = obj_new_class(name);
                vm_push(OBJ_VAL(klass));
                DISPATCH();
            }

            VM_CASE(OP_MARK_CLASS_KIND): {
                uint8_t k = READ_BYTE();
                Value top = vm_peek(0);
                if (IS_CLASS(top)) {
                    AS_CLASS(top)->kind = (ClassKind)k;
                }
                DISPATCH();
            }

            VM_CASE(OP_METHOD): {
                /* Stack: [class] [method_function]
                 * Operand: method name constant index */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* methodName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value method = vm_pop();  /* the function */
                Value klassVal = vm_peek(0);  /* the class stays on stack */
                if (!IS_CLASS(klassVal)) {
                    owl_emit_runtime_error(E_RT_METHOD_NEEDS_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* klass = AS_CLASS(klassVal);
                table_set(&klass->methods, methodName, method);
                DISPATCH();
            }

            VM_CASE(OP_OUT_PARAM_TO_LOCAL): {
                /* Operand: slot (1B), idx (1B). Lit vm.outParamReturns[idx]
                 * et l'écrit dans frame->slots[slot]. */
                uint8_t slot = READ_BYTE();
                uint8_t idx  = READ_BYTE();
                if (idx < vm.outParamCount) {
                    frame->slots[slot] = vm.outParamReturns[idx];
                }
                DISPATCH();
            }

            VM_CASE(OP_OUT_PARAM_TO_GLOBAL): {
                /* Operandes : nameConst (index de constante), idx (1 octet). */
                uint16_t nameIdx = READ_CONST_IDX();
                uint8_t  idx     = READ_BYTE();
                if (idx < vm.outParamCount) {
                    ObjString* name = AS_STRING(
                        frame->function->chunk->constants.values[nameIdx]);
                    /*  phase 2b : route via canon. La variable existe
                     * deja (declaree avant l'appel), on resout le verbatim
                     * canonique avant d'ecrire (preserve la graphie). */
                    Value dummy;
                    ObjString* verbatim = NULL;
                    if (vm_globals_canon_get(name, &dummy, &verbatim)) {
                        table_set(&vm.globals, verbatim, vm.outParamReturns[idx]);
                    } else {
                        /* Edge case : globale absente — passer par
                         * obj_globals_set_kv pour creer bilateralement. */
                        obj_globals_set_kv(name, vm.outParamReturns[idx]);
                    }
                }
                DISPATCH();
            }

            VM_CASE(OP_ENUM_VALUE): {
                /* Stack: [class] [value]
                 * Operand: field name constant index
                 * Stores value in class->defaults.
                 *   : peek val (pas pop) avant table_set
                 * qui peut grow → GC → val (si Obj) collecte. */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* fieldName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value val = vm_peek(0);
                Value klassVal = vm_peek(1);
                if (!IS_CLASS(klassVal)) {
                    owl_emit_runtime_error(E_RT_ENUM_VALUE_NEEDS_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* klass = AS_CLASS(klassVal);
                table_set(&klass->defaults, fieldName, val);
                vm_pop(); /* val (apres table_set) */
                DISPATCH();
            }

            VM_CASE(OP_STATIC_FIELD): {
                /*  : membre GLOBAL — un exemplaire par classe.
                 * Stack: [class] [value] (class stays). Stocke dans
                 * klass->staticFields (PAS copie dans instances, contrairement
                 * a defaults).
                 *
                 *   : peek val au lieu de pop avant le
                 * table_set qui peut grow (cap 0 → 8) → GC → val (singleton
                 * Obj) collecte car unrooted entre pop et table_set. Slot
                 * recoit du poison → cConfig.m_Inst lit IS_INSTANCE false. */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* fieldName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value val = vm_peek(0);
                Value klassVal = vm_peek(1);
                if (!IS_CLASS(klassVal)) {
                    owl_emit_runtime_error(E_RT_STATIC_FIELD_NEEDS_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* klass = AS_CLASS(klassVal);
                table_set(&klass->staticFields, fieldName, val);
                vm_pop(); /* val (apres table_set) */
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_PACK): {
                /* Pop N values from stack, create array containing them (in order) */
                uint8_t count = READ_BYTE();
                ObjArray* arr = obj_new_array();
                /*   : push arr AVANT les value_array_write
                 * qui peuvent grow → GC. Sans ce push, arr n'est rooté nulle
                 * part pendant les grows successifs et peut etre collecte. */
                vm_push(OBJ_VAL(arr));
                /* Stack apres push arr : [val1, ..., valN, arr], top=arr.
                 * peek(0)=arr, peek(1)=valN, ..., peek(count)=val1. */
                for (int i = count; i > 0; i--) {
                    value_array_write(&arr->elements, vm_peek(i));
                }
                /* Compacte : ecrit arr dans le slot val1 (le plus profond
                 * des count vals) puis trim count slots. Net : on retire
                 * count values, arr reste au top. */
                if (count > 0) {
                    vm.stackTop[-1 - count] = vm.stackTop[-1];
                    vm.stackTop -= count;
                }
                DISPATCH();
            }

            VM_CASE(OP_ARRAY_UNPACK): {
                /* Pop array, push N elements from it (base 1) */
                uint8_t count = READ_BYTE();
                Value arrVal = vm_pop();
                if (!IS_ARRAY(arrVal)) {
                    owl_emit_runtime_error(E_RT_MULTI_RETURN_NEEDS_ARRAY, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray* arr = AS_ARRAY(arrVal);
                for (int i = 0; i < count; i++) {
                    if (i < arr->elements.count) {
                        vm_push(arr->elements.values[i]);
                    } else {
                        vm_push(NIL_VAL);
                    }
                }
                DISPATCH();
            }

            VM_CASE(OP_COLLECTION): {
                uint8_t collType = READ_BYTE();
                ObjCollection* coll = obj_new_collection((CollectionType)collType);
                vm_push(OBJ_VAL(coll));
                DISPATCH();
            }

            VM_CASE(OP_INVOKE): {
                /* Stack: [instance] [arg1] ... [argN]
                 * Operands: method name index, arg count */
                uint16_t nameIdx = READ_CONST_IDX();
                uint8_t argCount = READ_BYTE();
                ObjString* methodName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);

                /* RAZ-auto dispatch PRÉ-INVOKE : même rôle que OP_CALL
                 * (cf. commentaire OP_CALL pour la doctrine complète et la
                 * sentinelle erreur-fraîche).  +  + 
                 * ligne 17 (contrat user). */
                owl_clear_error();

                Value receiver = vm_peek(argCount);

                /* Extension method dispatch for non-instance types */
                int rtype = method_receiver_type(receiver);
                if (rtype >= 0) {
                    int methodArity;
                    NativeFn fn = method_lookup((MethodReceiverType)rtype,
                        methodName->chars, methodName->length, &methodArity);
                    if (fn != NULL) {
                        int effectiveArgCount = argCount + 1;
                        if (methodArity != -1 && effectiveArgCount != methodArity) {
                            char expBuf[16], gotBuf[16];
                            snprintf(expBuf, sizeof expBuf, "%d", methodArity - 1);
                            snprintf(gotBuf, sizeof gotBuf, "%d", argCount);
                            owl_emit_runtime_error(E_RT_METHOD_BAD_ARITY,
                                                   "name", methodName->chars,
                                                   "expected", expBuf,
                                                   "got", gotBuf, NULL);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Value* args = vm.stackTop - argCount - 1;
                        /* MOCK INTERCEPTION : miroir de call_value(). Le
                         * dispatch méthode n'a que le NativeFn brut du binding,
                         * d'où la variante _fn. Le receveur est arg 0 : la forme
                         * `arch.NbFichier()` présente donc les mêmes args que la
                         * forme fonction `zipNbFichier(arch)` — interception
                         * identique. */
                        Value result;
                        Value mockResult;
                        if (mock_try_intercept_fn(fn, effectiveArgCount, args,
                                                  &mockResult)) {
                            result = mockResult;
                        } else {
                            result = fn(effectiveArgCount, args);
                        }
                        vm.stackTop -= (argCount + 1);
                        vm_push(result);
                        if (throwPending) {
                            throwPending = false;
                            if (handle_runtime_exception()) {
                                frame = &vm.frames[vm.frameCount - 1];
                                DISPATCH();
                            }
                            owl_emit_runtime_error(E_RT_UNHANDLED_EXCEPTION,
                                "message",
                                currentException ? currentException->chars : "unknown",
                                NULL);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        DISPATCH();
                    }
                }

                /*  : appel de methode globale via cClasse.Method(args).
                 * Receiver = ObjClass — lookup direct dans klass->methods.
                 * Slot 0 reste la classe (la methode ne doit pas utiliser ':',
                 * verifie au compile-time). */
                if (IS_OBJ(receiver) && OBJ_TYPE(receiver) == OBJ_CLASS) {
                    ObjClass* klass = AS_CLASS(receiver);
                    Value method;
                    if (!class_lookup_method(klass, methodName, &method)) {
                        owl_emit_runtime_error(E_RT_UNDEFINED_STATIC_METHOD,
                                               "method", methodName->chars,
                                               "class", klass->name->chars, NULL);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (!call(AS_FUNCTION(method), argCount)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    frame = &vm.frames[vm.frameCount - 1];
                    DISPATCH();
                }

                if (!IS_INSTANCE(receiver)) {
                    /*  — `FEN_X.maProc()` : le receveur est une fenetre
                     * parfaitement valide, ce n'est pas la resolution du type
                     * qui echoue mais la forme d'appel qui n'existe pas. Le
                     * message generique orientait vers un probleme de type et
                     * ne distinguait meme pas une procedure existante d'une
                     * inconnue. Dire ce qui ne va pas, et nommer les deux
                     * formes supportees. */
                    if (IS_WINDOW_REF(receiver)) {
                        ObjWindowRef* wref = AS_WINDOW_REF(receiver);
                        owl_emit_runtime_error(
                            E_RT_WINDOW_DOT_CALL_UNSUPPORTED,
                            "window",
                            (wref->name != NULL) ? wref->name->chars : "?",
                            "method", methodName->chars, NULL);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    owl_emit_runtime_error(E_RT_NO_METHOD_FOR_TYPE,
                                           "method", methodName->chars, NULL);
                    OWL_RT_RECOVER_DISPATCH();
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(receiver);

                /* Check fields first (might be a function stored as field) */
                Value fieldVal;
                if (table_get(&instance->fields, methodName, &fieldVal)) {
                    /* Replace receiver with the field value and call it */
                    vm.stackTop[-argCount - 1] = fieldVal;
                    if (!call_value(fieldVal, argCount)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    frame = &vm.frames[vm.frameCount - 1];
                    DISPATCH();
                }

                /* Look up method in class hierarchy */
                if (instance->klass == NULL) {
                    owl_emit_runtime_error(E_RT_INSTANCE_NO_CLASS_FOR_METHOD,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value method;
                if (!class_lookup_method(instance->klass, methodName, &method)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_METHOD,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }

                /*  : plus de fallback __set_ ici. Une méthode est
                 * une méthode nue, son nom dans la table de classe = clé
                 * unique. Toute incohérence d'arité = erreur côté call(). */

                /* Call the method with instance as slot 0 (receiver) */
                if (!call(AS_FUNCTION(method), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_INVOKE_SAFE): {
                /* Like OP_INVOKE but silently pushes Nil if method doesn't exist */
                uint16_t nameIdx = READ_CONST_IDX();
                uint8_t argCount = READ_BYTE();
                ObjString* methodName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);

                /* RAZ-auto dispatch PRÉ-INVOKE_SAFE : même rôle que OP_CALL
                 * (cf. commentaire OP_CALL pour la doctrine complète et la
                 * sentinelle erreur-fraîche).  +  + 
                 * ligne 17 (contrat user). */
                owl_clear_error();

                Value receiver = vm_peek(argCount);

                /* Extension method dispatch for non-instance types */
                {
                    int rtype = method_receiver_type(receiver);
                    if (rtype >= 0) {
                        int methodArity;
                        NativeFn fn = method_lookup((MethodReceiverType)rtype,
                            methodName->chars, methodName->length, &methodArity);
                        if (fn != NULL) {
                            int effectiveArgCount = argCount + 1;
                            if (methodArity != -1 && effectiveArgCount != methodArity) {
                                for (int j = 0; j <= argCount; j++) vm_pop();
                                vm_push(NIL_VAL);
                                DISPATCH();
                            }
                            Value* args = vm.stackTop - argCount - 1;
                            /* MOCK INTERCEPTION : miroir de call_value() /
                             * OP_INVOKE (cf. commentaire là-bas). */
                            Value result;
                            Value mockResult;
                            if (mock_try_intercept_fn(fn, effectiveArgCount, args,
                                                      &mockResult)) {
                                result = mockResult;
                            } else {
                                result = fn(effectiveArgCount, args);
                            }
                            vm.stackTop -= (argCount + 1);
                            vm_push(result);
                            if (throwPending) {
                                throwPending = false;
                                if (handle_runtime_exception()) {
                                    frame = &vm.frames[vm.frameCount - 1];
                                }
                            }
                            DISPATCH();
                        }
                    }
                }

                if (!IS_INSTANCE(receiver)) {
                    /* Not an instance — just pop args and receiver, push nil */
                    for (int j = 0; j <= argCount; j++) vm_pop();
                    vm_push(NIL_VAL);
                    DISPATCH();
                }
                ObjInstance* instance = AS_INSTANCE(receiver);

                if (instance->klass == NULL) {
                    for (int j = 0; j <= argCount; j++) vm_pop();
                    vm_push(NIL_VAL);
                    DISPATCH();
                }

                Value method;
                if (!class_lookup_method(instance->klass, methodName, &method)) {
                    /* Method doesn't exist — silently pop and push nil */
                    for (int j = 0; j <= argCount; j++) vm_pop();
                    vm_push(NIL_VAL);
                    DISPATCH();
                }

                if (!call(AS_FUNCTION(method), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_INHERIT): {
                /* Stack: [subclass] [superclass] */
                Value superVal = vm_pop();
                Value subVal = vm_peek(0);

                if (!IS_CLASS(superVal)) {
                    owl_emit_runtime_error(E_RT_SUPERCLASS_NEEDS_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!IS_CLASS(subVal)) {
                    owl_emit_runtime_error(E_RT_SUBCLASS_NEEDS_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjClass* superclass = AS_CLASS(superVal);
                ObjClass* subclass = AS_CLASS(subVal);

                /* Copy methods from superclass to subclass */
                table_add_all(&superclass->methods, &subclass->methods);
                /* Copy default fields */
                table_add_all(&superclass->defaults, &subclass->defaults);
                subclass->superclass = superclass;
                DISPATCH();
            }

            VM_CASE(OP_GET_THIS): {
                vm_push(frame->slots[0]);
                DISPATCH();
            }

            VM_CASE(OP_MOIMEME): {
                /*  — resout MoiMême au widget "courant" lu depuis
                 * vm.event_sender_stack. La stack est alimentee par les
                 * sites natifs qui ouvrent un contexte widget :
                 *   - open_window_instance (top-level fenetre + chaque
                 *     handler de champ) push l'ObjWindowRef/ObjUiControlRef
                 *     correspondant ;
                 *   - vm_etat_stack_push (top-level etat) idem ;
                 *   - les dispatcher d'evenements (TestSimuleSouris,
                 *     dispatch_event Qt, dispatch_image_event_*) push le
                 *     ref du champ emetteur autour de l'invocation.
                 *
                 * Heritage chain : la stack reste vivante tant qu'un push
                 * n'a pas pop. Une procedure appelee depuis un handler
                 * voit donc le meme MoiMême (le push parent reste au
                 * sommet jusqu'a la sortie du handler).
                 *
                 * Stack vide -> soft error E_RT_MOIMEME_NO_WIDGET (pose
                 * ErreurDetectee). On push un ObjUiControlRef "null" (decl
                 * et name NULL) : l'OP_STRUCT_GET qui suit (`MoiMême..X`)
                 * tombera silencieusement sur la branche IS_UI_CONTROL_REF
                 * dont resolve_ui_ref retourne NULL et push NIL_VAL. Sans
                 * hard runtime_error, le programme continue et le `Si
                 * ErreurDetectee Alors ...` du user reste fonctionnel. */
                if (vm_event_sender_depth() > 0) {
                    Value top = vm_event_sender_top();
                    vm_push(top);
                    DISPATCH();
                }
                owl_emit_native_error(E_RT_MOIMEME_NO_WIDGET, "MoiMême", NULL);
                ObjUiControlRef* nullRef = obj_new_ui_control_ref(NULL);
                vm_push(OBJ_VAL(nullRef));
                DISPATCH();
            }

            VM_CASE(OP_MONCHAMP): {
                /*  — champ courant STRICT. Contrairement a MoiMême,
                 * aucun repli : seul un champ (ObjUiControlRef, ou en GUI un
                 * ObjUiControl non-fenetre) au sommet de event_sender_stack est
                 * valide. Une fenetre (ObjWindowRef, ObjUiControl kind WINDOW)
                 * ou une stack vide => soft error E_RT_MONCHAMP_NO_CONTROL
                 * (pose ErreurDetectee, pas de hard-stop). On push un
                 * ObjUiControlRef null pour que `MonChamp..Prop` retombe
                 * silencieusement sur la branche IS_UI_CONTROL_REF. */
                Value top = (vm_event_sender_depth() > 0)
                            ? vm_event_sender_top() : NIL_VAL;
                bool isField = IS_UI_CONTROL_REF(top)
                    || (IS_UI_CONTROL(top)
                        && AS_UI_CONTROL(top)->kind != OWL_CONTROL_WINDOW);
                if (isField) {
                    vm_push(top);
                    DISPATCH();
                }
                owl_emit_native_error(E_RT_MONCHAMP_NO_CONTROL, "MonChamp", NULL);
                ObjUiControlRef* mcNull = obj_new_ui_control_ref(NULL);
                vm_push(OBJ_VAL(mcNull));
                DISPATCH();
            }

            VM_CASE(OP_MAFENETRE): {
                /*  — fenetre courante. Resout vers l'ObjWindowRef
                 * proxy global (pose au compile-time sous le nom strippe de la
                 * fenetre) de la fenetre top-of-stack ; en GUI c'est l'
                 * ObjUiControl(kind=WINDOW) shadow qui prend la cle. Aucune
                 * fenetre ouverte => soft error E_RT_MAFENETRE_NO_WINDOW, push
                 * un ObjWindowRef null (`..Prop` retombe silencieusement). */
                ObjWindowInstance* winst = vm_window_stack_top();
                if (winst != NULL && winst->decl != NULL
                    && winst->decl->name != NULL) {
                    char wbuf[256];
                    int wlen = scanner_strip_accents(
                        winst->decl->name, (int)strlen(winst->decl->name),
                        wbuf, sizeof(wbuf));
                    if (wlen > 0) {
                        ObjString* wkey = obj_copy_string(wbuf, wlen);
                        Value w;
                        if (table_get(&vm.globals, wkey, &w)
                            && (IS_WINDOW_REF(w) || IS_UI_CONTROL(w))) {
                            vm_push(w);
                            DISPATCH();
                        }
                    }
                }
                owl_emit_native_error(E_RT_MAFENETRE_NO_WINDOW, "MaFenêtre", NULL);
                ObjWindowRef* mfNull = obj_new_window_ref(NULL, NULL);
                vm_push(OBJ_VAL(mfNull));
                DISPATCH();
            }

            VM_CASE(OP_SET_FIELD_THIS): {
                /* Pas de RAZ ici (cf. note OP_DEFINE_GLOBAL + ). */
                /* Stack: [value]; operand: field name constant idx
                 * Sets this.fieldName = value */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* fieldName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value val = vm_peek(0);
                Value thisVal = frame->slots[0];
                if (!IS_INSTANCE(thisVal)) {
                    owl_emit_runtime_error(E_RT_THIS_NOT_INSTANCE, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(thisVal);
                table_set(&instance->fields, fieldName, val);
                DISPATCH();
            }

            VM_CASE(OP_INVOKE_SUPER): {
                /*  — Ancetre.Methode(args).
                 * Stack: [this] [arg1] ... [argN]
                 * Operands: methodName constant index, arg count.
                 * Lookup demarre dans instance->klass->superclass (skip de
                 * la classe courante => force la version parente, meme si
                 * la classe a override). */
                uint16_t nameIdx = READ_CONST_IDX();
                uint8_t argCount = READ_BYTE();
                ObjString* methodName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);

                owl_clear_error();

                Value receiver = vm_peek(argCount);
                if (!IS_INSTANCE(receiver)) {
                    owl_emit_runtime_error(E_RT_ANCETRE_NEEDS_INSTANCE,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(receiver);
                if (instance->klass == NULL || instance->klass->superclass == NULL) {
                    owl_emit_runtime_error(E_RT_ANCETRE_NO_PARENT,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value method;
                if (!class_lookup_method(instance->klass->superclass,
                                         methodName, &method)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_ANCESTOR_METHOD,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!call(AS_FUNCTION(method), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_INVOKE_DIRECT): {
                /*  — super-call by-name (cAncestor.Methode(args)).
                 * Stack: [this] [arg1] ... [argN] -> [result].
                 * Operands: classNameIdx, methodNameIdx, argCount.
                 * Lookup direct dans targetClass->methods (pas de walk
                 * virtual). Compile-time a deja valide que la classe cible
                 * est un ancetre strict de la classe courante. */
                uint16_t classIdx = READ_CONST_IDX();
                uint16_t nameIdx  = READ_CONST_IDX();
                uint8_t argCount = READ_BYTE();
                ObjString* className = AS_STRING(
                    frame->function->chunk->constants.values[classIdx]);
                ObjString* methodName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);

                owl_clear_error();

                Value classVal;
                /*  phase 2b : lookup canon. */
                if (!vm_globals_canon_get(className, &classVal, NULL) ||
                    !IS_CLASS(classVal)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_ANCESTOR_METHOD,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* targetClass = AS_CLASS(classVal);
                Value method;
                /* Lookup *direct* dans la table de methodes (pas de walk
                 * superclass). OP_INHERIT a deja copie les methodes parents
                 * dans la table de chaque classe — donc methods["Crier"] sur
                 * cAnimal == la version cAnimal.Crier, pas une override. */
                if (!table_get(&targetClass->methods, methodName, &method)) {
                    owl_emit_runtime_error(E_RT_UNDEFINED_ANCESTOR_METHOD,
                                           "method", methodName->chars, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!call(AS_FUNCTION(method), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                DISPATCH();
            }

            VM_CASE(OP_IS_CLASS): {
                /*  — operateur d'introspection (obj est cClasse).
                 * Stack: [obj] -> [bool].
                 * Operand: nom (string constant) — peut etre une CLASSE
                 *          ou une INTERFACE.
                 * Resolution : walk la chaine superclass ; a chaque niveau,
                 * (a) match par nom de classe → Vrai ;
                 * (b) presence du nom dans klass->interfaces → Vrai
                 *     (peuple par OP_CLASS_DECLARE_INTERFACE).
                 * Le (b) est verifie a chaque niveau du walk pour que
                 * l'heritage propage les interfaces declarees par les
                 * ancetres. */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* targetName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value obj = vm_pop();
                bool result = false;
                if (IS_INSTANCE(obj)) {
                    ObjInstance* inst = AS_INSTANCE(obj);
                    ObjClass* k = inst->klass;
                    while (k != NULL) {
                        if (k->name != NULL &&
                            k->name->length == targetName->length &&
                            memcmp(k->name->chars, targetName->chars,
                                   (size_t)targetName->length) == 0) {
                            result = true;
                            break;
                        }
                        /*  : cherche aussi parmi les interfaces
                         * declarees a ce niveau de la chaine. */
                        Value dummy;
                        if (table_get(&k->interfaces, targetName, &dummy)) {
                            result = true;
                            break;
                        }
                        k = k->superclass;
                    }
                }
                vm_push(BOOL_VAL(result));
                DISPATCH();
            }

            VM_CASE(OP_CLASS_DECLARE_INTERFACE): {
                /*  — inscrit une interface dans le set runtime de
                 * la classe au sommet de pile (peek, sans pop). */
                uint16_t nameIdx = READ_CONST_IDX();
                ObjString* ifaceName = AS_STRING(
                    frame->function->chunk->constants.values[nameIdx]);
                Value klassVal = vm_peek(0);
                if (!IS_CLASS(klassVal)) {
                    owl_emit_runtime_error(E_RT_INTERFACE_TOP_NOT_CLASS, NULL);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* klass = AS_CLASS(klassVal);
                table_set(&klass->interfaces, ifaceName, NIL_VAL);
                DISPATCH();
            }

#ifdef OWL_COMPUTED_GOTO
            L_BAD_OP:
            {
                char opBuf[16];
                snprintf(opBuf, sizeof opBuf, "%d", instruction);
                owl_emit_runtime_error(E_RT_UNKNOWN_OPCODE,
                                       "opcode", opBuf, NULL);
                return INTERPRET_RUNTIME_ERROR;
            }
#else
            default: {
                char opBuf[16];
                snprintf(opBuf, sizeof opBuf, "%d", instruction);
                owl_emit_runtime_error(E_RT_UNKNOWN_OPCODE,
                                       "opcode", opBuf, NULL);
                return INTERPRET_RUNTIME_ERROR;
            }
        }
    }
#endif

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
#undef COMPARE_OP
#undef OWL_RT_RECOVER_DISPATCH
}

/* ================================================================
 * PUBLIC: EXECUTE (pre-compiled function)
 * ================================================================ */

InterpretResult vm_execute(ObjFunction* function) {
    vm_push(OBJ_VAL(function));
    call(function, 0);
    InterpretResult r = run();
    /*  — warning de fin de process : un timer armé jamais déclenché
     * (script CLI sans boucle d'événements). Hors mode owl test (unitaire)
     * pour ne pas polluer la sortie de la suite, et seulement si le top-level
     * s'est terminé proprement. */
    if (r == INTERPRET_OK && !testing_is_unitaire()) {
        owl_timer_warn_active_on_exit();
    }
    return r;
}

/* ================================================================
 * PUBLIC: CALL GLOBAL (by name)
 * ================================================================ */

/* Re-entrant call: invoke a Value that is callable (ObjFunction / ObjNative)
 * from inside a native. The `argCount` arguments must already be pushed onto
 * the VM stack beneath the function. On success, the function's return value
 * is on the stack; on failure, returns an error code.
 *
 * Safe to call with a non-empty frame stack — the inner dispatch loop
 * terminates as soon as the re-entrantly pushed frame is popped, rather
 * than unwinding everything back to frame 0. */
/* Bridge: promote a soft error (set via owl_set_error) to a real exception.
 * Called by ErreurPropage when the user wants to escalate. */
/*  : variante enrichie du pont soft-error -> exception. Préserve le
 * code canonique (chaîne) ET le nom de la native d'origine (errFonction) au
 * lieu de les perdre. Utilisée par ErreurPropage pour faire converger
 * ExceptionInfo avec ErreurInfo. errPendant reste générique "<runtime>"
 * (comportement historique du pont, cf. commentaire ci-dessous). */
void vm_throw_from_erreur_coded(const char* msg, const char* code,
                                const char* native_name) {
    if (native_name != NULL && native_name[0] != '\0') {
        g_exc_native_name = obj_copy_string(native_name, (int)strlen(native_name));
        g_exc_native_name_pending = true;
    }
    g_exc_origin_pending = false;
    if (g_exc_info == NULL) g_exc_info = obj_copy_string("", 0);
    capture_exception_context(msg, code);
    throwPending = true;
}

void vm_throw_from_erreur(const char* msg, int code) {
    /* Pont générique partagé : murs de coercion (entier / monétaire),
     * validations bitwise (EntierDansPlage, ...), littéraux Matrice/Vecteur.
     * Aucun de ces appelants n'est la fonction qui « lève » au sens
     * utilisateur — on laisse donc l'origine générique <runtime> (posée par
     * capture_exception_context quand origin_pending est faux), cohérente
     * avec les autres exceptions runtime non instrumentées. errPendant
     * retourne ainsi "<runtime>" et non un nom d'appelant trompeur.
     * Le code entier (0 chez tous les appelants actuels) est stringifié pour
     * le contrat errCode chaîne ; 0 => errCode "". */
    if (code != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", code);
        vm_throw_from_erreur_coded(msg, buf, NULL);
    } else {
        vm_throw_from_erreur_coded(msg, NULL, NULL);
    }
}

InterpretResult vm_call_reentrant(Value callee, int argCount) {
    int entryFrame = vm.frameCount;
    if (!call_value(callee, argCount)) {
        /*  : un debordement de pile au POINT D'ENTREE de l'appel
         * reentrant (frameCount == FRAMES_MAX quand call() est invoque) arme
         * g_rt_fatal_vm au site FRAMES_MAX sans jamais passer par un pont
         * OWL_RT_RECOVER_DISPATCH (le pont ne vit que dans run(), pas ici).
         * Sans nettoyage, le marqueur survit au retour vers le consommateur C
         * (sort_compare_ctx, callbacks UI/PDF/LLM/TableauFiltre/Applique...)
         * qui avale l'echec — et fatalise a tort la PROCHAINE erreur USER du
         * meme run() top-level. Contrat : le marqueur ne survit pas au retour
         * du reentrant. On efface a la frontiere, symetrique du reset run(). */
        g_rt_fatal_vm = false;
        return INTERPRET_RUNTIME_ERROR;
    }
    /* Native calls don't push a frame — they've already run and pushed their
     * result. Nothing to dispatch, return immediately. */
    if (vm.frameCount == entryFrame) {
        /*  : la native invoquee a pu declencher un vm_call_reentrant
         * imbrique qui a arme le marqueur ; la frontiere le laisse propre. */
        g_rt_fatal_vm = false;
        return INTERPRET_OK;
    }
    int  savedExit     = g_run_exit_frame;
    bool savedBoundary = g_run_reentrant_boundary;
    g_run_exit_frame        = entryFrame;
    g_run_reentrant_boundary = true;
    InterpretResult r = run();
    g_run_exit_frame        = savedExit;
    g_run_reentrant_boundary = savedBoundary;
    /*  : run() reset le marqueur a l'entree et l'efface au pont, mais
     * la frontiere reentrante ne doit JAMAIS laisser fuiter une fatale-VM vers
     * le consommateur C. La fatale ne se propage pas au-dela du reentrant. */
    g_rt_fatal_vm = false;
    return r;
}

InterpretResult vm_call_global(const char* name) {
    ObjString* fnName = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(fnName)); /* root pendant canon lookup */
    Value fn;
    /*  phase 2b : lookup canon (insensible casse+accents). */
    bool ok = vm_globals_canon_get(fnName, &fn, NULL);
    vm_pop(); /* fnName */
    if (!ok) {
        fprintf(stderr, "Error: '%s' not found.\n", name);
        return INTERPRET_RUNTIME_ERROR;
    }
    vm_push(fn);
    if (!call_value(fn, 0)) {
        /*  : meme frontiere reentrante que vm_call_reentrant. Un
         * debordement au point d'entree arme g_rt_fatal_vm hors pont ; on
         * l'efface pour qu'il ne fatalise pas une erreur USER ulterieure. */
        g_rt_fatal_vm = false;
        return INTERPRET_RUNTIME_ERROR;
    }
    InterpretResult r = run();
    /*  : la fatale-VM ne franchit pas la frontiere reentrante. */
    g_rt_fatal_vm = false;
    return r;
}

/* Vrai si un global (fonction/native/var) existe sous ce nom, resolution
 * casse+accent insensible (invariants 1 & 2). Wrapper de vm_globals_canon_get :
 * le check d'existence suit la MEME regle de resolution que vm_call_global,
 * pour eviter qu'un lookup exact casse-sensible en amont ne masque un global
 * declare en casse non-canon (ex : PROCEDURE fermeture vs check "Fermeture"). */
bool vm_global_exists(const char* name) {
    ObjString* nameStr = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(nameStr)); /* root pendant canon lookup */
    Value out;
    bool found = vm_globals_canon_get(nameStr, &out, NULL);
    vm_pop(); /* nameStr */
    return found;
}

/* ================================================================
 * PUBLIC: INTERPRET
 * ================================================================ */

InterpretResult vm_interpret(const char* source) {
    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;
    return vm_execute(function);
}

