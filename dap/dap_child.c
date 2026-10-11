// SPDX-License-Identifier: AGPL-3.0-only
#include "dap_child.h"
#include "dap_server.h"   /* dap_log */
#include "lsp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE pipe_handle_t;
typedef HANDLE thread_handle_t;
typedef HANDLE process_handle_t;
#define PIPE_INVALID INVALID_HANDLE_VALUE
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
typedef int pipe_handle_t;
typedef pthread_t thread_handle_t;
typedef pid_t process_handle_t;
#define PIPE_INVALID (-1)
#endif

/* ================================================================
 * Etat global child (mono-instance)
 * ================================================================ */

static struct {
    pipe_handle_t   inWrite;     /* parent: write end of child's stdin */
    pipe_handle_t   outRead;     /* parent: read end of child's stdout */
    process_handle_t proc;
    thread_handle_t  reader;
    DapChildLineHandler handler;
    volatile bool    running;
    /* Slot reponse synchrone (D.2.5). Une seule commande synchrone a la
     * fois (pattern DAP est sequentiel). pendingId == 0 = aucune attente.
     * Le reader thread voit pendingId != 0 et un id matchant -> stocke
     * la reponse parsee dans pendingResponse et signale via un flag.
     * Le caller busy-wait sur pendingResponse. */
    volatile int64_t pendingId;
    JsonValue* volatile pendingResponse;
} g_child;

/* Mutex pour serialiser dap_child_send() (le thread principal DAP peut
 * envoyer plusieurs commandes, et un futur Pause depuis VSCode pourrait
 * arriver pendant qu'on en envoie une autre). */
#ifdef _WIN32
static CRITICAL_SECTION g_send_mutex;
static bool g_send_mutex_init = false;
static void send_lock_init(void) {
    if (!g_send_mutex_init) { InitializeCriticalSection(&g_send_mutex); g_send_mutex_init = true; }
}
#define send_lock()   EnterCriticalSection(&g_send_mutex)
#define send_unlock() LeaveCriticalSection(&g_send_mutex)
#else
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static void send_lock_init(void) {}
#define send_lock()   pthread_mutex_lock(&g_send_mutex)
#define send_unlock() pthread_mutex_unlock(&g_send_mutex)
#endif

/* ================================================================
 * Pipe write helper portable
 * ================================================================ */

static bool pipe_write_all(pipe_handle_t h, const char* buf, int len) {
#ifdef _WIN32
    DWORD written = 0;
    return WriteFile(h, buf, (DWORD)len, &written, NULL) && (int)written == len;
#else
    int total = 0;
    while (total < len) {
        int n = (int)write(h, buf + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
#endif
}

/* Lit jusqu'a 1 octet, blocking. Retourne -1 sur EOF/erreur. */
static int pipe_read_byte(pipe_handle_t h, char* out) {
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(h, out, 1, &got, NULL) || got == 0) return -1;
    return 1;
#else
    return (int)read(h, out, 1);
#endif
}

/* ================================================================
 * Thread reader : lit stdout child ligne par ligne, dispatche
 * ================================================================ */

#ifdef _WIN32
static DWORD WINAPI reader_thread(LPVOID arg) {
#else
static void* reader_thread(void* arg) {
#endif
    (void)arg;
    char line[8192];
    int n = 0;

    while (g_child.running) {
        char c;
        int r = pipe_read_byte(g_child.outRead, &c);
        if (r <= 0) {
            /* EOF : le child a ferme stdout (probablement parce qu'il
             * vient de mourir). On signale ca au handler avec une
             * ligne speciale {"event":"_child_eof"} pour que dap_server
             * puisse emettre TerminatedEvent et fermer la session. */
            if (g_child.handler) g_child.handler("{\"event\":\"_child_eof\"}");
            break;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            line[n] = '\0';
            if (n > 0) {
                /* Si une commande synchrone est en attente et que cette
                 * ligne est sa reponse (id matchant), on la stocke dans
                 * le slot et on N'appelle PAS le handler (qui est pour
                 * les events et les responses non sollicitees). */
                if (g_child.pendingId != 0) {
                    JsonValue* parsed = json_parse(line, n);
                    if (parsed) {
                        int64_t id = json_get_int(parsed, "id", -1);
                        if (id == g_child.pendingId) {
                            g_child.pendingResponse = parsed;
                            g_child.pendingId = 0;
                            n = 0;
                            continue;
                        }
                        json_free(parsed);
                    }
                }
                if (g_child.handler) g_child.handler(line);
            }
            n = 0;
            continue;
        }
        if (n < (int)sizeof(line) - 1) line[n++] = c;
        /* sinon overflow silencieux : on tronque */
    }

    g_child.running = false;

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================
 * Spawn cross-platform
 * ================================================================ */

#ifdef _WIN32
/* Localise le binaire owl.exe : env var OWL_DEBUGGEE_EXE, sinon meme
 * dossier que owl-dap.exe, sinon PATH. Buffer 1024 alloue par appelant. */
static bool resolve_owl_exe(char* out, int outSize) {
    const char* env = getenv("OWL_DEBUGGEE_EXE");
    if (env && env[0]) {
        snprintf(out, outSize, "%s", env);
        return true;
    }
    char selfPath[512];
    DWORD n = GetModuleFileNameA(NULL, selfPath, sizeof(selfPath));
    if (n > 0 && n < sizeof(selfPath)) {
        char* slash = strrchr(selfPath, '\\');
        if (slash) {
            *(slash + 1) = '\0';
            int written = snprintf(out, outSize, "%sowl.exe", selfPath);
            if (written > 0 && written < outSize) {
                DWORD attr = GetFileAttributesA(out);
                if (attr != INVALID_FILE_ATTRIBUTES) return true;
            }
        }
    }
    /* Fallback : PATH. CreateProcess gere lui-meme. */
    snprintf(out, outSize, "owl.exe");
    return true;
}
#else
static bool resolve_owl_exe(char* out, int outSize) {
    const char* env = getenv("OWL_DEBUGGEE_EXE");
    if (env && env[0]) { snprintf(out, outSize, "%s", env); return true; }
    /* Fallback : PATH via execvp. */
    snprintf(out, outSize, "owl");
    return true;
}
#endif

bool dap_child_spawn(const char* scriptPath, DapChildLineHandler handler) {
    if (g_child.running) {
        dap_log("child already running, refusing to spawn another");
        return false;
    }
    if (!scriptPath) {
        dap_log("dap_child_spawn: scriptPath is NULL");
        return false;
    }

    send_lock_init();
    g_child.handler = handler;

    char owlExe[1024];
    resolve_owl_exe(owlExe, sizeof(owlExe));

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdinR = NULL, hStdinW = NULL;
    HANDLE hStdoutR = NULL, hStdoutW = NULL;

    if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0)) {
        dap_log("CreatePipe stdin failed: %lu", GetLastError());
        return false;
    }
    if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0)) {
        dap_log("CreatePipe stdout failed: %lu", GetLastError());
        CloseHandle(hStdinR); CloseHandle(hStdinW);
        return false;
    }

    /* Le parent garde les ends "non herites" : write-stdin et read-stdout. */
    SetHandleInformation(hStdinW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

    /* Build command line : "owl.exe --debug-stdio script.owl" */
    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" --debug-stdio \"%s\"",
             owlExe, scriptPath);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinR;
    si.hStdOutput = hStdoutW;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);  /* herite stderr du parent */

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        dap_log("CreateProcess(%s) failed: %lu", cmdline, GetLastError());
        CloseHandle(hStdinR);  CloseHandle(hStdinW);
        CloseHandle(hStdoutR); CloseHandle(hStdoutW);
        return false;
    }

    /* Close les ends qui appartiennent au child : on ne les utilise pas. */
    CloseHandle(hStdinR);
    CloseHandle(hStdoutW);
    CloseHandle(pi.hThread);

    g_child.inWrite  = hStdinW;
    g_child.outRead  = hStdoutR;
    g_child.proc     = pi.hProcess;
    g_child.running  = true;
    g_child.reader   = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    if (g_child.reader == NULL) {
        dap_log("CreateThread reader failed");
        TerminateProcess(g_child.proc, 1);
        CloseHandle(g_child.proc);
        CloseHandle(g_child.inWrite);
        CloseHandle(g_child.outRead);
        g_child.running = false;
        return false;
    }
#else
    int pIn[2], pOut[2];
    if (pipe(pIn) < 0) { dap_log("pipe stdin failed"); return false; }
    if (pipe(pOut) < 0) { close(pIn[0]); close(pIn[1]); dap_log("pipe stdout failed"); return false; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pIn[0]); close(pIn[1]); close(pOut[0]); close(pOut[1]);
        dap_log("fork failed"); return false;
    }
    if (pid == 0) {
        /* Child : redirige stdin/stdout, exec owl --debug-stdio script. */
        dup2(pIn[0],  0);
        dup2(pOut[1], 1);
        close(pIn[0]); close(pIn[1]); close(pOut[0]); close(pOut[1]);
        execlp(owlExe, owlExe, "--debug-stdio", scriptPath, (char*)NULL);
        _exit(127);  /* execlp a echoue */
    }
    /* Parent : close les ends qui appartiennent au child. */
    close(pIn[0]);
    close(pOut[1]);

    g_child.inWrite = pIn[1];
    g_child.outRead = pOut[0];
    g_child.proc    = pid;
    g_child.running = true;
    if (pthread_create(&g_child.reader, NULL, reader_thread, NULL) != 0) {
        dap_log("pthread_create reader failed");
        kill(pid, SIGTERM);
        close(g_child.inWrite); close(g_child.outRead);
        g_child.running = false;
        return false;
    }
#endif

    dap_log("spawned: %s --debug-stdio %s", owlExe, scriptPath);
    return true;
}

/* ================================================================
 * Send + Kill + Status
 * ================================================================ */

bool dap_child_send(const char* jsonLine) {
    if (!g_child.running || g_child.inWrite == PIPE_INVALID) return false;
    int len = (int)strlen(jsonLine);

    send_lock();
    bool ok = pipe_write_all(g_child.inWrite, jsonLine, len)
           && pipe_write_all(g_child.inWrite, "\n", 1);
    send_unlock();

    if (!ok) dap_log("write to child stdin failed");
    return ok;
}

bool dap_child_send_json(JsonValue* msg) {
    if (!msg) return false;
    int len = 0;
    char* s = json_serialize(msg, &len);
    bool ok = false;
    if (s) {
        ok = dap_child_send(s);
        free(s);
    }
    json_free(msg);
    return ok;
}

/* Mutex separe pour serialiser les requests synchrones : tant qu'un
 * dap_child_request est en cours, un autre doit attendre. */
#ifdef _WIN32
static CRITICAL_SECTION g_req_mutex;
static bool g_req_mutex_init = false;
static void req_lock_init(void) {
    if (!g_req_mutex_init) { InitializeCriticalSection(&g_req_mutex); g_req_mutex_init = true; }
}
#define req_lock()   EnterCriticalSection(&g_req_mutex)
#define req_unlock() LeaveCriticalSection(&g_req_mutex)
static void req_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
static pthread_mutex_t g_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static void req_lock_init(void) {}
#define req_lock()   pthread_mutex_lock(&g_req_mutex)
#define req_unlock() pthread_mutex_unlock(&g_req_mutex)
static void req_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

/* Compteur global pour generer des ids unique pour le protocole interne. */
static int64_t g_next_internal_id = 1;

JsonValue* dap_child_request(JsonValue* msg, int timeoutMs) {
    if (!msg || !g_child.running) {
        json_free(msg);
        return NULL;
    }
    req_lock_init();
    req_lock();

    int64_t id = ++g_next_internal_id;
    json_object_set(msg, "id", json_new_int(id));

    g_child.pendingResponse = NULL;
    g_child.pendingId = id;

    if (!dap_child_send_json(msg)) {
        g_child.pendingId = 0;
        req_unlock();
        return NULL;
    }

    /* Busy-wait avec sleep 10ms. Pour un debugger humain, latence
     * imperceptible. Si on devient performance-critical un jour,
     * on passera en condvar. */
    int waited = 0;
    while (g_child.pendingResponse == NULL && waited < timeoutMs && g_child.running) {
        req_sleep_ms(10);
        waited += 10;
    }

    JsonValue* result = g_child.pendingResponse;
    g_child.pendingResponse = NULL;
    g_child.pendingId = 0;

    req_unlock();
    return result;  /* NULL si timeout */
}

void dap_child_kill(void) {
    if (!g_child.running) return;

    /* Graceful d'abord : close stdin -> child voit EOF, son thread debug
     * sort de fgets, le main thread peut terminer apres la VM. */
    if (g_child.inWrite != PIPE_INVALID) {
#ifdef _WIN32
        CloseHandle(g_child.inWrite);
#else
        close(g_child.inWrite);
#endif
        g_child.inWrite = PIPE_INVALID;
    }

    /* Attente bornee 500ms puis hard kill si toujours la. */
#ifdef _WIN32
    DWORD waitResult = WaitForSingleObject(g_child.proc, 500);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(g_child.proc, 1);
        WaitForSingleObject(g_child.proc, 1000);
    }
    CloseHandle(g_child.proc);
    if (g_child.outRead != PIPE_INVALID) CloseHandle(g_child.outRead);
    g_child.running = false;
    WaitForSingleObject(g_child.reader, 1000);
    CloseHandle(g_child.reader);
#else
    int status = 0;
    int loops = 5;
    while (loops-- > 0) {
        if (waitpid(g_child.proc, &status, WNOHANG) > 0) goto reaped;
        usleep(100 * 1000);
    }
    kill(g_child.proc, SIGTERM);
    waitpid(g_child.proc, &status, 0);
reaped:
    if (g_child.outRead != PIPE_INVALID) close(g_child.outRead);
    g_child.running = false;
    pthread_join(g_child.reader, NULL);
#endif

    g_child.outRead = PIPE_INVALID;
    g_child.inWrite = PIPE_INVALID;
    dap_log("child terminated");
}

bool dap_child_is_running(void) {
    return g_child.running;
}
