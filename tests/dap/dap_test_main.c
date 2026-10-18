/* tests/dap/dap_test_main.c — Harness boite noire pour le Debug Adapter OWL.
 *
 * Lance via 'make test-dap'. Contrairement au harness LSP (in-process),
 * le DAP se teste de bout en bout : on spawn owl-dap.exe, on lui parle en
 * vrai protocole DAP (framing Content-Length sur stdin/stdout), et on
 * observe ses responses + events. owl-dap spawn lui-meme owl.exe en
 * --debug-stdio (resolu via OWL_DEBUGGEE_EXE), donc un test exerce toute
 * la stack : serveur DAP + IPC child + runtime VM.
 *
 * Lecture a timeout (PeekNamedPipe / poll), pas de sleep aveugle : on
 * bloque sur le read jusqu'au message attendu, fail si deadline depassee.
 *
 * Chaque test affiche [OK]/[FAIL]. Code retour 0 si tous OK, 1 sinon.
 *
 * Defauts overridables par env :
 *   OWL_DAP_EXE   : chemin owl-dap.exe   (defaut build/owl-dap.exe)
 *   OWL_EXE       : chemin owl.exe       (defaut build/owl.exe, -> OWL_DEBUGGEE_EXE)
 *   OWL_DAP_FIXTURES : dossier fixtures  (defaut tests/dap/fixtures)
 */

#include "lsp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#endif

/* ================================================================
 * Horloge monotone (ms)
 * ================================================================ */
static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ================================================================
 * Process owl-dap spawne + pipes
 * ================================================================ */
typedef struct {
#ifdef _WIN32
    HANDLE inWrite;     /* parent -> child stdin */
    HANDLE outRead;     /* child stdout -> parent */
    HANDLE proc;
#else
    int    inWrite;
    int    outRead;
    pid_t  proc;
#endif
} DapProc;

/* Resout un chemin relatif en absolu (alloue, a free par l'appelant). */
static char* abspath_dup(const char* rel) {
#ifdef _WIN32
    char buf[1024];
    if (_fullpath(buf, rel, sizeof(buf))) return strdup(buf);
#else
    char buf[PATH_MAX];
    if (realpath(rel, buf)) return strdup(buf);
#endif
    return strdup(rel);
}

static const char* env_or(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static bool dap_spawn(DapProc* p) {
    const char* dapExe = env_or("OWL_DAP_EXE", "build/owl-dap.exe");
    const char* owlExe = env_or("OWL_EXE", "build/owl.exe");
    char* owlAbs = abspath_dup(owlExe);

#ifdef _WIN32
    /* owl-dap resout son child via OWL_DEBUGGEE_EXE -> on le pointe sur
     * notre owl.exe pour un spawn deterministe quel que soit le cwd. */
    SetEnvironmentVariableA("OWL_DEBUGGEE_EXE", owlAbs);

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    if (!CreatePipe(&inR, &inW, &sa, 0))  { free(owlAbs); return false; }
    if (!CreatePipe(&outR, &outW, &sa, 0)){ CloseHandle(inR); CloseHandle(inW); free(owlAbs); return false; }
    SetHandleInformation(inW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    char* dapAbs = abspath_dup(dapExe);
    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline), "\"%s\"", dapAbs);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    free(dapAbs);
    free(owlAbs);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); return false; }
    CloseHandle(pi.hThread);
    p->inWrite = inW;
    p->outRead = outR;
    p->proc = pi.hProcess;
    return true;
#else
    setenv("OWL_DEBUGGEE_EXE", owlAbs, 1);
    char* dapAbs = abspath_dup(dapExe);

    int inPipe[2], outPipe[2];
    if (pipe(inPipe) != 0)  { free(owlAbs); free(dapAbs); return false; }
    if (pipe(outPipe) != 0) { close(inPipe[0]); close(inPipe[1]); free(owlAbs); free(dapAbs); return false; }

    pid_t pid = fork();
    if (pid < 0) { free(owlAbs); free(dapAbs); return false; }
    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        execlp(dapAbs, dapAbs, (char*)NULL);
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    free(owlAbs);
    free(dapAbs);
    p->inWrite = inPipe[1];
    p->outRead = outPipe[0];
    p->proc = pid;
    return true;
#endif
}

/* Forward decls : dap_kill ferme proprement via ces helpers, definis plus bas. */
static bool dap_write_request(DapProc* p, int seq, const char* command, const char* argsJson);
static int  read_byte_deadline(DapProc* p, char* out, long long deadline);

/* Fermeture propre. CRITIQUE pour le gating make : sans disconnect, un
 * TerminateProcess brutal d'owl-dap laisse son child owl.exe ORPHELIN
 * (owl-dap n'a pas eu la main pour le tuer), et l'orphelin peut wedger
 * des locks fichier. On envoie donc disconnect (owl-dap -> handle_disconnect
 * -> dap_child_kill + shouldStop -> exit), on draine jusqu'a EOF, puis
 * TerminateProcess en simple filet de securite. */
static void dap_kill(DapProc* p) {
    if (p->inWrite) {
        dap_write_request(p, 999999, "disconnect", "{}");
        /* Draine la sortie jusqu'a EOF (owl-dap a ferme stdout en sortant)
         * ou deadline courte. EOF = -1 -> owl-dap et son child sont morts. */
        long long deadline = now_ms() + 2000;
        char c;
        while (read_byte_deadline(p, &c, deadline) == 1) { /* discard */ }
    }
#ifdef _WIN32
    if (p->inWrite)  CloseHandle(p->inWrite);
    if (p->proc)     { TerminateProcess(p->proc, 0); WaitForSingleObject(p->proc, 2000); CloseHandle(p->proc); }
    if (p->outRead)  CloseHandle(p->outRead);
#else
    if (p->inWrite >= 0) close(p->inWrite);
    if (p->proc > 0) { kill(p->proc, SIGKILL); waitpid(p->proc, NULL, 0); }
    if (p->outRead >= 0) close(p->outRead);
#endif
    p->inWrite = 0; p->outRead = 0; p->proc = 0;
}

/* ================================================================
 * Write / read framing DAP (Content-Length)
 * ================================================================ */
static bool dap_write_request(DapProc* p, int seq, const char* command, const char* argsJson) {
    char body[4096];
    snprintf(body, sizeof(body),
        "{\"seq\":%d,\"type\":\"request\",\"command\":\"%s\",\"arguments\":%s}",
        seq, command, argsJson ? argsJson : "{}");
    char frame[4608];
    int n = snprintf(frame, sizeof(frame), "Content-Length: %d\r\n\r\n%s",
                     (int)strlen(body), body);
#ifdef _WIN32
    DWORD written = 0;
    return WriteFile(p->inWrite, frame, (DWORD)n, &written, NULL) && (int)written == n;
#else
    int total = 0;
    while (total < n) {
        ssize_t w = write(p->inWrite, frame + total, n - total);
        if (w <= 0) return false;
        total += (int)w;
    }
    return true;
#endif
}

/* Lit un octet avec deadline. Retourne 1=ok, 0=timeout, -1=EOF/erreur. */
static int read_byte_deadline(DapProc* p, char* out, long long deadline) {
    for (;;) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(p->outRead, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(p->outRead, out, 1, &got, NULL) || got == 0) return -1;
            return 1;
        }
        if (now_ms() > deadline) return 0;
        Sleep(2);
#else
        struct pollfd pfd = { p->outRead, POLLIN, 0 };
        long long rem = deadline - now_ms();
        if (rem < 0) rem = 0;
        int pr = poll(&pfd, 1, (int)rem);
        if (pr == 0) return 0;
        if (pr < 0) return -1;
        ssize_t r = read(p->outRead, out, 1);
        if (r <= 0) return -1;
        return 1;
#endif
    }
}

/* Lit un message DAP complet (parse le framing Content-Length, retourne
 * le body parse en JsonValue, a free par l'appelant). NULL si timeout/EOF. */
static JsonValue* dap_read_message(DapProc* p, int timeoutMs) {
    long long deadline = now_ms() + timeoutMs;
    char header[256];
    int hn = 0;
    /* Lit l'entete jusqu'a "\r\n\r\n". */
    for (;;) {
        char c;
        int r = read_byte_deadline(p, &c, deadline);
        if (r <= 0) return NULL;
        if (hn < (int)sizeof(header) - 1) header[hn++] = c;
        if (hn >= 4 && header[hn-4]=='\r' && header[hn-3]=='\n'
                    && header[hn-2]=='\r' && header[hn-1]=='\n') break;
    }
    header[hn] = '\0';
    const char* cl = strstr(header, "Content-Length:");
    if (!cl) return NULL;
    int len = atoi(cl + 15);
    if (len <= 0 || len > 1 << 20) return NULL;

    char* body = malloc((size_t)len + 1);
    if (!body) return NULL;
    for (int i = 0; i < len; i++) {
        char c;
        int r = read_byte_deadline(p, &c, deadline);
        if (r <= 0) { free(body); return NULL; }
        body[i] = c;
    }
    body[len] = '\0';
    JsonValue* v = json_parse(body, len);
    free(body);
    return v;
}

/* Lit des messages jusqu'a en trouver un avec event==wantEvent (ou
 * type==response && command==wantCmd si wantEvent==NULL). Retourne le
 * message (a free) ou NULL si timeout cumulé / EOF. */
static JsonValue* dap_wait_for(DapProc* p, const char* wantEvent, const char* wantCmd, int timeoutMs) {
    long long deadline = now_ms() + timeoutMs;
    for (;;) {
        int rem = (int)(deadline - now_ms());
        if (rem <= 0) return NULL;
        JsonValue* msg = dap_read_message(p, rem);
        if (!msg) return NULL;
        const char* type = json_get_string(msg, "type");
        if (wantEvent && type && strcmp(type, "event") == 0) {
            const char* ev = json_get_string(msg, "event");
            if (ev && strcmp(ev, wantEvent) == 0) return msg;
            /* terminated/exited inattendu pendant qu'on attend autre chose :
             * abandonne (le scenario a fini trop tot). */
            if (ev && (strcmp(ev, "terminated") == 0)) { json_free(msg); return NULL; }
        }
        if (wantCmd && type && strcmp(type, "response") == 0) {
            const char* cmd = json_get_string(msg, "command");
            if (cmd && strcmp(cmd, wantCmd) == 0) return msg;
        }
        json_free(msg);
    }
}

/* ================================================================
 * Assertions
 * ================================================================ */
static int g_pass = 0, g_fail = 0;

static void report(const char* name, bool ok, const char* detail) {
    if (ok) { g_pass++; printf("[OK]   %s\n", name); }
    else    { g_fail++; printf("[FAIL] %s%s%s\n", name,
                                detail ? " — " : "", detail ? detail : ""); }
}

/* Extrait source.path d'un body d'event stopped. */
static const char* stopped_path(JsonValue* msg) {
    JsonValue* body = json_get(msg, "body");
    if (!body) return NULL;
    JsonValue* src = json_get(body, "source");
    if (!src) return NULL;
    return json_get_string(src, "path");
}
static int64_t stopped_line(JsonValue* msg) {
    JsonValue* body = json_get(msg, "body");
    return body ? json_get_int(body, "line", -1) : -1;
}

/* Vrai si le path se termine par le basename attendu (tolerant slash). */
static bool path_endswith(const char* path, const char* base) {
    if (!path || !base) return false;
    size_t lp = strlen(path), lb = strlen(base);
    if (lb > lp) return false;
    return strcmp(path + (lp - lb), base) == 0;
}

/* ================================================================
 * Sequence DAP commune jusqu'a configurationDone
 * ================================================================ */
static bool dap_handshake_launch(DapProc* p, const char* programAbs, int* seq) {
    char args[1280];
    if (!dap_write_request(p, (*seq)++, "initialize", "{}")) return false;
    JsonValue* r = dap_wait_for(p, NULL, "initialize", 4000);
    if (!r) return false;
    bool ok = json_get_bool(r, "success", false);
    json_free(r);
    if (!ok) return false;

    snprintf(args, sizeof(args), "{\"program\":\"%s\"}", programAbs);
    if (!dap_write_request(p, (*seq)++, "launch", args)) return false;
    r = dap_wait_for(p, NULL, "launch", 4000);
    if (!r) return false;
    json_free(r);
    return true;
}

/* Echappe les backslashes Windows pour un literal JSON. */
static void json_escape_path(const char* in, char* out, int outSize) {
    int j = 0;
    for (int i = 0; in[i] && j < outSize - 2; i++) {
        if (in[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else out[j++] = in[i];
    }
    out[j] = '\0';
}

static bool dap_set_breakpoints(DapProc* p, int* seq, const char* fileAbs, const int* lines, int nLines) {
    char esc[1024]; json_escape_path(fileAbs, esc, sizeof(esc));
    char args[2048];
    int n = snprintf(args, sizeof(args),
        "{\"source\":{\"path\":\"%s\"},\"breakpoints\":[", esc);
    for (int i = 0; i < nLines; i++)
        n += snprintf(args + n, sizeof(args) - n, "%s{\"line\":%d}", i ? "," : "", lines[i]);
    snprintf(args + n, sizeof(args) - n, "]}");
    if (!dap_write_request(p, (*seq)++, "setBreakpoints", args)) return false;
    JsonValue* r = dap_wait_for(p, NULL, "setBreakpoints", 4000);
    if (!r) return false;
    json_free(r);
    return true;
}

/* ================================================================
 * Tests
 * ================================================================ */

/* T1 : handshake initialize -> response success + event initialized. */
static void test_handshake(void) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("handshake_initialize", false, "spawn owl-dap echoue"); return; }

    int seq = 1;
    bool ok = dap_write_request(&p, seq++, "initialize", "{}");
    JsonValue* resp = ok ? dap_wait_for(&p, NULL, "initialize", 4000) : NULL;
    bool respOk = resp && json_get_bool(resp, "success", false);
    if (resp) json_free(resp);
    JsonValue* ev = dap_wait_for(&p, "initialized", NULL, 4000);
    bool evOk = ev != NULL;
    if (ev) json_free(ev);

    report("handshake_initialize", respOk && evOk,
           !respOk ? "pas de response initialize success" : (!evOk ? "pas d'event initialized" : NULL));
    dap_kill(&p);
}

/* T2 : breakpoints cross-fichiers (entree + classe voisine). Les deux
 * doivent hit, chacun dans son fichier, dans l'ordre d'execution
 * (cCalc.owl:10 pendant l'appel, puis main.owl:3). C'est le regression
 * guard du fix multi-fichiers (cache path -> [lines]). */
static void test_cross_file_breakpoints(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("cross_file_breakpoints", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024], calcRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/cross_file/main.owl", fixtures);
    snprintf(calcRel, sizeof(calcRel), "%s/cross_file/cCalc.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char* calcAbs = abspath_dup(calcRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    /* bp dans la classe enregistre EN PREMIER, puis dans l'entree : avant
     * le fix, le 2e setBreakpoints ecrasait le 1er (cache mono-fichier). */
    int lc[1] = {10}, lm[1] = {3};
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, calcAbs, lc, 1);
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, mainAbs, lm, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool hitClass = false, hitMain = false;
    if (seqOk) {
        /* 1er stop attendu : cCalc.owl:10 */
        JsonValue* s1 = dap_wait_for(&p, "stopped", NULL, 6000);
        if (s1) {
            hitClass = path_endswith(stopped_path(s1), "cCalc.owl") && stopped_line(s1) == 10;
            json_free(s1);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
            /* 2e stop attendu : main.owl:3 */
            JsonValue* s2 = dap_wait_for(&p, "stopped", NULL, 6000);
            if (s2) {
                hitMain = path_endswith(stopped_path(s2), "main.owl") && stopped_line(s2) == 3;
                json_free(s2);
                dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
            }
        }
    }
    report("cross_file_breakpoints", seqOk && hitClass && hitMain,
           !seqOk ? "sequence DAP echouee"
                  : (!hitClass ? "breakpoint classe (cCalc.owl:10) non hit — bug multi-fichiers ?"
                               : (!hitMain ? "breakpoint entree (main.owl:3) non hit" : NULL)));
    free(mainAbs); free(calcAbs);
    dap_kill(&p);
}

/* T3 : un breakpoint UNIQUEMENT dans la classe, suivi d'un setBreakpoints
 * VIDE sur l'entree (cas : aucun bp dans main cote VSCode). Avant le fix,
 * le setBreakpoints vide de main ecrasait le cache et le bp classe etait
 * perdu. Apres fix : le bp classe survit et hit. */
static void test_empty_other_file_preserves_class_bp(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("empty_setbp_preserves_class", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024], calcRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/cross_file/main.owl", fixtures);
    snprintf(calcRel, sizeof(calcRel), "%s/cross_file/cCalc.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char* calcAbs = abspath_dup(calcRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int lc[1] = {10};
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, calcAbs, lc, 1);
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, mainAbs, NULL, 0);  /* vide */
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool hitClass = false;
    if (seqOk) {
        JsonValue* s1 = dap_wait_for(&p, "stopped", NULL, 6000);
        if (s1) {
            hitClass = path_endswith(stopped_path(s1), "cCalc.owl") && stopped_line(s1) == 10;
            json_free(s1);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    report("empty_setbp_preserves_class", seqOk && hitClass,
           !seqOk ? "sequence DAP echouee"
                  : (!hitClass ? "bp classe efface par setBreakpoints vide sur l'entree — regression" : NULL));
    free(mainAbs); free(calcAbs);
    dap_kill(&p);
}

/* T4 : le scope Globals ne doit exposer QUE les variables user, pas les
 * constantes framework (langues, couleurs, MIME...). Regression guard du
 * snapshot de globales systeme (fix globals-pollution). */
static void test_globals_only_user(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("globals_only_user", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/globals_filter/main.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int bl[1] = {3};  /* Trace("stop") */
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, mainAbs, bl, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool hasUser = false, hasFramework = false, gotVars = false;
    if (seqOk) {
        JsonValue* s = dap_wait_for(&p, "stopped", NULL, 8000);
        if (s) {
            json_free(s);
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":1}");
            JsonValue* r = dap_wait_for(&p, NULL, "variables", 4000);
            if (r && json_get_bool(r, "success", false)) {
                JsonValue* body = json_get(r, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    gotVars = true;
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                        if (!nm) continue;
                        if (strcmp(nm, "userAlpha") == 0) hasUser = true;
                        /* echantillon de constantes framework qui fuyaient avant le fix */
                        if (strcmp(nm, "languageFrench") == 0 || strcmp(nm, "White") == 0
                            || strcmp(nm, "mimeTypePDF") == 0) hasFramework = true;
                    }
                }
            }
            if (r) json_free(r);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    report("globals_only_user", seqOk && gotVars && hasUser && !hasFramework,
           !seqOk ? "sequence DAP echouee"
                  : (!gotVars ? "pas de reponse variables (globals)"
                              : (!hasUser ? "variable user 'userAlpha' absente des globals"
                                          : (hasFramework ? "constante framework fuite dans les globals — regression" : NULL))));
    free(mainAbs);
    dap_kill(&p);
}

/* T5 : en pause dans le corps d'une PROCEDURE top-level, le debugger doit
 * exposer la frame de la procedure (stackTrace) et ses param/locales (scope
 * Locals). Regression guard du fix line-info : avant, le bp sur
 * une ligne de corps tapait dans la frame script (1 frame <main>, locals
 * vides) a cause d'une mauvaise attribution de ligne sur l'OP de definition. */
static void test_procedure_frame_locals(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("procedure_frame_locals", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/proc_locals/main.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int bl[1] = {3};  /* Trace("dans calcul"), dans le corps de calcul */
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, mainAbs, bl, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool hasProcFrame = false, hasParam = false, hasLoc = false, gotStack = false;
    if (seqOk) {
        JsonValue* s = dap_wait_for(&p, "stopped", NULL, 8000);
        if (s) {
            json_free(s);
            /* stackTrace : doit contenir une frame nommee "calcul". */
            dap_write_request(&p, seq++, "stackTrace", "{\"threadId\":1}");
            JsonValue* st = dap_wait_for(&p, NULL, "stackTrace", 4000);
            if (st && json_get_bool(st, "success", false)) {
                JsonValue* body = json_get(st, "body");
                JsonValue* frames = body ? json_get(body, "stackFrames") : NULL;
                if (frames && frames->type == JSON_ARRAY) {
                    gotStack = true;
                    for (int i = 0; i < frames->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(frames->as.arrayVal.items[i], "name");
                        if (nm && strcmp(nm, "calcul") == 0) hasProcFrame = true;
                    }
                }
            }
            if (st) json_free(st);
            /* Locals frame 0 : doit contenir param + loc. */
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":100}");
            JsonValue* v = dap_wait_for(&p, NULL, "variables", 4000);
            if (v && json_get_bool(v, "success", false)) {
                JsonValue* body = json_get(v, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                        if (!nm) continue;
                        if (strcmp(nm, "param") == 0) hasParam = true;
                        if (strcmp(nm, "loc") == 0) hasLoc = true;
                    }
                }
            }
            if (v) json_free(v);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    report("procedure_frame_locals", seqOk && gotStack && hasProcFrame && hasParam && hasLoc,
           !seqOk ? "sequence DAP echouee"
                  : (!gotStack ? "pas de stackTrace"
                              : (!hasProcFrame ? "frame 'calcul' absente du call stack (bp dans frame script ?)"
                                              : ((!hasParam || !hasLoc) ? "param/loc absents des Locals de la procedure" : NULL))));
    free(mainAbs);
    dap_kill(&p);
}

/* T6 : en pause dans une METHODE de classe, le scope Locals doit nommer le
 * receveur `this` (slot 0), pas "slot 0". + le param `n`. Regression guard du
 * fix d'etiquetage du slot receveur. */
static void test_method_this_label(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("method_this_label", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024], calcRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/method_this/main.owl", fixtures);
    snprintf(calcRel, sizeof(calcRel), "%s/method_this/cCalc.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char* calcAbs = abspath_dup(calcRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int bl[1] = {8};  /* corps de cCalc.Ajoute */
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, calcAbs, bl, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool hasThis = false, hasN = false, hasRawSlot = false, gotVars = false;
    if (seqOk) {
        JsonValue* s = dap_wait_for(&p, "stopped", NULL, 8000);
        if (s) {
            json_free(s);
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":100}");
            JsonValue* v = dap_wait_for(&p, NULL, "variables", 4000);
            if (v && json_get_bool(v, "success", false)) {
                JsonValue* body = json_get(v, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    gotVars = true;
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                        if (!nm) continue;
                        if (strcmp(nm, "this") == 0) hasThis = true;
                        if (strcmp(nm, "n") == 0) hasN = true;
                        if (strncmp(nm, "slot ", 5) == 0) hasRawSlot = true;
                    }
                }
            }
            if (v) json_free(v);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    report("method_this_label", seqOk && gotVars && hasThis && hasN && !hasRawSlot,
           !seqOk ? "sequence DAP echouee"
                  : (!gotVars ? "pas de reponse variables"
                              : (!hasThis ? "receveur non nomme 'this' dans les Locals"
                                          : (hasRawSlot ? "slot brut 'slot N' encore expose" : NULL))));
    free(mainAbs); free(calcAbs);
    dap_kill(&p);
}

/* T7 : breakpoint sur la DERNIERE ligne du corps d'une methode de classe
 * (heritage). Garde du fix line-info des methodes (meme classe de bug que
 *  pour les procedures, applique a compile_inline_class_method) :
 * avant, l'OP de definition de la methode dans le chunk de classe portait la
 * ligne du dernier token du corps, donc le bp tapait pendant la definition de
 * la classe (frame fichier-script) au lieu de l'execution. On verifie aussi le
 * drill-down du receveur : champs herites (sNom/nPattes) + propre (bInterieur). */
static void test_inherited_method_frame(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("inherited_method_frame", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024], chatRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/inherit_method/main.owl", fixtures);
    snprintf(chatRel, sizeof(chatRel), "%s/inherit_method/cChat.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char* chatAbs = abspath_dup(chatRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int bl[1] = {10};  /* RENVOYER desc, derniere ligne du corps de Presenter */
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, chatAbs, bl, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool inMethodFrame = false, hasThis = false, gotFields = false;
    bool hasInherited = false, hasOwn = false;
    if (seqOk) {
        JsonValue* s = dap_wait_for(&p, "stopped", NULL, 9000);
        if (s) {
            /* doit s'arreter dans cChat.owl (pas une frame script en cours
             * de definition) — on verifie la frame top via stackTrace. */
            json_free(s);
            dap_write_request(&p, seq++, "stackTrace", "{\"threadId\":1}");
            JsonValue* st = dap_wait_for(&p, NULL, "stackTrace", 4000);
            if (st && json_get_bool(st, "success", false)) {
                JsonValue* body = json_get(st, "body");
                JsonValue* frames = body ? json_get(body, "stackFrames") : NULL;
                if (frames && frames->type == JSON_ARRAY && frames->as.arrayVal.count > 0) {
                    const char* topName = json_get_string(frames->as.arrayVal.items[0], "name");
                    /* frame methode = nom de la methode (pas "<main>"). */
                    if (topName && strcmp(topName, "<main>") != 0) inMethodFrame = true;
                }
            }
            if (st) json_free(st);
            /* Locals : this present ; on recupere sa variablesReference pour driller. */
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":100}");
            JsonValue* v = dap_wait_for(&p, NULL, "variables", 4000);
            int64_t thisRef = 0;
            if (v && json_get_bool(v, "success", false)) {
                JsonValue* body = json_get(v, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        JsonValue* it = vars->as.arrayVal.items[i];
                        const char* nm = json_get_string(it, "name");
                        if (nm && strcmp(nm, "this") == 0) {
                            hasThis = true;
                            thisRef = json_get_int(it, "variablesReference", 0);
                        }
                    }
                }
            }
            if (v) json_free(v);
            /* Drill-down du receveur : champs herites + propre. */
            if (thisRef > 0) {
                char args[64];
                snprintf(args, sizeof(args), "{\"variablesReference\":%lld}", (long long)thisRef);
                dap_write_request(&p, seq++, "variables", args);
                JsonValue* d = dap_wait_for(&p, NULL, "variables", 4000);
                if (d && json_get_bool(d, "success", false)) {
                    JsonValue* body = json_get(d, "body");
                    JsonValue* vars = body ? json_get(body, "variables") : NULL;
                    if (vars && vars->type == JSON_ARRAY) {
                        gotFields = true;
                        for (int i = 0; i < vars->as.arrayVal.count; i++) {
                            const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                            if (!nm) continue;
                            if (strcmp(nm, "sNom") == 0 || strcmp(nm, "nPattes") == 0) hasInherited = true;
                            if (strcmp(nm, "bInterieur") == 0) hasOwn = true;
                        }
                    }
                }
                if (d) json_free(d);
            }
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    bool ok = seqOk && inMethodFrame && hasThis && gotFields && hasInherited && hasOwn;
    report("inherited_method_frame", ok,
           !seqOk ? "sequence DAP echouee"
                  : (!inMethodFrame ? "stop dans frame script (def de classe) au lieu de la methode — regression line-info"
                                   : (!hasThis ? "receveur 'this' absent"
                                              : (!gotFields ? "drill-down receveur vide"
                                                           : (!hasInherited ? "champ herite (sNom/nPattes) absent"
                                                                           : (!hasOwn ? "champ propre (bInterieur) absent" : NULL))))));
    free(mainAbs); free(chatAbs);
    dap_kill(&p);
}

/* T8 : breakpoint dans un handler d'evenement de fenetre (Clic d'un bouton),
 * declenche par TestSimuleSouris en mode headless. Verifie que le debug d'UI
 * fonctionne : la frame du handler est nommee (`Clic`), l'appelant est present
 * (2 frames), et les locales du handler sont exposees (`increment`). Ouvre()
 * fait assez de setup pour que le breakpoint soit applique avant le clic
 * simule (pas besoin de warmup). */
static void test_window_event_handler(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("window_event_handler", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024], fenRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/window_handler/main.owl", fixtures);
    snprintf(fenRel, sizeof(fenRel), "%s/window_handler/FEN_Test.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char* fenAbs = abspath_dup(fenRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    int bl[1] = {18};  /* Trace dans le handler Clic */
    seqOk = seqOk && dap_set_breakpoints(&p, &seq, fenAbs, bl, 1);
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool inHandlerFrame = false, hasCaller = false, hasLocal = false, gotStack = false;
    bool gotGlobals = false, hasWinVar = false, hasUiProxy = false;
    if (seqOk) {
        JsonValue* s = dap_wait_for(&p, "stopped", NULL, 8000);
        if (s) {
            json_free(s);
            dap_write_request(&p, seq++, "stackTrace", "{\"threadId\":1}");
            JsonValue* st = dap_wait_for(&p, NULL, "stackTrace", 4000);
            if (st && json_get_bool(st, "success", false)) {
                JsonValue* body = json_get(st, "body");
                JsonValue* frames = body ? json_get(body, "stackFrames") : NULL;
                if (frames && frames->type == JSON_ARRAY && frames->as.arrayVal.count > 0) {
                    gotStack = true;
                    const char* top = json_get_string(frames->as.arrayVal.items[0], "name");
                    if (top && strcmp(top, "surClic") == 0) inHandlerFrame = true;
                    if (frames->as.arrayVal.count >= 2) hasCaller = true;
                }
            }
            if (st) json_free(st);
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":100}");
            JsonValue* v = dap_wait_for(&p, NULL, "variables", 4000);
            if (v && json_get_bool(v, "success", false)) {
                JsonValue* body = json_get(v, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                        if (nm && strcmp(nm, "increment") == 0) hasLocal = true;
                    }
                }
            }
            if (v) json_free(v);
            /* Globals : les proxies UI (ObjWindowRef FEN_Test, ObjUiControlRef
             * BTN_Go) doivent etre filtres ; la vraie variable de fenetre
             * (compteur) reste visible. */
            dap_write_request(&p, seq++, "variables", "{\"variablesReference\":1}");
            JsonValue* g = dap_wait_for(&p, NULL, "variables", 4000);
            if (g && json_get_bool(g, "success", false)) {
                JsonValue* body = json_get(g, "body");
                JsonValue* vars = body ? json_get(body, "variables") : NULL;
                if (vars && vars->type == JSON_ARRAY) {
                    gotGlobals = true;
                    for (int i = 0; i < vars->as.arrayVal.count; i++) {
                        const char* nm = json_get_string(vars->as.arrayVal.items[i], "name");
                        if (!nm) continue;
                        if (strcmp(nm, "compteur") == 0) hasWinVar = true;
                        if (strcmp(nm, "FEN_Test") == 0 || strcmp(nm, "BTN_Go") == 0) hasUiProxy = true;
                    }
                }
            }
            if (g) json_free(g);
            dap_write_request(&p, seq++, "continue", "{\"threadId\":1}");
        }
    }
    report("window_event_handler",
           seqOk && gotStack && inHandlerFrame && hasCaller && hasLocal
               && gotGlobals && hasWinVar && !hasUiProxy,
           !seqOk ? "sequence DAP echouee"
                  : (!gotStack ? "pas de stackTrace (handler non atteint ?)"
                              : (!inHandlerFrame ? "frame top != 'surClic' (handler non resolu)"
                                                : (!hasCaller ? "appelant absent du call stack"
                                                             : (!hasLocal ? "locale 'increment' absente du handler"
                                                                         : (!gotGlobals ? "pas de scope Globals"
                                                                                       : (!hasWinVar ? "variable fenetre 'compteur' absente des Globals"
                                                                                                    : (hasUiProxy ? "proxy UI (FEN_Test/BTN_Go) fuite dans Globals" : NULL))))))));
    free(mainAbs); free(fenAbs);
    dap_kill(&p);
}

/* T9 : la sortie Trace() du debuggee (texte brut sur stdout, PAS du JSON
 * protocole) doit remonter en OutputEvent DAP category=stdout dans la Debug
 * Console. Regression guard : avant le fix, on_child_line jetait toute ligne
 * non-JSON (parse echoue -> return), donc Trace() n'arrivait jamais a VSCode.
 * On lance le fixture sans breakpoint (run jusqu'au bout) et on attend un
 * event output dont la category est stdout et dont le texte contient le
 * marqueur imprime par Trace. */
static void test_trace_output_forwarded(const char* fixtures) {
    DapProc p = {0};
    if (!dap_spawn(&p)) { report("trace_output_forwarded", false, "spawn owl-dap echoue"); return; }

    char mainRel[1024];
    snprintf(mainRel, sizeof(mainRel), "%s/trace_output/main.owl", fixtures);
    char* mainAbs = abspath_dup(mainRel);
    char mainEsc[1024]; json_escape_path(mainAbs, mainEsc, sizeof(mainEsc));

    int seq = 1;
    bool seqOk = dap_handshake_launch(&p, mainEsc, &seq);
    /* Aucun breakpoint : le script Trace puis se termine. */
    seqOk = seqOk && dap_write_request(&p, seq++, "configurationDone", "{}");

    bool gotStdoutTrace = false;
    if (seqOk) {
        /* Boucle sur les events output jusqu'a trouver celui du marqueur
         * (d'autres lignes stdout peuvent precede), ou deadline. */
        long long deadline = now_ms() + 8000;
        for (;;) {
            int rem = (int)(deadline - now_ms());
            if (rem <= 0) break;
            JsonValue* ev = dap_wait_for(&p, "output", NULL, rem);
            if (!ev) break;  /* timeout, EOF ou terminated : plus d'output a venir */
            JsonValue* body = json_get(ev, "body");
            const char* category = body ? json_get_string(body, "category") : NULL;
            const char* output   = body ? json_get_string(body, "output")   : NULL;
            if (category && strcmp(category, "stdout") == 0
                && output && strstr(output, "OWL_DAP_TRACE_MARKER_2266")) {
                gotStdoutTrace = true;
                json_free(ev);
                break;
            }
            json_free(ev);
        }
    }
    report("trace_output_forwarded", seqOk && gotStdoutTrace,
           !seqOk ? "sequence DAP echouee"
                  : "sortie Trace non remontee en OutputEvent stdout — ligne non-JSON jetee ?");
    free(mainAbs);
    dap_kill(&p);
}

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char* fixtures = env_or("OWL_DAP_FIXTURES", "tests/dap/fixtures");

    printf("=== owl-dap-test : harness boite noire DAP ===\n");
    test_handshake();
    test_cross_file_breakpoints(fixtures);
    test_empty_other_file_preserves_class_bp(fixtures);
    test_globals_only_user(fixtures);
    test_procedure_frame_locals(fixtures);
    test_method_this_label(fixtures);
    test_inherited_method_frame(fixtures);
    test_window_event_handler(fixtures);
    test_trace_output_forwarded(fixtures);

    printf("----------------------------------------------\n");
    printf("BILAN: %d cas, %d OK, %d FAIL\n", g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide : "BILAN: 0 OK, 0 FAIL" est un vert qui
     * ne prouve rien. */
    if (g_pass + g_fail == 0) {
        printf("BILAN: CORPUS VIDE — 0 cas execute. Un controle qui n'examine "
               "rien ne prouve rien.\n");
        return 1;
    }
    return g_fail == 0 ? 0 : 1;
}
