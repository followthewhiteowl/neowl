// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* net_reacteur.c — cœur réacteur (chantier W1).
 *
 * Boucle poll-many + roue de timers, mécanique événementielle PURE.
 * Cf. net_reacteur.h pour le contrat et l'isolation GC (100 % C-heap,
 * aucun Obj*, aucune connaissance de la VM).
 *
 * Le shim poll/WSAPoll est identique à net_tcp_server.c : cinq lignes
 * de plateforme, pas une réinvention de la couche socket. L'horloge
 * monotone est réutilisée telle quelle (owl_tcp_server_horloge_ms).
 *
 * Choix de structure : fds et timers en tableaux COMPACTS (swap-remove
 * à la suppression). O(n)/tick pour le scan des échéances et la
 * construction du pollset — cohérent avec le poll O(n)/tick lui-même,
 * suffisant pour des centaines de connexions (design §3). Une roue
 * hachée ou un epoll/kqueue s'ajouteront si l'échelle l'exige, derrière
 * la même API. */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600   /* WSAPoll : Vista+ (mingw-w64) */
  #endif
#endif

#include "net_reacteur.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define OWL_SRV_ERRNO   WSAGetLastError()
  #define OWL_SRV_EINTR   WSAEINTR
  #define owl_srv_poll    WSAPoll
  typedef WSAPOLLFD owl_srv_pollfd;
  #define REAC_SLEEP(ms)  Sleep((DWORD)(ms))
#else
  #include <poll.h>
  #include <time.h>
  #include <errno.h>
  #define OWL_SRV_ERRNO   errno
  #define OWL_SRV_EINTR   EINTR
  #define owl_srv_poll    poll
  typedef struct pollfd owl_srv_pollfd;
  static void reac_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define REAC_SLEEP(ms)  reac_sleep_ms((int)(ms))
#endif

/* Horloge monotone : réutilise celle du socket serveur (net_tcp_server.c).
 * Extern-déclarée plutôt qu'incluse pour ne pas traîner les en-têtes TLS
 * dans un module qui ne fait que du transport nu. */
extern uint64_t owl_tcp_server_horloge_ms(void);

/* Plafond de latence d'arrêt de owl_reacteur_boucle tant que W4 n'a pas
 * posé de fd de réveil : la boucle re-vérifie l'arrêt au plus tard tous
 * les 100 ms quand rien d'autre ne la réveille. Ce n'est pas un spin
 * (elle bloque bien dans poll/sleep), c'est la granularité d'arrêt. */
#define OWL_REAC_ARRET_LATENCE_MS 100

typedef struct {
    owl_socket_fd_t   fd;        /* OWL_INVALID_SOCKET = jamais (tableau compact) */
    short             interet;
    OwlReacteurFdCb   cb;
    void*             user;
} ReacFd;

typedef struct {
    OwlTimerId          id;      /* OWL_TIMER_NUL = jamais (tableau compact) */
    uint64_t            echeance_ms;
    OwlReacteurTimerCb  cb;
    void*               user;
} ReacTimer;

struct OwlReacteur {
    ReacFd*    fds;
    size_t     fds_n;
    size_t     fds_cap;

    ReacTimer* timers;
    size_t     timers_n;
    size_t     timers_cap;

    OwlTimerId prochain_id;      /* monotone, jamais réutilisé (pas d'ABA) */
    volatile int arret;

    /* Source d'horloge monotone. Défaut prod : owl_tcp_server_horloge_ms.
     * Overridable par les tests (owl_reacteur_horloge_test) pour piloter
     * une horloge virtuelle déterministe. Jamais NULL après cree. */
    uint64_t (*horloge)(void);

    owl_srv_pollfd* pollset;     /* scratch réutilisé entre ticks */
    size_t          pollset_cap;

    OwlTimerId*     due;         /* scratch : snapshot des timers échus */
    size_t          due_cap;
};

/* ---------- Croissance des tableaux ---------- */

static bool reac_reserve_fds(OwlReacteur* r, size_t besoin) {
    if (besoin <= r->fds_cap) return true;
    size_t cap = r->fds_cap ? r->fds_cap * 2 : 8;
    while (cap < besoin) cap *= 2;
    ReacFd* nv = (ReacFd*)realloc(r->fds, cap * sizeof(ReacFd));
    if (!nv) return false;
    r->fds = nv;
    r->fds_cap = cap;
    return true;
}

static bool reac_reserve_timers(OwlReacteur* r, size_t besoin) {
    if (besoin <= r->timers_cap) return true;
    size_t cap = r->timers_cap ? r->timers_cap * 2 : 8;
    while (cap < besoin) cap *= 2;
    ReacTimer* nv = (ReacTimer*)realloc(r->timers, cap * sizeof(ReacTimer));
    if (!nv) return false;
    r->timers = nv;
    r->timers_cap = cap;
    return true;
}

static bool reac_reserve_pollset(OwlReacteur* r, size_t besoin) {
    if (besoin <= r->pollset_cap) return true;
    size_t cap = r->pollset_cap ? r->pollset_cap * 2 : 8;
    while (cap < besoin) cap *= 2;
    owl_srv_pollfd* nv = (owl_srv_pollfd*)realloc(r->pollset, cap * sizeof(owl_srv_pollfd));
    if (!nv) return false;
    r->pollset = nv;
    r->pollset_cap = cap;
    return true;
}

static bool reac_reserve_due(OwlReacteur* r, size_t besoin) {
    if (besoin <= r->due_cap) return true;
    size_t cap = r->due_cap ? r->due_cap * 2 : 8;
    while (cap < besoin) cap *= 2;
    OwlTimerId* nv = (OwlTimerId*)realloc(r->due, cap * sizeof(OwlTimerId));
    if (!nv) return false;
    r->due = nv;
    r->due_cap = cap;
    return true;
}

/* ---------- Recherche ---------- */

static long reac_find_fd(const OwlReacteur* r, owl_socket_fd_t fd) {
    for (size_t i = 0; i < r->fds_n; i++)
        if (r->fds[i].fd == fd) return (long)i;
    return -1;
}

static long reac_find_timer(const OwlReacteur* r, OwlTimerId id) {
    for (size_t i = 0; i < r->timers_n; i++)
        if (r->timers[i].id == id) return (long)i;
    return -1;
}

/* ---------- Cycle de vie ---------- */

OwlReacteur* owl_reacteur_cree(void) {
    OwlReacteur* r = (OwlReacteur*)calloc(1, sizeof(OwlReacteur));
    if (!r) return NULL;
    r->prochain_id = 1;   /* 0 réservé à OWL_TIMER_NUL */
    r->horloge = owl_tcp_server_horloge_ms;   /* défaut prod : horloge réelle */
    return r;
}

void owl_reacteur_horloge_test(OwlReacteur* r, uint64_t (*horloge)(void)) {
    if (!r) return;
    r->horloge = horloge ? horloge : owl_tcp_server_horloge_ms;
}

void owl_reacteur_detruit(OwlReacteur* r) {
    if (!r) return;
    free(r->fds);
    free(r->timers);
    free(r->pollset);
    free(r->due);
    free(r);
}

/* ---------- Descripteurs ---------- */

bool owl_reacteur_ajoute_fd(OwlReacteur* r, owl_socket_fd_t fd,
                            short interet, OwlReacteurFdCb cb, void* user) {
    if (!r || fd == OWL_INVALID_SOCKET || !cb) return false;
    if (reac_find_fd(r, fd) >= 0) return false;   /* déjà enregistré */
    if (!reac_reserve_fds(r, r->fds_n + 1)) return false;
    ReacFd* e = &r->fds[r->fds_n++];
    e->fd = fd;
    e->interet = interet;
    e->cb = cb;
    e->user = user;
    return true;
}

bool owl_reacteur_modifie_interet(OwlReacteur* r, owl_socket_fd_t fd, short interet) {
    if (!r) return false;
    long i = reac_find_fd(r, fd);
    if (i < 0) return false;
    r->fds[i].interet = interet;
    return true;
}

bool owl_reacteur_retire_fd(OwlReacteur* r, owl_socket_fd_t fd) {
    if (!r) return false;
    long i = reac_find_fd(r, fd);
    if (i < 0) return false;
    r->fds[i] = r->fds[r->fds_n - 1];   /* swap-remove : ordre non significatif */
    r->fds_n--;
    return true;
}

size_t owl_reacteur_nb_fds(const OwlReacteur* r) { return r ? r->fds_n : 0; }

/* ---------- Roue de timers ---------- */

OwlTimerId owl_reacteur_arme_timer(OwlReacteur* r, uint64_t delai_ms,
                                   OwlReacteurTimerCb cb, void* user) {
    if (!r || !cb) return OWL_TIMER_NUL;
    if (!reac_reserve_timers(r, r->timers_n + 1)) return OWL_TIMER_NUL;
    OwlTimerId id = r->prochain_id++;
    ReacTimer* t = &r->timers[r->timers_n++];
    t->id = id;
    t->echeance_ms = r->horloge() + delai_ms;
    t->cb = cb;
    t->user = user;
    return id;
}

bool owl_reacteur_rearme_timer(OwlReacteur* r, OwlTimerId id, uint64_t delai_ms) {
    if (!r || id == OWL_TIMER_NUL) return false;
    long i = reac_find_timer(r, id);
    if (i < 0) return false;
    r->timers[i].echeance_ms = r->horloge() + delai_ms;
    return true;
}

bool owl_reacteur_annule_timer(OwlReacteur* r, OwlTimerId id) {
    if (!r || id == OWL_TIMER_NUL) return false;
    long i = reac_find_timer(r, id);
    if (i < 0) return false;
    r->timers[i] = r->timers[r->timers_n - 1];   /* swap-remove */
    r->timers_n--;
    return true;
}

size_t owl_reacteur_nb_timers(const OwlReacteur* r) { return r ? r->timers_n : 0; }

/* ---------- Boucle ---------- */

/* Timeout poll effectif = min(plus proche échéance, plafond). -1 = infini. */
static int reac_timeout_effectif(OwlReacteur* r, uint64_t now, int plafond) {
    bool a_timer = false;
    uint64_t min_ech = 0;
    for (size_t i = 0; i < r->timers_n; i++) {
        if (!a_timer || r->timers[i].echeance_ms < min_ech) {
            min_ech = r->timers[i].echeance_ms;
            a_timer = true;
        }
    }
    if (!a_timer) return plafond;   /* peut être -1 */

    int delta = (min_ech <= now) ? 0 : (int)(min_ech - now);
    if (plafond < 0) return delta;
    return (delta < plafond) ? delta : plafond;
}

static int reac_dispatch_fds(OwlReacteur* r, size_t np) {
    int count = 0;
    for (size_t i = 0; i < np; i++) {
        short rev = r->pollset[i].revents;
        if (rev == 0) continue;
        owl_socket_fd_t fd = r->pollset[i].fd;
        long idx = reac_find_fd(r, fd);
        if (idx < 0) continue;   /* retiré par un callback précédent de ce tick */

        short evts = 0;
        if (rev & POLLIN)  evts |= OWL_REAC_LECTURE;
        if (rev & POLLOUT) evts |= OWL_REAC_ECRITURE;
        if (rev & (POLLERR | POLLHUP | POLLNVAL)) evts |= OWL_REAC_ERREUR;

        /* Ne livrer que ce qui est encore demandé (l'intérêt a pu changer
         * dans un callback précédent), l'erreur toujours (un fd mort doit
         * pouvoir être nettoyé). */
        short interet = r->fds[idx].interet;
        short livre = (short)((evts & interet) | (evts & OWL_REAC_ERREUR));
        if (livre == 0) continue;

        count++;
        r->fds[idx].cb(r, fd, livre, r->fds[idx].user);
    }
    return count;
}

static int reac_dispatch_timers(OwlReacteur* r, uint64_t now) {
    /* Snapshot des échus AVANT de fire : un callback peut annuler ou
     * ré-armer d'autres timers, ou en ajouter. Les timers ajoutés par un
     * callback fireront à un tick ultérieur, jamais celui-ci. */
    size_t nd = 0;
    for (size_t i = 0; i < r->timers_n; i++) {
        if (r->timers[i].echeance_ms <= now) {
            if (!reac_reserve_due(r, nd + 1)) break;
            r->due[nd++] = r->timers[i].id;
        }
    }

    int count = 0;
    for (size_t k = 0; k < nd; k++) {
        OwlTimerId id = r->due[k];
        long idx = reac_find_timer(r, id);
        if (idx < 0) continue;                       /* annulé entre-temps */
        if (r->timers[idx].echeance_ms > now) continue; /* ré-armé au futur */

        OwlReacteurTimerCb cb = r->timers[idx].cb;
        void* user = r->timers[idx].user;
        /* One-shot : retirer AVANT de fire (l'id est mort côté callback). */
        r->timers[idx] = r->timers[r->timers_n - 1];
        r->timers_n--;

        count++;
        cb(r, id, user);
    }
    return count;
}

int owl_reacteur_tick(OwlReacteur* r, int timeout_plafond_ms) {
    if (!r) return -1;

    uint64_t now = r->horloge();
    int eff = reac_timeout_effectif(r, now, timeout_plafond_ms);

    /* Construire le pollset depuis les fds enregistrés. */
    size_t np = 0;
    if (r->fds_n > 0) {
        if (!reac_reserve_pollset(r, r->fds_n)) return -1;
        for (size_t i = 0; i < r->fds_n; i++) {
            short ev = 0;
            if (r->fds[i].interet & OWL_REAC_LECTURE)  ev |= POLLIN;
            if (r->fds[i].interet & OWL_REAC_ECRITURE) ev |= POLLOUT;
            r->pollset[np].fd = r->fds[i].fd;
            r->pollset[np].events = ev;
            r->pollset[np].revents = 0;
            np++;
        }
    }

    int pr = 0;
    if (np == 0) {
        /* Aucun fd à observer. Sans timer ni plafond fini, rien ne peut
         * réveiller : retour immédiat (le contrat garantit qu'une boucle
         * réelle porte toujours au moins un fd de réveil, W4). Sinon on
         * dort jusqu'à l'échéance/au plafond (pas de poll sur 0 fd :
         * WSAPoll le refuse). */
        if (eff < 0) return 0;
        if (eff > 0) REAC_SLEEP(eff);
    } else {
        for (;;) {
            pr = owl_srv_poll(r->pollset, (unsigned)np, eff);
            if (pr >= 0) break;
            if (OWL_SRV_ERRNO == OWL_SRV_EINTR) continue;
            return -1;
        }
    }

    uint64_t now2 = r->horloge();

    int count = 0;
    if (np > 0 && pr > 0) count += reac_dispatch_fds(r, np);
    count += reac_dispatch_timers(r, now2);
    return count;
}

void owl_reacteur_boucle(OwlReacteur* r) {
    if (!r) return;
    while (!r->arret) {
        if (owl_reacteur_tick(r, OWL_REAC_ARRET_LATENCE_MS) < 0) break;
    }
}

void owl_reacteur_demande_arret(OwlReacteur* r) {
    if (r) r->arret = 1;
}

bool owl_reacteur_arret_demande(const OwlReacteur* r) {
    return r && r->arret != 0;
}
