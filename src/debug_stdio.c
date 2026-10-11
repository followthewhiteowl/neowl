// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "debug_stdio.h"
#include "debug_api.h"
#include "lsp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
typedef HANDLE thread_t;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t thread_t;
#endif

/* ================================================================
 * Etat global (mono-instance par process)
 * ================================================================ */

static struct {
    thread_t thread;
    volatile bool started;
    volatile bool running;
    /* Gate de demarrage : le child NE DOIT PAS executer le script tant que
     * le debugger n'a pas fini d'envoyer sa config initiale (breakpoints) et
     * envoye `run`. Sans ce gate, un script rapide pouvait depasser une ligne
     * a breakpoint avant que le set_breakpoint correspondant n'arrive (course
     * de demarrage). Le reader thread positionne runRequested ; le main thread
     * bloque dans debug_stdio_wait_for_run() jusque-la. EOF (parent disparu)
     * debloque aussi pour ne pas figer le process. */
    volatile bool runRequested;
} g_stdio;

#ifdef _WIN32
static void stdio_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
static void stdio_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/* Mutex pour serialiser les writes stdout depuis le thread (qui repond
 * aux commandes) et plus tard depuis le hook debug (qui pourrait emettre
 * des events asynchrones "stopped"). Sans mutex, deux writes concurrents
 * pourraient entrelacer des bytes et casser le parsing cote parent.
 *
 * Pour V0 le hook ne fait pas d'event lui-meme (le parent polle via
 * get_state). Le mutex est donc inactif en pratique mais en place pour
 * eviter une regression quand on ajoutera les events asynchrones. */
#ifdef _WIN32
static CRITICAL_SECTION g_out_mutex;
#define out_lock()   EnterCriticalSection(&g_out_mutex)
#define out_unlock() LeaveCriticalSection(&g_out_mutex)
#define out_init()   InitializeCriticalSection(&g_out_mutex)
#else
static pthread_mutex_t g_out_mutex = PTHREAD_MUTEX_INITIALIZER;
#define out_lock()   pthread_mutex_lock(&g_out_mutex)
#define out_unlock() pthread_mutex_unlock(&g_out_mutex)
#define out_init()   ((void)0)
#endif

/* ================================================================
 * Send line + receive line
 * ================================================================ */

static void send_line(const char* line) {
    out_lock();
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    out_unlock();
}

/* Lit une ligne sur stdin, sans le \n. Retourne -1 sur EOF. */
static int recv_line(char* buf, int max) {
    if (!fgets(buf, max, stdin)) return -1;
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return n;
}

/* ================================================================
 * Helpers JSON
 * ================================================================ */

static void send_json_line(JsonValue* v) {
    int len = 0;
    char* s = json_serialize(v, &len);
    if (s) {
        send_line(s);
        free(s);
    }
    json_free(v);
}

static void send_response_ok(int64_t id, JsonValue* result) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "id", json_new_int(id));
    json_object_set(msg, "ok", json_new_bool(true));
    if (result) json_object_set(msg, "result", result);
    send_json_line(msg);
}

static void send_response_err(int64_t id, const char* errMsg) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "id", json_new_int(id));
    json_object_set(msg, "ok", json_new_bool(false));
    json_object_set(msg, "error", json_new_string(errMsg ? errMsg : "?"));
    send_json_line(msg);
}

static JsonValue* variable_to_json(const OwlDebugVariable* v) {
    JsonValue* o = json_new_object();
    json_object_set(o, "name",  json_new_string(v->name  ? v->name  : ""));
    json_object_set(o, "value", json_new_string(v->value ? v->value : ""));
    json_object_set(o, "type",  json_new_string(v->type  ? v->type  : "?"));
    json_object_set(o, "ref",   json_new_int(v->childrenRef));
    return o;
}

/* ================================================================
 * Dispatch commandes (identique a celui du listener TCP supprime —
 * meme protocole, transport different)
 * ================================================================ */

static void dispatch_cmd(int64_t id, const char* cmd, JsonValue* args) {
    if (!cmd) { send_response_err(id, "missing cmd"); return; }

    /* --- Breakpoints --- */
    if (strcmp(cmd, "set_breakpoint") == 0) {
        const char* file = args ? json_get_string(args, "file") : NULL;
        int line = args ? (int)json_get_int(args, "line", 0) : 0;
        if (line <= 0) { send_response_err(id, "invalid line"); return; }
        owl_debug_set_breakpoint(file, line);
        send_response_ok(id, NULL);
        return;
    }
    if (strcmp(cmd, "clear_breakpoint") == 0) {
        const char* file = args ? json_get_string(args, "file") : NULL;
        int line = args ? (int)json_get_int(args, "line", 0) : 0;
        owl_debug_clear_breakpoint(file, line);
        send_response_ok(id, NULL);
        return;
    }
    if (strcmp(cmd, "clear_all_breakpoints") == 0) {
        owl_debug_clear_all_breakpoints();
        send_response_ok(id, NULL);
        return;
    }

    /* --- Demarrage --- */
    if (strcmp(cmd, "run") == 0) {
        /* Le debugger a fini d'envoyer sa config (breakpoints) : on libere le
         * main thread qui attend dans debug_stdio_wait_for_run(). */
        g_stdio.runRequested = true;
        send_response_ok(id, NULL);
        return;
    }

    /* --- Controle execution --- */
    if (strcmp(cmd, "continue") == 0)  { owl_debug_continue();      send_response_ok(id, NULL); return; }
    if (strcmp(cmd, "pause") == 0)     { owl_debug_request_pause(); send_response_ok(id, NULL); return; }
    if (strcmp(cmd, "step_over") == 0) { owl_debug_step_over();     send_response_ok(id, NULL); return; }
    if (strcmp(cmd, "step_in") == 0)   { owl_debug_step_in();       send_response_ok(id, NULL); return; }
    if (strcmp(cmd, "step_out") == 0)  { owl_debug_step_out();      send_response_ok(id, NULL); return; }

    /* --- Etat & inspection --- */
    if (strcmp(cmd, "get_state") == 0) {
        JsonValue* res = json_new_object();
        json_object_set(res, "mode", json_new_int(owl_debug_mode()));
        const char* file = NULL; int line = 0;
        if (owl_debug_paused_location(&file, &line)) {
            json_object_set(res, "paused", json_new_bool(true));
            json_object_set(res, "file",
                file ? json_new_string(file) : json_new_null());
            json_object_set(res, "line", json_new_int(line));
        } else {
            json_object_set(res, "paused", json_new_bool(false));
        }
        send_response_ok(id, res);
        return;
    }
    if (strcmp(cmd, "get_stack") == 0) {
        OwlDebugFrame frames[256];
        int n = 0;
        if (!owl_debug_get_stack(frames, 256, &n)) {
            send_response_err(id, "VM not paused"); return;
        }
        JsonValue* arr = json_new_array();
        for (int i = 0; i < n; i++) {
            JsonValue* f = json_new_object();
            json_object_set(f, "index",    json_new_int(frames[i].index));
            json_object_set(f, "function",
                frames[i].functionName ? json_new_string(frames[i].functionName) : json_new_null());
            json_object_set(f, "file",
                frames[i].file ? json_new_string(frames[i].file) : json_new_null());
            json_object_set(f, "line",     json_new_int(frames[i].line));
            json_array_push(arr, f);
        }
        send_response_ok(id, arr);
        return;
    }
    if (strcmp(cmd, "get_locals") == 0) {
        int frameIdx = args ? (int)json_get_int(args, "frame", 0) : 0;
        OwlDebugVariable vars[256];
        int n = 0;
        if (!owl_debug_get_locals(frameIdx, vars, 256, &n)) {
            send_response_err(id, "VM not paused or invalid frame"); return;
        }
        JsonValue* arr = json_new_array();
        for (int i = 0; i < n; i++) json_array_push(arr, variable_to_json(&vars[i]));
        send_response_ok(id, arr);
        return;
    }
    if (strcmp(cmd, "get_globals") == 0) {
        OwlDebugVariable vars[256];
        int n = 0;
        if (!owl_debug_get_globals(vars, 256, &n)) {
            send_response_err(id, "VM not paused"); return;
        }
        JsonValue* arr = json_new_array();
        for (int i = 0; i < n; i++) json_array_push(arr, variable_to_json(&vars[i]));
        send_response_ok(id, arr);
        return;
    }
    if (strcmp(cmd, "get_children") == 0) {
        int ref = args ? (int)json_get_int(args, "ref", 0) : 0;
        OwlDebugVariable vars[256];
        int n = 0;
        if (!owl_debug_get_children(ref, vars, 256, &n)) {
            send_response_err(id, "invalid handle or VM not paused"); return;
        }
        JsonValue* arr = json_new_array();
        for (int i = 0; i < n; i++) json_array_push(arr, variable_to_json(&vars[i]));
        send_response_ok(id, arr);
        return;
    }
    if (strcmp(cmd, "eval") == 0) {
        const char* expr = args ? json_get_string(args, "expr") : NULL;
        char value[64];
        char type[64];
        bool ok = owl_debug_eval(expr ? expr : "", value, type);
        JsonValue* res = json_new_object();
        json_object_set(res, "value", json_new_string(value));
        json_object_set(res, "type",  json_new_string(type));
        json_object_set(res, "ok",    json_new_bool(ok));
        send_response_ok(id, res);
        return;
    }

    send_response_err(id, "unknown command");
}

/* ================================================================
 * Thread principal de lecture stdin
 * ================================================================ */

#ifdef _WIN32
static DWORD WINAPI stdio_thread(LPVOID arg) {
#else
static void* stdio_thread(void* arg) {
#endif
    (void)arg;
    char buf[8192];
    /* Banner d'accueil pour que le parent puisse confirmer la connexion. */
    send_line("{\"event\":\"hello\",\"server\":\"owl-debug-stdio\",\"version\":\"0.1\"}");

    while (g_stdio.running) {
        int n = recv_line(buf, sizeof(buf));
        if (n < 0) {            /* EOF : parent a ferme stdin */
            /* Debloque le gate de demarrage : si le parent disparait avant
             * d'envoyer `run`, le main thread ne doit pas rester fige. */
            g_stdio.runRequested = true;
            break;
        }
        if (n == 0) continue;

        JsonValue* parsed = json_parse(buf, n);
        if (!parsed) {
            send_response_err(0, "malformed JSON");
            continue;
        }
        int64_t id = json_get_int(parsed, "id", 0);
        const char* cmd = json_get_string(parsed, "cmd");
        JsonValue* args = json_get(parsed, "args");
        dispatch_cmd(id, cmd, args);
        json_free(parsed);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================
 * Public API
 * ================================================================ */

/* Callback appele par owl_debug_hook juste avant la pause. On envoie un
 * event JSON sur stdout que le parent (owl-dap.exe) traduira en
 * StoppedEvent DAP. Doit etre non-bloquant — send_line est protege par
 * mutex donc safe vis-a-vis du thread reader. */
static void on_pause(const char* file, int line, const char* reason) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "event",  json_new_string("stopped"));
    json_object_set(msg, "reason", json_new_string(reason ? reason : "pause"));
    if (file) json_object_set(msg, "file", json_new_string(file));
    json_object_set(msg, "line", json_new_int(line));
    int len = 0;
    char* s = json_serialize(msg, &len);
    if (s) {
        send_line(s);
        free(s);
    }
    json_free(msg);
}

bool debug_stdio_start(void) {
    if (g_stdio.started) return true;

#ifdef _WIN32
    /* Stdin/stdout en binary pour ne pas convertir les newlines (le
     * parent ecrit du \n et on doit le voir tel quel). */
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    out_init();
    g_stdio.running = true;
    g_stdio.started = true;
    owl_debug_set_pause_callback(on_pause);

    /* Attach AVANT de lancer le thread : si un breakpoint set tres tot
     * tombe avant que le thread soit pret, la VM doit deja etre "armed". */
    owl_debug_attach();

#ifdef _WIN32
    g_stdio.thread = CreateThread(NULL, 0, stdio_thread, NULL, 0, NULL);
    if (g_stdio.thread == NULL) {
        fprintf(stderr, "[owl-debug-stdio] CreateThread failed\n");
        return false;
    }
#else
    if (pthread_create(&g_stdio.thread, NULL, stdio_thread, NULL) != 0) {
        fprintf(stderr, "[owl-debug-stdio] pthread_create failed\n");
        return false;
    }
#endif
    return true;
}

void debug_stdio_wait_for_run(void) {
    /* Bloque le main thread jusqu'a ce que le debugger envoie `run` (gate
     * anti-course de demarrage), ou jusqu'a EOF stdin (parent disparu). No-op
     * si le mode stdio n'a pas demarre. */
    if (!g_stdio.started) return;
    while (!g_stdio.runRequested) {
        stdio_sleep_ms(2);
    }
}
