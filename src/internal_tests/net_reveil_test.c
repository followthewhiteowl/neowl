// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du réveil inter-thread self-pipe (chantier W4).
 *
 * Exécuté via `owl --self-test-net` (cf main.c). Model-free, sockets
 * loopback RÉELLES (la paire du self-pipe) + un thread tiers réel.
 * Aucun HTTP, aucune VM.
 *
 * Couvre :
 *   - réveil déterministe depuis un THREAD TIERS : la boucle dort dans
 *     poll, le thread signale, la boucle se réveille bien avant le
 *     plafond et le callback tire
 *   - coalescing : N signals avant drain → un seul réveil, cb tire une
 *     fois, et le fd est intégralement drainé (pas de re-réveil fantôme)
 *   - pas de perte de réveil sous rafale concurrente (plusieurs threads)
 *   - pas de spin : self-pipe enregistré mais non signalé → poll bloque
 *     toute la durée du plafond
 *   - isolation GC (§8 Q1) : SEULS des octets traversent la frontière ;
 *     le callback ne reçoit qu'un pointeur C-heap opaque, jamais un
 *     Value/Obj*. Structurel : ce fichier n'inclut ni object.h ni vm.h.
 *
 * Retourne 0 si tout passe, sinon le nombre d'échecs. */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "net_reveil.h"
#include "net_reacteur.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  typedef HANDLE rv_thread_t;
#else
  #include <pthread.h>
  #include <time.h>
  #include <unistd.h>
  typedef pthread_t rv_thread_t;
#endif

extern uint64_t owl_tcp_server_horloge_ms(void);

static int g_pass = 0;
static int g_fail = 0;

#define RCHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ---------- Portabilité thread + sleep ---------- */

static void rv_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&ts, NULL);
#endif
}

/* Argument du thread tiers : le réveil à signaler, un délai avant de
 * signaler, et un nombre de coups. Tout est C-heap pur — aucun Obj*,
 * aucun Value ne franchit la frontière de thread (§8 Q1). */
typedef struct {
    OwlReveil* rv;
    int        delai_ms;
    int        coups;
} SignaleurArg;

#ifdef _WIN32
static DWORD WINAPI rv_signaleur_fn(LPVOID p) {
#else
static void* rv_signaleur_fn(void* p) {
#endif
    SignaleurArg* a = (SignaleurArg*)p;
    if (a->delai_ms > 0) rv_sleep_ms(a->delai_ms);
    for (int i = 0; i < a->coups; i++) owl_reveil_signale(a->rv);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int rv_thread_lance(rv_thread_t* t, SignaleurArg* a) {
#ifdef _WIN32
    *t = CreateThread(NULL, 0, rv_signaleur_fn, a, 0, NULL);
    return *t != NULL ? 0 : -1;
#else
    return pthread_create(t, NULL, rv_signaleur_fn, a) == 0 ? 0 : -1;
#endif
}

static void rv_thread_joint(rv_thread_t t) {
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

/* ---------- Callback de réveil : compte les invocations ---------- */

typedef struct {
    int   reveils;      /* nombre d'appels du callback */
    void* vu_user;      /* dernier pointeur user reçu (contrôle d'identité) */
} ReveilRec;

static void reveil_cb(void* user) {
    ReveilRec* rec = (ReveilRec*)user;
    rec->reveils++;
    rec->vu_user = user;
}

/* ================================================================ */

static void test_cree_detruit(void) {
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, NULL, NULL), "cree self-pipe");
    RCHECK(rv.fd_lecture != OWL_INVALID_SOCKET, "fd_lecture valide");
    RCHECK(rv.fd_ecriture != OWL_INVALID_SOCKET, "fd_ecriture valide");
    RCHECK(rv.fd_lecture != rv.fd_ecriture, "les deux bouts distincts");
    owl_reveil_detruit(&rv, NULL);
}

static void test_signale_puis_tick(void) {
    /* Signal AVANT le tick : le premier tick voit l'octet et draine. */
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, reveil_cb, NULL), "cree (signale_puis_tick)");

    OwlReacteur* r = owl_reacteur_cree();
    ReveilRec rec; memset(&rec, 0, sizeof(rec));
    rv.user = &rec;
    RCHECK(owl_reveil_enregistre(&rv, r), "enregistre dans le reacteur");
    RCHECK(owl_reacteur_nb_fds(r) == 1, "nb_fds == 1 (fd de reveil)");

    owl_reveil_signale(&rv);
    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 2000);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 1, "tick dispatch == 1 (reveil)");
    RCHECK(rec.reveils == 1, "cb reveil tire 1x");
    RCHECK(rec.vu_user == &rec, "cb recoit le bon user (C-heap, pas d'Obj*)");
    RCHECK(dt < 500, "reveil immediat, pas d'attente jusqu'au plafond");

    /* Deuxième tick : fd drainé, plus rien de prêt → rien ne tire. */
    memset(&rec, 0, sizeof(rec));
    int n2 = owl_reacteur_tick(r, 50);
    RCHECK(n2 == 0, "tick2 == 0 (fd draine, pas de re-reveil fantome)");
    RCHECK(rec.reveils == 0, "cb ne re-tire pas apres drain");

    owl_reveil_detruit(&rv, r);
    owl_reacteur_detruit(r);
}

static void test_reveil_thread_tiers(void) {
    /* Le cœur du chantier : la boucle DORT dans poll(5000ms), un thread
     * tiers signale après 60ms, la boucle se réveille bien avant le
     * plafond. Déterministe : le thread signale une fois. */
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, reveil_cb, NULL), "cree (thread_tiers)");

    OwlReacteur* r = owl_reacteur_cree();
    ReveilRec rec; memset(&rec, 0, sizeof(rec));
    rv.user = &rec;
    owl_reveil_enregistre(&rv, r);

    SignaleurArg arg; arg.rv = &rv; arg.delai_ms = 60; arg.coups = 1;
    rv_thread_t th;
    RCHECK(rv_thread_lance(&th, &arg) == 0, "lance thread tiers");

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 5000);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 1, "tick == 1 (reveil du thread tiers)");
    RCHECK(rec.reveils == 1, "cb tire depuis le reveil du thread");
    RCHECK(dt >= 40, "reveil ~a l'echeance du signal (>=40ms)");
    RCHECK(dt < 2500, "reveil au signal, PAS au plafond 5000 (<2500ms)");

    rv_thread_joint(th);
    owl_reveil_detruit(&rv, r);
    owl_reacteur_detruit(r);
}

static void test_coalescing(void) {
    /* N signals empilés AVANT le drain → un seul réveil suffit : le
     * callback tire une seule fois, et le fd est intégralement drainé
     * (le tick suivant ne re-réveille pas). */
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, reveil_cb, NULL), "cree (coalescing)");

    OwlReacteur* r = owl_reacteur_cree();
    ReveilRec rec; memset(&rec, 0, sizeof(rec));
    rv.user = &rec;
    owl_reveil_enregistre(&rv, r);

    for (int i = 0; i < 200; i++) owl_reveil_signale(&rv);

    int n = owl_reacteur_tick(r, 1000);
    RCHECK(n == 1, "coalescing: tick == 1 dispatch (un seul reveil fd)");
    RCHECK(rec.reveils == 1, "coalescing: cb tire 1x pour 200 signals");

    /* Prouve le drain complet : plus rien de prêt. */
    memset(&rec, 0, sizeof(rec));
    int n2 = owl_reacteur_tick(r, 50);
    RCHECK(n2 == 0, "coalescing: fd draine, tick2 == 0");
    RCHECK(rec.reveils == 0, "coalescing: pas de reveil residuel");

    owl_reveil_detruit(&rv, r);
    owl_reacteur_detruit(r);
}

static void test_rafale_concurrente(void) {
    /* Plusieurs threads signalent en rafale : aucun réveil perdu. La
     * boucle draine en plusieurs ticks, mais au TOTAL au moins un
     * réveil est vu, et à la fin tout est drainé (pas de spin infini). */
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, reveil_cb, NULL), "cree (rafale)");

    OwlReacteur* r = owl_reacteur_cree();
    ReveilRec rec; memset(&rec, 0, sizeof(rec));
    rv.user = &rec;
    owl_reveil_enregistre(&rv, r);

    enum { NTH = 4 };
    SignaleurArg args[NTH];
    rv_thread_t ths[NTH];
    for (int i = 0; i < NTH; i++) {
        args[i].rv = &rv; args[i].delai_ms = 0; args[i].coups = 50;
        RCHECK(rv_thread_lance(&ths[i], &args[i]) == 0, "lance thread rafale");
    }
    for (int i = 0; i < NTH; i++) rv_thread_joint(ths[i]);

    /* Tout est écrit : un tick draine tout ce qui est arrivé. */
    int n = owl_reacteur_tick(r, 1000);
    RCHECK(n == 1, "rafale: tick draine (>=1 dispatch)");
    RCHECK(rec.reveils >= 1, "rafale: au moins un reveil vu (aucun perdu)");

    /* Draine à fond : après ça, plus rien de prêt. */
    int gardefou = 0;
    while (owl_reacteur_tick(r, 50) > 0 && gardefou < 100) gardefou++;
    RCHECK(gardefou < 100, "rafale: le fd finit par se vider (pas de spin)");

    int nfinal = owl_reacteur_tick(r, 50);
    RCHECK(nfinal == 0, "rafale: fd entierement draine a la fin");

    owl_reveil_detruit(&rv, r);
    owl_reacteur_detruit(r);
}

static void test_pas_de_spin(void) {
    /* Self-pipe enregistré mais JAMAIS signalé : la boucle doit bloquer
     * toute la durée du plafond. Si le fd de réveil « fuyait » un
     * POLLIN permanent (mauvais drain), dt ~= 0. */
    OwlReveil rv;
    RCHECK(owl_reveil_cree(&rv, reveil_cb, NULL), "cree (spin)");

    OwlReacteur* r = owl_reacteur_cree();
    ReveilRec rec; memset(&rec, 0, sizeof(rec));
    rv.user = &rec;
    owl_reveil_enregistre(&rv, r);

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 200);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 0, "spin: tick == 0 au repos");
    RCHECK(rec.reveils == 0, "spin: aucun reveil au repos");
    RCHECK(dt >= 150, "spin: poll a bloque ~200ms (pas de spin)");

    owl_reveil_detruit(&rv, r);
    owl_reacteur_detruit(r);
}

int net_reveil_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_cree_detruit();
    test_signale_puis_tick();
    test_reveil_thread_tiers();
    test_coalescing();
    test_rafale_concurrente();
    test_pas_de_spin();

    fprintf(stderr, "net_reveil: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
