// SPDX-License-Identifier: AGPL-3.0-only
/* http_llm_annulation_crash_test.c — repro du CRASH race (,
 * priorite 1).
 *
 * Ticket : 
 *
 * CONSTAT TESTEUR (verbatim, 2026-08-22) : 4 generations STREAMING
 * concurrentes + kill -9 des clients en plein stream -> le serveur
 * CRASHE, reproductible 3/3 en bare-metal, JAMAIS sous gdb -> RACE
 * CONDITION dans le chemin deconnexion/annulation concurrent du
 * reacteur streaming (W5/W7, chemin executeur-async, /1706).
 * La garde UAF  (http_stress_gardes_test.c, famille close
 * mid-stream) NE couvre PAS ce chemin : elle monte le producteur en
 * handler_rep BLOQUANT simple (pas cfg.executeur_async), donc un seul
 * flux a la fois, jamais de concurrence reelle entre plusieurs jobs
 * streaming resolus par le reacteur. Ce fichier reproduit le chemin de
 * PRODUCTION exact : cfg.executeur_async pose (comme serve_llm.c:1173),
 * >= 4 flux concurrents, fermeture BRUTALE (RST) mid-stream, en boucle.
 *
 * ⚠️ ISOLE DU `--self-test-net` PAR DEFAUT (contrainte critique du
 * ticket) : un CRASH tue le process hote, ce qui tuerait
 * `--self-test-net` en cours de route pour TOUS les agents qui tournent
 * en parallele sur ce depot multi-agents (fichiers partages, pas de
 * sandbox process par agent). Tant que la race n'est pas corrigee, ce
 * fichier n'est PAS appele depuis la liste net_familles de
 * `--self-test-net` (cf. main.c). Il est cable derriere un point
 * d'entree SEPARE et EXPLICITE :
 *
 *     ./build/owl.exe --self-test-net-crash-1723
 *
 * A lancer SEUL (pas dans un run parallele avec d'autres self-tests
 * partageant le process), idealement sous WinDbg avec build/owl.pdb
 * charge (gdb MASQUE la race, cf. constat testeur) ou avec un handler
 * minidump-on-crash installe, pour capturer la pile au crash. L'implementer
 * cablera ce point d'entree dans net_familles par defaut UNE FOIS la race
 * corrigee (le test devient alors la garde de non-regression permanente).
 *
 * ASSERTION : le process SURVIT a la boucle de stress (N iterations de
 * CRASH_N flux concurrents + fermeture RST mid-stream), et une requete
 * de sonde envoyee APRES la boucle est servie correctement -- preuve
 * que l'executeur/reacteur restent dans un etat coherent, pas seulement
 * "n'a pas encore crashe".
 *
 * DISCIPLINE ANTI-FIGEMENT : chaque generation simulee est bornee
 * (CRASH_MAX_CHUNKS * CRASH_PACE_MS, quelques centaines de ms max meme
 * si jamais coupee), chaque lecture cliente est bornee par SO_RCVTIMEO,
 * chaque iteration ferme deterministiquement ses threads (join) avant
 * la suivante. Si la race ne crashe pas ce process en sandbox (heisenbug
 * connu du testeur : jamais reproduit sous gdb), le test finit par un
 * PASS explicite plutot que de pendre -- documente honnetement dans le
 * rapport d'agent, PAS presente comme un repro fiabilise si ce n'est pas
 * le cas.
 *
 * NOTE test_writer (2026-08-22) : tente en sandbox (pose du fichier) avec
 * synchronisation barriere (les CRASH_N flux d'une meme iteration ferment
 * TOUS au meme instant, pas echelonne) jusqu'a CRASH_N=16 / CRASH_ITERS=300
 * (~4800 cycles connect+stream+RST) SANS crash observe. Coherent avec le
 * constat testeur "jamais sous gdb" -- cette sandbox amortit probablement
 * la fenetre de course de la meme facon qu'un debogueur attache
 * (scheduling different du bare-metal). Le repro fiable reste celui du
 * testeur (bare-metal, cf. REPRO-hang-serve-llm.md) ; ce fichier est la
 * garde automatisee qui DOIT rester verte apres le fix, et reste un outil
 * de chasse pour l'implementer (augmenter CRASH_N / CRASH_ITERS localement
 * si besoin de re-tenter en environnement moins amorti).
 */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE cr_thread_t;
  typedef SOCKET cr_sock_t;
  #define CR_INVALID INVALID_SOCKET
  #define CR_THREAD_RET DWORD WINAPI
  #define CR_THREAD_0   0
  #define CR_START(t, fn, arg) do { (t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL); } while (0)
  #define CR_JOIN(t)    do { if (t) { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } } while (0)
  #define CR_SLEEP(ms)  Sleep((DWORD)(ms))
  #define CR_CLOSESOCK  closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t cr_thread_t;
  typedef int cr_sock_t;
  #define CR_INVALID (-1)
  #define CR_THREAD_RET void*
  #define CR_THREAD_0   NULL
  #define CR_START(t, fn, arg) do { if (pthread_create(&(t), NULL, (fn), (arg)) != 0) (t) = 0; } while (0)
  #define CR_JOIN(t)    do { if (t) pthread_join((t), NULL); } while (0)
  static void cr_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define CR_SLEEP(ms)  cr_sleep_ms((int)(ms))
  #define CR_CLOSESOCK  close
#endif

extern uint64_t owl_tcp_server_horloge_ms(void);

/* ================================================================
 * Producteur de test : stream pace, model-free (PAS de LLM). Chemin
 * data-plane (/genN, hors des routes de controle owl_http_route_est_
 * controle) -> sert via cfg.executeur_async, le chemin de production
 * reel. /petit sert de sonde finale de survie.
 * ================================================================ */

#define CRASH_MAX_CHUNKS 200   /* borne haute : ~400ms max si jamais coupe */
#define CRASH_PACE_MS    2

static void producteur_crash(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    const char* chemin = req->chemin ? req->chemin : "";

    if (strcmp(chemin, "/petit") == 0) {
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1 << 12);
        if (!f) return;
        owl_http_rep_flux_pousse(f, "data: tok0\n\n", 12);
        owl_http_rep_flux_pousse(f, "data: [DONE]\n\n", 14);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
        return;
    }

    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1 << 16);
    if (!f) return;
    char buf[40];
    for (int i = 0; i < CRASH_MAX_CHUNKS; i++) {
        int n = snprintf(buf, sizeof(buf), "data: tok%d\n\n", i);
        if (n < 0) n = 0;
        if (owl_http_rep_flux_pousse(f, buf, (size_t)n) == OWL_HTTP_FLUX_ABANDON) {
            owl_http_rep_flux_relache(f);
            return;
        }
        CR_SLEEP(CRASH_PACE_MS);
    }
    owl_http_rep_flux_termine(f);
    owl_http_rep_flux_relache(f);
}

/* ================================================================
 * Client RAW : connecte, envoie la requete de flux, lit un peu (borne
 * par SO_RCVTIMEO), puis COUPE BRUTALEMENT (RST via SO_LINGER{on,0}) —
 * motif "kill -9 en plein stream" du repro testeur, pas une fermeture
 * propre. Concu pour etre lance en rafale concurrente (CRASH_N threads).
 * ================================================================ */

typedef struct {
    int  port;
    int  indice;   /* route /genN distincte par flux concurrent */
    volatile long* barriere;   /* compteur partage : synchronise le RST de
                                 * tous les flux au MEME instant, comme
                                 * `kill -9 $(jobs -p)` dans le repro testeur
                                 * (une mort simultanee, pas des morts
                                 * echelonnees independamment). */
    int  n_attendus;
} CrashClientArg;

static CR_THREAD_RET crash_client_run(void* arg) {
    CrashClientArg* ca = (CrashClientArg*)arg;

    cr_sock_t s = (cr_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (s == CR_INVALID) return CR_THREAD_0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)ca->port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        CR_CLOSESOCK(s); return CR_THREAD_0;
    }

    char req[64];
    snprintf(req, sizeof(req), "GET /gen%d HTTP/1.1\r\nHost: h\r\n\r\n", ca->indice);
    send(s, req, (int)strlen(req), 0);

#ifdef _WIN32
    DWORD tv = 30;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 30 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
    char buf[256];
    recv(s, buf, sizeof(buf), 0);   /* borne par le SO_RCVTIMEO ci-dessus */

    /* Barriere par spin (bornee : chaque thread arrive au plus tard apres
     * son propre SO_RCVTIMEO, donc l'attente ici est courte en pratique) —
     * tous les flux concurrents attendent que TOUS aient fini leur lecture
     * avant de couper, pour un RST simultane plutot qu'echelonne. */
    long avant;
#ifdef _WIN32
    avant = InterlockedIncrement(ca->barriere);
#else
    avant = __sync_add_and_fetch(ca->barriere, 1);
#endif
    (void)avant;
    while (*ca->barriere < ca->n_attendus) {
        CR_SLEEP(0);
    }

    /* RST immediat : SO_LINGER {on, 0} fait sauter la fermeture propre —
     * exactement le kill -9 mid-stream du repro testeur, ici synchronise
     * entre tous les flux concurrents de l'iteration. */
    struct linger lg; lg.l_onoff = 1; lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
    CR_CLOSESOCK(s);
    return CR_THREAD_0;
}

/* ================================================================
 * Sonde finale (mock peer) : requete courte APRES le stress, preuve que
 * le process a survecu ET reste dans un etat coherent (pas juste "pas
 * encore mort").
 * ================================================================ */

static void sonde_prepare(char* requete, size_t cap, const char* chemin) {
    snprintf(requete, cap, "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", chemin);
}

/* ================================================================
 * Boucle de stress : CRASH_ITERS iterations de CRASH_N flux concurrents
 * via l'executeur async (chemin de production W7), chacun coupe en RST
 * mid-stream. Montage UNIQUE reutilise sur toute la boucle (pas de
 * remontage par iteration) : c'est la charge repetee + concurrente qui
 * ouvre la fenetre de course, pas une seule occurrence isolee.
 * ================================================================ */

#define CRASH_N       8    /* >= 4, aligne sur le repro testeur (marge x2) */
#define CRASH_ITERS   150  /* boucle large : la race est un heisenbug bare-metal */

int http_llm_annulation_crash_self_test_run(void) {
    printf("== [ISOLE, hors --self-test-net] Repro CRASH race annulation streaming (fix/1723) ==\n");
    printf("   %d iterations x %d flux concurrents (executeur-async) + RST mid-stream\n",
           CRASH_ITERS, CRASH_N);
    fflush(stdout);

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = CRASH_N + 4;
    xc.plafond_attente_ms = 8000;
    xc.producteur         = producteur_crash;
    xc.producteur_user    = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) { fprintf(stderr, "  FAIL executeur_cree\n"); return 1; }
    if (!owl_http_executeur_demarre(x)) {
        fprintf(stderr, "  FAIL executeur_demarre\n");
        owl_http_executeur_libere(x);
        return 1;
    }

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, CRASH_N + 8, false)) {
        fprintf(stderr, "  FAIL ecoute_impossible\n");
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return 1;
    }
    int port = owl_tcp_listener_port(&l);

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;   /* chemin de production reel (serve_llm.c:1173) */

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = CRASH_N + 4;
    pc.connexions_max = 96;   /* large marge : la sonde finale ne doit jamais
                                * se heurter a une contre-pression transitoire
                                * du pool, seulement a un eventuel crash reel. */

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) {
        fprintf(stderr, "  FAIL pool_demarre\n");
        owl_tcp_listener_ferme(&l);
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return 1;
    }

    for (int it = 0; it < CRASH_ITERS; it++) {
        cr_thread_t th[CRASH_N];
        CrashClientArg args[CRASH_N];
        volatile long barriere = 0;
        for (int i = 0; i < CRASH_N; i++) {
            args[i].port       = port;
            args[i].indice     = i;
            args[i].barriere   = &barriere;
            args[i].n_attendus = CRASH_N;
            CR_START(th[i], crash_client_run, &args[i]);
        }
        for (int i = 0; i < CRASH_N; i++) CR_JOIN(th[i]);

        /* Progres visible et flushe : si le process crashe a l'iteration
         * K, cette ligne pour l'iteration K-1 (au moins) a deja atteint
         * le journal — un crash silencieux sans AUCUNE trace serait
         * indiagnosticable. */
        if ((it % 10) == 9) {
            fprintf(stderr, "  [crash-repro] iteration %d/%d survecue\n", it + 1, CRASH_ITERS);
            fflush(stderr);
        }
    }

    /* Laisse le transport achever le nettoyage des connexions RST de la
     * derniere iteration avant de solliciter la sonde -- evite de
     * confondre un 503 de contre-pression transitoire (pool momentanement
     * plein) avec une preuve d'incoherence. */
    CR_SLEEP(500);

    /* Sonde finale : le serveur doit encore repondre correctement APRES
     * la charge de stress, pas juste "le process est encore la". */
    char requete[96];
    sonde_prepare(requete, sizeof(requete), "/petit");
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = port;
    mc.octets                     = requete;
    mc.octets_len                 = strlen(requete);
    mc.decoupe                    = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = 8000;
    mc.fermer_apres_ecriture      = 1;
    OwlMockPeer* sonde = owl_mock_peer_demarre(&mc);
    owl_mock_peer_joint(sonde);
    size_t reponse_len = 0;
    const char* reponse = owl_mock_peer_recu(sonde, &reponse_len);
    int survie_ok = (strstr(reponse, "[DONE]") != NULL);
    owl_mock_peer_libere(sonde);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    if (!survie_ok) {
        fprintf(stderr, "  FAIL crash_repro_sonde_finale_non_servie (reponse=%.80s)\n",
                reponse ? reponse : "(vide)");
        printf("   TOTAL repro CRASH annulation (fix/1723) : 0/1 -- process survit mais sonde finale KO\n");
        return 1;
    }

    printf("   TOTAL repro CRASH annulation (fix/1723) : 1/1 -- %d iterations, process survit, sonde servie\n",
           CRASH_ITERS);
    return 0;
}
