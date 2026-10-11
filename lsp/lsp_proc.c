// SPDX-License-Identifier: AGPL-3.0-only
/* lsp_proc.c —  : spawn borné (timeout) pour `owl --check`.
 *
 * Fichier isolé : il inclut <windows.h> (Windows) SANS le scanner OWL, pour
 * éviter le conflit de symboles (TokenType...) qui casse la compilation si
 * windows.h et scanner.h cohabitent. Aucune dépendance OWL ici.
 *
 *  : le drain du pipe lit TOUTE la sortie de l'enfant, sans limite de
 * TAILLE. L'ancienne borne fixe (buffer 8 Ko) provoquait un deadlock : dès que
 * le buffer était plein, le parent arrêtait de lire, le pipe OS se remplissait,
 * l'enfant bloquait en écriture, et le watchdog concluait à tort « boucle
 * possible ». On garde la borne de TEMPS (un vrai hang est toujours tué), on
 * supprime la borne de taille.
 *
 *  Lot 1 : le spawn est désormais découpé en start/poll (asynchrone).
 * Le LSP est mono-thread ; attendre la fin de l'enfant dans la boucle
 * d'événements la fige pour toute la durée du spawn. Ce fichier héberge aussi
 * les primitives de lecture stdin, qui ont besoin de PeekNamedPipe. */
#include "lsp_proc.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static int64_t lsp_proc_now_ms(void) { return (int64_t)GetTickCount64(); }
#else
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
static int64_t lsp_proc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

void lsp_proc_sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Buffer de drain qui croît par realloc. `total` (suivi séparément) reste le
 * VRAI nombre d'octets lus même si un realloc échoue : dans ce cas on continue
 * de vider le pipe (pour ne jamais bloquer l'enfant) mais on cesse
 * d'accumuler. */
typedef struct {
    char* data;
    int len;
    int cap;
} DrainBuf;

/* Réserve de quoi stocker `extra` octets de plus + le NUL final. Renvoie 0 si
 * l'allocation échoue (OOM) — l'appelant continue alors à drainer en jetant. */
static int drainbuf_ensure(DrainBuf* b, int extra) {
    int need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    int ncap = b->cap ? b->cap : 8192;
    while (ncap < need) {
        if (ncap > (1 << 30)) { ncap = need; break; }
        ncap *= 2;
    }
    char* nd = (char*)realloc(b->data, (size_t)ncap);
    if (!nd) return 0;
    b->data = nd; b->cap = ncap;
    return 1;
}

static void drainbuf_append(DrainBuf* b, const char* src, int n, int* total) {
    *total += n;
    if (!drainbuf_ensure(b, n)) return;  /* OOM : on jette mais on continue de drainer */
    memcpy(b->data + b->len, src, (size_t)n);
    b->len += n;
}

/* ================================================================
 * CHECK ASYNCHRONE ( Lot 1)
 * ================================================================ */

/* Taille demandée pour le pipe stdin qui porte l'overlay. L'overlay est écrit
 * EN UNE FOIS au démarrage puis stdin est fermé (EOF) : anti-deadlock, mais
 * l'écriture bloquerait si l'overlay dépassait la capacité du pipe et que
 * l'enfant n'a pas encore commencé à lire (or il peut mettre des secondes à
 * démarrer à froid). Un pipe large déporte cette écriture dans le noyau et
 * rend le démarrage non bloquant pour toute taille réaliste de buffers
 * ouverts. */
#define LSP_OVERLAY_PIPE_BYTES (1 << 20)

struct LspCheckJob {
    DrainBuf buf;
    int      total;
    int64_t  deadline;
    /*  Lot 2a — requête servie par le process RÉSIDENT plutôt que par
     * un spawn dédié. `request` est conservé pour pouvoir rejouer la requête si
     * le résident meurt en cours de route ; `bodyStart` vaut -1 tant que
     * l'en-tête Content-Length de la réponse n'est pas complet. */
    int      resident;
    int      timeoutMs;
    char*    request;
    int      retried;
    int      bodyStart;
    int      bodyLen;
#ifdef _WIN32
    HANDLE hProc;
    HANDLE hThread;
    HANDLE rd;
#else
    pid_t pid;
    int   fd;
#endif
};

/* Spawn dédié à UN check (chemin historique). Conservé comme REPLI : si le
 * résident ne démarre pas, les diagnostics doivent continuer d'exister. C'est
 * aussi l'oracle des tests d'équivalence (`lsp_run_check_bounded`). */
static LspCheckJob* lsp_check_start_oneshot(const char* exePath,
                                            const char* checkTarget,
                                            const char* projectRoot,
                                            const char* lang,
                                            const char* overlayJson,
                                            int timeoutMs) {
    /*  : `--lang en` rend les diagnostics en anglais. Toute autre
     * valeur (NULL inclus) = défaut FR, aucun flag passé. */
    int wantEN = (lang != NULL && lang[0] == 'e' && lang[1] == 'n' && lang[2] == '\0');
    int useOverlay = (overlayJson != NULL && overlayJson[0] != '\0');
    int useProject = (projectRoot != NULL && projectRoot[0] != '\0');

    LspCheckJob* job = (LspCheckJob*)calloc(1, sizeof(LspCheckJob));
    if (!job) return NULL;

#ifdef _WIN32
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { free(job); return NULL; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    /* Pipe stdin optionnel : l'enfant hérite du read-end, le parent garde le
     * write-end (non hérité) pour y pousser l'overlay. */
    HANDLE rdIn = NULL, wrIn = NULL;
    if (useOverlay) {
        if (!CreatePipe(&rdIn, &wrIn, &sa, LSP_OVERLAY_PIPE_BYTES)) {
            CloseHandle(rd); CloseHandle(wr); free(job); return NULL;
        }
        SetHandleInformation(wrIn, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = rdIn;  /* NULL si pas d'overlay */
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    /* Ordre impose par le dispatch CLI (src/main.c) : --check <cible> vient en
     * premier, puis --project <racine> (mode fichier actif, ), puis
     * --overlay-stdin. --lang est un flag de session strippe en amont. */
    char projFrag[1100];
    if (useProject) snprintf(projFrag, sizeof(projFrag), " --project \"%s\"", projectRoot);
    else projFrag[0] = '\0';
    const char* ovFrag = useOverlay ? " --overlay-stdin" : "";
    char cmdline[2048];
    if (wantEN)
        snprintf(cmdline, sizeof(cmdline), "\"%s\" --lang en --check \"%s\"%s%s",
                 exePath, checkTarget, projFrag, ovFrag);
    else
        snprintf(cmdline, sizeof(cmdline), "\"%s\" --check \"%s\"%s%s",
                 exePath, checkTarget, projFrag, ovFrag);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (rdIn) CloseHandle(rdIn);
        if (wrIn) CloseHandle(wrIn);
        free(job);
        return NULL;
    }
    /*  : la deadline est armée APRÈS la création du process, pas avant.
     * `timeoutMs` borne le temps que l'ENFANT a pour répondre — la création de
     * process est du temps OS que l'enfant ne contrôle pas, et elle peut bloquer
     * plusieurs secondes (scan à l'exécution). Avec l'horloge armée avant le
     * spawn, ce délai consommait tout le budget -> faux LSP_RUN_TIMEOUT. */
    job->deadline = lsp_proc_now_ms() + timeoutMs;
    CloseHandle(wr);  /* le parent ne fait que lire stdout */
    if (rdIn) CloseHandle(rdIn);  /* le read-end appartient à l'enfant */
    /* Écrit tout l'overlay puis ferme stdin (EOF) AVANT toute lecture stdout :
     * l'enfant slurpe TOUT stdin au début, donc pas d'entre-blocage. */
    if (wrIn) {
        const char* wp = overlayJson;
        int remaining = (int)strlen(overlayJson);
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(wrIn, wp, (DWORD)remaining, &written, NULL) || written == 0) break;
            wp += written; remaining -= (int)written;
        }
        CloseHandle(wrIn);  /* EOF -> l'enfant termine son slurp stdin */
    }
    job->hProc = pi.hProcess;
    job->hThread = pi.hThread;
    job->rd = rd;
    return job;
#else
    int fds[2];
    if (pipe(fds) != 0) { free(job); return NULL; }
    /* Pipe stdin optionnel ( Lot 2). inFds[0] = read (enfant),
     * inFds[1] = write (parent). */
    int inFds[2] = { -1, -1 };
    if (useOverlay && pipe(inFds) != 0) {
        close(fds[0]); close(fds[1]); free(job); return NULL;
    }
#ifdef F_SETPIPE_SZ
    if (useOverlay) (void)fcntl(inFds[1], F_SETPIPE_SZ, LSP_OVERLAY_PIPE_BYTES);
#endif
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        if (useOverlay) { close(inFds[0]); close(inFds[1]); }
        free(job);
        return NULL;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        if (useOverlay) {
            dup2(inFds[0], STDIN_FILENO);
            close(inFds[0]); close(inFds[1]);
        }
        /* argv monte dynamiquement : --lang/--project/--overlay-stdin optionnels.
         * Ordre impose par le dispatch CLI (src/main.c) : --check <cible> puis
         * --project <racine> puis --overlay-stdin. */
        const char* av[10];
        int ac = 0;
        av[ac++] = exePath;
        if (wantEN) { av[ac++] = "--lang"; av[ac++] = "en"; }
        av[ac++] = "--check";
        av[ac++] = checkTarget;
        if (useProject) { av[ac++] = "--project"; av[ac++] = projectRoot; }
        if (useOverlay) av[ac++] = "--overlay-stdin";
        av[ac] = NULL;
        execv(exePath, (char* const*)av);
        _exit(127);
    }
    job->deadline = lsp_proc_now_ms() + timeoutMs;  /* l'enfant existe : le chrono part */
    close(fds[1]);
    if (useOverlay) {
        close(inFds[0]);  /* read-end appartient à l'enfant */
        const char* wp = overlayJson;
        size_t remaining = strlen(overlayJson);
        while (remaining > 0) {
            ssize_t written = write(inFds[1], wp, remaining);
            if (written <= 0) break;
            wp += written; remaining -= (size_t)written;
        }
        close(inFds[1]);  /* EOF -> l'enfant termine son slurp stdin */
    }
    job->pid = pid;
    job->fd = fds[0];
    return job;
#endif
}

/* ================================================================
 * PROCESS RÉSIDENT ( Lot 2a)
 *
 * `owl.exe` pèse ~100 Mo : son seul chargement coûte ~150 ms à chaud et
 * jusqu'à ~5 s à froid (scan antivirus), AVANT tout travail utile. Ce plancher
 * était repayé à chaque check débouncé (400 ms pendant la frappe). On le paie
 * désormais UNE fois par session : `owl --check-server` reste vivant et sert
 * les requêtes sur un couple de pipes.
 *
 * Framing identique dans les deux sens : `Content-Length: <n>\r\n\r\n<n octets>`
 * (cf. le protocole détaillé côté owl, src/main.c). Le corps de réponse est
 * exactement ce qu'aurait écrit `owl --check`.
 *
 * Le résident ne survit JAMAIS au LSP : stdin fermé (EOF) au stop, kill en
 * garde-fou, et sur Windows un Job Object KILL_ON_JOB_CLOSE qui le tue même si
 * le LSP est terminé brutalement (crash, kill du client).
 * ================================================================ */

static struct {
    int  alive;
    int  stopped;    /* shutdown demandé : plus aucun (re)démarrage */
    char exePath[1024];
#ifdef _WIN32
    HANDLE hProc, hIn, hOut, hJob;
#else
    pid_t pid;
    int   fdIn, fdOut;
#endif
} g_srv;

static void srv_kill(void) {
    if (!g_srv.alive) return;
    g_srv.alive = 0;
#ifdef _WIN32
    if (g_srv.hIn) { CloseHandle(g_srv.hIn); g_srv.hIn = NULL; }   /* EOF : sortie propre */
    if (g_srv.hProc) {
        if (WaitForSingleObject(g_srv.hProc, 300) != WAIT_OBJECT_0)
            TerminateProcess(g_srv.hProc, 1);
        CloseHandle(g_srv.hProc); g_srv.hProc = NULL;
    }
    if (g_srv.hOut) { CloseHandle(g_srv.hOut); g_srv.hOut = NULL; }
    if (g_srv.hJob) { CloseHandle(g_srv.hJob); g_srv.hJob = NULL; }
#else
    if (g_srv.fdIn >= 0) { close(g_srv.fdIn); g_srv.fdIn = -1; }
    if (g_srv.pid > 0) {
        int st;
        for (int i = 0; i < 30; i++) {           /* ~300 ms de grâce */
            if (waitpid(g_srv.pid, &st, WNOHANG) == g_srv.pid) { g_srv.pid = -1; break; }
            lsp_proc_sleep_ms(10);
        }
        if (g_srv.pid > 0) { kill(g_srv.pid, SIGKILL); waitpid(g_srv.pid, &st, 0); g_srv.pid = -1; }
    }
    if (g_srv.fdOut >= 0) { close(g_srv.fdOut); g_srv.fdOut = -1; }
#endif
}

/* Arrêt DÉFINITIF (shutdown / atexit) : le verrou empêche qu'un check encore
 * en vol relance un résident juste après, qui survivrait au LSP. */
void lsp_check_server_stop(void) { g_srv.stopped = 1; srv_kill(); }

static void srv_atexit(void) { srv_kill(); }

/* Démarre le résident si nécessaire. 0 = indisponible (l'appelant retombe sur
 * le spawn par check). */
static int srv_ensure(const char* exePath) {
    if (g_srv.alive) return 1;
    if (g_srv.stopped) return 0;
    if (exePath == NULL || exePath[0] == '\0') return 0;

    static int atexitRegistered = 0;
    if (!atexitRegistered) { atexit(srv_atexit); atexitRegistered = 1; }

    snprintf(g_srv.exePath, sizeof(g_srv.exePath), "%s", exePath);

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE outRd = NULL, outWr = NULL, inRd = NULL, inWr = NULL;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) return 0;
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&inRd, &inWr, &sa, LSP_OVERLAY_PIPE_BYTES)) {
        CloseHandle(outRd); CloseHandle(outWr); return 0;
    }
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    /* stderr du résident : celui du LSP, JAMAIS le pipe de réponse — un
     * message d'erreur qui s'y glisserait désynchroniserait le framing. */
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    char cmdline[1200];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" --check-server", exePath);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(outRd); CloseHandle(outWr);
        CloseHandle(inRd);  CloseHandle(inWr);
        return 0;
    }
    CloseHandle(outWr);            /* appartient à l'enfant */
    CloseHandle(inRd);
    CloseHandle(pi.hThread);

    /* Filet anti-orphelin : si le LSP disparaît sans passer par srv_kill
     * (crash, kill du client), la fermeture du handle de job tue le résident. */
    g_srv.hJob = CreateJobObjectA(NULL, NULL);
    if (g_srv.hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_srv.hJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
        AssignProcessToJobObject(g_srv.hJob, pi.hProcess);
    }
    g_srv.hProc = pi.hProcess;
    g_srv.hIn = inWr;
    g_srv.hOut = outRd;
#else
    int outFds[2], inFds[2];
    if (pipe(outFds) != 0) return 0;
    if (pipe(inFds) != 0) { close(outFds[0]); close(outFds[1]); return 0; }
#ifdef F_SETPIPE_SZ
    (void)fcntl(inFds[1], F_SETPIPE_SZ, LSP_OVERLAY_PIPE_BYTES);
#endif
    signal(SIGPIPE, SIG_IGN);   /* écrire vers un résident mort = erreur, pas mort du LSP */
    pid_t pid = fork();
    if (pid < 0) {
        close(outFds[0]); close(outFds[1]); close(inFds[0]); close(inFds[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(inFds[0], STDIN_FILENO);
        dup2(outFds[1], STDOUT_FILENO);
        close(inFds[0]); close(inFds[1]);
        close(outFds[0]); close(outFds[1]);
        execl(exePath, exePath, "--check-server", (char*)NULL);
        _exit(127);
    }
    close(inFds[0]);
    close(outFds[1]);
    g_srv.pid = pid;
    g_srv.fdIn = inFds[1];
    g_srv.fdOut = outFds[0];
#endif
    g_srv.alive = 1;
    return 1;
}

int lsp_check_server_warmup(const char* exePath) { return srv_ensure(exePath); }

int lsp_check_server_is_alive(void) { return g_srv.alive; }

/* Écrit une requête framée. 0 = échec (résident mort ou pipe cassé). */
static int srv_send(const char* request) {
    char header[64];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n",
                        (int)strlen(request));
    const char* parts[2] = { header, request };
    int lens[2] = { hlen, (int)strlen(request) };
    for (int p = 0; p < 2; p++) {
        const char* wp = parts[p];
        int remaining = lens[p];
        while (remaining > 0) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(g_srv.hIn, wp, (DWORD)remaining, &written, NULL) || written == 0)
                return 0;
#else
            ssize_t written = write(g_srv.fdIn, wp, (size_t)remaining);
            if (written <= 0) return 0;
#endif
            wp += written; remaining -= (int)written;
        }
    }
    return 1;
}

/* Ajoute `s` à `dst` en échappant ce que JSON exige d'un chemin (guillemet,
 * antislash Windows). Les autres caractères de contrôle sont déjà rejetés en
 * amont par le garde anti-injection du handler. */
static void json_escape_append(char* dst, int cap, const char* s) {
    int n = (int)strlen(dst);
    for (; *s && n < cap - 3; s++) {
        if (*s == '"' || *s == '\\') dst[n++] = '\\';
        dst[n++] = *s;
    }
    dst[n] = '\0';
}

/* Construit la requête résident :
 *   {"target":"...","lang":"fr|en","overlays":[...]}
 * `target` et `lang` sont EN TÊTE : côté serveur, ils sont extraits sans parser
 * JSON complet, en s'arrêtant avant "overlays" (dont les contenus sont du texte
 * utilisateur arbitraire, potentiellement porteur d'une séquence `"target":`).
 * L'overlay est réinjecté tel quel : `overlayJson` est l'objet
 * `{"overlays":[...]}` déjà sérialisé, on en reprend l'intérieur. */
static char* srv_build_request(const char* checkTarget, const char* projectRoot,
                               const char* lang, const char* overlayJson) {
    int wantEN = (lang != NULL && strcmp(lang, "en") == 0);
    int useProject = (projectRoot != NULL && projectRoot[0] != '\0');
    size_t overlayLen = overlayJson ? strlen(overlayJson) : 0;
    size_t projLen = useProject ? strlen(projectRoot) : 0;
    size_t cap = strlen(checkTarget) * 2 + projLen * 2 + overlayLen + 160;
    char* req = (char*)malloc(cap);
    if (!req) return NULL;
    snprintf(req, cap, "{\"target\":\"");
    json_escape_append(req, (int)cap, checkTarget);
    strncat(req, wantEN ? "\",\"lang\":\"en\"" : "\",\"lang\":\"fr\"", cap - strlen(req) - 1);
    /* `project` reste EN TÊTE, avant `overlays` : le serveur (src/main.c
     * check_server_get_str) borne sa recherche avant les contenus overlay
     * (texte utilisateur arbitraire). Présence = mode fichier actif. */
    if (useProject) {
        strncat(req, ",\"project\":\"", cap - strlen(req) - 1);
        json_escape_append(req, (int)cap, projectRoot);
        strncat(req, "\"", cap - strlen(req) - 1);
    }
    if (overlayLen >= 2 && overlayJson[0] == '{' && overlayJson[overlayLen - 1] == '}') {
        size_t n = strlen(req);
        req[n++] = ',';
        memcpy(req + n, overlayJson + 1, overlayLen - 2);
        req[n + overlayLen - 2] = '\0';
    }
    strncat(req, "}", cap - strlen(req) - 1);
    return req;
}

/* Draine ce que le résident a déjà écrit. Renvoie 0 si le pipe est mort. */
static int srv_drain(LspCheckJob* job) {
    char chunk[4096];
#ifdef _WIN32
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_srv.hOut, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail == 0) return 1;
        DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
        DWORD got = 0;
        if (!ReadFile(g_srv.hOut, chunk, want, &got, NULL) || got == 0) return 0;
        drainbuf_append(&job->buf, chunk, (int)got, &job->total);
    }
#else
    for (;;) {
        struct pollfd pfd; pfd.fd = g_srv.fdOut; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0) return 1;
        ssize_t n = read(g_srv.fdOut, chunk, sizeof(chunk));
        if (n > 0) { drainbuf_append(&job->buf, chunk, (int)n, &job->total); continue; }
        return 0;   /* EOF : le résident est parti */
    }
#endif
}

/* Récolte finale : ferme les handles, NUL-termine le buffer et le cède à
 * l'appelant, libère le job. */
static int lsp_check_finish(LspCheckJob* job, LspRunStatus st,
                            LspRunStatus* outStatus, char** outBuf, int* outLen) {
    if (!job->buf.data) {
        /* Sortie vide (projet propre) : rendre un buffer NUL-terminé non-NULL
         * pour que l'appelant puisse json_parse sans cas particulier. */
        job->buf.data = (char*)malloc(1);
        if (!job->buf.data) st = LSP_RUN_SPAWN_FAIL;
        else { job->buf.data[0] = '\0'; job->buf.len = 0; }
    } else {
        job->buf.data[job->buf.len] = '\0';  /* place réservée par drainbuf_ensure */
    }
    *outStatus = st;
    *outBuf = job->buf.data;   /* possédé par l'appelant, à free */
    *outLen = job->buf.len;
    free(job);
    return 1;
}

/* Récolte finale d'une requête RÉSIDENT : le corps de la réponse est extrait du
 * flux accumulé (le buffer de drain peut déjà contenir le début de la réponse
 * suivante — il n'y en a jamais, un seul check en vol, mais la découpe reste
 * stricte sur Content-Length). */
static int srv_finish(LspCheckJob* job, LspRunStatus st,
                      LspRunStatus* outStatus, char** outBuf, int* outLen) {
    char* body = NULL;
    int len = 0;
    if (st == LSP_RUN_OK && job->bodyStart >= 0) {
        len = job->bodyLen;
        body = (char*)malloc((size_t)len + 1);
        if (body) {
            if (len > 0) memcpy(body, job->buf.data + job->bodyStart, (size_t)len);
            body[len] = '\0';
        } else {
            len = 0;
            st = LSP_RUN_SPAWN_FAIL;
        }
    }
    if (!body) {
        body = (char*)malloc(1);
        if (body) body[0] = '\0';
        len = 0;
    }
    free(job->buf.data);
    free(job->request);
    *outStatus = st;
    *outBuf = body;
    *outLen = len;
    free(job);
    return 1;
}

/* Avance une requête servie par le résident. Même contrat que lsp_check_poll. */
static int srv_poll(LspCheckJob* job, LspRunStatus* outStatus,
                    char** outBuf, int* outLen) {
    int pipeOk = g_srv.alive ? srv_drain(job) : 0;

    /* En-tête complet ? */
    if (job->bodyStart < 0 && job->buf.data != NULL) {
        job->buf.data[job->buf.len] = '\0';
        char* sep = strstr(job->buf.data, "\r\n\r\n");
        if (sep) {
            const char* cl = strstr(job->buf.data, "Content-Length:");
            if (cl == NULL || cl > sep) {
                srv_kill();   /* flux non conforme : la session n'est plus fiable */
                return srv_finish(job, LSP_RUN_SPAWN_FAIL, outStatus, outBuf, outLen);
            }
            job->bodyLen = (int)strtol(cl + 15, NULL, 10);
            job->bodyStart = (int)(sep - job->buf.data) + 4;
        }
    }
    if (job->bodyStart >= 0 && job->buf.len >= job->bodyStart + job->bodyLen) {
        return srv_finish(job, LSP_RUN_OK, outStatus, outBuf, outLen);
    }

    if (!pipeOk) {
        /* Le résident est mort avant d'avoir répondu (crash, EOF). On le
         * relance et on rejoue la requête UNE fois : sans ça, la frappe en
         * cours resterait sans diagnostics jusqu'au check suivant. */
        srv_kill();
        if (!job->retried && job->request && srv_ensure(g_srv.exePath) &&
            srv_send(job->request)) {
            job->retried = 1;
            job->buf.len = 0;
            job->total = 0;
            job->bodyStart = -1;
            job->bodyLen = 0;
            job->deadline = lsp_proc_now_ms() + job->timeoutMs;
            return 0;
        }
        return srv_finish(job, LSP_RUN_SPAWN_FAIL, outStatus, outBuf, outLen);
    }

    if (lsp_proc_now_ms() > job->deadline) {
        /* Un check qui ne rend pas la main bloque le résident pour toujours :
         * on le tue (le suivant en relancera un neuf). */
        srv_kill();
        return srv_finish(job, LSP_RUN_TIMEOUT, outStatus, outBuf, outLen);
    }
    return 0;
}

LspCheckJob* lsp_check_start(const char* exePath,
                             const char* checkTarget,
                             const char* projectRoot,
                             const char* lang,
                             const char* overlayJson,
                             int timeoutMs) {
    if (srv_ensure(exePath)) {
        char* request = srv_build_request(checkTarget, projectRoot, lang, overlayJson);
        if (request != NULL && srv_send(request)) {
            LspCheckJob* job = (LspCheckJob*)calloc(1, sizeof(LspCheckJob));
            if (job) {
                job->resident = 1;
                job->request = request;
                job->bodyStart = -1;
                job->timeoutMs = timeoutMs;
                job->deadline = lsp_proc_now_ms() + timeoutMs;
                return job;
            }
        }
        free(request);
        /* Pipe cassé au premier octet : le résident est HS, on repart sur un
         * spawn dédié pour ne pas perdre ce verdict. */
        srv_kill();
    }
    return lsp_check_start_oneshot(exePath, checkTarget, projectRoot, lang,
                                   overlayJson, timeoutMs);
}

int lsp_check_poll(LspCheckJob* job, LspRunStatus* outStatus,
                   char** outBuf, int* outLen) {
    char chunk[4096];
    LspRunStatus st = LSP_RUN_OK;
    int done = 0;

    if (job->resident) return srv_poll(job, outStatus, outBuf, outLen);

#ifdef _WIN32
    for (;;) {  /* draine à fond ce qui est déjà disponible */
        DWORD avail = 0;
        if (!PeekNamedPipe(job->rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
        DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
        DWORD got = 0;
        if (!ReadFile(job->rd, chunk, want, &got, NULL) || got == 0) break;
        drainbuf_append(&job->buf, chunk, (int)got, &job->total);
    }
    if (WaitForSingleObject(job->hProc, 0) == WAIT_OBJECT_0) {
        for (;;) {  /* draine le reste du pipe après la fin du process */
            DWORD a2 = 0;
            if (!PeekNamedPipe(job->rd, NULL, 0, NULL, &a2, NULL) || a2 == 0) break;
            DWORD want = a2 > sizeof(chunk) ? (DWORD)sizeof(chunk) : a2;
            DWORD got = 0;
            if (!ReadFile(job->rd, chunk, want, &got, NULL) || got == 0) break;
            drainbuf_append(&job->buf, chunk, (int)got, &job->total);
        }
        done = 1;
    } else if (lsp_proc_now_ms() > job->deadline) {
        TerminateProcess(job->hProc, 1);
        WaitForSingleObject(job->hProc, 2000);
        st = LSP_RUN_TIMEOUT;
        done = 1;
    }
    if (!done) return 0;
    CloseHandle(job->hProc); CloseHandle(job->hThread); CloseHandle(job->rd);
#else
    for (;;) {
        struct pollfd pfd; pfd.fd = job->fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0) break;
        ssize_t n = read(job->fd, chunk, sizeof(chunk));
        if (n > 0) { drainbuf_append(&job->buf, chunk, (int)n, &job->total); continue; }
        if (n == 0) done = 1;   /* EOF : l'enfant a fermé sa sortie */
        break;
    }
    if (!done && lsp_proc_now_ms() > job->deadline) {
        kill(job->pid, SIGKILL);
        st = LSP_RUN_TIMEOUT;
        done = 1;
    }
    if (!done) return 0;
    close(job->fd);
    int status; (void)status;
    waitpid(job->pid, &status, 0);
#endif
    return lsp_check_finish(job, st, outStatus, outBuf, outLen);
}

void lsp_check_abort(LspCheckJob* job) {
    if (!job) return;
    if (job->resident) {
        /* Le résident écrira sa réponse quoi qu'il arrive : l'abandonner sans
         * la lire désynchroniserait la session. On coupe le process — le
         * prochain check en relancera un. */
        srv_kill();
        free(job->buf.data);
        free(job->request);
        free(job);
        return;
    }
#ifdef _WIN32
    TerminateProcess(job->hProc, 1);
    WaitForSingleObject(job->hProc, 2000);
    CloseHandle(job->hProc); CloseHandle(job->hThread); CloseHandle(job->rd);
#else
    kill(job->pid, SIGKILL);
    close(job->fd);
    int status; (void)status;
    waitpid(job->pid, &status, 0);
#endif
    free(job->buf.data);
    free(job);
}

LspRunStatus lsp_run_check_bounded(const char* exePath,
                                   const char* checkTarget,
                                   const char* lang,
                                   char* out, int cap,
                                   int timeoutMs, int* outLen) {
    *outLen = 0;
    if (cap < 1) return LSP_RUN_SPAWN_FAIL;
    out[0] = '\0';
    /* Spawn dédié volontaire : c'est la référence « ce qu'aurait rendu
     * `owl --check` », utilisée comme oracle par les tests d'équivalence du
     * mode résident. */
    LspCheckJob* job = lsp_check_start_oneshot(exePath, checkTarget, NULL, lang, NULL, timeoutMs);
    if (!job) return LSP_RUN_SPAWN_FAIL;
    LspRunStatus st = LSP_RUN_OK;
    char* buf = NULL;
    int len = 0;
    while (!lsp_check_poll(job, &st, &buf, &len)) lsp_proc_sleep_ms(5);
    if (st == LSP_RUN_SPAWN_FAIL) { free(buf); return st; }
    /* `out` reçoit une copie tronquée à cap-1 ; `*outLen` reste la VRAIE
     * longueur capturée (peut dépasser cap). Le pipe a été drainé en entier,
     * donc plus de deadlock : on rend LSP_RUN_OK même quand la sortie excède
     * le buffer. */
    int copy = len < cap - 1 ? len : cap - 1;
    if (buf && copy > 0) memcpy(out, buf, (size_t)copy);
    out[copy] = '\0';
    *outLen = len;
    free(buf);
    return st;
}

/* ================================================================
 * STDIN ( Lot 1)
 *
 * Lecture entièrement gérée ici, avec un buffer explicite : `lsp_stdin_wait`
 * doit pouvoir répondre « rien à lire » de façon FIABLE. Avec <stdio.h>, des
 * octets déjà avalés par le buffer de la libc resteraient invisibles à un
 * peek au niveau OS, et un message client pourrait dormir le temps d'un tick.
 * ================================================================ */

static char g_inBuf[65536];
static int  g_inLen = 0;   /* octets valides dans g_inBuf */
static int  g_inPos = 0;   /* prochain octet à consommer */
static int  g_inEof = 0;

/* Remplit le buffer si vide (BLOQUANT). 1 = au moins un octet dispo, 0 = EOF. */
static int stdin_fill(void) {
    if (g_inPos < g_inLen) return 1;
    if (g_inEof) return 0;
    g_inPos = g_inLen = 0;
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), g_inBuf,
                  (DWORD)sizeof(g_inBuf), &got, NULL) || got == 0) {
        g_inEof = 1;
        return 0;
    }
    g_inLen = (int)got;
#else
    ssize_t n;
    do { n = read(STDIN_FILENO, g_inBuf, sizeof(g_inBuf)); }
    while (n < 0 && errno == EINTR);
    if (n <= 0) { g_inEof = 1; return 0; }
    g_inLen = (int)n;
#endif
    return 1;
}

int lsp_stdin_wait(int timeoutMs) {
    if (g_inPos < g_inLen) return 1;
    if (g_inEof) return -1;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    int64_t deadline = lsp_proc_now_ms() + timeoutMs;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            /* Peek indisponible : console, fichier redirigé, ou pipe non
             * observable (les pipes MSYS/Cygwin répondent ERROR_BROKEN_PIPE
             * alors qu'ils sont bien vivants). On déclare « lisible » et on
             * laisse la lecture bloquante trancher — dégradation sûre : plus
             * de tick, mais jamais de message client qui dort. Les clients
             * réels (VSCode/Node) utilisent des pipes Windows natifs, où le
             * peek répond correctement. */
            return 1;
        }
        if (avail > 0) return 1;
        if (lsp_proc_now_ms() >= deadline) return 0;
        Sleep(5);
    }
#else
    struct pollfd pfd; pfd.fd = STDIN_FILENO; pfd.events = POLLIN; pfd.revents = 0;
    int pr;
    do { pr = poll(&pfd, 1, timeoutMs); } while (pr < 0 && errno == EINTR);
    return pr > 0 ? 1 : 0;
#endif
}

int lsp_stdin_read_line(char* dst, int cap) {
    int n = 0;
    if (cap < 1) return 0;
    for (;;) {
        if (!stdin_fill()) break;
        char c = g_inBuf[g_inPos++];
        if (n < cap - 1) dst[n++] = c;
        if (c == '\n') break;
    }
    dst[n] = '\0';
    return n;
}

int lsp_stdin_read_exact(char* dst, int n) {
    int got = 0;
    while (got < n) {
        if (!stdin_fill()) break;
        int avail = g_inLen - g_inPos;
        int want = n - got;
        if (want > avail) want = avail;
        memcpy(dst + got, g_inBuf + g_inPos, (size_t)want);
        g_inPos += want;
        got += want;
    }
    return got;
}
