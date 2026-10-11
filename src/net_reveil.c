// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* net_reveil.c — réveil inter-thread self-pipe (chantier W4).
 *
 * Cf. net_reveil.h pour le contrat et l'isolation GC. Paire de sockets
 * loopback 127.0.0.1 : seul mécanisme portable ET pollable par WSAPoll
 * (eventfd absent de Windows, pipes Windows non pollables). Le côté
 * lecture s'enregistre dans le réacteur (W1) comme un fd ordinaire ; le
 * côté écriture est signalé depuis n'importe quel thread.
 *
 * La création de la paire réutilise le même tour de passe-passe que le
 * self-test réacteur : listener éphémère sur 127.0.0.1, connect(),
 * accept(). Mono-thread au montage — la frontière de thread ne joue
 * qu'à l'exécution (signale depuis le thread tiers). */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "net_reveil.h"

#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define RV_CLOSE   closesocket
  static void rv_non_bloquant(owl_socket_fd_t fd) { u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define RV_CLOSE   close
  static void rv_non_bloquant(owl_socket_fd_t fd) {
      int fl = fcntl(fd, F_GETFL, 0);
      if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif

/* Réutilise l'init WSA global (idempotent sous Win32). */
extern bool owl_tcp_global_init(void);

/* ---------- Création de la paire loopback ---------- */

static int rv_make_pair(owl_socket_fd_t* pr, owl_socket_fd_t* pw) {
    owl_tcp_global_init();
    owl_socket_fd_t lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == OWL_INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;                       /* port éphémère */
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(lst, (struct sockaddr*)&addr, sizeof(addr)) != 0) { RV_CLOSE(lst); return -1; }
    socklen_t al = sizeof(addr);
    if (getsockname(lst, (struct sockaddr*)&addr, &al) != 0) { RV_CLOSE(lst); return -1; }
    if (listen(lst, 1) != 0) { RV_CLOSE(lst); return -1; }

    owl_socket_fd_t cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == OWL_INVALID_SOCKET) { RV_CLOSE(lst); return -1; }
    if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        RV_CLOSE(cli); RV_CLOSE(lst); return -1;
    }
    owl_socket_fd_t srv = accept(lst, NULL, NULL);
    RV_CLOSE(lst);
    if (srv == OWL_INVALID_SOCKET) { RV_CLOSE(cli); return -1; }

    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    setsockopt(srv, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    rv_non_bloquant(cli);
    rv_non_bloquant(srv);

    /* Le côté lecture est celui enregistré dans la boucle ; le côté
     * écriture est celui signalé. Le choix des deux extrémités est
     * arbitraire (paire full-duplex) — on écrit d'un bout, on draine
     * l'autre, toujours dans le même sens. */
    *pr = srv;
    *pw = cli;
    return 0;
}

/* ---------- Drain côté boucle ---------- */

/* Callback réacteur : draine TOUS les octets pending sans bloquer
 * (coalescing : N signals → un seul réveil), puis délègue au callback
 * utilisateur. Le contenu des octets est ignoré : c'est un pur signal.
 * OWL_REAC_ERREUR est toléré ici — le self-pipe ne « meurt » pas en
 * fonctionnement normal, mais un drain défensif reste sûr. */
static void rv_drain_cb(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    (void)r;
    (void)evts;
    OwlReveil* rv = (OwlReveil*)user;

    char poub[256];
    for (;;) {
        int n = (int)recv(fd, poub, (int)sizeof(poub), 0);
        if (n <= 0) break;                     /* vidé (EWOULDBLOCK) ou fermé */
        if (n < (int)sizeof(poub)) break;      /* moins qu'un plein buffer → fini */
    }

    if (rv->cb) rv->cb(rv->user);
}

/* ---------- API ---------- */

bool owl_reveil_cree(OwlReveil* rv, OwlReveilCb cb, void* user) {
    rv->fd_lecture  = OWL_INVALID_SOCKET;
    rv->fd_ecriture = OWL_INVALID_SOCKET;
    rv->cb   = cb;
    rv->user = user;
    if (rv_make_pair(&rv->fd_lecture, &rv->fd_ecriture) != 0) {
        rv->fd_lecture  = OWL_INVALID_SOCKET;
        rv->fd_ecriture = OWL_INVALID_SOCKET;
        return false;
    }
    return true;
}

bool owl_reveil_enregistre(OwlReveil* rv, OwlReacteur* r) {
    return owl_reacteur_ajoute_fd(r, rv->fd_lecture, OWL_REAC_LECTURE,
                                  rv_drain_cb, rv);
}

void owl_reveil_signale(OwlReveil* rv) {
    if (rv->fd_ecriture == OWL_INVALID_SOCKET) return;
    /* 1 octet non-bloquant. Si le tampon d'envoi est plein (des réveils
     * déjà pending non drainés), send échoue avec EWOULDBLOCK : on
     * l'ignore — un octet est déjà en attente, la boucle sera réveillée.
     * C'est exactement le coalescing (plusieurs signals → un réveil). */
    char c = 'w';
    (void)send(rv->fd_ecriture, &c, 1, 0);
}

void owl_reveil_detruit(OwlReveil* rv, OwlReacteur* r) {
    if (r && rv->fd_lecture != OWL_INVALID_SOCKET) {
        owl_reacteur_retire_fd(r, rv->fd_lecture);
    }
    if (rv->fd_lecture != OWL_INVALID_SOCKET) {
        RV_CLOSE(rv->fd_lecture);
        rv->fd_lecture = OWL_INVALID_SOCKET;
    }
    if (rv->fd_ecriture != OWL_INVALID_SOCKET) {
        RV_CLOSE(rv->fd_ecriture);
        rv->fd_ecriture = OWL_INVALID_SOCKET;
    }
}
