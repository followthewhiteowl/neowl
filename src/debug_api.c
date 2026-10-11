// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Debug API — implementation.
 *
 * D.1.1+D.1.2 : etat + breakpoints + hook fast-path.
 * D.1.3        : hook actif — pause sur breakpoint / pause demandee, et
 *                wait jusqu'a continue. Implementation par busy-wait
 *                + sleep 10ms (assez fin pour un debugger humain, simple
 *                et portable). On migrera vers une condvar quand le
 *                thread DAP externe sera en place (D.2.3).
 */

#include "debug_api.h"
#include "vm.h"
#include "object.h"
#include "value.h"
#include "table.h"
#include "owl_registry.h"
#include "owl_globalvar_catalog.h"  /* filtrage des globals système prédéfinies */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward-declarations du compiler — on ne peut pas include "compiler.h"
 * direct car il pulle scanner.h qui definit `TokenType`, en conflit avec
 * winnt.h::TokenType (cf piege documente dans CLAUDE.md). On evite donc
 * la chaine d'include et on declare la surface minimale necessaire. */
typedef struct {
    int line;
    int column;
    char message[256];
} CompileDiagnostic;
extern ObjFunction* compile_as_function(const char* source);
extern void compile_set_error_capture(CompileDiagnostic* buffer, int capacity, int* count);

#ifdef _WIN32
#include <windows.h>
static void debug_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void debug_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/* ================================================================
 * Etat global
 * ================================================================ */

/* Breakpoint = (filename interne ou NULL, ligne). Stocke en tableau
 * dynamique simple : la profondeur typique est < 100 breakpoints, lookup
 * lineaire avec strcmp est negligeable face au cout de la dispatch loop.
 * Si on devient gros plus tard, on passera en hashmap (file -> set<int>). */
/* `volatile`, lifecycle, semantique de `mode`/`stepDepth` : la documentation
 * historique de ces champs vit maintenant a cote du `typedef OwlDebugState`
 * dans debug_api.h ( : struct exposee pour permettre l'inline du
 * hot path). On garde ici la definition du breakpoint (interne au .c). */
struct OwlBreakpoint {
    char* file;     /* heap-owned, NULL si single-file mode */
    int   line;
};
typedef struct OwlBreakpoint OwlBreakpoint;

OwlDebugState owl_debug_state;  /*  : expose au header pour inline */
static OwlDebugPauseCallback g_pause_cb = NULL;

/* Snapshot des globales SYSTEME, pris a l'attach (avant l'execution du
 * script). Tout ce qui est present dans vm.globals a ce moment est du
 * framework : natives, constantes, et surtout leurs alias EN/ASCII qui
 * vivent en runtime-only dans vm.globals sans entree registre (invariant
 * bilingue). Les lookups registre (is_system_symbol) ne resolvent que le
 * nom canonique et laissaient donc fuiter ~120 alias (White, languageFrench,
 * mimeTypePDF...) dans le panel Variables. Le snapshot capture toutes les
 * formes d'un coup. Les globales USER, creees pendant l'execution, ne sont
 * pas dedans -> elles restent visibles.
 * Les cles sont des ObjString* partagees avec vm.globals (root GC, jamais
 * retirees) : pointeurs valides pour toute la vie du process, pas de mark
 * separe necessaire. */
static Table g_system_globals;
static bool  g_system_globals_built = false;

/* Forward decl pour usage dans owl_debug_hook (la table elle-meme est
 * definie plus bas avec les helpers de drill-down). */
static void debug_handles_clear(void);

void owl_debug_set_pause_callback(OwlDebugPauseCallback cb) {
    g_pause_cb = cb;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

void owl_debug_init(void) {
    owl_debug_state.mode = DEBUG_OFF;
    owl_debug_state.pauseRequested = false;
    owl_debug_state.bps = NULL;
    owl_debug_state.bpCount = 0;
    owl_debug_state.bpCapacity = 0;
    owl_debug_state.lastFile = NULL;
    owl_debug_state.lastLine = -1;
    owl_debug_state.pausedFile = NULL;
    owl_debug_state.pausedLine = -1;
    owl_debug_state.stepDepth = 0;
}

void owl_debug_free(void) {
    for (int i = 0; i < owl_debug_state.bpCount; i++) {
        free(owl_debug_state.bps[i].file);
    }
    free(owl_debug_state.bps);
    owl_debug_state.bps = NULL;
    owl_debug_state.bpCount = 0;
    owl_debug_state.bpCapacity = 0;
    owl_debug_state.mode = DEBUG_OFF;
}

void owl_debug_attach(void) {
    owl_debug_state.mode = DEBUG_RUN;
    owl_debug_state.pauseRequested = false;
    owl_debug_state.lastFile = NULL;
    owl_debug_state.lastLine = -1;
    owl_debug_state.pausedFile = NULL;
    owl_debug_state.pausedLine = -1;

    /* Snapshot des globales systeme : on est appele avant l'execution du
     * script, donc vm.globals ne contient ici QUE le framework. */
    if (g_system_globals_built) table_free(&g_system_globals);
    table_init(&g_system_globals);
    for (int i = 0; i < vm.globals.capacity; i++) {
        Entry* e = &vm.globals.entries[i];
        if (e->key == NULL) continue;
        table_set(&g_system_globals, e->key, BOOL_VAL(true));
    }
    g_system_globals_built = true;
}

void owl_debug_detach(void) {
    /* Reveiller toute VM en pause avant de detacher : sinon la VM
     * resterait coincee dans la boucle d'attente. */
    owl_debug_state.mode = DEBUG_OFF;
    owl_debug_state.pauseRequested = false;
}

/* owl_debug_mode() : devenue static inline dans debug_api.h. */

/* ================================================================
 * Breakpoints
 * ================================================================ */

/* Compare deux paths en tolerant : backslash <-> slash, case-insensitive.
 * VSCode envoie "c:\...\file.owl" (backslash, minuscule) et la VM stocke
 * le path tel qu'il a ete passe par argv ("C:/.../file.owl" en general).
 * Sans normalisation, le strcmp echouerait et le breakpoint ne hit jamais.
 * Approche tolerante : on travaille sur Windows-friendly aussi sur POSIX
 * en debug (acceptable). Pour identite stricte, le frontend doit deja
 * resolver les paths absolus via realpath / GetFullPathName. */
static bool path_match(const char* a, const char* b) {
    if (a == NULL && b == NULL) return true;
    if (a == NULL || b == NULL) return false;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Compare deux specs file:line. file=NULL cote bp = wildcard "n'importe
 * quel fichier" (utile en V0 quand le frontend ne connait pas le path
 * exact stocke par la VM). file=NULL cote runtime = pas de fichier
 * source -> ne match que les bp wildcard. */
static bool bp_match(const OwlBreakpoint* bp, const char* file, int line) {
    if (bp->line != line) return false;
    if (bp->file == NULL) return true;       /* bp wildcard */
    if (file == NULL) return false;          /* bp specifique mais file inconnu */
    return path_match(bp->file, file);
}

static int bp_find(const char* file, int line) {
    for (int i = 0; i < owl_debug_state.bpCount; i++) {
        if (bp_match(&owl_debug_state.bps[i], file, line)) return i;
    }
    return -1;
}

bool owl_debug_set_breakpoint(const char* file, int line) {
    if (bp_find(file, line) >= 0) return false;

    if (owl_debug_state.bpCount >= owl_debug_state.bpCapacity) {
        int newCap = owl_debug_state.bpCapacity == 0 ? 8 : owl_debug_state.bpCapacity * 2;
        OwlBreakpoint* newBps = realloc(owl_debug_state.bps, newCap * sizeof(OwlBreakpoint));
        if (!newBps) return false;
        owl_debug_state.bps = newBps;
        owl_debug_state.bpCapacity = newCap;
    }
    OwlBreakpoint* bp = &owl_debug_state.bps[owl_debug_state.bpCount++];
    bp->file = file ? strdup(file) : NULL;
    bp->line = line;
    return true;
}

bool owl_debug_clear_breakpoint(const char* file, int line) {
    int idx = bp_find(file, line);
    if (idx < 0) return false;
    free(owl_debug_state.bps[idx].file);
    /* Compaction : remplace par le dernier element. Ordre non significatif. */
    owl_debug_state.bps[idx] = owl_debug_state.bps[--owl_debug_state.bpCount];
    return true;
}

void owl_debug_clear_all_breakpoints(void) {
    for (int i = 0; i < owl_debug_state.bpCount; i++) {
        free(owl_debug_state.bps[i].file);
    }
    owl_debug_state.bpCount = 0;
}

bool owl_debug_has_breakpoint(const char* file, int line) {
    return bp_find(file, line) >= 0;
}

/* ================================================================
 * Hook dispatch (D.1.3 actif)
 * ================================================================ */

/* Decide si la VM doit s'arreter au changement de ligne courant. */
static bool should_pause_here(const char* file, int line) {
    if (owl_debug_state.pauseRequested) return true;
    int depth = vm.frameCount;
    switch (owl_debug_state.mode) {
        case DEBUG_STEP_IN:
            return true;
        case DEBUG_STEP_OVER:
            /* Pause si on est dans la frame d'origine (depth ==) ou si
             * on en est sorti par return (depth <). Les calls plus
             * profonds (depth >) sont skippes silencieusement. */
            return depth <= owl_debug_state.stepDepth;
        case DEBUG_STEP_OUT:
            return depth < owl_debug_state.stepDepth;
        default:
            break;
    }
    if (owl_debug_has_breakpoint(file, line)) return true;
    return false;
}

void owl_debug_hook(const char* file, int line) {
    /* Fast path : debug detache, return immediat. */
    if (owl_debug_state.mode == DEBUG_OFF) return;

    /* Optim : ne reagir qu'au CHANGEMENT de ligne. */
    bool sameFile = (owl_debug_state.lastFile == file) ||
                    (owl_debug_state.lastFile && file && strcmp(owl_debug_state.lastFile, file) == 0);
    if (sameFile && owl_debug_state.lastLine == line) return;

    owl_debug_state.lastFile = file;
    owl_debug_state.lastLine = line;

    if (!should_pause_here(file, line)) return;

    /* Determine la raison pour le frontend (DAP StoppedEvent) avant de
     * basculer en PAUSED — la lecture du mode courant (STEP_OVER, etc.)
     * doit se faire ici, avant qu'on bascule sur DEBUG_PAUSED. */
    const char* reason = "pause";
    if (owl_debug_state.mode == DEBUG_STEP_OVER ||
        owl_debug_state.mode == DEBUG_STEP_IN ||
        owl_debug_state.mode == DEBUG_STEP_OUT) {
        reason = "step";
    } else if (owl_debug_has_breakpoint(file, line)) {
        reason = "breakpoint";
    }

    /* Pause : enregistre la position et boucle jusqu'a ce que le frontend
     * appelle owl_debug_continue() ou owl_debug_detach(). La VM est
     * littéralement bloquee ici — tout le programme est figé, ce qui est
     * exactement le contrat d'un debugger sync. */
    owl_debug_state.mode = DEBUG_PAUSED;
    owl_debug_state.pausedFile = file;
    owl_debug_state.pausedLine = line;
    owl_debug_state.pauseRequested = false;
    /* Reset les handles drill-down : les Value* du tour precedent ne sont
     * plus valides (la VM a continue / GC potentiel). Le frontend va
     * re-demander get_locals/get_globals qui re-alloueront. */
    debug_handles_clear();

    /* Notifie le frontend AVANT de bloquer : il doit pouvoir envoyer
     * StoppedEvent + repondre aux requetes get_stack/locals pendant
     * qu'on attend ici. */
    if (g_pause_cb) g_pause_cb(file, line, reason);

    while (owl_debug_state.mode == DEBUG_PAUSED) {
        debug_sleep_ms(10);
    }

    /* Sortie de pause : continue/step/detach a deja modifie le mode. */
    owl_debug_state.pausedFile = NULL;
    owl_debug_state.pausedLine = -1;
}

/* ================================================================
 * Controle execution
 * ================================================================ */

void owl_debug_request_pause(void) {
    if (owl_debug_state.mode == DEBUG_OFF) return;
    owl_debug_state.pauseRequested = true;
}

void owl_debug_continue(void) {
    if (owl_debug_state.mode == DEBUG_PAUSED) {
        owl_debug_state.mode = DEBUG_RUN;
    }
}

/* Helper commun aux 3 step : capture la profondeur courante et bascule
 * dans le mode demande. La VM (en boucle d'attente sur DEBUG_PAUSED)
 * detecte le changement de mode et reprend. */
static void start_step(OwlDebugMode m) {
    if (owl_debug_state.mode != DEBUG_PAUSED) return;
    owl_debug_state.stepDepth = vm.frameCount;
    owl_debug_state.mode = m;
}

void owl_debug_step_over(void) { start_step(DEBUG_STEP_OVER); }
void owl_debug_step_in (void)  { start_step(DEBUG_STEP_IN); }
void owl_debug_step_out(void)  { start_step(DEBUG_STEP_OUT); }

bool owl_debug_paused_location(const char** outFile, int* outLine) {
    if (owl_debug_state.mode != DEBUG_PAUSED) return false;
    if (outFile) *outFile = owl_debug_state.pausedFile;
    if (outLine) *outLine = owl_debug_state.pausedLine;
    return true;
}

/* ================================================================
 * Inspection (D.1.4)
 * ================================================================ */

/* Buffers thread-local pour les strings de noms/valeurs/types des
 * OwlDebugVariable. La VM est mono-thread, on alloue assez gros pour le
 * pire cas raisonnable (256 vars × 64 bytes par champ). Le frontend doit
 * copier les strings s'il veut les conserver apres un autre appel
 * d'inspection — c'est documente dans le header. */
#define DEBUG_VAR_MAX 256
#define DEBUG_STR_LEN 64
static char g_var_names [DEBUG_VAR_MAX][DEBUG_STR_LEN];
static char g_var_values[DEBUG_VAR_MAX][DEBUG_STR_LEN];
static char g_var_types [DEBUG_VAR_MAX][DEBUG_STR_LEN];

/* ================================================================
 * Table de handles pour drill-down (D.4.1)
 *
 * Chaque container expose au debugger recoit un handle (entier > 0).
 * Le debugger peut ensuite appeler owl_debug_get_children(handle) pour
 * obtenir les sous-elements. Le handle est invalide au continue/step
 * (les Values pourraient bouger via GC).
 *
 * Implementation simple : tableau dynamique d'indexes 1-based ; le
 * handle 0 est reserve "pas de children". On stocke directement les
 * Value (copie) — pour les Obj heap, c'est le pointer Obj* qui peut
 * etre redirige post-GC, mais comme la VM est paused, pas de GC ici.
 */
#define DEBUG_HANDLE_MAX 1024
static Value g_handles[DEBUG_HANDLE_MAX];
static int   g_handle_count = 0;   /* nombre d'handles alloues (0..MAX) */

static void debug_handles_clear(void) {
    g_handle_count = 0;
}

/* Alloue un handle pour `v`. Retourne l'index 1-based, ou 0 si plein
 * (limite atteinte) ou si la value n'est pas un container connu. */
static bool is_container(Value v) {
    return IS_ARRAY(v) || IS_ASSOC_ARRAY(v) || IS_INSTANCE(v) || IS_JSON(v);
}

static int debug_alloc_handle(Value v) {
    if (!is_container(v)) return 0;
    if (g_handle_count >= DEBUG_HANDLE_MAX) return 0;
    g_handles[g_handle_count] = v;
    return ++g_handle_count;  /* 1-based */
}

/* Retourne un nom de type lisible canon pour une Value. */
static const char* type_name_for(Value v) {
    /* NIL : en canon c'est rare (toutes les declarations primitives
     * sont initialisees a 0/""/Faux). Quand on voit NIL en debug, c'est
     * souvent une chaine non encore set qui s'affiche "" cote runtime.
     * Pour matcher l'attente user, on affiche "chaine" (paire avec ""
     * dans value_repr) plutot que le technique "Null"/"(vide)". */
    if (IS_NIL(v))       return "chaine";
    if (IS_INTEGER(v))   return "entier";
    if (IS_REAL(v))      return "reel";
    if (IS_BOOL(v))      return "booleen";
    if (IS_STRING(v))    return "chaine";
    if (IS_DATE(v))      return "Date";
    if (IS_TIME(v))      return "Heure";
    if (IS_DATETIME(v))  return "DateHeure";
    if (IS_DURATION(v))  return "Duree";
    if (IS_ARRAY(v))     return "tableau";
    if (IS_ASSOC_ARRAY(v)) return "tableau associatif";
    if (IS_JSON(v))      return "JSON";
    if (IS_FUNCTION(v) || IS_NATIVE(v)) return "Procedure";
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName && inst->structName->chars) return inst->structName->chars;
        return "Structure";
    }
    return "?";
}

/* Stringifie via value_to_string + tronque a DEBUG_STR_LEN-1.
 * Buffer fourni par l'appelant (cell d'un tableau g_var_values).
 * Pour les containers, on affiche un summary plus utile que <object>. */
/* Borne haute de la zone sentinelle OWL. Les sentinels sont encodes en
 * INT32_MIN + (famille * 0x100) + slot (cf src/sentinels.h : 256 slots par
 * famille, base 0x100 entre familles). On couvre 256 familles soit
 * [INT32_MIN, INT32_MIN + 0x10000]. L'ancienne bande s'arretait a
 * INT32_MIN+13648 et ratait les familles hautes (ex la valeur -2147469824
 * observee = famille 54, hors ancienne bande) — d'ou le hover qui affichait
 * une valeur sentinelle brute au lieu d'etre detecte. */
#define OWL_SENTINEL_ZONE_HI  (INT64_C(-2147483648) + INT64_C(0x10000))

/* Reverse lookup : pour un INTEGER dans la plage sentinel canon (valeur
 * d'enum ou constante magique encodee), scanne le registry des
 * constantes et retourne le label si trouve. NULL sinon.
 *
 * Cas typique : Methode = -2147481343 (sentinel httpGet). On veut
 * afficher "httpGet" pour le user, pas la valeur brute. */
static const char* sentinel_to_label(Value v) {
    if (!IS_INTEGER(v)) return NULL;
    int64_t target = AS_INTEGER(v);
    /* Plage des sentinels (cf. is_owl_sentinel_value). */
    if (target < INT64_C(-2147483648) || target > OWL_SENTINEL_ZONE_HI) return NULL;
    int n = owl_registry_consts_count();
    for (int i = 0; i < n; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c) continue;
        if (!IS_INTEGER(c->value)) continue;
        if (AS_INTEGER(c->value) == target) return c->name;
    }
    return NULL;
}

/* Lookup helper : retourne l'OwlField* d'un type builtin pour un nom donné,
 * ou NULL. Le matching est case-insensitive ASCII (les fields canon n'ont
 * pas d'accents en OWL — les sous-champs accentués sont gérés via name vs
 * nameAccented). */
static const OwlField* lookup_field(const char* structName, const char* fieldName) {
    if (!structName || !fieldName) return NULL;
    const OwlTypeDef* td = owl_registry_type_find(structName);
    if (!td || !td->fields) return NULL;
    for (const OwlField* f = td->fields; f->name; f++) {
        if (strcasecmp(f->name, fieldName) == 0) return f;
        if (f->nameAccented && strcasecmp(f->nameAccented, fieldName) == 0) return f;
    }
    return NULL;
}

/* value_repr étoffé : accepte un OwlField* optionnel pour résoudre les
 * entiers contre un enum bound (allowedBase). Quand IgnoreErreur=0 et
 * le field est typé httpIgnoreErreur, on affiche le label canon de
 * la valeur 0 dans cet enum (ex: "httpIgnoreAucune"). */
static void value_repr_with_field(Value v, char* buf, const OwlField* fctx);

static void value_repr(Value v, char* buf) {
    value_repr_with_field(v, buf, NULL);
}

static void value_repr_with_field(Value v, char* buf, const OwlField* fctx) {
    /* Containers : summary lisible plutot que <object>. */
    if (IS_ARRAY(v)) {
        snprintf(buf, DEBUG_STR_LEN, "tableau (%d elements)", AS_ARRAY(v)->elements.count);
        return;
    }
    if (IS_ASSOC_ARRAY(v)) {
        snprintf(buf, DEBUG_STR_LEN, "tableau associatif (%d entrees)", AS_ASSOC_ARRAY(v)->count);
        return;
    }
    if (IS_JSON(v)) {
        ObjJson* j = (ObjJson*)AS_OBJ(v);
        switch (j->jtype) {
            case JSON_T_OBJECT:
                snprintf(buf, DEBUG_STR_LEN, "JSON {%d champs}", j->as.objectVal.count);
                return;
            case JSON_T_ARRAY:
                snprintf(buf, DEBUG_STR_LEN, "JSON [%d]", j->as.arrayVal.count);
                return;
            default:
                break;  /* fall-through au value_to_string */
        }
    }
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        const char* sn = (inst->structName && inst->structName->chars)
                       ? inst->structName->chars : "Structure";
        /* Pour les instances user (struct declarees), fieldOrder donne le
         * compte canon. Pour les builtin types (httpRequête, Cadre, ...)
         * fieldOrder est NULL — on rapporte le nb de fields actuellement
         * stockes dans la table (ceux qui ont ete set explicitement). */
        int nFields = inst->fieldOrder
            ? inst->fieldOrder->elements.count
            : inst->fields.count;
        snprintf(buf, DEBUG_STR_LEN, "%s (%d champs)", sn, nFields);
        return;
    }
    /* NIL en debug : la VM stocke NIL pour une chaine non-initialisee
     * (`s est une chaîne` sans valeur). En canon, le user attend "".
     * Pour distinguer un vrai NIL (apparu rarement, ex: JSON.Lit qui
     * retourne nil sur cle absente) du sentinel "non encore set", on
     * affiche "" — cohorent avec ce que voit Trace, Taille(), etc.
     * Si on veut voir le vrai NIL, on regardera le type qui est
     * (vide) dans ces cas (cf type_name_for). */
    if (IS_NIL(v)) {
        snprintf(buf, DEBUG_STR_LEN, "%s", "\"\"");
        return;
    }
    /* Sentinel d'enum/constante : reverse lookup pour afficher le
     * label canonique (ex: httpGet au lieu de -2147481343). */
    {
        const char* label = sentinel_to_label(v);
        if (label) {
            snprintf(buf, DEBUG_STR_LEN, "%s", label);
            return;
        }
    }
    /* Field-aware enum resolution : si le field a un allowedBase ou
     * allowedOptions, résout l'entier en label canon (match exact pour
     * enum non-combinable, décomposition bitmask pour combinable).
     *   `Méthode = httpGet`
     *   `IgnoreErreur = httpIgnoreCertificatInvalide + httpIgnoreRedirection`
     *   `VersionSSL = httpTLS_v1_2`
     */
    if (fctx && IS_INTEGER(v)) {
        int64_t intVal = AS_INTEGER(v);
        const OwlEnum* enums[2] = { fctx->allowedBase, fctx->allowedOptions };
        for (int ei = 0; ei < 2; ei++) {
            const OwlEnum* en = enums[ei];
            if (!en) continue;
            if (en->combinable) {
                if (owl_enum_decompose_bitmask(en, intVal, buf, DEBUG_STR_LEN)) {
                    return;
                }
            } else {
                const char* label = owl_enum_label_for_int(en, intVal);
                if (label) {
                    snprintf(buf, DEBUG_STR_LEN, "%s", label);
                    return;
                }
            }
        }
    }
    /* Chaine vide : value_to_string retourne "" — affichage trompeur
     * indistinguable du sentinel "valeur absente". On marque explicite. */
    if (IS_STRING(v) && AS_STRING(v)->length == 0) {
        snprintf(buf, DEBUG_STR_LEN, "%s", "\"\"");
        return;
    }
    /* Primitives : passage par le pretty printer canon. */
    ObjString* s = value_to_string(v);
    if (s && s->chars) {
        /* On wrap les chaines avec des guillemets pour les distinguer
         * d'un nombre ou booleen. Lisible en debug. */
        if (IS_STRING(v)) {
            int avail = DEBUG_STR_LEN - 3;  /* 2 guillemets + null */
            int len = (int)s->length < avail ? (int)s->length : avail;
            buf[0] = '"';
            memcpy(buf + 1, s->chars, len);
            buf[1 + len] = '"';
            buf[2 + len] = '\0';
            return;
        }
        size_t n = s->length < DEBUG_STR_LEN - 1 ? s->length : DEBUG_STR_LEN - 1;
        memcpy(buf, s->chars, n);
        buf[n] = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* Helper : remplit une OwlDebugVariable dans les buffers thread-local
 * a l'index `idx`, alloue un handle si la value est un container.
 * fctx (optionnel) = OwlField pour résolution enum (allowedBase/Options). */
static void fill_var_field(int idx, const char* name, Value v, OwlDebugVariable* out,
                            const OwlField* fctx) {
    size_t nameLen = strlen(name);
    if (nameLen >= DEBUG_STR_LEN) nameLen = DEBUG_STR_LEN - 1;
    memcpy(g_var_names[idx], name, nameLen);
    g_var_names[idx][nameLen] = '\0';
    value_repr_with_field(v, g_var_values[idx], fctx);
    snprintf(g_var_types[idx], DEBUG_STR_LEN, "%s", type_name_for(v));
    out[idx].name  = g_var_names[idx];
    out[idx].value = g_var_values[idx];
    out[idx].type  = g_var_types[idx];
    out[idx].childrenRef = debug_alloc_handle(v);
}

static void fill_var(int idx, const char* name, Value v, OwlDebugVariable* out) {
    fill_var_field(idx, name, v, out, NULL);
}

bool owl_debug_get_stack(OwlDebugFrame* out, int max, int* outCount) {
    if (owl_debug_state.mode != DEBUG_PAUSED) {
        if (outCount) *outCount = 0;
        return false;
    }
    int n = vm.frameCount < max ? vm.frameCount : max;
    /* Du plus recent (top, index 0) au plus ancien (main). vm.frames[]
     * est stocke dans l'ordre inverse : frames[frameCount-1] = top. */
    for (int i = 0; i < n; i++) {
        CallFrame* f = &vm.frames[vm.frameCount - 1 - i];
        out[i].index = i;
        out[i].functionName = (f->function && f->function->name)
            ? f->function->name->chars : "<main>";
        out[i].file = (f->function && f->function->filename)
            ? f->function->filename->chars : NULL;
        int offset = (int)(f->ip - f->function->chunk->code);
        out[i].line = (offset >= 0 && offset < f->function->chunk->count)
            ? f->function->chunk->lines[offset] : 0;
    }
    if (outCount) *outCount = n;
    return true;
}

bool owl_debug_get_locals(int frameIdx, OwlDebugVariable* out, int max, int* outCount) {
    if (owl_debug_state.mode != DEBUG_PAUSED) {
        if (outCount) *outCount = 0;
        return false;
    }
    if (frameIdx < 0 || frameIdx >= vm.frameCount) {
        if (outCount) *outCount = 0;
        return false;
    }
    CallFrame* f = &vm.frames[vm.frameCount - 1 - frameIdx];
    /* Slots = frame->slots .. (frame+1)->slots ou stackTop pour le top. */
    Value* slotsEnd = (frameIdx == 0)
        ? vm.stackTop
        : vm.frames[vm.frameCount - frameIdx].slots;
    int total = (int)(slotsEnd - f->slots);
    if (total < 0) total = 0;
    int n = total < max ? total : max;
    if (n > DEBUG_VAR_MAX) n = DEBUG_VAR_MAX;

    /* Filtre : skip slot 0 quand c'est juste la fonction elle-meme (toujours
     * presente comme implicit "this" dans le calling convention OWL). On
     * n'a pas de noms locaux V0, donc afficher "slot 0 = <Procedure>" est
     * juste du bruit. Skip aussi les slots Procedure suivants (closures,
     * helpers internes du compiler). */
    int outIdx = 0;
    char tmpName[DEBUG_STR_LEN];
    for (int i = 0; i < n; i++) {
        Value v = f->slots[i];
        if (IS_FUNCTION(v) || IS_NATIVE(v)) continue;
        /*  — nom reel de la locale/parametre si la table de debug
         * info de la fonction le connait ; sinon fallback "slot N" (slots
         * temporaires/internes sans nom declare). */
        /*  — IP du point de pause dans le chunk de la frame : filtre
         * les noms de locales selon leur plage de validite, sinon un slot
         * recycle entre deux scopes sequentiels afficherait le nom du scope
         * deja sorti (first-match perime). */
        int frameIp = (f->function && f->function->chunk && f->ip)
            ? (int)(f->ip - f->function->chunk->code)
            : -1;
        const char* realName = (f->function)
            ? obj_function_local_name_at(f->function, i, frameIp) : NULL;
        if (realName) {
            fill_var(outIdx, realName, v, out);
        } else {
            snprintf(tmpName, DEBUG_STR_LEN, "slot %d", i);
            fill_var(outIdx, tmpName, v, out);
        }
        outIdx++;
    }
    if (outCount) *outCount = outIdx;
    return true;
}

/* Filtre les globals affichees au debugger : on ne veut que les variables
 * USER (definies dans le code .owl), pas les ~1500 natives + constantes
 * du registre stdlib qui sont stockees dans vm.globals pour le dispatch.
 */
static bool is_value_in_registry_enum(const char* name) {
    /* Scan tous les enums du registry et leurs values. Une valeur d'enum
     * (owlEntier, errLine, familleChamp, etc.) est un symbole systeme, pas
     * une variable user. */
    int n = owl_registry_enums_count();
    for (int i = 0; i < n; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->values) continue;
        for (int j = 0; e->values[j] != NULL; j++) {
            if (strcmp(e->values[j], name) == 0) return true;
        }
    }
    return false;
}

/* Whitelist de globals systeme non-registry qui restent dans vm.globals :
 * sentinels d'iteration, helpers de stdlib, constants magiques. Un grep
 * dans le code OWL les trouve : ErreurDetectee/etc, FirstElement/etc,
 * champDebut/Fin, LastElement, etc.
 *
 * Plutot que d'enumerer une liste qui evolue, on detecte par patterns :
 *   - sentinels : INTEGER avec valeur dans la zone des reserves OWL
 *   - templates de struct : OBJ_INSTANCE detenu en global, pas une vraie
 *     donnee user (Trait, Coin, Style, Cadre, Police, Theme, Image, ...)
 */
static bool is_owl_sentinel_value(Value v) {
    if (!IS_INTEGER(v)) return false;
    int64_t i = AS_INTEGER(v);
    /* Borne basse = INT32_MIN (aucun int32 en dessous). Un user qui passerait
     * un entier de cette zone reservee pour une vraie donnee est extremement
     * rare (65536 valeurs sur les ~4 milliards de l'espace int32). */
    return i >= INT64_C(-2147483648) && i <= OWL_SENTINEL_ZONE_HI;
}

static bool is_system_symbol(const char* name, Value v) {
    if (!name || !name[0]) return true;

    /* Helpers internes : __XxxNew, __XxxCoerce, __wl$Xxx, etc. */
    if (name[0] == '_' && name[1] == '_') return true;

    /* Native function ou constante du registry. */
    if (owl_registry_find(name) != NULL) return true;
    if (owl_registry_const_find(name) != NULL) return true;

    /* Valeur d'enum (wlXxx, errXxx, familleXxx, filtreXxx, etc.). */
    if (is_value_in_registry_enum(name)) return true;

    /* Templates de structures builtin (Chrono, Cadre, Style, ...). */
    if (owl_registry_type_find(name) != NULL) return true;

    /* Sentinel OWL (INTEGER dans la plage reservee). */
    if (is_owl_sentinel_value(v)) return true;

    /* Variables globales prédéfinies du langage (ErreurDétectée, …) —
     * source de vérité unique : owl_globalvar_catalog. */
    if (owl_globalvar_catalog_is_global_name(name)) return true;

    /* Procedures = natives encore non-registres ou helpers. Si l'user
     * declare une PROCEDURE Foo, elle apparait aussi mais le panel
     * Variables n'est pas l'endroit pour les fonctions. */
    if (IS_FUNCTION(v) || IS_NATIVE(v)) return true;

    /* Proxies UI : quand une fenetre est ouverte, son ref global (ObjWindowRef
     * `FEN_X`) et les refs de ses controles (ObjUiControlRef `BTN_X`) sont
     * injectes dans vm.globals (apres le snapshot d'attach -> non filtres par
     * lui). Ce sont des objets de resolution dynamique sans valeur de debug
     * (affiches "<object>" / "" type "?"). On les masque du panel Variables.
     * Les vraies variables de la fenetre (ex. un compteur) restent visibles :
     * ce sont des donnees, pas des proxies. */
    if (IS_WINDOW_REF(v) || IS_UI_CONTROL_REF(v)) return true;

    /* Instances de struct : differencier templates (la global key ==
     * structName, ex global "Trait" est une instance vide dont struct
     * Name="Trait" — c'est le TEMPLATE de type, pas une donnee user)
     * des instances user (global "req" avec structName="httpRequête",
     * global "monObj" avec structName="Personne", ...).
     *
     * Cette logique evite de skipper les vraies vars user dont le type
     * est un template builtin, ce qui etait le bug avec
     * `req est une httpRequête` -> req etait masque. */
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName && inst->structName->chars
            && strcmp(name, inst->structName->chars) == 0) {
            return true;  /* template (key == structName) */
        }
        return false;     /* instance user */
    }

    return false;
}

bool owl_debug_get_globals(OwlDebugVariable* out, int max, int* outCount) {
    if (owl_debug_state.mode != DEBUG_PAUSED) {
        if (outCount) *outCount = 0;
        return false;
    }
    int n = 0;
    int cap = max < DEBUG_VAR_MAX ? max : DEBUG_VAR_MAX;
    /* Itere la table des globals. table.entries est un open-addressing
     * array, capacity peut etre 0 si vm.globals jamais init (impossible
     * en pratique : stdlib_init() y stocke des constantes). */
    for (int i = 0; i < vm.globals.capacity && n < cap; i++) {
        Entry* e = &vm.globals.entries[i];
        if (e->key == NULL) continue;
        const char* keyStr = e->key->chars ? e->key->chars : "?";
        /* Snapshot systeme : capture toutes les formes (canon + alias EN/ASCII)
         * presentes a l'attach. is_system_symbol reste en complement (helpers
         * __, structurel) pour ce qui apparaitrait apres l'attach. */
        Value tmp;
        if (g_system_globals_built && table_get(&g_system_globals, e->key, &tmp)) continue;
        if (is_system_symbol(keyStr, e->value)) continue;
        /*  — globale d'element UI : la cle interne est mangled
         * `<ELEM>$<var>` (scope element, ). Le panel Variables affiche
         * le nom NU (apres le `$`) : l'utilisateur voit `compteur`, pas la
         * mecanique de scoping interne. Les procedures cross-file `Coll$Proc`
         * sont deja filtrees par is_system_symbol (IS_FUNCTION). */
        const char* displayName = keyStr;
        const char* dollar = strchr(keyStr, '$');
        if (dollar != NULL && dollar[1] != '\0') displayName = dollar + 1;
        fill_var(n, displayName, e->value, out);
        n++;
    }
    if (outCount) *outCount = n;
    return true;
}

/* ================================================================
 * Eval (D.4.2) — compile + execute une expression dans le contexte
 * paused. Acces aux globals via vm.globals partagee.
 *
 * Architecture : on compile la string `RENVOYER (expr)` via
 * compile_as_function, on appelle la fonction via vm_call_reentrant
 * (qui execute reentrant dans la VM). Le resultat est sur la stack
 * apres le call.
 *
 * Sandbox : on capture les erreurs compile via compile_set_error_capture
 * pour ne pas que les `[file:line] Error at ...` se retrouvent dans
 * le DAP stdout (qui corromprait le protocole). On capture aussi
 * stderr du runtime via vm_set_json_errors temporaire.
 *
 * Limites V0 :
 *   - locals nommes du frame courant non accessibles (le compiler ne
 *     voit que les globals au top-level)
 *   - pas de prevention des side effects : si l'expr ecrit un global,
 *     l'etat VM est modifie. Documente, le user assume.
 * ================================================================ */
/*  — true si `expr` est un identifiant nu (lettres ASCII/accents
 * UTF-8, chiffres, underscore — aucun operateur/point/parenthese/espace). */
static bool is_bare_identifier(const char* expr) {
    if (!expr || !expr[0]) return false;
    for (const char* p = expr; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 0x80) || (c == '_') ||
                  (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

bool owl_debug_eval(const char* expr, char* out, char* outType) {
    if (owl_debug_state.mode != DEBUG_PAUSED || !expr || !expr[0]) {
        snprintf(out, DEBUG_STR_LEN, "%s", "(non disponible)");
        snprintf(outType, DEBUG_STR_LEN, "%s", "?");
        return false;
    }

    /*  (Voie B) — resolution par nom dans la frame courante AVANT
     * tout compile isole. Si l'expression est un identifiant nu et qu'il
     * correspond a une locale/parametre de la fonction au point de pause, on
     * lit directement frame->slots[slot]. Priorite LOCALE STRICTE : un nom
     * present dans la frame ne doit jamais retomber sur une constante/globale
     * registry homonyme (c'est le bug sTexte d'origine). Un nom ABSENT de la
     * frame n'est PAS resolu ici — il poursuit vers le compile isole, qui
     * echouera honnetement s'il n'est pas non plus une vraie globale. */
    if (is_bare_identifier(expr) && vm.frameCount > 0) {
        CallFrame* f = &vm.frames[vm.frameCount - 1];
        if (f->function) {
            /*  — filtre la resolution par nom sur l'IP de pause : un
             * nom dont le scope est sorti (slot recycle) ne resout plus sur la
             * valeur du scope courant. */
            int frameIp = (f->function->chunk && f->ip)
                ? (int)(f->ip - f->function->chunk->code)
                : -1;
            int slot = obj_function_resolve_local(f->function, expr,
                                                  (int)strlen(expr), frameIp);
            if (slot >= 0) {
                Value v = f->slots[slot];
                value_repr(v, out);
                snprintf(outType, DEBUG_STR_LEN, "%s", type_name_for(v));
                return true;
            }
        }
    }

    /* Construit le source de la fonction transitoire. PAS de parens
     * englobantes : `RENVOYER (a, b)` declenche le multi-return de canon
     * (OP_ARRAY_PACK), nous voulons juste un seul retour. */
    char src[1024];
    int written = snprintf(src, sizeof(src), "RENVOYER %s\n", expr);
    if (written <= 0 || written >= (int)sizeof(src)) {
        snprintf(out, DEBUG_STR_LEN, "%s", "(expression trop longue)");
        snprintf(outType, DEBUG_STR_LEN, "%s", "?");
        return false;
    }

    /* Capture des erreurs compile pour ne pas polluer stderr. */
    CompileDiagnostic errs[4];
    int errCount = 0;
    compile_set_error_capture(errs, 4, &errCount);
    ObjFunction* fn = compile_as_function(src);
    compile_set_error_capture(NULL, 0, NULL);  /* restore */
    if (!fn || errCount > 0) {
        const char* msg = (errCount > 0 && errs[0].message[0])
            ? errs[0].message : "compile error";
        snprintf(out, DEBUG_STR_LEN, "(erreur: %.40s)", msg);
        snprintf(outType, DEBUG_STR_LEN, "%s", "?");
        return false;
    }

    /* Suspend temporairement le mode debug pour que le hook ne
     * re-intercepte pas pendant l'eval (sinon recursion infinie). */
    OwlDebugMode savedMode = owl_debug_state.mode;
    owl_debug_state.mode = DEBUG_OFF;

    /* Push fn + call. La signature vm_call_reentrant prend la callee
     * deja sur la stack avec ses args sous (ici 0 args). */
    vm_push(OBJ_VAL((Obj*)fn));
    InterpretResult r = vm_call_reentrant(OBJ_VAL((Obj*)fn), 0);
    owl_debug_state.mode = savedMode;

    if (r != INTERPRET_OK) {
        snprintf(out, DEBUG_STR_LEN, "%s", "(erreur runtime)");
        snprintf(outType, DEBUG_STR_LEN, "%s", "?");
        /* Cleanup : un ou deux items peuvent rester sur la stack si exception. */
        return false;
    }

    /* Le resultat est sur le top de la stack apres le call reentrant. */
    Value result = vm_pop();

    /*  — anti-mensonge sur identifiant local. Le compiler d'eval ne
     * voit que les globals au top-level (cf. "Limites V0" plus haut) : un
     * identifiant nu qui est en realite un parametre ou une locale du frame
     * courant est resolu, par casse-insensibilite canon, comme la CONSTANTE
     * registry homonyme (ex `sTexte` -> constante `STexte`). On obtient alors
     * sa valeur sentinelle (-2147469824 & co), affichee telle quelle au hover,
     * ce qui fait croire a un bug runtime (cf. session de debug perdue qui a
     * motive ce ticket).
     *
     * Voie A : quand l'expression est un IDENTIFIANT NU et que le resultat est
     * une valeur sentinelle OWL, on refuse d'afficher cette valeur trompeuse et
     * on rend un message honnete. La vraie resolution par nom dans la frame
     * (table de noms de locales dans ObjFunction) est un chantier compiler
     * separe (Voie B, backlog).
     *
     * On ne penalise PAS le hover sur une vraie constante : value_repr ferait
     * un reverse-lookup (sentinel_to_label) et afficherait le label canon. Mais
     * dans le doute d'un identifiant nu, le message honnete vaut mieux qu'une
     * etiquette potentiellement fausse (le user croit lire SA variable). */
    if (is_owl_sentinel_value(result)) {
        bool bareIdent = true;
        for (const char* p = expr; *p; p++) {
            unsigned char c = (unsigned char)*p;
            /* Un identifiant OWL : lettres (ASCII + accents UTF-8 >= 0x80),
             * chiffres, underscore. Tout autre caractere (operateur, point,
             * parenthese, espace median) = ce n'est plus un nom nu. */
            bool ok = (c >= 0x80) || (c == '_') ||
                      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9');
            if (!ok) { bareIdent = false; break; }
        }
        if (bareIdent) {
            snprintf(out, DEBUG_STR_LEN,
                     "(locale non resolvable par nom dans cette version)");
            snprintf(outType, DEBUG_STR_LEN, "%s", "?");
            return true;
        }
    }

    value_repr(result, out);
    snprintf(outType, DEBUG_STR_LEN, "%s", type_name_for(result));
    return true;
}

/* ================================================================
 * Drill-down : owl_debug_get_children (D.4.1)
 * ================================================================ */

bool owl_debug_get_children(int childrenRef, OwlDebugVariable* out, int max, int* outCount) {
    if (outCount) *outCount = 0;
    if (owl_debug_state.mode != DEBUG_PAUSED) return false;
    if (childrenRef <= 0 || childrenRef > g_handle_count) return false;
    Value v = g_handles[childrenRef - 1];

    int n = 0;
    int cap = max < DEBUG_VAR_MAX ? max : DEBUG_VAR_MAX;
    char nameBuf[DEBUG_STR_LEN];

    if (IS_ARRAY(v)) {
        ObjArray* a = AS_ARRAY(v);
        int total = a->elements.count;
        if (total > cap) total = cap;
        for (int i = 0; i < total; i++) {
            /* Index 1-based en canon. */
            snprintf(nameBuf, DEBUG_STR_LEN, "[%d]", i + 1);
            fill_var(n, nameBuf, a->elements.values[i], out);
            n++;
        }
    } else if (IS_ASSOC_ARRAY(v)) {
        ObjAssocArray* aa = AS_ASSOC_ARRAY(v);
        int total = aa->count;
        if (total > cap) total = cap;
        for (int i = 0; i < total; i++) {
            const char* k = (aa->keys[i] && aa->keys[i]->chars) ? aa->keys[i]->chars : "?";
            fill_var(n, k, aa->values[i], out);
            n++;
        }
    } else if (IS_JSON(v)) {
        ObjJson* j = (ObjJson*)AS_OBJ(v);
        if (j->jtype == JSON_T_ARRAY) {
            int total = j->as.arrayVal.count;
            if (total > cap) total = cap;
            for (int i = 0; i < total; i++) {
                snprintf(nameBuf, DEBUG_STR_LEN, "[%d]", i);
                /* JSON children sont des ObjJson*, on les wrap dans une Value. */
                Value childV = OBJ_VAL((Obj*)j->as.arrayVal.items[i]);
                fill_var(n, nameBuf, childV, out);
                n++;
            }
        } else if (j->jtype == JSON_T_OBJECT) {
            int total = j->as.objectVal.count;
            if (total > cap) total = cap;
            for (int i = 0; i < total; i++) {
                const char* k = (j->as.objectVal.keys[i] && j->as.objectVal.keys[i]->chars)
                              ? j->as.objectVal.keys[i]->chars : "?";
                Value childV = OBJ_VAL((Obj*)j->as.objectVal.values[i]);
                fill_var(n, k, childV, out);
                n++;
            }
        }
        /* Autres jtypes (BOOL, INT, REAL, STRING) : pas de children, ne devraient
         * pas avoir un handle. Ne pas tomber ici en pratique. */
    } else if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        /* Pour les instances de types builtin (httpRequête, Cadre, ...),
         * récupère le TypeDef pour résoudre les enums (allowedBase) sur
         * les fields à l'affichage. Pour les structs user-defined, td=NULL
         * et fill_var fonctionne sans contexte enum. */
        const char* sn = (inst->structName && inst->structName->chars)
                       ? inst->structName->chars : NULL;
        /* Iterate via fieldOrder pour preserver l'ordre de declaration
         * (struct user-defined). Si NULL (instance de type builtin :
         * httpRequête, Cadre, ... — fields enregistres via FIELDS_*
         * cote C), fallback sur la table directement. */
        if (inst->fieldOrder) {
            int total = inst->fieldOrder->elements.count;
            if (total > cap) total = cap;
            for (int i = 0; i < total; i++) {
                Value fname = inst->fieldOrder->elements.values[i];
                if (!IS_STRING(fname)) continue;
                const char* k = AS_CSTRING(fname);
                Value fval;
                if (table_get(&inst->fields, AS_STRING(fname), &fval)) {
                    const OwlField* fctx = lookup_field(sn, k);
                    /*  : computed field -> appeler le getter pour
                     * l'etat live (sinon snapshot stale, ex Chrono..Valeur). */
                    if (fctx != NULL && fctx->computeGetter != NULL) {
                        fval = fctx->computeGetter(1, &v);
                    }
                    fill_var_field(n, k, fval, out, fctx);
                    n++;
                }
            }
        } else {
            /* Iter brutale de la table : ordre non determine mais au moins
             * tous les fields qui ont ete set sur l'instance sont visibles. */
            for (int i = 0; i < inst->fields.capacity && n < cap; i++) {
                Entry* e = &inst->fields.entries[i];
                if (e->key == NULL) continue;
                const char* k = e->key->chars ? e->key->chars : "?";
                const OwlField* fctx = lookup_field(sn, k);
                Value fval = e->value;
                if (fctx != NULL && fctx->computeGetter != NULL) {
                    fval = fctx->computeGetter(1, &v);
                }
                fill_var_field(n, k, fval, out, fctx);
                n++;
            }
        }
    }

    if (outCount) *outCount = n;
    return true;
}
