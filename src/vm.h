// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_vm_h
#define owl_vm_h

#include "common.h"
#include "chunk.h"
#include "table.h"
#include "value.h"
#include "object.h"
#include "ui_decl.h"
#include "etat_decl.h"

#define FRAMES_MAX 256
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

/* ================================================================
 * CALL FRAME - One per active function call
 * ================================================================ */

typedef struct {
    ObjFunction* function;
    uint8_t* ip;        /* instruction pointer within this function */
    Value* slots;       /* pointer into the VM stack (first local slot) */
    int exceptionBase;  /* exceptionCount when this frame started; handlers
                         * pushed by this frame's body are popped on return. */
    int excSaveBase;    /*  : excSaveDepth when this frame started ;
                         * exception-context snapshots pushed by this frame
                         * (notably general-form handlers that never OP_END_TRY)
                         * are unwound + restored on return. */
} CallFrame;

/* ================================================================
 * VIRTUAL MACHINE
 * ================================================================ */

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frameCount;

    Value stack[STACK_MAX];
    Value* stackTop;

    Table globals;      /* global variable names -> values */
    Table types;        /* type names -> ObjInstance templates (builtins +
                         * user-declared types). Separe pour preserver
                         * l'invariant 5 du langage (coexistence type/native
                         * de meme orthographe canon : `httpRequête` type
                         * dans vm.types, `HTTPRequête()` native dans
                         * vm.globals). Lookup par OP_GET_TYPE_GLOBAL. */
    /*  phase 2 — index secondaire canon -> verbatim. La cle
     * verbatim (graphie user) reste celle stockee dans `globals`/`types`,
     * preservant l'invariant "casse de declaration = forme affichee" et
     * coexistence des homographes (`httpRequête` type vs `HTTPRequête`
     * native, qui vivent deja en namespaces distincts depuis phase 1).
     * L'index canon (scanner_canonicalize_identifier) rend le lookup
     * runtime tolerant casse+accents : canoniser la cle recherchee, lire
     * le verbatim associe, puis lookup direct dans la table principale.
     * Insertions bilaterales centralisees dans obj_globals_set* / vm_type_register. */
    Table globals_canon_idx;
    Table types_canon_idx;
    Table strings;      /* string interning table */

    Obj* objects;       /* head of all-objects list (for GC) */

    /* DSL Fenetre v1 : declarations top-level (Fenetre FEN_X ... FIN).
     * Tableau dynamique de pointeurs alloues par le compiler, libere
     * dans vm_free. Lookup lineaire (peu de fenetres en pratique). */
    OwlWindowDecl** windows;
    int             windowCount;
    int             windowCapacity;

    /* DSL Etat V2 : declarations top-level (ETA_X est un Etat ... FIN). */
    OwlEtatDecl**   etats;
    int             etatCount;
    int             etatCapacity;

    /* Out-params variadic — buffer de retour rempli par call_value() avant
     * le pop de la stack, consommé ensuite par OP_OUT_PARAM_WRITEBACK
     * émis par le compilateur. Capacité largement suffisante pour les
     * usages canon (RegexVerifie ~10 groupes max en pratique). */
    Value outParamReturns[16];
    int   outParamCount;

    /* Stack des contextes d'itération POUR TOUT actifs. Permet à
     * Supprime(tab, ÉlémentCourant) de cibler la clé/index en cours
     * du POUR TOUT le plus interne. La valeur stockée est la "clé"
     * canonique :
     *   - tableau 1D : INTEGER_VAL(index 1-based)
     *   - assoc      : OBJ_VAL(string clé)
     *   - liste/etc  : INTEGER_VAL(index)
     * Push à OP_ITER_BEGIN, set à chaque iteration, pop à OP_ITER_END.
     * Profondeur 16 = nesting réaliste. */
    Value iterCurrentKey[16];
    Value iterTarget[16];   /* la cible (tableau/assoc) en cours d'itération */
    int   iterDepth;

    /*  : mode headless explicite (ex-bypass scattered
     * testing_is_unitaire() dans les natives UI). Quand actif, les dialogs
     * modales (Info/Erreur/Avertissement/Toast/OuiNon/Confirmer) et Ouvre()
     * court-circuitent l'instanciation Qt (fallback NIL ou default_btn pour
     * les choix). Pilote depuis OWL par la native TestModeHeadless(actif).
     * Init Faux par defaut (owl standard) ; le runner test (testing.c) le
     * met a Vrai au demarrage de chaque fichier de test. */
    bool headless_mode;

    /*  : stack runtime des fenetres ouvertes (multi-instance).
     * Push a Ouvre/OuvreFille/OuvreSoeur, pop a Ferme. Le top = fenetre
     * "courante" qui resout les ObjUiControlRef ambigus (cf decisions
     * ). Roots GC : chaque entree est marquee depuis mark_roots,
     * elle-meme propage aux controls[] et a valeur_renvoyee.
     *
     * next_instance_id est partage par toutes les instances pour garantir
     * l'unicite globale (et pas seulement par-decl). */
    ObjWindowInstance** window_stack;
    int                 window_stack_count;
    int                 window_stack_capacity;
    int                 next_instance_id;
    /*  lot D — stack runtime des Etats en cours d'impression
     * (Etats vivants). Miroir strict de window_stack : push a l'entree de
     * iImprimeEtat, pop a la sortie. resolve_ui_ref consulte cette stack
     * en repli quand window_stack ne resout pas. */
    ObjEtatInstance**   etat_stack;
    int                 etat_stack_count;
    int                 etat_stack_capacity;

    /*  — stack runtime du widget "courant" pour MoiMême/MySelf.
     * Push a l'entree d'un handler d'evenement (cf. stdlib_ui.c
     * call_event_block / invoke_callback / dispatch_event*), pop a la
     * sortie. OP_MOIMEME lit le sommet ; si vide, repli sur etat_stack
     * top (top-level Etat) puis window_stack top (top-level Fenetre) ;
     * sinon soft-error E_RT_MOIMEME_NO_WIDGET (pose ErreurDetectee).
     *
     * Profondeur 32 : nesting realiste handler -> proc -> ... La meme
     * stack porte l'heritage chain : tant qu'un push n'a pas pop, toutes
     * les procedures appelees depuis le handler voient le meme MoiMême. */
    Value event_sender_stack[32];
    int   event_sender_stack_count;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

/* Global VM instance */
extern VM vm;

void vm_init(void);
void vm_free(void);
InterpretResult vm_interpret(const char* source);

/*  lot 1 : marque les ObjString* du contexte d'exception courant
 * (currentException + 6 g_exc_*). Appele depuis `mark_roots()` (gc.c).
 * Ces globals sont statiques a vm.c — d'ou la fonction dediee. */
void vm_exception_mark_roots(void);

/* Alias style clox / Crafting Interpreters pour les tests internes qui
 * reprennent la nomenclature du livre. Identique a gc_collect(). */
void collectGarbage(void);

/* Execute a pre-compiled function (no recompilation). */
InterpretResult vm_execute(ObjFunction* function);

/* Call a global procedure by name (looks up in vm.globals). */
InterpretResult vm_call_global(const char* name);

/* Vrai si un global existe sous ce nom (resolution casse+accent insensible,
 * invariants 1 & 2). Meme regle de resolution que vm_call_global. */
bool vm_global_exists(const char* name);

/* Re-entrant call: invoke a callable Value from inside a native. Arguments
 * must already be on the stack below `callee`. */
InterpretResult vm_call_reentrant(Value callee, int argCount);

/* DSL Fenetre v1 : enregistrement et lookup des declarations top-level.
 * register prend ownership du pointeur (libere par vm_free). En cas de
 * doublon de nom : retourne false sans liberer. */
bool                 vm_window_register(OwlWindowDecl* decl);
const OwlWindowDecl* vm_window_lookup(const char* name);

/* Compte le nombre de controles declares (toutes fenetres confondues) dont
 * le nom matche `name` (case-insensitive). Utilise par Occurrence(champ). */
int                  vm_count_controls_named(const char* name);

/* Cherche un widget user-declare par nom dans Fenetres + Etats. Retourne
 * son OwlControlKindInfo* (avec defaultProperty / is_widget / nameFR) ou
 * NULL si absent. Utilise par le compilo pour resoudre `WIDGET = X` ->
 * `WIDGET..<defaultProperty> = X` conforme canon. */
const OwlControlKindInfo* vm_find_widget_info_by_name(const char* name);

/* Lot V (live preview) : libere toutes les decls et vide vm.windows.
 * Utilise par le hot reload pour repartir d'un etat propre avant la
 * recompilation. */
void                 vm_windows_clear(void);

/*  : stack runtime multi-instance.
 *  - push  : ajoute inst en sommet (croit le tableau si necessaire).
 *  - pop   : retire et retourne le sommet (NULL si vide).
 *  - top   : retourne le sommet sans pop (NULL si vide).
 *  - clear : vide la stack sans toucher au tableau (capacity preservee).
 *            Les instances popees deviennent unreachable -> GC.
 * Aucune validation sur inst non-NULL pour push (caller responsable). */
void                 vm_window_stack_push(ObjWindowInstance* inst);
ObjWindowInstance*   vm_window_stack_pop(void);
ObjWindowInstance*   vm_window_stack_top(void);
void                 vm_window_stack_clear(void);

/*  — une instance de la fenetre `name` est-elle actuellement ouverte ?
 * Scanne vm.window_stack (push a Ouvre/OuvreFille/OuvreSoeur, pop a Ferme),
 * match casse+accent insensible sur decl->name (invariants 1 & 2). Utilise par
 * l'acces qualifie exterieur `FEN_X.gVar` : instance ouverte -> valeur, sinon
 * exception (E_RT_WINDOW_NOT_OPEN). */
bool                 vm_window_instance_is_open(const char* name);

/*  lot D — stack runtime des Etats en cours d'impression. Miroir
 * strict de window_stack. Memes conventions (push/pop/top/clear). */
void                 vm_etat_stack_push(ObjEtatInstance* inst);
ObjEtatInstance*     vm_etat_stack_pop(void);
ObjEtatInstance*     vm_etat_stack_top(void);

/*  — stack runtime du widget "courant" pour MoiMême/MySelf.
 *  - push  : pose le widget actif (ObjUiControlInstance / ObjWindowInstance /
 *            ObjEtatInstance — toute Value qui aura du sens dans
 *            `MoiMême..PropName`). Silent no-op si profondeur depassee.
 *  - pop   : retire le sommet. Aucune validation paire push/pop.
 *  - top   : retourne le sommet (NIL_VAL si stack vide).
 *  - depth : taille courante (debug / asserts).
 * Le widget peut etre NIL_VAL (par symetrie avec call_event_block qui
 * push NIL pour MaFenetre/MonChamp) — dans ce cas MoiMême resout via
 * les replis etat_stack/window_stack. */
void   vm_event_sender_push(Value widget);
void   vm_event_sender_pop(void);
Value  vm_event_sender_top(void);
int    vm_event_sender_depth(void);

/*  — resolution d'un proxy ObjUiControlRef (pose au compile par
 * ) vers son ObjUiControlInstance* dans la fenetre top-of-stack,
 * avec repli sur l'Etat top-of-stack ( lot D). Retourne NULL et
 * emet une soft-error E_RT_UI_FIELD_NO_WINDOW_OPEN ou
 * E_RT_UI_FIELD_NOT_IN_WINDOW si aucun contexte ne resout. Expose pour
 * les natives (stdlib_ui.c) qui doivent aussi accepter la forme proxy
 * en plus de la forme directe ObjUiControl. */
ObjUiControlInstance* resolve_ui_ref(ObjUiControlRef* ref);
void                 vm_etat_stack_clear(void);

/* DSL Etat V2 : enregistrement et lookup des declarations top-level.
 * register prend ownership du pointeur (libere par vm_free).
 * Doublon de nom : retourne false sans liberer. */
bool                 vm_etat_register(OwlEtatDecl* decl);
const OwlEtatDecl*   vm_etat_lookup(const char* name);

/* Miroir de vm_windows_clear pour le registre de declaration des Etats :
 * libere les decls et remet etatCount a 0 (sans toucher etatCapacity). */
void                 vm_etats_clear(void);

/* Pose le code/message d'erreur système qui sera lu via ExceptionInfo
 * (errCodeSystème / errMessageSystème) sur la prochaine levée. Mécanisme
 * opt-in : appelé par une native syscall AVANT la levée. Sans appel, les
 * deux valeurs restent à 0 / "" pour la levée à venir. */
void owl_set_exception_system_error(int sys_code, const char* sys_message);

/* Arme (ou desarme) la capture du contexte d'exception sur une erreur runtime
 * NON rattrapée. En temps normal une erreur sans handler QUAND EXCEPTION
 * n'alimente pas currentException/g_exc_* (elle part sur stderr puis unwind).
 * L'exécuteur de service arme ce drapeau autour de l'invocation d'un handler de
 * requête : si le handler plante sans try interne, le message/ligne/pile du
 * crash sont capturés et lisibles via ExceptionInfo() dans le hook surErreur.
 * À désarmer après usage (le drapeau ne doit pas survivre à la frontière). */
void vm_set_capture_uncaught(bool enabled);

/* Remet à zéro le contexte d'exception courant (currentException + tous les
 * g_exc_*). Utilisé par l'exécuteur de service pour éviter qu'un contexte
 * peuplé sur le crash d'une requête ne fuite (stale) vers la requête suivante,
 * après que le hook surErreur l'a consommé. */
void vm_exception_context_reset(void);

/* Lève une exception fatale rattrapable depuis une native stdlib : pose
 * l'origine (errPendant) ET le nom de la native (errFonction), capture le
 * contexte à partir du message rendu et arme le throw pending consommé par le
 * dispatch OP_CALL. `code_canonical` alimente errCode (symbole du registre
 * erreurs, ex "E_RT_ENUM_VALUE_NOT_FOUND") et permet errCodeNum (E0NNN) ;
 * NULL / "" => errCode vide. Sans handler QUAND EXCEPTION englobant, termine
 * en exception non rattrapée. */
void owl_vm_raise_native(const char* origin, const char* code_canonical,
                         const char* message);

/* Promeut une soft-error en exception en préservant le code canonique (chaîne)
 * et le nom de la native d'origine (errFonction). Utilisé par ErreurPropage
 * pour faire converger ExceptionInfo avec ErreurInfo. */
void vm_throw_from_erreur_coded(const char* msg, const char* code,
                                const char* native_name);

/* Stack operations (used by compiler and natives) */
void vm_push(Value value);
Value vm_pop(void);
Value vm_peek(int distance);

/* Ctrl-C / SIGINT handler entry point. Sets a flag the VM dispatch loop
 * checks at safe points (back-edges of loops, function entry). When set,
 * the current execution is aborted with a catchable "Interrupted"
 * runtime exception — the REPL resumes at the prompt, a kernel gets a
 * structured error. Safe to call from a signal/console handler: it only
 * writes an atomic flag, no allocation or printing. */
void vm_request_interrupt(void);

/* GIL VM — mutex VM global dedie, NON-RECURSIF (distinct de
 * l'executeur x->m et du flux r->fmx). Acquis au bord externe d'un handler
 * (execute_job), jamais par opcode ni par vm_call_reentrant. A N=1 (unique
 * thread executeur actuel) il n'est jamais contendu : plomberie inerte,
 * comportement identique. Non-recursif impose sur les deux OS (assertion de
 * propriete + ERRORCHECK POSIX) car CRITICAL_SECTION serait recursif. */
void owl_vm_gil_acquire(void);
void owl_vm_gil_release(void);

/* Structured-error mode: when enabled, runtime errors are emitted as a
 * single JSON line on stderr ({"kind":"runtime","message":"...",
 * "file":"...","line":N,"stack":[...]}) instead of the default
 * human-readable format. Consumed by notebook kernels / CI linters /
 * editor integrations. Set once at startup; not per-thread. */
void vm_set_json_errors(bool enabled);

/* Current source location (file/line) from top VM stack frame.
 * Used by owl_emit_runtime_error to populate error context. If no frame
 * active (startup / pre-run), outFile/outLine set to NULL/0. */
void vm_current_location(const char** outFile, int* outLine);

/* Transport interne : rejoue le format runtime_error() legacy a partir
 * d'un message deja rendu. Le helper owl_emit_runtime_error (registry
 * erreurs) forwarde ici apres avoir applique owl_error_render_fr_va.
 * Preserve : stack trace, capture exception handler (longjmp implicite),
 * mode JSON via g_json_error_mode, snippet source. */
void vm_emit_runtime_error_rendered(const char* message);

/*  lot 2 — GC « context-aware ». mark_roots (gc.c) itère le registre
 * de contextes d'exécution vivants : vm_exec_context_count() donne leur nombre
 * (1 à N=1, l'unique contexte adossé au singleton), vm_mark_context_roots(i)
 * marque les racines par-contexte du contexte #i (aujourd'hui : les snapshots
 * vivants de sa pile de sauvegarde du contexte d'exception, ). */
int  vm_exec_context_count(void);
void vm_mark_context_roots(int index);

/*  lot 4 — park/restore M2 de l'état d'exécution vif (arbitrage A1).
 * owl_vm_exec_park() copie l'état (pile, frames, contexte d'exception, piles
 * auxiliaires) dans un snapshot possédé (heap), l'enregistre au registre parké
 * (marqué par mark_roots depuis SA copie, jamais via frame->slots), et remet le
 * singleton à un baseline vide. owl_vm_exec_restore() recopie le snapshot dans
 * les MÊMES arrays physiques (pointeurs absolus réalignés) et le libère. Appelés
 * SOUS le GIL VM (http_exec.c) autour du point de blocage flux : park juste
 * avant de relâcher le GIL, restore juste après l'avoir réacquis. */
void* owl_vm_exec_park(void);
void  owl_vm_exec_restore(void* handle);

/*  — slot ambiant « flux courant » (pont transport -> VM pour la
 * native FluxÉcrit). Pointeur OPAQUE cote VM : c'est un OwlHttpProdCtx* du
 * transport, JAMAIS dereferences ici. Sauve/restaure par park/restore comme un
 * SCALAIRE (un void* n'est PAS une racine GC : aucun marquage). Sans ce
 * park/restore, un handler parke a son point de yield puis restaure
 * retrouverait le flux d'un AUTRE handler (croisement A/B). */
void  owl_vm_flux_courant_set(void* flux);
void* owl_vm_flux_courant_get(void);

/*  : clone une instance template enregistree dans vm.types par
 * son nom canon (ex "Cadre"). Retourne une instance fraiche deep-clonee ou
 * NIL_VAL si absente / non-instance. Sert au runtime UI pour materialiser
 * une prop composee OWL_PROP_TYPE_OBJECT en mode headless. */
Value owl_clone_global_template(const char* name);

/*  : clone profond d'une Value (instance + sous-elements imbriques,
 * tableaux, assoc, collections, json). Scalaires et chaines internees sont
 * partages tels quels. Sert au runtime UI pour donner a chaque widget SA
 * propre copie du Style affecte (isolation : surcharge par widget individuelle,
 * la source thème/style global reste intacte). GC-safe (roote pendant l'alloc). */
Value owl_value_deep_clone(Value v);

/*  — enregistre un template type (ObjInstance) dans vm.types.
 * Mecanique miroir de obj_globals_set mais ciblant la table dediee aux
 * types. Discipline GC : push name + val avant table_set (qui peut grow
 * et trigger un GC). Utilise par stdlib_builtin_types.c pour Chrono,
 * httpRequête, Cadre, Police, etc. */
void vm_type_register(const char* name, Value val);

/*  — arithmetique element-par-element / broadcast scalaire sur
 * Vecteur. `op` vaut '+', '-', '*' ou '/'. Au moins un operande est un
 * Vecteur (le caller a deja teste IS_VECTOR avant d'appeler). Produit un
 * NOUVEAU Vecteur dans *out ; les operandes ne sont jamais mutes. La
 * division par un element (ou scalaire) nul, une taille incompatible ou
 * un operande de type invalide sont signales par le statut de retour — le
 * caller les traduit en exception rattrapable selon son contexte (opcode
 * VM ou native fluent). Un operande scalaire entier est coerce en reel.
 * Broadcast : scalaire a gauche accepte uniquement pour '+' et '*'
 * (commutatifs) ; pour '-' et '/', seul le scalaire a droite est valide. */
typedef enum {
    VEC_ARITH_OK = 0,
    VEC_ARITH_DIV_ZERO,       /* division par un element/scalaire nul */
    VEC_ARITH_SIZE_MISMATCH,  /* deux Vecteur de tailles differentes */
    VEC_ARITH_ARRAY_OPERAND,  /* operande tableau : refuse (pas d'inference) */
    VEC_ARITH_TYPE,           /* operande non numerique / forme non supportee */
} VecArithStatus;
VecArithStatus vm_vector_arith(Value a, Value b, char op, Value* out);

/* Traduit un statut d'arithmetique Vecteur en code d'erreur (OwlErrorCode,
 * renvoye en int pour ne pas coupler ce header a owl_errors.h). Partage par
 * l'opcode VM et la native fluente pour un message unique par cas. */
int owl_vector_arith_error_code(VecArithStatus st);

/*  — pendant 2D du Vecteur : arithmetique element-par-element
 * (Hadamard pour '*', PAS le produit matriciel) ou broadcast scalaire sur
 * Matrice. `op` vaut '+', '-', '*' ou '/'. Au moins un operande est une
 * Matrice (le caller a deja teste IS_MATRIX). Produit une NOUVELLE Matrice
 * dans *out ; les operandes ne sont jamais mutes. Contrat identique au
 * Vecteur : division par un element/scalaire nul, dimensions incompatibles
 * (lignes OU colonnes) et operande de type invalide sont signales par le
 * statut ; scalaire a gauche accepte uniquement pour '+' et '*'. */
typedef enum {
    MAT_ARITH_OK = 0,
    MAT_ARITH_DIV_ZERO,       /* division par un element/scalaire nul */
    MAT_ARITH_SIZE_MISMATCH,  /* deux Matrice de dimensions differentes */
    MAT_ARITH_ARRAY_OPERAND,  /* operande tableau : refuse (pas d'inference) */
    MAT_ARITH_TYPE,           /* operande non numerique / forme non supportee */
} MatArithStatus;
MatArithStatus vm_matrix_arith(Value a, Value b, char op, Value* out);

/* Traduit un statut d'arithmetique Matrice en code d'erreur (cf. son
 * homologue Vecteur). */
int owl_matrix_arith_error_code(MatArithStatus st);

/* Canonical Value -> ObjString coercion. Matches canon: integer/real
 * render like `ChaîneVersNumérique` inverse, bool → "1"/"0", Null → "0",
 * Date/Heure/DateHeure → fixed-width numeric, Durée → normalized token,
 * strings pass through. Returned ObjString may be freshly allocated
 * (subject to GC) — don't cache raw pointers across VM operations. */
ObjString* value_to_string(Value value);

/*  — lit/ecrit la liste par defaut (`..Contenu`, chaine LF) d'une
 * COLONNE combo a partir de son proxy-colonne (`TABLE.COL`). Memes stockage et
 * effets de bord que la propriete `..Contenu` colonne. Retourne false si
 * `proxy` n'est pas une instance proxy-colonne exploitable. */
bool owl_vm_table_colproxy_content_get(Value proxy, Value* out);
bool owl_vm_table_colproxy_content_set(Value proxy, Value strVal);

#endif
