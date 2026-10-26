// SPDX-License-Identifier: AGPL-3.0-only
#include "dap_server.h"
#include "dap_child.h"
#include "lsp_json.h"   /* parser JSON commun, mutualise avec le LSP */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ================================================================
 * Etat global serveur DAP. Mono-thread pour l'instant ; le multi-thread
 * (thread DAP I/O ↔ thread VM) viendra en D.2.3.
 * ================================================================ */

static struct {
    int64_t nextSeq;       /* incremente a chaque message envoye (req|resp|event) */
    bool    shouldStop;    /* signale par disconnect/terminate, fait sortir dap_run */
    bool    initialized;   /* true apres reception de `initialize` */
} g_dap;

/* ================================================================
 * LOGGING (stderr — stdout reserve au protocole)
 * ================================================================ */

void dap_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[owl-dap] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

/* ================================================================
 * Transport JSON-RPC framing Content-Length (identique a LSP).
 * ================================================================ */

static char* read_message(int* outLen) {
    int contentLength = -1;
    char headerLine[1024];

    for (;;) {
        if (!fgets(headerLine, sizeof(headerLine), stdin)) return NULL;
        if (strcmp(headerLine, "\r\n") == 0 || strcmp(headerLine, "\n") == 0) break;
        if (strncmp(headerLine, "Content-Length:", 15) == 0) {
            contentLength = atoi(headerLine + 15);
        }
    }
    if (contentLength <= 0) return NULL;

    char* body = malloc(contentLength + 1);
    int bytesRead = (int)fread(body, 1, contentLength, stdin);
    if (bytesRead < contentLength) { free(body); return NULL; }
    body[contentLength] = '\0';
    *outLen = contentLength;
    return body;
}

/* Serialise send_message : les deux threads reader (stdout + stderr) du child
 * forwardent des OutputEvents en parallele, et le thread principal DAP envoie
 * ses responses. Sans verrou, deux frames Content-Length peuvent s'entrelacer
 * au niveau octet et corrompre le flux vu par VSCode. */
#ifdef _WIN32
static CRITICAL_SECTION g_send_mutex;
static bool g_send_mutex_init = false;
static void send_mutex_ensure(void) {
    if (!g_send_mutex_init) { InitializeCriticalSection(&g_send_mutex); g_send_mutex_init = true; }
}
#define send_mutex_lock()   EnterCriticalSection(&g_send_mutex)
#define send_mutex_unlock() LeaveCriticalSection(&g_send_mutex)
#else
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static void send_mutex_ensure(void) {}
#define send_mutex_lock()   pthread_mutex_lock(&g_send_mutex)
#define send_mutex_unlock() pthread_mutex_unlock(&g_send_mutex)
#endif

static void send_message(const char* json, int len) {
    send_mutex_ensure();
    send_mutex_lock();
    fprintf(stdout, "Content-Length: %d\r\n\r\n", len);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
    send_mutex_unlock();
}

/* ================================================================
 * Send response / event
 * ================================================================ */

void dap_send_response(int64_t requestSeq, const char* command,
                       bool success, JsonValue* body) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "seq", json_new_int(g_dap.nextSeq++));
    json_object_set(msg, "type", json_new_string("response"));
    json_object_set(msg, "request_seq", json_new_int(requestSeq));
    json_object_set(msg, "command", json_new_string(command ? command : ""));
    json_object_set(msg, "success", json_new_bool(success));
    if (success) {
        if (body) json_object_set(msg, "body", body);
    } else {
        /* En cas d'echec, body est interprete comme un texte de message
         * d'erreur pour l'utilisateur. Si NULL, on met un fallback. */
        json_object_set(msg, "message",
            json_new_string("command not implemented"));
        if (body) json_free(body);
    }

    int len;
    char* json = json_serialize(msg, &len);
    send_message(json, len);
    free(json);
    json_free(msg);
}

void dap_send_event(const char* eventName, JsonValue* body) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "seq", json_new_int(g_dap.nextSeq++));
    json_object_set(msg, "type", json_new_string("event"));
    json_object_set(msg, "event", json_new_string(eventName ? eventName : ""));
    if (body) json_object_set(msg, "body", body);

    int len;
    char* json = json_serialize(msg, &len);
    send_message(json, len);
    free(json);
    json_free(msg);
}

/* ================================================================
 * Handlers — D.2.1 : minimum viable pour qu'un client DAP (VSCode)
 * puisse round-tripper sans crasher. Les vrais handlers (launch,
 * setBreakpoints, continue, ...) viendront en D.2.5+.
 * ================================================================ */

/* ================================================================
 * Callback du child reader thread : pour V0, on log juste ce qu'on
 * recoit du child. Le mapping vers des events DAP (StoppedEvent,
 * OutputEvent, etc.) viendra en D.2.5+ avec les handlers concrets.
 * ================================================================ */

/* Reconnait les events asynchrones du child (typiquement "stopped" emis
 * par owl_debug_hook) et les traduit en events DAP que VSCode comprend.
 * Les responses synchrones (avec id) sont gerees par dap_child_request
 * en amont — elles ne passent pas par ici. */
/* Wrap une ligne child (texte brut) en OutputEvent DAP category=stdout, pour
 * que VSCode l'affiche dans la Debug Console. Sert aux sorties user code (Trace,
 * messages d'erreur OWL) qui ne sont pas du protocole. */
static void dap_forward_output_line(const char* line, const char* category) {
    JsonValue* body = json_new_object();
    json_object_set(body, "category", json_new_string(category));
    char buf[8200];
    snprintf(buf, sizeof(buf), "%s\n", line);
    json_object_set(body, "output", json_new_string(buf));
    dap_send_event("output", body);
}

static void dap_forward_stdout_line(const char* line) {
    dap_forward_output_line(line, "stdout");
}

static void on_child_line(const char* line, int channel) {
    /* stderr du child = sortie runtime brute (erreurs d'execution owl). Jamais
     * du protocole : forward direct en OutputEvent category=stderr pour que la
     * Debug Console l'affiche (sinon debogueur muet sur erreur runtime). */
    if (channel == DAP_CHILD_STDERR) {
        dap_forward_output_line(line, "stderr");
        return;
    }

    /* Parse rapide pour detecter un event. Une ligne non-JSON (ex : Trace
     * user code en texte brut sur stdout) n'est pas du protocole : on la
     * forward telle quelle en OutputEvent pour la Debug Console. */
    JsonValue* msg = json_parse(line, (int)strlen(line));
    if (!msg) {
        dap_forward_stdout_line(line);
        return;  /* msg NULL : pas de json_free */
    }

    const char* eventName = json_get_string(msg, "event");
    if (!eventName) {
        /* JSON valide sans "event" : probablement du user code aussi. */
        dap_forward_stdout_line(line);
        json_free(msg);
        return;
    }

    if (strcmp(eventName, "stopped") == 0) {
        /* Traduction en StoppedEvent DAP : le UI VSCode passera la fenetre
         * en mode debug (gutter pointer, Variables panel, etc.). */
        const char* reason = json_get_string(msg, "reason");
        const char* file   = json_get_string(msg, "file");
        int64_t line       = json_get_int(msg, "line", 0);

        JsonValue* body = json_new_object();
        json_object_set(body, "reason",
            json_new_string(reason ? reason : "pause"));
        json_object_set(body, "threadId", json_new_int(1));   /* mono-thread */
        json_object_set(body, "allThreadsStopped", json_new_bool(true));
        if (file) {
            JsonValue* src = json_new_object();
            json_object_set(src, "path", json_new_string(file));
            json_object_set(body, "source", src);
        }
        json_object_set(body, "line", json_new_int(line));
        dap_send_event("stopped", body);
    } else if (strcmp(eventName, "hello") == 0) {
        /* Banner de demarrage — log uniquement, pas d'event DAP. */
        dap_log("child ready");
    } else if (strcmp(eventName, "_child_eof") == 0) {
        /* Le child a ferme stdout = il est mort (script termine normalement
         * ou crash). On envoie TerminatedEvent + ExitedEvent a VSCode pour
         * qu'il ferme proprement la session debug. */
        dap_send_event("terminated", NULL);
        JsonValue* exitBody = json_new_object();
        json_object_set(exitBody, "exitCode", json_new_int(0));
        dap_send_event("exited", exitBody);
    } else {
        dap_log("unknown event from child: %s", eventName);
    }
    json_free(msg);
}

/* ================================================================
 * Pending breakpoints — DAP envoie setBreakpoints AVANT launch (et
 * meme avant configurationDone). On stocke en cache et on flush au
 * moment du spawn.
 *
 * Cache multi-fichiers : VSCode envoie un setBreakpoints PAR fichier
 * (full-replace de ce fichier). Une fenetre + sa classe = deux fichiers,
 * donc deux requetes. On garde une table path -> [lines] pour ne pas
 * ecraser les breakpoints d'un fichier quand un autre est (re)configure.
 * Le child (debug_api.c) indexe deja ses breakpoints par fichier.
 * ================================================================ */

#define DAP_PENDING_MAX_LINES 64
#define DAP_PENDING_MAX_FILES 32

typedef struct {
    char* path;                        /* strdup ; NULL = slot inutilise */
    int   lines[DAP_PENDING_MAX_LINES];
    int   count;
} DapPendingFile;

static DapPendingFile g_pendingFiles[DAP_PENDING_MAX_FILES];
static int            g_pendingFileCount;

/* Compare tolerant slash/casse — VSCode peut envoyer "c:\..." la ou un
 * autre chemin canon utilise "/". Meme logique que path_match cote child. */
static bool pending_path_eq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int pending_find_file(const char* path) {
    for (int i = 0; i < g_pendingFileCount; i++) {
        if (g_pendingFiles[i].path && pending_path_eq(g_pendingFiles[i].path, path))
            return i;
    }
    return -1;
}

/* Retourne l'index du slot (existant ou cree). -1 si table pleine. */
static int pending_get_or_add_file(const char* path) {
    int idx = pending_find_file(path);
    if (idx >= 0) return idx;
    if (g_pendingFileCount >= DAP_PENDING_MAX_FILES) return -1;
    char* dup = strdup(path);
    if (!dup) return -1;
    idx = g_pendingFileCount++;
    g_pendingFiles[idx].path = dup;
    g_pendingFiles[idx].count = 0;
    return idx;
}

static void pending_reset_all(void) {
    for (int i = 0; i < g_pendingFileCount; i++) {
        free(g_pendingFiles[i].path);
        g_pendingFiles[i].path = NULL;
        g_pendingFiles[i].count = 0;
    }
    g_pendingFileCount = 0;
}

static void send_set_breakpoint_to_child(const char* file, int line) {
    JsonValue* req = json_new_object();
    json_object_set(req, "cmd", json_new_string("set_breakpoint"));
    JsonValue* a = json_new_object();
    if (file) json_object_set(a, "file", json_new_string(file));
    json_object_set(a, "line", json_new_int(line));
    json_object_set(req, "args", a);
    JsonValue* resp = dap_child_request(req, 2000);
    if (resp) json_free(resp);
}

/* Flush TOUS les fichiers du cache vers le child (au spawn ou apres un
 * clear_all en live-update). */
static void flush_pending_breakpoints_to_child(void) {
    for (int i = 0; i < g_pendingFileCount; i++) {
        DapPendingFile* pf = &g_pendingFiles[i];
        if (!pf->path) continue;
        for (int j = 0; j < pf->count; j++) {
            send_set_breakpoint_to_child(pf->path, pf->lines[j]);
        }
    }
}

/* ================================================================
 * Handler launch — DAP spec :
 *   {"command":"launch", "arguments":{"program":"<path>", ...}}
 * On extrait le program, on spawn owl --debug-stdio program, et on
 * repond launch=success si le spawn a reussi.
 * ================================================================ */

/* Program path memorise au moment de launch, spawn differe a
 * configurationDone. Convention DAP : avec supportsConfigurationDoneRequest
 * a true, le debuggee ne doit PAS demarrer immediatement a launch ; il
 * doit attendre que VSCode finisse d'envoyer ses configurations
 * (setBreakpoints, etc.) puis configurationDone qui declenche le run.
 *
 * Sans ce defer, VSCode envoyait parfois `launch` AVANT `setBreakpoints`
 * (ordre defini par le client) et le child finissait d'executer le
 * script avant meme que les breakpoints arrivent. Bug latent jusqu'a
 * maintenant, revele par les logs DAP tracker. */
static char g_pending_program[1024] = {0};

static void handle_launch(int64_t seq, JsonValue* args) {
    const char* program = args ? json_get_string(args, "program") : NULL;
    dap_log("launch program=%s (deferred until configurationDone)",
            program ? program : "(null)");
    if (!program || !program[0]) {
        dap_log("launch FAILED: program is empty");
        dap_send_response(seq, "launch", false, NULL);
        return;
    }
    /* Memorise le program pour configurationDone. Pas de spawn encore. */
    strncpy(g_pending_program, program, sizeof(g_pending_program) - 1);
    g_pending_program[sizeof(g_pending_program) - 1] = '\0';
    dap_send_response(seq, "launch", true, NULL);
}

static void handle_initialize(int64_t seq, JsonValue* args) {
    (void)args;
    g_dap.initialized = true;

    /* Capabilities : on declare ce qu'on supporte. Pour V0, on est honnete :
     * presque rien fonctionnel, mais on declare les supports basiques pour
     * qu'un client se comporte normalement (ex. configurationDone). */
    JsonValue* caps = json_new_object();
    json_object_set(caps, "supportsConfigurationDoneRequest", json_new_bool(true));
    json_object_set(caps, "supportsTerminateRequest",         json_new_bool(true));
    json_object_set(caps, "supportsEvaluateForHovers",        json_new_bool(true));
    json_object_set(caps, "supportsConditionalBreakpoints",   json_new_bool(false));
    json_object_set(caps, "supportsStepBack",                 json_new_bool(false));
    /* Plus de capabilities a flag a partir de D.2.5+ a mesure qu'on
     * implemente les commandes. */

    dap_send_response(seq, "initialize", true, caps);

    /* Le protocole demande aussi un event "initialized" apres la response,
     * pour signaler au client qu'il peut envoyer ses configs (breakpoints,
     * etc.). On le fait des maintenant, le client le verra. */
    dap_send_event("initialized", NULL);
}

static void handle_disconnect(int64_t seq) {
    /* DAP spec : disconnect doit aussi terminer le debuggee. */
    if (dap_child_is_running()) dap_child_kill();
    pending_reset_all();
    dap_send_response(seq, "disconnect", true, NULL);
    g_dap.shouldStop = true;
}

static void handle_terminate(int64_t seq) {
    if (dap_child_is_running()) dap_child_kill();
    pending_reset_all();
    dap_send_response(seq, "terminate", true, NULL);
    g_dap.shouldStop = true;
}

/* configurationDone : VSCode signale qu'il a fini d'envoyer ses configs
 * initiales (breakpoints, etc.). MAINTENANT on spawn le child avec les
 * breakpoints deja appliques avant l'execution. C'est le pattern DAP
 * conforme : launch memorise, configurationDone declenche le run.
 *
 * Si pas de program memorise (ex: attach mode futur, ou launch a echoue),
 * on repond OK quand meme. */
static void handle_configurationDone(int64_t seq) {
    if (g_pending_program[0] && !dap_child_is_running()) {
        if (!dap_child_spawn(g_pending_program, on_child_line)) {
            dap_log("configurationDone FAILED: dap_child_spawn returned false");
            dap_send_response(seq, "configurationDone", false, NULL);
            g_pending_program[0] = '\0';
            return;
        }
        flush_pending_breakpoints_to_child();
        /* Gate anti-course : le child attend `run` avant d'executer le script.
         * On l'envoie APRES le flush des breakpoints, donc tous les bp sont
         * enregistres cote VM avant la moindre ligne executee. */
        JsonValue* runReq = json_new_object();
        json_object_set(runReq, "cmd", json_new_string("run"));
        JsonValue* runResp = dap_child_request(runReq, 2000);
        if (runResp) json_free(runResp);
        g_pending_program[0] = '\0';
    }
    dap_send_response(seq, "configurationDone", true, NULL);
}

/* setBreakpoints : VSCode envoie l'ensemble des breakpoints d'un fichier
 * (full replace, pas incremental). Format DAP :
 *   args = { source: {path: "..."}, breakpoints: [{line:N}, ...] }
 * On clear cote child puis set un par un, et on retourne la liste de
 * Breakpoint avec verified=true (V0 : on suppose que toute ligne est
 * valide ; un check d'existence cote VM viendra plus tard). */
/* Helpers : forward d'une commande de controle vers le child. La VM est
 * en pause -> repond instantanement OK. Apres ce return, la VM va reprendre
 * et hit le prochain breakpoint/step -> emit un nouveau StoppedEvent
 * automatiquement via le callback. */
static void forward_simple_cmd(int64_t seq, const char* dapCmd, const char* childCmd) {
    if (!dap_child_is_running()) {
        dap_send_response(seq, dapCmd, false, NULL);
        return;
    }
    JsonValue* req = json_new_object();
    json_object_set(req, "cmd", json_new_string(childCmd));
    JsonValue* resp = dap_child_request(req, 2000);
    bool ok = resp && json_get_bool(resp, "ok", false);
    if (resp) json_free(resp);
    /* DAP continue/next/etc retournent un body simple ; on declare aussi
     * allThreadsContinued pour que VSCode mette a jour son UI mono-thread. */
    JsonValue* body = json_new_object();
    if (strcmp(dapCmd, "continue") == 0)
        json_object_set(body, "allThreadsContinued", json_new_bool(true));
    dap_send_response(seq, dapCmd, ok, body);
}

static void handle_continue(int64_t seq) { forward_simple_cmd(seq, "continue", "continue"); }
static void handle_next    (int64_t seq) { forward_simple_cmd(seq, "next",     "step_over"); }
static void handle_stepIn  (int64_t seq) { forward_simple_cmd(seq, "stepIn",   "step_in"); }
static void handle_stepOut (int64_t seq) { forward_simple_cmd(seq, "stepOut",  "step_out"); }
static void handle_pause   (int64_t seq) { forward_simple_cmd(seq, "pause",    "pause"); }

/* ================================================================
 * Inspection : stackTrace, scopes, variables (D.2.7 + D.4.1 drill-down)
 *
 * Convention variablesReference (decodee par handle_variables) :
 *   0           : sentinel DAP "pas de children" (variable primitive)
 *   1           : scope globals
 *   100 + i     : scope locals du frame i
 *   1000 + h    : container handle h cote child (drill-down JSON/tableau/struct)
 * ================================================================ */
#define VAR_REF_GLOBALS         1
#define VAR_REF_LOCALS_BASE     100
#define VAR_REF_CHILD_BASE      1000

static JsonValue* convert_var_to_dap(JsonValue* v) {
    /* Le child renvoie {name, value, type, ref}. ref != 0 => container,
     * on remappe en variablesReference DAP via VAR_REF_CHILD_BASE. */
    JsonValue* out = json_new_object();
    json_object_set(out, "name",
        json_new_string(json_get_string(v, "name") ? json_get_string(v, "name") : ""));
    json_object_set(out, "value",
        json_new_string(json_get_string(v, "value") ? json_get_string(v, "value") : ""));
    json_object_set(out, "type",
        json_new_string(json_get_string(v, "type") ? json_get_string(v, "type") : "?"));
    int64_t childRef = json_get_int(v, "ref", 0);
    int64_t dapRef = (childRef > 0) ? (VAR_REF_CHILD_BASE + childRef) : 0;
    json_object_set(out, "variablesReference", json_new_int(dapRef));
    return out;
}

static void handle_stackTrace(int64_t seq, JsonValue* args) {
    (void)args;  /* startFrame/levels ignores en V0 — retour complet */
    if (!dap_child_is_running()) {
        dap_send_response(seq, "stackTrace", false, NULL);
        return;
    }
    JsonValue* req = json_new_object();
    json_object_set(req, "cmd", json_new_string("get_stack"));
    JsonValue* resp = dap_child_request(req, 2000);
    if (!resp || !json_get_bool(resp, "ok", false)) {
        if (resp) json_free(resp);
        dap_send_response(seq, "stackTrace", false, NULL);
        return;
    }

    JsonValue* result = json_get(resp, "result");
    JsonValue* outFrames = json_new_array();
    int total = 0;
    if (result && result->type == JSON_ARRAY) {
        total = result->as.arrayVal.count;
        for (int i = 0; i < total; i++) {
            JsonValue* f = result->as.arrayVal.items[i];
            int64_t index = json_get_int(f, "index", i);
            const char* fn = json_get_string(f, "function");
            const char* file = json_get_string(f, "file");
            int64_t line = json_get_int(f, "line", 0);

            JsonValue* dapFrame = json_new_object();
            json_object_set(dapFrame, "id", json_new_int(index));
            json_object_set(dapFrame, "name",
                json_new_string(fn ? fn : "<main>"));
            if (file) {
                JsonValue* src = json_new_object();
                json_object_set(src, "path", json_new_string(file));
                /* `name` purement display */
                const char* slash = strrchr(file, '/');
                const char* bs    = strrchr(file, '\\');
                const char* base  = slash ? slash + 1 : (bs ? bs + 1 : file);
                json_object_set(src, "name", json_new_string(base));
                json_object_set(dapFrame, "source", src);
            }
            json_object_set(dapFrame, "line", json_new_int(line));
            json_object_set(dapFrame, "column", json_new_int(1));
            json_array_push(outFrames, dapFrame);
        }
    }
    json_free(resp);

    JsonValue* body = json_new_object();
    json_object_set(body, "stackFrames", outFrames);
    json_object_set(body, "totalFrames", json_new_int(total));
    dap_send_response(seq, "stackTrace", true, body);
}

static void handle_scopes(int64_t seq, JsonValue* args) {
    int64_t frameId = args ? json_get_int(args, "frameId", 0) : 0;
    JsonValue* arr = json_new_array();
    /* ORDRE IMPORTANT : VSCode auto-déplie le 1er scope retourné. On met
     * Globals en premier pour qu'il s'expand par défaut (vue souhaitée
     * pour l'utilisateur — peu de globals filtrés via is_system_symbol).
     * Locals reste auto-déplié aussi via presentationHint="locals". */

    /* Globals — scope unique partage entre frames, varRef = 1. */
    JsonValue* globals = json_new_object();
    json_object_set(globals, "name", json_new_string("Globals"));
    json_object_set(globals, "presentationHint", json_new_string("locals"));
    json_object_set(globals, "variablesReference", json_new_int(VAR_REF_GLOBALS));
    json_object_set(globals, "expensive", json_new_bool(false));
    json_array_push(arr, globals);

    /* Locals — scope par frame, varRef = base + frameId. */
    JsonValue* locals = json_new_object();
    json_object_set(locals, "name", json_new_string("Locals"));
    json_object_set(locals, "presentationHint", json_new_string("locals"));
    json_object_set(locals, "variablesReference",
        json_new_int(VAR_REF_LOCALS_BASE + frameId));
    json_object_set(locals, "expensive", json_new_bool(false));
    json_array_push(arr, locals);

    JsonValue* body = json_new_object();
    json_object_set(body, "scopes", arr);
    dap_send_response(seq, "scopes", true, body);
}

static void handle_variables(int64_t seq, JsonValue* args) {
    int64_t ref = args ? json_get_int(args, "variablesReference", 0) : 0;
    if (!dap_child_is_running() || ref <= 0) {
        dap_send_response(seq, "variables", false, NULL);
        return;
    }

    /* Construit la commande interne selon le type de scope. */
    JsonValue* req = json_new_object();
    if (ref == VAR_REF_GLOBALS) {
        json_object_set(req, "cmd", json_new_string("get_globals"));
    } else if (ref >= VAR_REF_CHILD_BASE) {
        /* Drill-down container : ref - 1000 = handle child. */
        int childRef = (int)(ref - VAR_REF_CHILD_BASE);
        json_object_set(req, "cmd", json_new_string("get_children"));
        JsonValue* a = json_new_object();
        json_object_set(a, "ref", json_new_int(childRef));
        json_object_set(req, "args", a);
    } else if (ref >= VAR_REF_LOCALS_BASE) {
        int frameIdx = (int)(ref - VAR_REF_LOCALS_BASE);
        json_object_set(req, "cmd", json_new_string("get_locals"));
        JsonValue* a = json_new_object();
        json_object_set(a, "frame", json_new_int(frameIdx));
        json_object_set(req, "args", a);
    } else {
        json_free(req);
        dap_send_response(seq, "variables", false, NULL);
        return;
    }

    JsonValue* resp = dap_child_request(req, 2000);
    if (!resp || !json_get_bool(resp, "ok", false)) {
        if (resp) json_free(resp);
        dap_send_response(seq, "variables", false, NULL);
        return;
    }
    JsonValue* result = json_get(resp, "result");
    JsonValue* outVars = json_new_array();
    if (result && result->type == JSON_ARRAY) {
        for (int i = 0; i < result->as.arrayVal.count; i++) {
            json_array_push(outVars, convert_var_to_dap(result->as.arrayVal.items[i]));
        }
    }
    json_free(resp);

    JsonValue* body = json_new_object();
    json_object_set(body, "variables", outVars);
    dap_send_response(seq, "variables", true, body);
}

/* True si `s` est un identifier OWL simple (lettre|_ puis lettres|chiffres|_).
 * V0 : on accepte aussi les accents (latin1 high bytes), pour matcher la
 * politique du scanner. */
static bool is_simple_ident(const char* s) {
    if (!s || !s[0]) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c >= 0x80) continue;
        return false;
    }
    return true;
}

/* Trim whitespace en place (modifie buf). */
static void trim_inplace(char* s) {
    char* start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

/* Tente de parser une string en entier. Retourne true + valeur si ok. */
static bool parse_int(const char* s, int64_t* out) {
    if (!s || !s[0]) return false;
    char* endp = NULL;
    long long v = strtoll(s, &endp, 10);
    if (endp == s || *endp != '\0') return false;
    *out = (int64_t)v;
    return true;
}

/* Cherche une variable par nom dans une JsonValue array (issue de
 * get_locals/get_globals). Retourne le sous-objet ou NULL. */
static JsonValue* find_var_by_name(JsonValue* arr, const char* name) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < arr->as.arrayVal.count; i++) {
        JsonValue* v = arr->as.arrayVal.items[i];
        const char* n = json_get_string(v, "name");
        if (n && strcmp(n, name) == 0) return v;
    }
    return NULL;
}

/* Resolve un atom : ident -> sa valeur entiere (cherche locals+globals),
 * litteral entier -> sa valeur. Renvoie false si non resolvable. */
static bool resolve_atom_int(const char* atom, int64_t frameId, int64_t* out) {
    /* Litteral entier ? */
    if (parse_int(atom, out)) return true;
    if (!is_simple_ident(atom)) return false;
    /* Cherche dans locals + globals via le child. */
    JsonValue* req = json_new_object();
    json_object_set(req, "cmd", json_new_string("get_locals"));
    JsonValue* a = json_new_object();
    json_object_set(a, "frame", json_new_int(frameId));
    json_object_set(req, "args", a);
    JsonValue* localsResp = dap_child_request(req, 2000);
    JsonValue* found = NULL;
    if (localsResp && json_get_bool(localsResp, "ok", false)) {
        found = find_var_by_name(json_get(localsResp, "result"), atom);
    }
    bool ok = false;
    if (found) {
        const char* val = json_get_string(found, "value");
        if (val && parse_int(val, out)) ok = true;
    }
    if (localsResp) json_free(localsResp);
    if (ok) return true;

    JsonValue* gReq = json_new_object();
    json_object_set(gReq, "cmd", json_new_string("get_globals"));
    JsonValue* gResp = dap_child_request(gReq, 2000);
    if (gResp && json_get_bool(gResp, "ok", false)) {
        found = find_var_by_name(json_get(gResp, "result"), atom);
        if (found) {
            const char* val = json_get_string(found, "value");
            if (val && parse_int(val, out)) ok = true;
        }
    }
    if (gResp) json_free(gResp);
    return ok;
}

/* Mini eval : supporte `<atom> <op> <atom>` ou op in {+, -, *, /} et
 * `<atom>` seul. Pour V0 : entiers seulement. Suffit pour la majorite des
 * expressions dans Watch/Hover (`x`, `i + 1`, `n * 2`, `total / count`).
 * Retourne false si pas matchable. */
static bool try_simple_arith(const char* expr, int64_t frameId, int64_t* out) {
    /* Cherche un operateur (premier - * / +). On scan a la main car on
     * ne veut pas matcher les '-' du litteral negatif. */
    char buf[256];
    strncpy(buf, expr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim_inplace(buf);

    /* Atom seul ? */
    int64_t a;
    if (resolve_atom_int(buf, frameId, &a)) { *out = a; return true; }

    /* Cherche un operateur top-level. Skip le premier char (peut etre un
     * '-' unaire pour litteral). */
    int n = (int)strlen(buf);
    int opPos = -1;
    char op = '\0';
    for (int i = 1; i < n; i++) {
        char c = buf[i];
        if (c == '+' || c == '-' || c == '*' || c == '/') {
            opPos = i;
            op = c;
            break;
        }
    }
    if (opPos < 0) return false;

    char left[256], right[256];
    snprintf(left,  sizeof(left),  "%.*s", opPos, buf);
    snprintf(right, sizeof(right), "%s",   buf + opPos + 1);
    trim_inplace(left);
    trim_inplace(right);

    int64_t l, r;
    if (!resolve_atom_int(left,  frameId, &l)) return false;
    if (!resolve_atom_int(right, frameId, &r)) return false;
    switch (op) {
        case '+': *out = l + r; return true;
        case '-': *out = l - r; return true;
        case '*': *out = l * r; return true;
        case '/': if (r == 0) return false; *out = l / r; return true;
    }
    return false;
}

/* DAP evaluate : Watch panel, Hover, Debug Console.
 * D.4.2 : eval complet via la commande "eval" cote child qui compile
 * + execute l'expression dans la VM paused. Couvre toutes les expr OWL
 * valides : `x + 1`, `Maj(s)`, `tab[i]`, `req..URL`, etc.
 *
 * Fallback : si l'eval echoue (compile error, runtime error), on tente
 * encore le mini-eval arithmetique pour ne pas perdre les expressions
 * triviales si jamais le child a un souci. */
static void handle_evaluate(int64_t seq, JsonValue* args) {
    if (!dap_child_is_running()) {
        dap_send_response(seq, "evaluate", false, NULL);
        return;
    }
    const char* expr = args ? json_get_string(args, "expression") : NULL;
    int64_t frameId = args ? json_get_int(args, "frameId", 0) : 0;
    if (!expr || !expr[0]) {
        dap_send_response(seq, "evaluate", false, NULL);
        return;
    }

    /* Tentative principale : eval complet via le child. */
    JsonValue* evalReq = json_new_object();
    json_object_set(evalReq, "cmd", json_new_string("eval"));
    JsonValue* evalArgs = json_new_object();
    json_object_set(evalArgs, "expr", json_new_string(expr));
    json_object_set(evalReq, "args", evalArgs);
    JsonValue* evalResp = dap_child_request(evalReq, 3000);
    if (evalResp && json_get_bool(evalResp, "ok", false)) {
        JsonValue* result = json_get(evalResp, "result");
        if (result) {
            bool evalOk = json_get_bool(result, "ok", false);
            const char* val = json_get_string(result, "value");
            const char* type = json_get_string(result, "type");
            if (evalOk && val) {
                JsonValue* body = json_new_object();
                json_object_set(body, "result", json_new_string(val));
                json_object_set(body, "type", json_new_string(type ? type : "?"));
                json_object_set(body, "variablesReference", json_new_int(0));
                json_free(evalResp);
                dap_send_response(seq, "evaluate", true, body);
                return;
            }
        }
        json_free(evalResp);
    } else if (evalResp) {
        json_free(evalResp);
    }

    /* Fallback : mini-eval arithmetique pour `x + 1`, `n * 2`, etc.
     * (cas triviaux qui peuvent marcher meme si l'eval child a echoue). */
    int64_t arithVal = 0;
    if (try_simple_arith(expr, frameId, &arithVal)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", (long long)arithVal);
        JsonValue* body = json_new_object();
        json_object_set(body, "result", json_new_string(buf));
        json_object_set(body, "type", json_new_string("entier"));
        json_object_set(body, "variablesReference", json_new_int(0));
        dap_send_response(seq, "evaluate", true, body);
        return;
    }

    if (!is_simple_ident(expr)) {
        /* Reponse "soft" : le hover/watch sur une expression complexe
         * affichera "<not supported>" sans erreur sonore. */
        JsonValue* body = json_new_object();
        json_object_set(body, "result", json_new_string("<expression non evaluee>"));
        json_object_set(body, "type", json_new_string("?"));
        json_object_set(body, "variablesReference", json_new_int(0));
        dap_send_response(seq, "evaluate", true, body);
        return;
    }

    /* Cherche d'abord dans locals[frameId] puis dans globals. */
    JsonValue* req = json_new_object();
    json_object_set(req, "cmd", json_new_string("get_locals"));
    JsonValue* a = json_new_object();
    json_object_set(a, "frame", json_new_int(frameId));
    json_object_set(req, "args", a);
    JsonValue* localsResp = dap_child_request(req, 2000);
    JsonValue* found = NULL;
    if (localsResp && json_get_bool(localsResp, "ok", false)) {
        found = find_var_by_name(json_get(localsResp, "result"), expr);
    }
    if (!found) {
        JsonValue* gReq = json_new_object();
        json_object_set(gReq, "cmd", json_new_string("get_globals"));
        JsonValue* gResp = dap_child_request(gReq, 2000);
        if (gResp && json_get_bool(gResp, "ok", false)) {
            found = find_var_by_name(json_get(gResp, "result"), expr);
        }
        if (gResp && !found) json_free(gResp);
        else if (gResp) {
            /* found pointe dans gResp -> on doit le copier avant de free */
            const char* val  = json_get_string(found, "value");
            const char* type = json_get_string(found, "type");
            JsonValue* body = json_new_object();
            json_object_set(body, "result", json_new_string(val ? val : ""));
            json_object_set(body, "type", json_new_string(type ? type : "?"));
            json_object_set(body, "variablesReference", json_new_int(0));
            json_free(gResp);
            if (localsResp) json_free(localsResp);
            dap_send_response(seq, "evaluate", true, body);
            return;
        }
    }
    if (found) {
        const char* val  = json_get_string(found, "value");
        const char* type = json_get_string(found, "type");
        JsonValue* body = json_new_object();
        json_object_set(body, "result", json_new_string(val ? val : ""));
        json_object_set(body, "type", json_new_string(type ? type : "?"));
        json_object_set(body, "variablesReference", json_new_int(0));
        json_free(localsResp);
        dap_send_response(seq, "evaluate", true, body);
        return;
    }
    if (localsResp) json_free(localsResp);

    /* Pas trouve. Pour le hover : reponse soft. Pour watch : VSCode
     * affichera "<not available>" si on retourne false ; on retourne
     * true avec result="<unknown>" pour rester poli. */
    JsonValue* body = json_new_object();
    json_object_set(body, "result", json_new_string("<inconnu>"));
    json_object_set(body, "type", json_new_string("?"));
    json_object_set(body, "variablesReference", json_new_int(0));
    dap_send_response(seq, "evaluate", true, body);
}

/* DAP threads : VSCode demande la liste des threads. OWL est mono-thread,
 * on retourne un seul thread "main". */
static void handle_threads(int64_t seq) {
    JsonValue* arr = json_new_array();
    JsonValue* t = json_new_object();
    json_object_set(t, "id", json_new_int(1));
    json_object_set(t, "name", json_new_string("main"));
    json_array_push(arr, t);
    JsonValue* body = json_new_object();
    json_object_set(body, "threads", arr);
    dap_send_response(seq, "threads", true, body);
}

static void handle_setBreakpoints(int64_t seq, JsonValue* args) {
    JsonValue* source = args ? json_get(args, "source") : NULL;
    const char* path = source ? json_get_string(source, "path") : NULL;
    JsonValue* bps = args ? json_get(args, "breakpoints") : NULL;
    int count = (bps && bps->type == JSON_ARRAY) ? bps->as.arrayVal.count : 0;

    /* Full-replace PAR FICHIER (semantique DAP) : on remplace uniquement
     * les breakpoints de CE fichier dans le cache, sans toucher aux autres
     * fichiers deja configures. count==0 -> le fichier est vide (l'user a
     * retire tous ses breakpoints), le slot reste mais ne flushe rien. */
    int applied = 0;
    int appliedLines[DAP_PENDING_MAX_LINES];
    if (path) {
        int idx = pending_get_or_add_file(path);
        if (idx >= 0) {
            DapPendingFile* pf = &g_pendingFiles[idx];
            pf->count = 0;
            for (int i = 0; i < count && pf->count < DAP_PENDING_MAX_LINES; i++) {
                int64_t line = json_get_int(bps->as.arrayVal.items[i], "line", 0);
                if (line > 0) pf->lines[pf->count++] = (int)line;
            }
            applied = pf->count;
            memcpy(appliedLines, pf->lines, (size_t)applied * sizeof(int));
        } else {
            /* Table pleine : on ne cache pas mais on repond les lignes pour
             * ne pas planter le client. */
            dap_log("setBreakpoints: pending file table full, %s not cached", path);
            for (int i = 0; i < count && applied < DAP_PENDING_MAX_LINES; i++) {
                int64_t line = json_get_int(bps->as.arrayVal.items[i], "line", 0);
                if (line > 0) appliedLines[applied++] = (int)line;
            }
        }
    }

    /* Child en vie : ré-applique TOUS les fichiers du cache (clear global
     * puis re-flush) pour rester coherent apres ce full-replace par fichier. */
    if (dap_child_is_running()) {
        JsonValue* clearReq = json_new_object();
        json_object_set(clearReq, "cmd", json_new_string("clear_all_breakpoints"));
        JsonValue* clearResp = dap_child_request(clearReq, 2000);
        if (clearResp) json_free(clearResp);
        flush_pending_breakpoints_to_child();
    }

    /* Reponse DAP : un Breakpoint par bp du fichier courant, verified=true
     * (V0 optimiste — un check de validite ligne viendra plus tard). */
    JsonValue* respArr = json_new_array();
    for (int i = 0; i < applied; i++) {
        JsonValue* bpResp = json_new_object();
        json_object_set(bpResp, "verified", json_new_bool(true));
        json_object_set(bpResp, "line", json_new_int(appliedLines[i]));
        json_array_push(respArr, bpResp);
    }
    JsonValue* body = json_new_object();
    json_object_set(body, "breakpoints", respArr);
    dap_send_response(seq, "setBreakpoints", true, body);
}

static void dispatch_request(int64_t seq, const char* command, JsonValue* args) {
    if (!command) {
        dap_send_response(seq, "", false, NULL);
        return;
    }
    dap_log("request seq=%lld command=%s", (long long)seq, command);
    if (strcmp(command, "initialize") == 0)         { handle_initialize(seq, args);    return; }
    if (strcmp(command, "launch") == 0)             { handle_launch(seq, args);        return; }
    if (strcmp(command, "configurationDone") == 0)  { handle_configurationDone(seq);   return; }
    if (strcmp(command, "setBreakpoints") == 0)     { handle_setBreakpoints(seq, args); return; }
    if (strcmp(command, "threads") == 0)            { handle_threads(seq); return; }
    if (strcmp(command, "continue") == 0)           { handle_continue(seq); return; }
    if (strcmp(command, "next") == 0)               { handle_next(seq); return; }
    if (strcmp(command, "stepIn") == 0)             { handle_stepIn(seq); return; }
    if (strcmp(command, "stepOut") == 0)            { handle_stepOut(seq); return; }
    if (strcmp(command, "pause") == 0)              { handle_pause(seq); return; }
    if (strcmp(command, "stackTrace") == 0)         { handle_stackTrace(seq, args); return; }
    if (strcmp(command, "scopes") == 0)             { handle_scopes(seq, args); return; }
    if (strcmp(command, "variables") == 0)          { handle_variables(seq, args); return; }
    if (strcmp(command, "evaluate") == 0)            { handle_evaluate(seq, args); return; }
    if (strcmp(command, "disconnect") == 0)         { handle_disconnect(seq);          return; }
    if (strcmp(command, "terminate") == 0)          { handle_terminate(seq);           return; }

    /* Toute autre commande : reponse "not implemented" pour ne pas
     * laisser le client poireauter. Le code reel viendra en D.2.5+. */
    dap_log("not implemented: %s", command);
    dap_send_response(seq, command, false, NULL);
}

/* ================================================================
 * Boucle principale
 * ================================================================ */

void dap_run(void) {
#ifdef _WIN32
    /* Force binary mode sur stdin/stdout pour preserver les CRLF du
     * framing Content-Length sans translation MSVCRT. */
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    g_dap.nextSeq = 1;
    g_dap.shouldStop = false;
    g_dap.initialized = false;

    dap_log("started, awaiting initialize");

    while (!g_dap.shouldStop) {
        int len = 0;
        char* msg = read_message(&len);
        if (!msg) break;  /* EOF ou erreur transport : sortie propre */

        JsonValue* parsed = json_parse(msg, len);
        free(msg);
        if (!parsed) {
            dap_log("malformed JSON received");
            continue;
        }

        const char* type = json_get_string(parsed, "type");
        if (type && strcmp(type, "request") == 0) {
            int64_t seq = json_get_int(parsed, "seq", 0);
            const char* command = json_get_string(parsed, "command");
            JsonValue* args = json_get(parsed, "arguments");
            dispatch_request(seq, command, args);
        } else {
            dap_log("ignoring non-request message type=%s", type ? type : "?");
        }

        json_free(parsed);
    }

    dap_log("exiting");
}
