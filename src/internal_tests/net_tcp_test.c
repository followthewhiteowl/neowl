// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du socket TCP.
 *
 * Exécuté via `owl --self-test-net` (cf main.c). Couvre :
 *   - init / shutdown globaux (idempotents)
 *   - Connexion à un port libre → échec propre avec last_error rempli
 *   - Loopback complet : mini-serveur dans un thread, client se
 *     connecte, échange un message, vérifie round-trip
 *
 * Le mini-serveur utilise les API socket OS directement (pas
 * net_tcp.h — pour rester orthogonal et pouvoir tester independamment
 * la couche client). */

#include "net_tcp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE tcp_thread_t;
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef pthread_t tcp_thread_t;
#endif

static int g_pass = 0;
static int g_fail = 0;

#define TCP_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * Mini-serveur loopback dans un thread.
 *
 * Écoute sur 127.0.0.1:port (port assigné dynamiquement par l'OS via
 * bind sur 0). Reçoit "PING" (4 bytes), répond "PONG\n" (5 bytes),
 * ferme. Un seul accept, donc le thread se termine après une
 * connexion.
 * ================================================================ */

typedef struct {
    int    port;             /* OUT : port assigné par l'OS */
    int    ready;            /* OUT : 1 quand listen() a réussi */
    char   last_request[16]; /* OUT : ce que le serveur a reçu */
} ServerCtx;

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID arg) {
#else
static void* server_thread(void* arg) {
#endif
    ServerCtx* ctx = (ServerCtx*)arg;
    ctx->ready = 0;
    ctx->last_request[0] = '\0';

#ifdef _WIN32
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return 0;
#else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
#endif

    /* Reuse address pour éviter les TIME_WAIT entre runs. */
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(0);  /* OS assigne */

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(srv); return 0;
#else
        close(srv); return NULL;
#endif
    }

    /* Récupère le port assigné. */
    socklen_t addrLen = sizeof(addr);
    if (getsockname(srv, (struct sockaddr*)&addr, &addrLen) != 0) {
#ifdef _WIN32
        closesocket(srv); return 0;
#else
        close(srv); return NULL;
#endif
    }
    ctx->port = ntohs(addr.sin_port);

    if (listen(srv, 1) != 0) {
#ifdef _WIN32
        closesocket(srv); return 0;
#else
        close(srv); return NULL;
#endif
    }
    ctx->ready = 1;

    /* Accept une connexion (blocant). */
#ifdef _WIN32
    SOCKET cli = accept(srv, NULL, NULL);
    closesocket(srv);
    if (cli == INVALID_SOCKET) return 0;
#else
    int cli = accept(srv, NULL, NULL);
    close(srv);
    if (cli < 0) return NULL;
#endif

    /* Lit jusqu'a 4 bytes. */
    char buf[16] = {0};
    int n = recv(cli, buf, 4, 0);
    if (n > 0) {
        size_t cap = sizeof(ctx->last_request) - 1;
        size_t cp = (size_t)n < cap ? (size_t)n : cap;
        memcpy(ctx->last_request, buf, cp);
        ctx->last_request[cp] = '\0';
    }

    /* Reponse. */
    send(cli, "PONG\n", 5, 0);

#ifdef _WIN32
    shutdown(cli, SD_BOTH); closesocket(cli);
    return 0;
#else
    shutdown(cli, SHUT_RDWR); close(cli);
    return NULL;
#endif
}

static int spawn_server(tcp_thread_t* th, ServerCtx* ctx) {
#ifdef _WIN32
    *th = CreateThread(NULL, 0, server_thread, ctx, 0, NULL);
    return (*th != NULL) ? 0 : -1;
#else
    return pthread_create(th, NULL, server_thread, ctx);
#endif
}

static void join_server(tcp_thread_t th) {
#ifdef _WIN32
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
}

/* Attend jusqu'a ce que ctx->ready == 1 (avec timeout 2s). */
static int wait_server_ready(ServerCtx* ctx, int timeout_ms) {
    int waited = 0;
    while (ctx->ready == 0 && waited < timeout_ms) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
#endif
        waited += 10;
    }
    return ctx->ready;
}

/* ================================================================
 * Tests
 * ================================================================ */

static void test_global_init(void) {
    TCP_CHECK(owl_tcp_global_init(), "global_init OK");
    /* Idempotent : 2 init successifs doivent marcher. */
    TCP_CHECK(owl_tcp_global_init(), "global_init idempotent");
    owl_tcp_global_shutdown();
    /* Apres 1 shutdown, init compte > 0 donc winsock encore actif. */
    OwlTcpSocket s;
    owl_tcp_init(&s);
    /* test triviale : init structure */
    TCP_CHECK(s.fd == OWL_INVALID_SOCKET, "owl_tcp_init met fd a INVALID");
    TCP_CHECK(s.connected == false, "owl_tcp_init met connected a false");
    owl_tcp_global_shutdown();
}

static void test_connect_refused(void) {
    /* On ne peut pas garantir qu'un port soit fermé… mais 1 est privilégié
     * et l'OS refusera la connexion d'un client non-root. */
    TCP_CHECK(owl_tcp_global_init(), "global_init pre test_connect_refused");

    OwlTcpSocket s;
    owl_tcp_init(&s);
    /* Port 1 sur localhost : pas un service standard, devrait refuser. */
    bool ok = owl_tcp_connect(&s, "127.0.0.1", 1, 1000);
    TCP_CHECK(!ok, "connect a 127.0.0.1:1 echoue (port ferme)");
    TCP_CHECK(s.last_error[0] != '\0', "last_error est rempli sur echec");
    owl_tcp_close(&s);
    owl_tcp_global_shutdown();
}

static void test_loopback(void) {
    TCP_CHECK(owl_tcp_global_init(), "global_init pre test_loopback");

    ServerCtx ctx = {0, 0, {0}};
    tcp_thread_t th;
    int rc = spawn_server(&th, &ctx);
    TCP_CHECK(rc == 0, "spawn server thread");
    if (rc != 0) { owl_tcp_global_shutdown(); return; }

    TCP_CHECK(wait_server_ready(&ctx, 2000), "server ready in 2s");
    if (!ctx.ready) {
        join_server(th);
        owl_tcp_global_shutdown();
        return;
    }

    /* Client : connect au port assigne. */
    OwlTcpSocket s;
    owl_tcp_init(&s);
    bool connected = owl_tcp_connect(&s, "127.0.0.1", ctx.port, 2000);
    TCP_CHECK(connected, "client connect au server loopback");
    if (!connected) {
        join_server(th);
        owl_tcp_global_shutdown();
        return;
    }

    /* Envoie "PING". */
    int sent = owl_tcp_send(&s, "PING", 4);
    TCP_CHECK(sent == 4, "send 4 bytes");

    /* Recoit "PONG\n". */
    char buf[16] = {0};
    bool got = owl_tcp_recv_exact(&s, buf, 5);
    TCP_CHECK(got, "recv_exact 5 bytes");
    TCP_CHECK(memcmp(buf, "PONG\n", 5) == 0, "round-trip PING/PONG");

    owl_tcp_close(&s);
    join_server(th);

    /* Verifie que le serveur a bien recu "PING". */
    TCP_CHECK(strcmp(ctx.last_request, "PING") == 0, "server a recu PING");

    owl_tcp_global_shutdown();
}

int net_tcp_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_global_init();
    test_connect_refused();
    test_loopback();

    printf("net_tcp self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
