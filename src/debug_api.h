// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_debug_api_h
#define owl_debug_api_h

/* Debug API — D.1 du Lot D (Debugger DAP).
 *
 * Surface minimale et locale a la VM pour piloter pause/step/breakpoint.
 * NE depend pas du protocole DAP : un futur owl-dap.exe (ou autre frontend
 * comme un panel web) consommera cette API via IPC. Ici on expose juste les
 * primitives runtime.
 *
 * Etat global unique : `owl_debug_state` (pas de re-entrance multi-VM cible). Toutes
 * les fonctions sont thread-safe via flags atomiques pour les transitions
 * RUN <-> PAUSE depuis un thread debugger externe.
 */

#include <stdbool.h>
#include <stdint.h>

/* Modes de l'execution. Transition pilotee par le debugger externe. */
typedef enum {
    DEBUG_OFF       = 0, /* Pas de debug attache : zero overhead, hook no-op. */
    DEBUG_RUN       = 1, /* Debug attache, execution libre, check breakpoints. */
    DEBUG_PAUSED    = 2, /* VM en pause sur un point d'arret ou suspension. */
    DEBUG_STEP_OVER = 3, /* Avance jusqu'a la prochaine ligne dans la frame courante. */
    DEBUG_STEP_IN   = 4, /* Avance jusqu'a la prochaine ligne (entrer dans appels). */
    DEBUG_STEP_OUT  = 5, /* Avance jusqu'au retour de la frame courante. */
} OwlDebugMode;

/* ================================================================
 * Hot-path inline
 *
 * `owl_debug_mode()` est appele dans `PRE_OPCODE_HOOK()` (vm.c) avant
 * chaque opcode (~18M fois/sec sur fib_iter). Sans inline cross-unite
 * (LTO non actif), le call externe annule le gain computed-goto
 *. Pattern canon VM (CPython, V8) : exposer la struct
 * d'etat dans le header pour permettre l'inline.
 *
 * L'etat reste detenu par debug_api.c (lifecycle owl_debug_init/free) ;
 * seule la struct est declaree publique. Les autres champs sont
 * accedes uniquement depuis debug_api.c.
 * ================================================================ */

struct OwlBreakpoint;  /* fwd decl, defini dans debug_api.c */

typedef struct {
    /* Mode actif. Hot path : lu a chaque opcode via owl_debug_mode(). */
    volatile OwlDebugMode mode;
    volatile bool pauseRequested;
    struct OwlBreakpoint* bps;
    int bpCount;
    int bpCapacity;
    const char* lastFile;
    int lastLine;
    const char* pausedFile;
    int pausedLine;
    int stepDepth;
} OwlDebugState;

extern OwlDebugState owl_debug_state;

/* Mode courant. Inline pour hot path dispatch VM. */
static inline OwlDebugMode owl_debug_mode(void) {
    return owl_debug_state.mode;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Initialise l'etat debug. Mode = DEBUG_OFF par defaut (transparent). */
void owl_debug_init(void);

/* Libere les ressources (table breakpoints). Appele par vm_free. */
void owl_debug_free(void);

/* Active le mode debug. Apres attach, mode = DEBUG_RUN, les hooks check
 * breakpoints et step state a chaque changement de ligne source. */
void owl_debug_attach(void);

/* Detache : retour au mode DEBUG_OFF, breakpoints conserves mais ignores. */
void owl_debug_detach(void);

/* ================================================================
 * Breakpoints
 * ================================================================ */

/* Pose un breakpoint sur file:line. file peut etre NULL (single-file mode).
 * Retourne true si le breakpoint est nouveau, false s'il existait deja.
 * Le file/line ne sont pas valides ici : le breakpoint est juste enregistre.
 * Si la ligne n'a pas de bytecode emis (commentaire, blank), il ne sera
 * jamais hit — c'est au frontend DAP de remonter ce diag a l'utilisateur. */
bool owl_debug_set_breakpoint(const char* file, int line);

/* Supprime un breakpoint. Retourne true si trouve+supprime, false si absent. */
bool owl_debug_clear_breakpoint(const char* file, int line);

/* Vide tous les breakpoints (typiquement appele par DAP setBreakpoints
 * qui pousse la liste complete a chaque changement). */
void owl_debug_clear_all_breakpoints(void);

/* True si un breakpoint existe a file:line. Utilise par le hook dispatch. */
bool owl_debug_has_breakpoint(const char* file, int line);

/* ================================================================
 * Hook dispatch (appele par vm.c, ne doit jamais bloquer en mode OFF)
 * ================================================================ */

/* Appele par la dispatch loop avant chaque opcode. Si DEBUG_OFF, no-op
 * inline. Sinon, detecte le changement de ligne, check si breakpoint
 * match ou mode step actif, et bloque la VM si oui jusqu'a continue. */
void owl_debug_hook(const char* file, int line);

/* Callback appele par le hook JUSTE AVANT que la VM ne se mette en pause.
 * Permet au frontend (debug_stdio) d'emettre un event asynchrone qui sera
 * traduit en StoppedEvent DAP cote owl-dap.exe. Sans ca, le client ne sait
 * pas que la VM est en pause sans poller en boucle.
 *
 * `reason` est un de : "breakpoint", "step", "pause", "entry".
 *
 * Le callback s'execute dans le thread VM ; il doit etre non-bloquant et
 * thread-safe vis-a-vis des autres writes (typiquement : fputs sur stdout
 * serialise par mutex). */
typedef void (*OwlDebugPauseCallback)(const char* file, int line, const char* reason);
void owl_debug_set_pause_callback(OwlDebugPauseCallback cb);

/* ================================================================
 * Controle execution (appelees par le frontend DAP en D.2 ; pour
 * l'instant utilisable depuis n'importe quel thread/contexte)
 * ================================================================ */

/* Demande un pause asynchrone : la VM s'arretera au prochain changement
 * de ligne. Equivalent du bouton "Pause" dans VSCode. */
void owl_debug_request_pause(void);

/* Reprend l'execution apres un pause. Mode -> RUN.
 * No-op si la VM n'est pas en pause. */
void owl_debug_continue(void);

/* Step over : avance jusqu'a la prochaine ligne source dans la frame
 * courante (ou une frame parent si on en sort par return). N'entre pas
 * dans les fonctions appelees. */
void owl_debug_step_over(void);

/* Step into : avance d'une ligne source quelle que soit la frame.
 * Entre dans les fonctions appelees. */
void owl_debug_step_in(void);

/* Step out : avance jusqu'a sortir de la frame courante (ie. apres le
 * RENVOYER ou la fin de procedure). */
void owl_debug_step_out(void);

/* Position courante quand la VM est en pause. file peut etre NULL.
 * Lecture stable tant que la VM est en pause (pas de mutation
 * concurrente). Retourne false si la VM n'est pas en pause. */
bool owl_debug_paused_location(const char** outFile, int* outLine);

/* ================================================================
 * Inspection (D.1.4)
 *
 * Toutes ces fonctions ne sont valides que quand owl_debug_paused_location()
 * retourne true (VM en pause). Lecture stable tant que la VM ne reprend pas.
 * Le frontend (futur owl-dap.exe) appelle ces API pour repondre aux
 * messages DAP `stackTrace`, `scopes`, `variables`.
 *
 * Strings retournees pointent vers des buffers internes a la VM (chunks,
 * ObjStrings, etc.) — valides tant que la VM est en pause. Le frontend doit
 * les copier s'il veut les conserver apres `owl_debug_continue()`.
 * ================================================================ */

typedef struct {
    int         index;          /* 0 = frame courante (top), N-1 = main */
    const char* functionName;   /* "<main>" ou nom de la procedure */
    const char* file;           /* fichier source ou NULL */
    int         line;           /* ligne source de l'IP du frame */
} OwlDebugFrame;

typedef struct {
    /* Pour les locals : "slot N" tant qu'on n'a pas de localNames table.
     * Pour les globals : nom canonique de la variable. */
    const char* name;
    /* Valeur stringifiee canon (via value_to_string). Pointer vers un
     * buffer thread-local interne, valide jusqu'au prochain appel
     * d'inspection. Le frontend copie s'il veut conserver. */
    const char* value;
    /* Type lisible : "entier", "chaine", "tableau", "Date", ... */
    const char* type;
    /* Handle enfant si la value est un container (tableau/json/struct/...).
     * 0 si primitive. Le frontend utilise ce handle pour appeler
     * owl_debug_get_children() et obtenir les sous-elements. Le handle
     * reste valide tant que la VM est en pause ; flush au continue/step. */
    int childrenRef;
} OwlDebugVariable;

/* Remplit `out` avec les frames du call stack courant, du plus recent
 * (top, index 0) au plus ancien. `max` = capacite du tableau, `outCount`
 * recoit le nombre effectif (peut etre < max ou == max si tronque).
 * Retourne false si la VM n'est pas en pause. */
bool owl_debug_get_stack(OwlDebugFrame* out, int max, int* outCount);

/* Variables locales du frame `frameIdx` (0 = top). Slots argument inclus
 * en premier. Names = "slot N" tant qu'on n'a pas de table de noms. */
bool owl_debug_get_locals(int frameIdx, OwlDebugVariable* out, int max, int* outCount);

/* Variables globales (parcours de vm.globals). N peut etre grand : le
 * frontend pagine. */
bool owl_debug_get_globals(OwlDebugVariable* out, int max, int* outCount);

/* Evalue une expression OWL dans le contexte courant (frame paused).
 * L'expression peut etre n'importe quelle expression OWL valide qui
 * retourne une valeur : `x + 1`, `Maj(s)`, `tab[i]`, `req..URL`, ...
 *
 * Implementation : compile une fonction transitoire `RENVOYER (expr)`,
 * exec via la VM. Acces aux globals OK ; les locals nommes du frame
 * courant sont accessibles tant que le compiler les voit (chainage
 * via le scope du <main> de l'executable).
 *
 * out (au moins DEBUG_STR_LEN) recoit la value stringifiee.
 * outType (au moins DEBUG_STR_LEN) recoit le nom du type.
 * Retourne true si l'eval a reussi, false en cas d'erreur compile ou
 * runtime — out contient alors le message d'erreur. */
bool owl_debug_eval(const char* expr, char* out, char* outType);

/* Recupere les enfants d'un container reference par `childrenRef`. Pour :
 *  - tableau OWL : enfants nommes [1], [2], ...
 *  - tableau associatif : enfants nommes par leur cle
 *  - struct/instance : enfants nommes par champ
 *  - JSON object : enfants nommes par cle
 *  - JSON array : enfants nommes [0], [1], ...
 *  - chaine, entier, date, etc. : aucun enfant (childrenRef=0 d'avance)
 *
 * Retourne false si childrenRef est invalide (handle perime ou inconnu). */
bool owl_debug_get_children(int childrenRef, OwlDebugVariable* out, int max, int* outCount);

#endif
