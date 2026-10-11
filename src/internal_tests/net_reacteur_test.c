// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du cœur réacteur (chantier W1).
 *
 * Exécuté via `owl --self-test-net` (cf main.c). Model-free, sockets
 * loopback RÉELLES (paires) + timers. Aucun HTTP, aucune VM.
 *
 * Couvre :
 *   - réveil sur POLLIN (données prêtes), sur POLLOUT (écriture)
 *   - modifie_interet (bascule lecture <-> écriture)
 *   - désenregistrement (plus notifié)
 *   - timer : plus proche échéance réveille le poll ; échéance dépassée
 *     notifiée ; annulation ; ré-armement (repousse l'échéance)
 *   - pas de spin : au repos (fd non prêt, aucun timer dû) la boucle
 *     bloque bien dans poll pour toute la durée du plafond
 *   - robustesse : fd fermé côté pair, timeout=0 (non bloquant),
 *     plusieurs fds/timers mêlés, réacteur vide
 *
 * Retourne 0 si tout passe, sinon le nombre d'échecs. */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "net_reacteur.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define RC_CLOSE   closesocket
  static void rc_non_bloquant(owl_socket_fd_t fd) { u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define RC_CLOSE   close
  static void rc_non_bloquant(owl_socket_fd_t fd) {
      int fl = fcntl(fd, F_GETFL, 0);
      if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif

extern uint64_t owl_tcp_server_horloge_ms(void);

static int g_pass = 0;
static int g_fail = 0;

/* Horloge virtuelle pour les tests de roue de timers PURE (aucun fd) :
 * le test avance g_virt_ms à la main et tick en non bloquant (0), donc
 * les échéances firent selon le temps virtuel, jamais le wall-clock.
 * Déterministe et insensible à la charge CPU (cf. fix flakiness sous
 * charge). Les tests qui valident le timing RÉEL du poll/sleep
 * (timer_min_reveil, timer_immediat, pas_de_spin) gardent l'horloge
 * réelle par défaut. */
static uint64_t g_virt_ms = 0;
static uint64_t rc_horloge_virtuelle(void) { return g_virt_ms; }

#define RCHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ---------- Paire loopback réelle (127.0.0.1, port éphémère) ----------
 * Mono-thread : connect() vers un listener local aboutit via le
 * handshake noyau sans qu'accept() soit encore appelé (backlog). */
static int rc_make_pair(owl_socket_fd_t* pa, owl_socket_fd_t* pb) {
    owl_tcp_global_init();
    owl_socket_fd_t lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == OWL_INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(lst, (struct sockaddr*)&addr, sizeof(addr)) != 0) { RC_CLOSE(lst); return -1; }
    socklen_t al = sizeof(addr);
    if (getsockname(lst, (struct sockaddr*)&addr, &al) != 0) { RC_CLOSE(lst); return -1; }
    if (listen(lst, 1) != 0) { RC_CLOSE(lst); return -1; }

    owl_socket_fd_t cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == OWL_INVALID_SOCKET) { RC_CLOSE(lst); return -1; }
    if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        RC_CLOSE(cli); RC_CLOSE(lst); return -1;
    }
    owl_socket_fd_t srv = accept(lst, NULL, NULL);
    RC_CLOSE(lst);
    if (srv == OWL_INVALID_SOCKET) { RC_CLOSE(cli); return -1; }

    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    setsockopt(srv, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    rc_non_bloquant(cli);
    rc_non_bloquant(srv);

    *pa = cli;
    *pb = srv;
    return 0;
}

static void rc_ecrit_un_octet(owl_socket_fd_t fd) {
    char c = 'x';
    send(fd, &c, 1, 0);
}

/* ---------- Enregistreurs ---------- */

typedef struct {
    int appels;
    int lect;
    int ecr;
    int err;
    owl_socket_fd_t last_fd;
} FdRec;

static void fd_cb(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    (void)r;
    FdRec* rec = (FdRec*)user;
    rec->appels++;
    if (evts & OWL_REAC_LECTURE)  rec->lect++;
    if (evts & OWL_REAC_ECRITURE) rec->ecr++;
    if (evts & OWL_REAC_ERREUR)   rec->err++;
    rec->last_fd = fd;
}

typedef struct {
    int        fires;
    OwlTimerId last_id;
} TimerRec;

static void timer_cb(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)r;
    TimerRec* rec = (TimerRec*)user;
    rec->fires++;
    rec->last_id = id;
}

/* callback qui se désenregistre lui-même pendant le dispatch */
static void fd_cb_autoretire(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    FdRec* rec = (FdRec*)user;
    rec->appels++;
    if (evts & OWL_REAC_LECTURE) rec->lect++;
    rec->last_fd = fd;
    owl_reacteur_retire_fd(r, fd);
}

/* ================================================================ */

static void test_pollin_reveil(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (pollin)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    RCHECK(r != NULL, "cree (pollin)");
    FdRec rec; memset(&rec, 0, sizeof(rec));

    RCHECK(owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &rec), "ajoute_fd LECTURE");
    RCHECK(owl_reacteur_nb_fds(r) == 1, "nb_fds == 1");

    rc_ecrit_un_octet(b);
    int n = owl_reacteur_tick(r, 1000);
    RCHECK(n == 1, "tick dispatch == 1 (pollin)");
    RCHECK(rec.appels == 1, "cb appelé 1x (pollin)");
    RCHECK(rec.lect == 1, "cb LECTURE (pollin)");
    RCHECK(rec.ecr == 0, "cb pas ECRITURE (pollin)");
    RCHECK(rec.last_fd == a, "cb bon fd (pollin)");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_pollout_reveil(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (pollout)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    RCHECK(owl_reacteur_ajoute_fd(r, a, OWL_REAC_ECRITURE, fd_cb, &rec), "ajoute_fd ECRITURE");

    int n = owl_reacteur_tick(r, 1000);
    RCHECK(n == 1, "tick dispatch == 1 (pollout)");
    RCHECK(rec.ecr == 1, "cb ECRITURE (socket vide écrivable)");
    RCHECK(rec.lect == 0, "cb pas LECTURE (pollout)");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_modifie_interet(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (modif)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_ECRITURE, fd_cb, &rec);

    /* écrivable d'abord */
    owl_reacteur_tick(r, 1000);
    RCHECK(rec.ecr == 1, "modif: ECRITURE avant bascule");

    /* bascule vers LECTURE, envoie une donnée : ne doit plus voir ECRITURE */
    memset(&rec, 0, sizeof(rec));
    RCHECK(owl_reacteur_modifie_interet(r, a, OWL_REAC_LECTURE), "modifie_interet -> LECTURE");
    rc_ecrit_un_octet(b);
    owl_reacteur_tick(r, 1000);
    RCHECK(rec.lect == 1, "modif: LECTURE après bascule");
    RCHECK(rec.ecr == 0, "modif: plus d'ECRITURE après bascule");

    RCHECK(owl_reacteur_modifie_interet(r, (owl_socket_fd_t)0x7fffffff, OWL_REAC_LECTURE) == false,
           "modifie_interet fd inconnu -> false");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_desenregistrement(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (desenr)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &rec);
    rc_ecrit_un_octet(b);

    RCHECK(owl_reacteur_retire_fd(r, a), "retire_fd == true");
    RCHECK(owl_reacteur_nb_fds(r) == 0, "nb_fds == 0 après retrait");
    RCHECK(owl_reacteur_retire_fd(r, a) == false, "retire_fd 2e fois -> false");

    int n = owl_reacteur_tick(r, 50);
    RCHECK(n == 0, "tick == 0 après retrait");
    RCHECK(rec.appels == 0, "cb jamais appelé après retrait");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_autoretire_pendant_dispatch(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (autoretire)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb_autoretire, &rec);
    rc_ecrit_un_octet(b);

    owl_reacteur_tick(r, 1000);
    RCHECK(rec.appels == 1, "autoretire: cb appelé 1x");
    RCHECK(owl_reacteur_nb_fds(r) == 0, "autoretire: fd retiré depuis le cb");

    /* re-tick : le fd retiré ne rappelle pas, même si données encore là */
    memset(&rec, 0, sizeof(rec));
    owl_reacteur_tick(r, 50);
    RCHECK(rec.appels == 0, "autoretire: plus de cb après auto-retrait");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_timer_min_reveil(void) {
    /* Un fd enregistré NON prêt + un timer 50ms : le poll doit se
     * réveiller à l'échéance du timer, pas au plafond de 5000ms. */
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (timer_min)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec frec; memset(&frec, 0, sizeof(frec));
    TimerRec trec; memset(&trec, 0, sizeof(trec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &frec);

    OwlTimerId id = owl_reacteur_arme_timer(r, 50, timer_cb, &trec);
    RCHECK(id != OWL_TIMER_NUL, "arme_timer -> id valide");
    RCHECK(owl_reacteur_nb_timers(r) == 1, "nb_timers == 1");

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 5000);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 1, "tick == 1 (timer échu)");
    RCHECK(trec.fires == 1, "timer a fire 1x");
    RCHECK(trec.last_id == id, "timer bon id");
    RCHECK(frec.appels == 0, "fd non prêt non notifié");
    RCHECK(dt >= 30, "réveil ~à l'échéance timer (>=30ms)");
    RCHECK(dt < 2000, "réveil au timer, PAS au plafond 5000 (<2000ms)");
    RCHECK(owl_reacteur_nb_timers(r) == 0, "timer one-shot consommé");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_timer_immediat(void) {
    OwlReacteur* r = owl_reacteur_cree();
    TimerRec trec; memset(&trec, 0, sizeof(trec));
    owl_reacteur_arme_timer(r, 0, timer_cb, &trec);

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 1000);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 1, "tick == 1 (timer immédiat)");
    RCHECK(trec.fires == 1, "timer 0ms fire immédiatement");
    RCHECK(dt < 500, "timer immédiat ne bloque pas jusqu'au plafond");

    owl_reacteur_detruit(r);
}

static void test_annulation(void) {
    /* Horloge virtuelle : roue de timers PURE, déterministe. */
    OwlReacteur* r = owl_reacteur_cree();
    g_virt_ms = 5000;
    owl_reacteur_horloge_test(r, rc_horloge_virtuelle);

    TimerRec trec; memset(&trec, 0, sizeof(trec));
    OwlTimerId id = owl_reacteur_arme_timer(r, 40, timer_cb, &trec);   /* échéance 5040 */

    RCHECK(owl_reacteur_annule_timer(r, id), "annule_timer == true");
    RCHECK(owl_reacteur_nb_timers(r) == 0, "nb_timers == 0 après annulation");
    RCHECK(owl_reacteur_annule_timer(r, id) == false, "annule 2e fois -> false");

    /* on dépasse largement l'échéance d'origine : l'annulé ne fire jamais */
    g_virt_ms = 5200;
    owl_reacteur_tick(r, 0);   /* non bloquant : temps virtuel piloté à la main */
    RCHECK(trec.fires == 0, "timer annulé ne fire jamais");

    owl_reacteur_detruit(r);
}

static void test_rearme(void) {
    /* Timer 40ms ré-armé à 300ms : ne doit PAS fire avant la nouvelle
     * échéance, puis fire exactement 1x au-delà. Prouve que l'échéance
     * est repoussée. Horloge VIRTUELLE : le temps est piloté à la main,
     * tick en non bloquant (0) — aucun wall-clock, donc reproductible
     * sous n'importe quelle charge CPU. */
    OwlReacteur* r = owl_reacteur_cree();
    g_virt_ms = 1000;                                   /* origine arbitraire non nulle */
    owl_reacteur_horloge_test(r, rc_horloge_virtuelle);

    TimerRec trec; memset(&trec, 0, sizeof(trec));
    OwlTimerId id = owl_reacteur_arme_timer(r, 40, timer_cb, &trec);   /* échéance 1040 */

    RCHECK(owl_reacteur_rearme_timer(r, id, 300), "rearme_timer == true");  /* échéance 1300 */
    RCHECK(owl_reacteur_nb_timers(r) == 1, "nb_timers == 1 après rearme");

    /* t=1120 : au-delà de l'échéance d'origine (1040), avant la nouvelle
     * (1300) -> ne DOIT pas fire (l'échéance a bien été repoussée). */
    g_virt_ms = 1120;
    owl_reacteur_tick(r, 0);
    RCHECK(trec.fires == 0, "rearmé : pas de fire avant nouvelle échéance");

    /* t=1300 : nouvelle échéance atteinte -> fire exactement 1x. */
    g_virt_ms = 1300;
    owl_reacteur_tick(r, 0);
    RCHECK(trec.fires == 1, "rearmé : fire à la nouvelle échéance");
    RCHECK(trec.last_id == id, "rearmé : bon id au fire");
    RCHECK(owl_reacteur_nb_timers(r) == 0, "rearmé : timer one-shot consommé après fire");

    /* le timer a expiré : le ré-armer échoue (id mort). */
    RCHECK(owl_reacteur_rearme_timer(r, id, 100) == false, "rearme timer expiré -> false");

    owl_reacteur_detruit(r);
}

static void test_pas_de_spin(void) {
    /* fd enregistré NON prêt, aucun timer : tick(plafond) doit BLOQUER
     * dans poll toute la durée du plafond. S'il spinnait, dt ~= 0. */
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (spin)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &rec);

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 200);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    RCHECK(n == 0, "tick == 0 au repos");
    RCHECK(rec.appels == 0, "aucun cb au repos");
    RCHECK(dt >= 150, "pas de spin : poll a bloqué ~200ms (>=150)");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_timeout_zero(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (t0)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &rec);

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, 0);           /* non bloquant */
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;
    RCHECK(n == 0, "tick(0) == 0 (rien prêt)");
    RCHECK(dt < 100, "tick(0) ne bloque pas");

    owl_reacteur_detruit(r);
    RC_CLOSE(a); RC_CLOSE(b);
}

static void test_fd_ferme(void) {
    owl_socket_fd_t a, b;
    if (rc_make_pair(&a, &b) != 0) { g_fail++; fprintf(stderr, "  FAIL: make_pair (ferme)\n"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec rec; memset(&rec, 0, sizeof(rec));
    owl_reacteur_ajoute_fd(r, a, OWL_REAC_LECTURE, fd_cb, &rec);

    RC_CLOSE(b);   /* le pair raccroche */

    int n = owl_reacteur_tick(r, 1000);
    RCHECK(n >= 1, "fd fermé : tick notifie");
    RCHECK(rec.appels >= 1, "fd fermé : cb appelé");
    /* selon la plateforme : POLLHUP/ERR (-> ERREUR) ou POLLIN (recv==0) */
    RCHECK(rec.err > 0 || rec.lect > 0, "fd fermé : ERREUR ou LECTURE remonté");

    owl_reacteur_detruit(r);
    RC_CLOSE(a);
}

static void test_plusieurs_meles(void) {
    owl_socket_fd_t a1, b1, a2, b2;
    if (rc_make_pair(&a1, &b1) != 0 || rc_make_pair(&a2, &b2) != 0) {
        g_fail++; fprintf(stderr, "  FAIL: make_pair (meles)\n"); return;
    }

    OwlReacteur* r = owl_reacteur_cree();
    FdRec r1, r2; memset(&r1, 0, sizeof(r1)); memset(&r2, 0, sizeof(r2));
    TimerRec trec; memset(&trec, 0, sizeof(trec));

    owl_reacteur_ajoute_fd(r, a1, OWL_REAC_LECTURE, fd_cb, &r1);
    owl_reacteur_ajoute_fd(r, a2, OWL_REAC_LECTURE, fd_cb, &r2);
    RCHECK(owl_reacteur_nb_fds(r) == 2, "nb_fds == 2 (mêlés)");

    owl_reacteur_arme_timer(r, 30, timer_cb, &trec);
    rc_ecrit_un_octet(b2);   /* seul a2 devient prêt */

    /* tick 1 : données sur a2 prêtes immédiatement, timer pas encore dû */
    int n1 = owl_reacteur_tick(r, 1000);
    RCHECK(n1 == 1, "mêlés: tick1 dispatch == 1 (a2)");
    RCHECK(r2.lect == 1, "mêlés: a2 notifié");
    RCHECK(r1.appels == 0, "mêlés: a1 non notifié");
    RCHECK(trec.fires == 0, "mêlés: timer pas encore échu au tick1");

    /* Draine l'octet de a2 : le réacteur ne consomme JAMAIS les octets
     * (c'est le rôle du callback / W2). Sans ce drain, a2 reste lisible
     * et re-réveille le poll à chaque tick, affamant le timer. */
    { char poub[8]; recv(a2, poub, (int)sizeof(poub), 0); }

    /* tick 2 : rien de prêt, on attend jusqu'au timer */
    int n2 = owl_reacteur_tick(r, 1000);
    RCHECK(n2 == 1, "mêlés: tick2 dispatch == 1 (timer)");
    RCHECK(trec.fires == 1, "mêlés: timer échu au tick2");

    owl_reacteur_detruit(r);
    RC_CLOSE(a1); RC_CLOSE(b1); RC_CLOSE(a2); RC_CLOSE(b2);
}

static void test_reacteur_vide(void) {
    OwlReacteur* r = owl_reacteur_cree();
    RCHECK(owl_reacteur_nb_fds(r) == 0, "vide: nb_fds == 0");
    RCHECK(owl_reacteur_nb_timers(r) == 0, "vide: nb_timers == 0");

    /* aucun fd, aucun timer, plafond infini : rien à attendre -> 0 immédiat */
    uint64_t t0 = owl_tcp_server_horloge_ms();
    int n = owl_reacteur_tick(r, -1);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;
    RCHECK(n == 0, "vide: tick(-1) == 0");
    RCHECK(dt < 100, "vide: tick(-1) ne bloque pas indéfiniment");

    RCHECK(owl_reacteur_arret_demande(r) == false, "vide: arrêt non demandé");

    owl_reacteur_detruit(r);
}

int net_reacteur_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_pollin_reveil();
    test_pollout_reveil();
    test_modifie_interet();
    test_desenregistrement();
    test_autoretire_pendant_dispatch();
    test_timer_min_reveil();
    test_timer_immediat();
    test_annulation();
    test_rearme();
    test_pas_de_spin();
    test_timeout_zero();
    test_fd_ferme();
    test_plusieurs_meles();
    test_reacteur_vide();

    fprintf(stderr, "net_reacteur: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
