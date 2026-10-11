// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* net_tcp_server.c — socket serveur TCP a echeances (, phase A0.1).
 * Cf. l'en-tete pour le pourquoi du fichier separe de net_tcp.c. */

#ifdef _WIN32
  /* WSAPoll est apparu avec Vista : mingw-w64 ne le declare que si
   * _WIN32_WINNT >= 0x0600. Doit preceder tout include winsock. */
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "net_tcp_server.h"
#include "net_reacteur.h"   /* front reacteur (, W2) */
#include "net_reveil.h"     /* reveil inter-thread self-pipe (, W4) */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <stdlib.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #define OWL_SRV_CLOSE   closesocket
  #define OWL_SRV_ERRNO   WSAGetLastError()
  #define OWL_SRV_EINTR   WSAEINTR
  #define OWL_SRV_EAGAIN  WSAEWOULDBLOCK
  #define owl_srv_poll    WSAPoll
  typedef WSAPOLLFD owl_srv_pollfd;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <pthread.h>
  #include <fcntl.h>
  #define OWL_SRV_CLOSE   close
  #define OWL_SRV_ERRNO   errno
  #define OWL_SRV_EINTR   EINTR
  #define OWL_SRV_EAGAIN  EAGAIN
  #define owl_srv_poll    poll
  typedef struct pollfd owl_srv_pollfd;
#endif

/* ================================================================
 * Horloge monotone
 * ================================================================ */

uint64_t owl_tcp_server_horloge_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

/* ================================================================
 * Arret propre
 * ================================================================ */

static volatile sig_atomic_t g_arret = 0;
static int g_arret_installe = 0;

void owl_tcp_server_demande_arret(void) { g_arret = 1; }
bool owl_tcp_server_arret_demande(void) { return g_arret != 0; }

#ifdef _WIN32
static BOOL WINAPI owl_srv_console_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_arret = 1;
            return TRUE;
        default:
            return FALSE;
    }
}
#else
static void owl_srv_signal_handler(int sig) { (void)sig; g_arret = 1; }
#endif

void owl_tcp_server_installe_arret(void) {
    if (g_arret_installe) return;
    g_arret_installe = 1;
#ifdef _WIN32
    SetConsoleCtrlHandler(owl_srv_console_handler, TRUE);
#else
    signal(SIGTERM, owl_srv_signal_handler);
    signal(SIGINT,  owl_srv_signal_handler);
    signal(SIGPIPE, SIG_IGN);   /* un client qui coupe ne tue pas le process */
#endif
}

/* ================================================================
 * Ecoute
 * ================================================================ */

static void srv_erreur(OwlTcpListener* l, const char* quoi) {
    snprintf(l->erreur, sizeof(l->erreur), "%s (code %d)", quoi, OWL_SRV_ERRNO);
}

static bool srv_est_loopback(const struct in_addr* a) {
    return (ntohl(a->s_addr) >> 24) == 127;
}

bool owl_tcp_listener_ouvre(OwlTcpListener* l, const char* hote, int port,
                            int backlog, bool autoriser_non_loopback) {
    memset(l, 0, sizeof(*l));
    l->fd = OWL_INVALID_SOCKET;

    if (!owl_tcp_global_init()) {
        snprintf(l->erreur, sizeof(l->erreur), "initialisation reseau impossible");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, hote && hote[0] ? hote : "127.0.0.1", &addr.sin_addr) != 1) {
        snprintf(l->erreur, sizeof(l->erreur), "adresse d'ecoute invalide : %s",
                 hote ? hote : "(nulle)");
        return false;
    }

    /* Garde-fou de bind (contrat de transport §10bis b) : TLS serveur
     * n'existe pas en A0. Refuser le non-loopback par defaut rend
     * l'exposition accidentelle en clair impossible — on reporte le
     * travail TLS, pas la securite. */
    if (!autoriser_non_loopback && !srv_est_loopback(&addr.sin_addr)) {
        snprintf(l->erreur, sizeof(l->erreur),
                 "ecoute non-loopback refusee sans chiffrement configure");
        return false;
    }

    l->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->fd == OWL_INVALID_SOCKET) { srv_erreur(l, "socket"); return false; }

#ifndef _WIN32
    /* SO_REUSEADDR n'a pas la meme semantique sur Windows (il y autorise
     * le vol de port par un autre processus) : on ne le pose que sur
     * POSIX, ou il ne fait que court-circuiter TIME_WAIT. */
    {
        int un = 1;
        setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&un, sizeof(un));
    }
#endif

    if (bind(l->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        srv_erreur(l, "bind");
        OWL_SRV_CLOSE(l->fd); l->fd = OWL_INVALID_SOCKET;
        return false;
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(l->fd, (struct sockaddr*)&addr, &alen) == 0) {
        l->port = ntohs(addr.sin_port);
    } else {
        l->port = port;
    }

    if (listen(l->fd, backlog > 0 ? backlog : 16) != 0) {
        srv_erreur(l, "listen");
        OWL_SRV_CLOSE(l->fd); l->fd = OWL_INVALID_SOCKET;
        return false;
    }
    return true;
}

bool owl_tcp_listener_ouvre_tls(OwlTcpListener* l, const char* hote, int port,
                                int backlog, const OwlTlsServeurMateriel* tls) {
    return owl_tcp_listener_ouvre(l, hote, port, backlog, tls != NULL);
}

int owl_tcp_listener_port(const OwlTcpListener* l) { return l->port; }
const char* owl_tcp_listener_erreur(const OwlTcpListener* l) { return l->erreur; }

void owl_tcp_listener_ferme(OwlTcpListener* l) {
    if (l->fd != OWL_INVALID_SOCKET) {
        OWL_SRV_CLOSE(l->fd);
        l->fd = OWL_INVALID_SOCKET;
    }
}

/* Passe une socket en mode non-bloquant. Sans ca, poll(POLLOUT)+send se
 * comporte comme une ecriture bloquante : le poll dit « au moins un octet
 * dispo » mais send(chunk_entier) bloque jusqu'a ce que TOUT le chunk soit
 * bufferise — ce qui n'arrive jamais si le pair ne draine plus, et le
 * timeout du poll est alors contourne. La logique EAGAIN de conn_ecrit /
 * conn_lit n'est operante que sur une socket non-bloquante. */
static bool srv_met_non_bloquant(owl_socket_fd_t fd) {
#ifdef _WIN32
    u_long nb = 1;
    return ioctlsocket(fd, FIONBIO, &nb) == 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) != -1;
#endif
}

/* Attente d'un evenement sur un fd. 1 = pret, 0 = timeout, -1 = erreur. */
static int srv_attend(owl_socket_fd_t fd, short evenements, int timeout_ms) {
    owl_srv_pollfd p;
    memset(&p, 0, sizeof(p));
    p.fd     = fd;
    p.events = evenements;

    for (;;) {
        int r = owl_srv_poll(&p, 1, timeout_ms);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (OWL_SRV_ERRNO == OWL_SRV_EINTR) continue;
        return -1;
    }
}

int owl_tcp_attend(owl_socket_fd_t fd, int lecture, int timeout_ms) {
    return srv_attend(fd, lecture ? POLLIN : POLLOUT, timeout_ms);
}

int owl_tcp_listener_accepte(OwlTcpListener* l, int timeout_ms, OwlTcpConn* out) {
    memset(out, 0, sizeof(*out));
    out->fd = OWL_INVALID_SOCKET;

    int r = srv_attend(l->fd, POLLIN, timeout_ms);
    if (r == 0) return OWL_TCPS_TIMEOUT;
    if (r < 0)  { srv_erreur(l, "poll(accept)"); return OWL_TCPS_ERREUR; }

    struct sockaddr_in pair;
    socklen_t plen = sizeof(pair);
    owl_socket_fd_t fd = accept(l->fd, (struct sockaddr*)&pair, &plen);
    if (fd == OWL_INVALID_SOCKET) { srv_erreur(l, "accept"); return OWL_TCPS_ERREUR; }

    int un = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));

    /* Non-bloquant OBLIGATOIRE : sinon send() sur un pair qui ne draine
     * plus bloque le worker indefiniment et contourne timeout_ecriture_ms.
     * Sur echec, on ferme plutot que de laisser passer une socket bloquante
     * qui reintroduirait le gel silencieusement. */
    if (!srv_met_non_bloquant(fd)) {
        srv_erreur(l, "non-bloquant");
        OWL_SRV_CLOSE(fd);
        return OWL_TCPS_ERREUR;
    }

    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &pair.sin_addr, ip, sizeof(ip))) {
        snprintf(ip, sizeof(ip), "?");
    }
    out->fd = fd;
    snprintf(out->pair, sizeof(out->pair), "%s:%d", ip, (int)ntohs(pair.sin_port));
    return OWL_TCPS_OK;
}

/* ================================================================
 * Lecture / ecriture a echeance
 * ================================================================ */

int owl_tcp_conn_lit(OwlTcpConn* c, void* buf, size_t len, int timeout_ms) {
    if (c->fd == OWL_INVALID_SOCKET) return OWL_TCPS_ERREUR;
    if (c->tls) return owl_tls_session_lit(c->tls, buf, len, timeout_ms);

    uint64_t fin = owl_tcp_server_horloge_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
        int restant = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t maintenant = owl_tcp_server_horloge_ms();
            if (maintenant >= fin) return OWL_TCPS_TIMEOUT;
            restant = (int)(fin - maintenant);
        }

        int r = srv_attend(c->fd, POLLIN, restant);
        if (r == 0) return OWL_TCPS_TIMEOUT;
        if (r < 0)  return OWL_TCPS_ERREUR;

        int n = recv(c->fd, (char*)buf, (int)len, 0);
        if (n > 0)  return n;
        if (n == 0) return OWL_TCPS_FERME;
        /* Socket non-bloquante : un POLLIN spurious (ou un octet deja
         * consomme entre poll et recv) peut donner EAGAIN -> on repolle
         * avec le budget restant, symetrique a conn_ecrit pour POLLOUT. */
        if (OWL_SRV_ERRNO == OWL_SRV_EINTR ||
            OWL_SRV_ERRNO == OWL_SRV_EAGAIN) continue;
        return OWL_TCPS_ERREUR;
    }
}

int owl_tcp_conn_ecrit(OwlTcpConn* c, const void* buf, size_t len, int timeout_ms) {
    if (c->fd == OWL_INVALID_SOCKET) return OWL_TCPS_ERREUR;
    if (c->tls) return owl_tls_session_ecrit(c->tls, buf, len, timeout_ms);

    const char* p = (const char*)buf;
    size_t ecrit = 0;
    uint64_t fin = owl_tcp_server_horloge_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    while (ecrit < len) {
        int restant = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t maintenant = owl_tcp_server_horloge_ms();
            if (maintenant >= fin) return OWL_TCPS_TIMEOUT;
            restant = (int)(fin - maintenant);
        }

        int r = srv_attend(c->fd, POLLOUT, restant);
        if (r == 0) return OWL_TCPS_TIMEOUT;
        if (r < 0)  return OWL_TCPS_ERREUR;

        int n = send(c->fd, p + ecrit, (int)(len - ecrit), 0);
        if (n > 0) { ecrit += (size_t)n; continue; }
        if (n < 0 && (OWL_SRV_ERRNO == OWL_SRV_EINTR ||
                      OWL_SRV_ERRNO == OWL_SRV_EAGAIN)) continue;
        return OWL_TCPS_ERREUR;
    }
    return OWL_TCPS_OK;
}

/* Ecriture non bloquante (, W5) : le drain reacteur n'attend jamais
 * sur POLLOUT (c'est la boucle poll qui pilote la disponibilite). Envoie ce
 * que le tampon accepte tout de suite. TLS non gere ici : le front reacteur
 * sert le clair uniquement (les connexions chiffrees restent sur le pool
 * worker-par-connexion historique, cf. owl_http_serveur_pool_demarre). */
int owl_tcp_conn_ecrit_dispo(OwlTcpConn* c, const void* buf, size_t len) {
    if (!c || c->fd == OWL_INVALID_SOCKET) return -1;
    if (c->tls) return -1;   /* jamais atteint : reacteur = clair */
    if (len == 0) return 0;

    int n = send(c->fd, (const char*)buf, (int)len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;
    if (OWL_SRV_ERRNO == OWL_SRV_EINTR || OWL_SRV_ERRNO == OWL_SRV_EAGAIN) return 0;
    return -1;
}

/* Sonde non bloquante : le pair est-il DEFINITIVEMENT parti ?
 *
 * A n'appeler que sur signal POLLHUP/POLLERR du reacteur pendant un drain. Le
 * piege mesure : POLLHUP remonte par WSAPoll ne distingue PAS un demi-close
 * (le pair a fait shutdown(SEND) apres sa requete mais LIT encore la reponse
 * SSE — motif normal, et exactement ce que fait le mock peer de test) d'un
 * client vraiment disparu. Et un peek en lecture ne suffit pas non plus : un
 * FIN propre comme un demi-close donnent TOUS DEUX recv==0 (EOF). Seule une
 * ERREUR DE CONNEXION (ECONNRESET/ECONNABORTED) prouve le depart : elle
 * n'apparait que quand le pair a aussi ferme sa LECTURE et qu'un octet en vol
 * a declenche un RST. C'est le seul discriminant fiable cote serveur.
 *
 * true  = pair perdu (RST/erreur de connexion) -> couper la generation ;
 * false = pair present OU indetermine (recv>0, EOF demi-close, EAGAIN) :
 *         NE PAS couper (couper un demi-close qui lit encore = regression).
 *
 * NB : un client qui FERME completement sans laisser d'octet en vol (FIN
 * propre, aucune donnee non lue) reste indetectable par ce chemin — il ne
 * genere pas de RST. Sur un vrai flux volumineux, l'echec d'ecriture (send)
 * finit par le trancher ; sur loopback a faible volume, aucun signal transport
 * n'existe (limite physique, cf. note "EN PAUSE" du ticket). */
bool owl_tcp_conn_pair_perdu(OwlTcpConn* c) {
    if (!c || c->fd == OWL_INVALID_SOCKET) return true;
    if (c->tls) return false;   /* reacteur = clair : jamais atteint ici */

    char sonde[1];
    int n = recv(c->fd, sonde, 1, MSG_PEEK);
    int e = OWL_SRV_ERRNO;
    if (n >= 0) return false;   /* donnees, ou EOF (demi-close) : pas une preuve de depart */
    if (e == OWL_SRV_EINTR || e == OWL_SRV_EAGAIN) return false;
    return true;                /* RST / ECONNRESET : pair perdu */
}

/* Budget de drain a la fermeture. Mesure : la version
 * precedente rendait la main des 50 ms de silence, ce qui suffisait pour
 * un client qui a deja tout envoye et PAS pour un client qui a encore des
 * octets en vol — le 200 puis le 400 partaient bien, et le RST Windows
 * les effacait du tampon de reception du pair AVANT qu'il les lise. Un
 * client keep-alive a par construction des octets en vol au moment ou on
 * decide de fermer : le budget doit etre une echeance, pas un silence.
 * Borne absolue, donc insensible a un pair qui distille. */
#define OWL_TCPS_DRAIN_FERMETURE_MS 500

/* ================================================================
 * POOL DE WORKERS RESEAU (, phase A1.1b)
 *
 * Meme pattern de thread que src/net_mock_peer.c et src/http_exec.c :
 * CreateThread / pthread_create, aucune dependance nouvelle.
 *
 * L'accept est SERIALISE par un verrou dedie. Sans lui, N threads en
 * poll() sur le meme descripteur d'ecoute sont tous reveilles par une
 * seule connexion, un seul gagne l'accept et les autres se bloquent
 * dans accept() sur une socket bloquante — donc sourds a la demande
 * d'arret. Le verrou d'accept rend chaque worker seul maitre du couple
 * poll+accept pendant sa fenetre, et l'echeance de ce poll est la
 * latence maximale d'arret. C'est cher en theorie (un accept a la fois)
 * et gratuit en pratique : accepter coute infiniment moins que servir.
 *
 * Le plafond est reserve AVANT l'accept, pas verifie avant. Verifie
 * avant, deux workers peuvent le franchir ensemble : chacun lit
 * « il reste une place » puis accepte. Le compteur de reservation est
 * donc distinct du compteur de connexions en service — c'est ce dernier
 * qui alimente le pic, sans quoi un worker en attente d'accept compterait
 * comme une connexion en vol et le pic ne mesurerait plus rien.
 * ================================================================ */

#define OWL_TCPS_WORKERS_MAX     64
#define OWL_TCPS_WORKERS_DEFAUT  4
#define OWL_TCPS_ACCEPT_MS       50

#ifdef _WIN32
  typedef CRITICAL_SECTION tp_mutex_t;
  typedef HANDLE           tp_thread_t;
  static void tp_mutex_init(tp_mutex_t* m)    { InitializeCriticalSection(m); }
  static void tp_mutex_detruit(tp_mutex_t* m) { DeleteCriticalSection(m); }
  static void tp_verrouille(tp_mutex_t* m)    { EnterCriticalSection(m); }
  static void tp_deverrouille(tp_mutex_t* m)  { LeaveCriticalSection(m); }
  #define TP_SLEEP(ms) Sleep((DWORD)(ms))
#else
  typedef pthread_mutex_t tp_mutex_t;
  typedef pthread_t       tp_thread_t;
  static void tp_mutex_init(tp_mutex_t* m)    { pthread_mutex_init(m, NULL); }
  static void tp_mutex_detruit(tp_mutex_t* m) { pthread_mutex_destroy(m); }
  static void tp_verrouille(tp_mutex_t* m)    { pthread_mutex_lock(m); }
  static void tp_deverrouille(tp_mutex_t* m)  { pthread_mutex_unlock(m); }
  static void tp_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define TP_SLEEP(ms) tp_sleep_ms((int)(ms))
#endif

/* Connexion acceptee par le front reacteur, en attente de classification
 * (liste `pending`, propriete EXCLUSIVE du thread reacteur, sans verrou)
 * puis, si data-plane, en file de dispatch vers les workers (`dnext`, sous
 * p->m). Une RConn n'est jamais dans les deux a la fois. */
typedef struct RConn {
    OwlTcpConn      conn;
    OwlTcpPool*     p;
    OwlTimerId      idle;    /* echeance absolue de lecture de la ligne de requete */
    struct RConn*   prec;    /* liste pending (doublement chainee) */
    struct RConn*   suiv;
    struct RConn*   dnext;   /* file de dispatch (simplement chainee) */
} RConn;

/* Flux remis par un worker au reacteur (, W5), en attente de prise
 * en charge par le thread boucle. File partagee worker->reacteur (sous p->m). */
typedef struct FluxRemis {
    OwlTcpConn        conn;   /* propriete transferee du worker au reacteur */
    void*             appctx; /* contexte de drain opaque (cadrage HTTP au-dessus) */
    uint64_t          echeance; /* echeance absolue de synthese (W7) ; 0 = aucune */
    struct FluxRemis* suiv;
} FluxRemis;

/* Flux ACTIF, drainee par le reacteur. Propriete EXCLUSIVE du thread boucle
 * (aucun verrou) : cree a la prise en charge, detruite au FINI ou a l'arret. */
typedef struct FluxActif {
    OwlTcpConn        conn;
    void*             appctx;
    OwlTcpPool*       p;
    int               pollout;  /* interet POLLOUT actuellement enregistre ? */
    /* Echeance de synthese (W7) : un drain en attente (exec async pas encore
     * resolu) n'a aucun POLLOUT a attendre et le producteur ne le reveillera
     * pas s'il n'a jamais demarre. Ce timer re-pousse un pas de drain a
     * l'echeance pour que essaie voie l'expiry et synthetise le 503. */
    uint64_t          echeance;
    OwlTimerId        echeance_timer;
    /* Borne de DUREE applicative ( P5), DISTINCTE de echeance_timer :
     * armee a la RESOLUTION du flux (1er pas de drain ou le flux devient vivant),
     * elle coupe le flux a l'echeance (marque consommateur fini + finalise). Le
     * flux resolu une fois -> delaimax_vu bloque toute nouvelle interrogation. */
    OwlTimerId        delaimax_timer;
    int               delaimax_vu;   /* flux resolu : ne plus interroger le delai */
    struct FluxActif* suiv;
} FluxActif;

struct OwlTcpPool {
    OwlTcpListener* l;
    OwlTcpPoolSert  sert;
    void*           user;

    int nb_workers;
    int connexions_max;
    int accept_timeout_ms;

    const OwlTlsServeurMateriel* tls;
    int tls_handshake_timeout_ms;

    tp_mutex_t m;         /* compteurs et drapeau d'arret */
    tp_mutex_t accept_m;  /* serialise poll+accept */

    int    arret;
    size_t reserves;      /* places prises, accept compris */
    size_t en_vol;        /* connexions REELLEMENT en service */
    size_t pic;
    size_t servies;
    size_t handshakes_echoues;

    tp_thread_t threads[OWL_TCPS_WORKERS_MAX];
    int         nb_lances;

    /* ---- Front reacteur (, W2) ---- */
    int             reacteur_mode;
    OwlReacteur*    reacteur;      /* possede par le thread reacteur */
    OwlTcpClassifie classe;
    void*           classe_user;
    int             idle_ms;       /* echeance de la phase pre-classification */
    tp_thread_t     reac_thread;
    int             reac_lance;
    RConn*          pending;       /* connexions en attente de classification */
    RConn*          disp_tete;     /* file de dispatch data-plane (sous m) */
    RConn*          disp_queue;

    /* ---- Drain de flux dans la boucle (, W5) ---- */
    OwlTcpPoolSertDp  sert_dp;     /* service data-plane capable de prise en charge */
    OwlTcpFluxDrain   flux_drain;  /* avance un flux actif (thread boucle) */
    OwlTcpFluxDetruit flux_detruit;/* libere le contexte appli d'un flux */
    OwlTcpFluxDelaiMax flux_delaimax; /* borne de duree applicative (P5), NULL = aucune */
    void*             flux_user;
    OwlReveil         reveil;      /* self-pipe (W4) : producteur -> boucle */
    int               reveil_actif;
    FluxRemis*        remis_tete;  /* flux remis worker->reacteur (sous m) */
    FluxRemis*        remis_queue;
    FluxActif*        flux_actifs; /* flux actifs (thread boucle uniquement) */

    /* Concurrence TENUE PAR LE REACTEUR (W7) : nombre de flux/jetons pris en
     * charge par la boucle simultanement, et son pic. C'est la concurrence
     * pertinente une fois le worker libere apres le handoff (l'ex-`pic` des
     * workers ne mesurait plus que la fenetre de SETUP, tres courte en async).
     * Ecrits sous p->m par le thread boucle, lus sans verrou (comme `pic`). */
    size_t            reac_en_vol;
    size_t            reac_pic;
};

/* Cap transport de la lecture de la ligne de requete AVANT classification
 *. Le pool ne connait pas les limites HTTP (timeout_entetes_ms
 * vit dans OwlHttpLimites, au-dessus) : une echeance absolue genereuse ici
 * borne le slowloris qui n'enverrait jamais de ligne de requete complete.
 * Le service (sert) reapplique ensuite ses propres timeouts fins. */
#define OWL_TCPS_REAC_CLASSE_MS 10000
/* Taille du peek de classification : la ligne de requete (methode + cible +
 * version) tient tres en-dessous. Au-dela, la cible est trop longue -> le
 * parser du service tranchera (414), on classe data-plane par defaut. */
#define OWL_TCPS_REAC_PEEK 8192

void owl_tcp_pool_config_defaut(OwlTcpPoolConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->nb_workers        = OWL_TCPS_WORKERS_DEFAUT;
    cfg->accept_timeout_ms = OWL_TCPS_ACCEPT_MS;
}

#define OWL_TCPS_HANDSHAKE_MS 10000

static void tp_boucle(OwlTcpPool* p) {
    /* Contexte mbedTLS POSSEDE par CE worker, construit ici — donc au
     * demarrage du pool, jamais dans le chemin d'une connexion. C'est
     * ce qui remplace MBEDTLS_THREADING_C : rien de mutable n'est
     * partage, ni le generateur, ni la CLE PRIVEE (dont mbedTLS mute le
     * cache d'aveuglement RSA et la table de precalcul EC a chaque
     * signature). Cf. net_tls_server.h pour les references vendor. */
    OwlTlsServeurCtx* tls_ctx = NULL;
    if (p->tls) {
        tls_ctx = owl_tls_serveur_ctx_cree(p->tls);
        if (!tls_ctx) return;   /* worker inutilisable : il ne sert rien en clair */
    }

    for (;;) {
        tp_verrouille(&p->m);
        if (p->arret) { tp_deverrouille(&p->m); goto sortie; }
        if (p->reserves >= (size_t)p->connexions_max) {
            tp_deverrouille(&p->m);
            TP_SLEEP(2);   /* plafond atteint : la connexion reste dans le backlog */
            continue;
        }
        p->reserves++;
        tp_deverrouille(&p->m);

        /* L'echeance du poll borne la latence d'arret : inutile de
         * relire le drapeau ici, ce qui ferait une lecture hors verrou. */
        OwlTcpConn conn;
        tp_verrouille(&p->accept_m);
        int r = owl_tcp_listener_accepte(p->l, p->accept_timeout_ms, &conn);
        tp_deverrouille(&p->accept_m);

        if (r != OWL_TCPS_OK) {
            tp_verrouille(&p->m);
            p->reserves--;
            tp_deverrouille(&p->m);
            if (r == OWL_TCPS_ERREUR) TP_SLEEP(2);   /* listener ferme ou en erreur */
            continue;
        }

        tp_verrouille(&p->m);
        p->en_vol++;
        if (p->en_vol > p->pic) p->pic = p->en_vol;
        tp_deverrouille(&p->m);

        /* Handshake APRES l'accept et AVANT `sert` : le service ne voit
         * qu'une connexion, chiffree ou non. Un handshake qui n'aboutit
         * pas ne franchit jamais cette porte — c'est la que se joue le
         * « le serveur EXIGE TLS ». */
        int echec_handshake = 0;
        if (tls_ctx) {
            OwlTlsSession* sess = NULL;
            if (owl_tls_serveur_handshake(tls_ctx, conn.fd,
                                          p->tls_handshake_timeout_ms,
                                          &sess) == OWL_TCPS_OK) {
                conn.tls = sess;
            } else {
                echec_handshake = 1;
            }
        }

        if (echec_handshake) {
            /* Aucune reponse applicative : le pair n'a pas de canal ou
             * la recevoir. On rend l'emplacement, ce qui est exactement
             * ce qui empeche un slowloris de handshake d'immobiliser le
             * worker. */
            owl_tcp_conn_ferme(&conn);
        } else {
            /* Tout l'etat de la connexion vit sous cet appel, donc sur la
             * pile de CE worker. Rien n'est partage entre deux connexions. */
            p->sert(&conn, p->user);
        }

        tp_verrouille(&p->m);
        p->en_vol--;
        p->reserves--;
        p->servies++;
        if (echec_handshake) p->handshakes_echoues++;
        tp_deverrouille(&p->m);
    }

sortie:
    owl_tls_serveur_ctx_libere(tls_ctx);
}

#ifdef _WIN32
static DWORD WINAPI tp_thread_fn(LPVOID arg) { tp_boucle((OwlTcpPool*)arg); return 0; }
#else
static void* tp_thread_fn(void* arg) { tp_boucle((OwlTcpPool*)arg); return NULL; }
#endif

OwlTcpPool* owl_tcp_pool_demarre(OwlTcpListener* l, const OwlTcpPoolConfig* cfg,
                                 OwlTcpPoolSert sert, void* user) {
    if (!l || !sert) return NULL;

    OwlTcpPool* p = (OwlTcpPool*)calloc(1, sizeof(OwlTcpPool));
    if (!p) return NULL;

    p->l    = l;
    p->sert = sert;
    p->user = user;

    p->nb_workers = (cfg && cfg->nb_workers > 0) ? cfg->nb_workers : OWL_TCPS_WORKERS_DEFAUT;
    if (p->nb_workers > OWL_TCPS_WORKERS_MAX) p->nb_workers = OWL_TCPS_WORKERS_MAX;
    p->connexions_max = (cfg && cfg->connexions_max > 0) ? cfg->connexions_max : p->nb_workers;
    p->accept_timeout_ms = (cfg && cfg->accept_timeout_ms > 0) ? cfg->accept_timeout_ms
                                                               : OWL_TCPS_ACCEPT_MS;
    p->tls = cfg ? cfg->tls : NULL;
    p->tls_handshake_timeout_ms =
        (cfg && cfg->tls_handshake_timeout_ms > 0) ? cfg->tls_handshake_timeout_ms
                                                   : OWL_TCPS_HANDSHAKE_MS;

    tp_mutex_init(&p->m);
    tp_mutex_init(&p->accept_m);

    for (int i = 0; i < p->nb_workers; i++) {
#ifdef _WIN32
        p->threads[i] = CreateThread(NULL, 0, tp_thread_fn, p, 0, NULL);
        if (p->threads[i] == NULL) break;
#else
        if (pthread_create(&p->threads[i], NULL, tp_thread_fn, p) != 0) break;
#endif
        p->nb_lances++;
    }

    if (p->nb_lances == 0) {
        tp_mutex_detruit(&p->accept_m);
        tp_mutex_detruit(&p->m);
        free(p);
        return NULL;
    }
    return p;
}

/* ================================================================
 * FRONT REACTEUR (, W2)
 *
 * Thread reacteur : accept non borne + peek/classification. Workers :
 * service bloquant du data-plane. Le controle est servi INLINE sur le
 * thread reacteur (pas de worker). Les callbacks du reacteur tournent tous
 * sur le thread reacteur (net_reacteur n'est pas thread-safe) : la liste
 * `pending` n'a donc pas de verrou ; seule la file de dispatch et les
 * compteurs sont partages avec les workers (sous p->m).
 * ================================================================ */

static void reac_pending_ajoute(OwlTcpPool* p, RConn* rc) {
    rc->prec = NULL;
    rc->suiv = p->pending;
    if (p->pending) p->pending->prec = rc;
    p->pending = rc;
}

static void reac_pending_retire(OwlTcpPool* p, RConn* rc) {
    if (rc->prec) rc->prec->suiv = rc->suiv; else p->pending = rc->suiv;
    if (rc->suiv) rc->suiv->prec = rc->prec;
    rc->prec = rc->suiv = NULL;
}

/* Ferme une connexion encore en phase pending (jamais dispatchee). */
static void reac_ferme_pending(OwlTcpPool* p, RConn* rc) {
    if (rc->idle != OWL_TIMER_NUL) owl_reacteur_annule_timer(p->reacteur, rc->idle);
    owl_reacteur_retire_fd(p->reacteur, rc->conn.fd);
    reac_pending_retire(p, rc);
    owl_tcp_conn_ferme(&rc->conn);
    free(rc);
}

/* Echeance de lecture de la ligne de requete atteinte : le pair n'a pas
 * envoye de requete classifiable a temps -> fermeture (parade slowloris). */
static void reac_on_idle(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)r; (void)id;
    RConn* rc = (RConn*)user;
    rc->idle = OWL_TIMER_NUL;   /* one-shot : deja desarme */
    reac_ferme_pending(rc->p, rc);
}

/* Un fd pending signale un evenement : peek + classification.
 *
 * On NE court-circuite PAS sur OWL_REAC_ERREUR : un pair qui a fait un
 * shutdown(SEND) apres avoir envoye sa requete (half-close) fait remonter
 * POLLHUP par WSAPoll ALORS QUE les octets de la requete sont encore en
 * tampon. C'est le peek qui tranche — s'il rend des octets, on classe ;
 * s'il rend 0 (EOF vrai) ou une erreur ferme, on ferme. */
static void reac_on_conn(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    RConn* rc = (RConn*)user;
    OwlTcpPool* p = rc->p;

    char buf[OWL_TCPS_REAC_PEEK];
    int n = recv(fd, buf, (int)sizeof(buf) - 1, MSG_PEEK);
    if (n == 0) { reac_ferme_pending(p, rc); return; }   /* EOF avant requete */
    if (n < 0) {
        /* EAGAIN/EINTR sans evenement d'erreur : reveil spurieux, on attend.
         * Avec un evenement d'erreur ET aucune donnee : fd mort, on ferme. */
        if ((OWL_SRV_ERRNO == OWL_SRV_EINTR || OWL_SRV_ERRNO == OWL_SRV_EAGAIN)
            && !(evts & OWL_REAC_ERREUR)) return;
        reac_ferme_pending(p, rc);
        return;
    }

    int classe = p->classe(buf, (size_t)n, p->classe_user);
    if (classe == OWL_TCPS_CLASSE_INCOMPLET) {
        /* Ligne de requete trop longue pour le peek : laisser le parser du
         * service trancher (414) en la traitant comme data-plane. Sinon,
         * attendre plus d'octets sur un prochain tick. */
        if (n >= (int)sizeof(buf) - 1) classe = OWL_TCPS_CLASSE_DATAPLANE;
        else return;
    }

    /* Classification faite : sortir de la phase pending. */
    if (rc->idle != OWL_TIMER_NUL) { owl_reacteur_annule_timer(r, rc->idle); rc->idle = OWL_TIMER_NUL; }
    owl_reacteur_retire_fd(r, fd);
    reac_pending_retire(p, rc);

    if (classe == OWL_TCPS_CLASSE_CONTROLE) {
        /* Servi INLINE sur le thread reacteur : rapide, ne touche NI VM NI
         * modele. Ne consomme pas de worker -> jamais affame par le
         * data-plane sature (design §8 Q1). */
        tp_verrouille(&p->m);
        p->en_vol++;
        if (p->en_vol > p->pic) p->pic = p->en_vol;
        tp_deverrouille(&p->m);

        p->sert(&rc->conn, p->user);

        tp_verrouille(&p->m);
        p->en_vol--;
        p->servies++;
        tp_deverrouille(&p->m);
        free(rc);
    } else {
        /* Data-plane : dispatch FIFO aux workers. */
        tp_verrouille(&p->m);
        rc->dnext = NULL;
        if (p->disp_queue) p->disp_queue->dnext = rc; else p->disp_tete = rc;
        p->disp_queue = rc;
        tp_deverrouille(&p->m);
    }
}

/* Le listener est lisible : accepter TOUT (non borne par connexions_max). */
static void reac_on_listener(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    (void)fd;
    OwlTcpPool* p = (OwlTcpPool*)user;
    if (evts & OWL_REAC_ERREUR) return;

    for (;;) {
        OwlTcpConn conn;
        int rc = owl_tcp_listener_accepte(p->l, 0, &conn);
        if (rc != OWL_TCPS_OK) break;   /* TIMEOUT (plus rien) ou ERREUR */

        RConn* rcn = (RConn*)calloc(1, sizeof(RConn));
        if (!rcn) { owl_tcp_conn_ferme(&conn); continue; }
        rcn->conn = conn;
        rcn->p    = p;

        if (!owl_reacteur_ajoute_fd(r, conn.fd, OWL_REAC_LECTURE, reac_on_conn, rcn)) {
            owl_tcp_conn_ferme(&rcn->conn);
            free(rcn);
            continue;
        }
        rcn->idle = owl_reacteur_arme_timer(r, (uint64_t)p->idle_ms, reac_on_idle, rcn);
        reac_pending_ajoute(p, rcn);
    }
}

/* ---------- Drain de flux dans la boucle (, W5) ----------
 *
 * Toutes ces fonctions tournent sur le THREAD BOUCLE, sauf reac_prise /
 * reac_signale (thread-safe, appelees par un worker / le producteur). Aucun
 * objet GC ne transite : que des OwlTcpConn et des appctx opaques (le cadrage
 * HTTP + la reponse C-heap vivent au-dessus, http_server.c). */

static void reac_on_flux(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user);

static void reac_flux_actif_retire(OwlTcpPool* p, FluxActif* fa) {
    FluxActif** pp = &p->flux_actifs;
    while (*pp && *pp != fa) pp = &(*pp)->suiv;
    if (*pp) {
        *pp = fa->suiv;
        tp_verrouille(&p->m);
        if (p->reac_en_vol > 0) p->reac_en_vol--;
        tp_deverrouille(&p->m);
    }
}

/* Termine un flux actif : retire le fd, ferme la conn, detruit le contexte
 * appli (qui relache la reponse refcomptee), retire de la liste. */
static void reac_flux_finalise(OwlTcpPool* p, FluxActif* fa) {
    if (fa->echeance_timer != OWL_TIMER_NUL) {
        owl_reacteur_annule_timer(p->reacteur, fa->echeance_timer);
        fa->echeance_timer = OWL_TIMER_NUL;
    }
    /* P5 : annuler le timer DelaiMax (flux fini avant l'echeance -> pas de fuite,
     * pas de faux positif apres coup). No-op si expiry-en-cours (one-shot deja
     * desarme) ou jamais arme. */
    if (fa->delaimax_timer != OWL_TIMER_NUL) {
        owl_reacteur_annule_timer(p->reacteur, fa->delaimax_timer);
        fa->delaimax_timer = OWL_TIMER_NUL;
    }
    owl_reacteur_retire_fd(p->reacteur, fa->conn.fd);
    /* La connexion est SERVIE au moment ou sa reponse a fini d'etre emise :
     * incrementer AVANT de fermer, pour que le EOF vu par le client se
     * produise happens-after ce comptage (le worker a delegue ce comptage
     * ici en rendant 1, cf. tp_reac_worker). */
    tp_verrouille(&p->m);
    p->servies++;
    tp_deverrouille(&p->m);
    owl_tcp_conn_ferme(&fa->conn);
    if (p->flux_detruit) p->flux_detruit(fa->appctx, p->flux_user);
    reac_flux_actif_retire(p, fa);
    free(fa);
}

/* Echeance de synthese atteinte (W7) : re-armer POLLOUT pour qu'un pas de
 * drain tourne et que essaie voie l'expiry. Si POLLOUT est deja enregistre
 * (drain en cours), rien a forcer. Timer one-shot : deja desarme au declenche. */
static void reac_on_echeance(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)id;
    FluxActif* fa = (FluxActif*)user;
    fa->echeance_timer = OWL_TIMER_NUL;
    if (!fa->pollout) {
        if (owl_reacteur_ajoute_fd(r, fa->conn.fd, OWL_REAC_ECRITURE,
                                   reac_on_flux, fa)) {
            fa->pollout = 1;
        } else {
            reac_flux_finalise(fa->p, fa);
        }
    }
}

/* Echeance DelaiMax atteinte ( P5) : la borne de DUREE applicative du
 * flux est ecoulee. Meme effet qu'une mort de socket -> reac_flux_finalise, qui
 * marque le consommateur fini (via flux_detruit) : la prochaine pousse du
 * producteur rend ABANDON (-> exception cote OWL), et le transport est libere A
 * L'HEURE. Le fd et l'echeance_timer W7 sont annules dans finalise. Timer
 * one-shot : deja desarme au declenche, on annule juste notre trace d'id. */
static void reac_on_delaimax(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)r; (void)id;
    FluxActif* fa = (FluxActif*)user;
    fa->delaimax_timer = OWL_TIMER_NUL;
    reac_flux_finalise(fa->p, fa);
}

/* Un flux actif est pret en ecriture (ou reveille) : avancer son drain. */
static void reac_on_flux(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    (void)fd;
    FluxActif*  fa = (FluxActif*)user;
    OwlTcpPool* p  = fa->p;

    OwlTcpFluxProgres pr = p->flux_drain(&fa->conn, fa->appctx, evts, p->flux_user);

    /* P5 : a la RESOLUTION du flux (pas encore FINI), interroger sa borne de
     * duree UNE seule fois et armer un timer dedie. La resolution survient dans
     * le drain (jeton async -> FLUX, ou flux direct des le 1er pas). Retour < 0 =
     * pas encore resolu (re-interroger au pas suivant) ; 0 = illimite (pas de
     * timer) ; > 0 = armer. */
    if (pr != OWL_TCPS_FLUX_FINI && !fa->delaimax_vu && p->flux_delaimax) {
        int64_t d = p->flux_delaimax(fa->appctx, p->flux_user);
        if (d >= 0) {
            fa->delaimax_vu = 1;
            if (d > 0) {
                fa->delaimax_timer = owl_reacteur_arme_timer(
                    p->reacteur, (uint64_t)d, reac_on_delaimax, fa);
            }
        }
    }

    switch (pr) {
        case OWL_TCPS_FLUX_ATTEND:
            /* Rien a ecrire maintenant : RETIRER le fd du reacteur (pas juste
             * couper POLLOUT). Un demi-close du pair (shutdown SEND apres la
             * requete, alors qu'il LIT encore) fait remonter POLLHUP en
             * continu par WSAPoll — et POLLERR/HUP est livre meme sans interet
             * demande. Le laisser enregistre spinnerait sur ce HUP. On le
             * retire ; le producteur le re-enregistrera via le self-pipe quand
             * un chunk arrive (reac_reveil_cb). */
            owl_reacteur_retire_fd(r, fa->conn.fd);
            fa->pollout = 0;
            break;
        case OWL_TCPS_FLUX_ENCORE:
            /* Encore des octets a ecrire : garder POLLOUT (le fd reste
             * enregistre depuis la prise en charge ou un reveil). */
            fa->pollout = 1;
            break;
        case OWL_TCPS_FLUX_FINI:
            reac_flux_finalise(p, fa);
            break;
    }
}

/* Callback du self-pipe (W4), sur le thread boucle : (1) prendre en charge
 * les flux remis par les workers ; (2) re-armer POLLOUT sur les flux en
 * creux (un chunk vient d'arriver quelque part). */
static void reac_reveil_cb(void* user) {
    OwlTcpPool* p = (OwlTcpPool*)user;

    tp_verrouille(&p->m);
    FluxRemis* remis = p->remis_tete;
    p->remis_tete = p->remis_queue = NULL;
    tp_deverrouille(&p->m);

    while (remis) {
        FluxRemis* nx = remis->suiv;
        FluxActif* fa = (FluxActif*)calloc(1, sizeof(FluxActif));
        if (fa) {
            fa->conn           = remis->conn;
            fa->appctx         = remis->appctx;
            fa->p              = p;
            fa->pollout        = 1;
            fa->echeance       = remis->echeance;
            fa->echeance_timer = OWL_TIMER_NUL;
            if (owl_reacteur_ajoute_fd(p->reacteur, fa->conn.fd, OWL_REAC_ECRITURE,
                                       reac_on_flux, fa)) {
                fa->suiv       = p->flux_actifs;
                p->flux_actifs = fa;
                tp_verrouille(&p->m);
                p->reac_en_vol++;
                if (p->reac_en_vol > p->reac_pic) p->reac_pic = p->reac_en_vol;
                tp_deverrouille(&p->m);
                /* Armer le timer de synthese (W7) si une echeance est portee :
                 * un drain en attente d'exec ne recevra aucun POLLOUT utile
                 * ni reveil producteur tant que le job n'a pas demarre. */
                if (fa->echeance > 0) {
                    uint64_t maintenant = owl_tcp_server_horloge_ms();
                    uint64_t delai = (fa->echeance > maintenant)
                                         ? (fa->echeance - maintenant) : 0;
                    fa->echeance_timer = owl_reacteur_arme_timer(
                        p->reacteur, delai, reac_on_echeance, fa);
                }
            } else {
                owl_tcp_conn_ferme(&fa->conn);
                if (p->flux_detruit) p->flux_detruit(fa->appctx, p->flux_user);
                free(fa);
            }
        } else {
            owl_tcp_conn_ferme(&remis->conn);
            if (p->flux_detruit) p->flux_detruit(remis->appctx, p->flux_user);
        }
        free(remis);
        remis = nx;
    }

    /* Re-enregistrer POLLOUT sur les flux retires (en creux de production).
     * Grossier (on reveille tous les creux, pas seulement celui qui a recu un
     * chunk), mais sans spin : un flux sans donnee retombe en ATTEND (retire)
     * au pas suivant. La liste flux_actifs survit au retrait du fd — c'est elle
     * qui pilote la re-inscription. */
    FluxActif* fa = p->flux_actifs;
    while (fa) {
        FluxActif* nx = fa->suiv;
        if (!fa->pollout) {
            if (owl_reacteur_ajoute_fd(p->reacteur, fa->conn.fd, OWL_REAC_ECRITURE,
                                       reac_on_flux, fa)) {
                fa->pollout = 1;
            } else {
                /* Re-inscription impossible : terminer proprement ce flux. */
                reac_flux_finalise(p, fa);
            }
        }
        fa = nx;
    }
}

bool owl_tcp_pool_reac_prise(OwlTcpPool* p, OwlTcpConn* conn, void* appctx,
                             uint64_t echeance_ms) {
    if (!p || !p->reacteur_mode || !conn) return false;

    FluxRemis* fr = (FluxRemis*)calloc(1, sizeof(FluxRemis));
    if (!fr) return false;
    fr->conn     = *conn;
    fr->appctx   = appctx;
    fr->echeance = echeance_ms;

    tp_verrouille(&p->m);
    if (p->arret) { tp_deverrouille(&p->m); free(fr); return false; }
    if (p->remis_queue) p->remis_queue->suiv = fr; else p->remis_tete = fr;
    p->remis_queue = fr;
    tp_deverrouille(&p->m);

    conn->fd = OWL_INVALID_SOCKET;   /* propriete transferee : pas de double close */
    owl_tcp_pool_reac_signale(p);
    return true;
}

void owl_tcp_pool_reac_signale(OwlTcpPool* p) {
    if (!p || !p->reveil_actif) return;
    owl_reveil_signale(&p->reveil);
}

static void tp_reacteur_boucle(OwlTcpPool* p) {
    for (;;) {
        tp_verrouille(&p->m);
        int stop = p->arret;
        tp_deverrouille(&p->m);
        if (stop) break;
        owl_reacteur_tick(p->reacteur, 50);
    }

    /* Fermer toutes les connexions encore en attente de classification. Les
     * connexions deja dispatchees (file de dispatch) sont drainees et
     * fermees par les workers. */
    while (p->pending) reac_ferme_pending(p, p->pending);

    /* Flux remis mais pas encore pris en charge : les fermer et detruire. */
    tp_verrouille(&p->m);
    FluxRemis* remis = p->remis_tete;
    p->remis_tete = p->remis_queue = NULL;
    tp_deverrouille(&p->m);
    while (remis) {
        FluxRemis* nx = remis->suiv;
        owl_tcp_conn_ferme(&remis->conn);
        if (p->flux_detruit) p->flux_detruit(remis->appctx, p->flux_user);
        free(remis);
        remis = nx;
    }

    /* Flux actifs : fermer proprement (le detruit marque consommateur_fini
     * cote appli -> le producteur eventuel s'arrete et relache sa reference). */
    while (p->flux_actifs) {
        FluxActif* fa = p->flux_actifs;
        p->flux_actifs = fa->suiv;
        owl_reacteur_retire_fd(p->reacteur, fa->conn.fd);
        owl_tcp_conn_ferme(&fa->conn);
        if (p->flux_detruit) p->flux_detruit(fa->appctx, p->flux_user);
        free(fa);
    }

    if (p->reveil_actif) {
        owl_reveil_detruit(&p->reveil, p->reacteur);
        p->reveil_actif = 0;
    }
    owl_reacteur_retire_fd(p->reacteur, p->l->fd);
}

/* Worker data-plane : fait le SETUP (lecture requete + handler). Si la
 * reponse est un flux, sert_dp la REMET au reacteur (owl_tcp_pool_reac_prise)
 * et le worker est libere aussitot — c'est ce qui supprime la retention du
 * worker sur le drain (, W5). Sinon, sert en entier (bufferise) et
 * ferme, comme le pool historique. La concurrence de SETUP reste bornee par
 * le NOMBRE de workers = min(nb_workers, connexions_max). */
static void tp_reac_worker(OwlTcpPool* p) {
    for (;;) {
        tp_verrouille(&p->m);
        RConn* rc = p->disp_tete;
        if (rc) {
            p->disp_tete = rc->dnext;
            if (!p->disp_tete) p->disp_queue = NULL;
            rc->dnext = NULL;
            p->en_vol++;
            if (p->en_vol > p->pic) p->pic = p->en_vol;
        }
        int stop = p->arret && !rc && p->disp_tete == NULL;
        tp_deverrouille(&p->m);

        if (rc) {
            /* sert_dp rend 1 si la conn a ete REMISE au reacteur (flux ou
             * jeton exec pending, rc->conn.fd invalide au retour) ; sinon la
             * conn est servie + fermee ici. Une connexion remise n'est
             * « servie » qu'une fois sa reponse EMISE : c'est le reacteur qui
             * l'y comptera au finalize (reac_flux_finalise). Sans ce report,
             * `servies` atteindrait N avant meme que le reacteur n'ait ecrit
             * la moindre reponse async (W7) — un arret pourrait alors couper
             * avant l'emission. Servi inline -> compte ici, tout de suite. */
            int pris = 0;
            if (p->sert_dp) pris = p->sert_dp(p, &rc->conn, p->user);
            else            p->sert(&rc->conn, p->user);
            tp_verrouille(&p->m);
            p->en_vol--;
            if (!pris) p->servies++;
            tp_deverrouille(&p->m);
            free(rc);
            continue;
        }
        if (stop) break;
        TP_SLEEP(1);   /* file vide : petit repos (pas de condvar, cf. pool historique) */
    }
}

#ifdef _WIN32
static DWORD WINAPI tp_reacteur_thread_fn(LPVOID arg) { tp_reacteur_boucle((OwlTcpPool*)arg); return 0; }
static DWORD WINAPI tp_reac_worker_fn(LPVOID arg)     { tp_reac_worker((OwlTcpPool*)arg);     return 0; }
#else
static void* tp_reacteur_thread_fn(void* arg) { tp_reacteur_boucle((OwlTcpPool*)arg); return NULL; }
static void* tp_reac_worker_fn(void* arg)     { tp_reac_worker((OwlTcpPool*)arg);     return NULL; }
#endif

OwlTcpPool* owl_tcp_pool_reacteur_demarre(OwlTcpListener* l,
                                          const OwlTcpPoolConfig* cfg,
                                          OwlTcpPoolSert sert, void* user,
                                          OwlTcpClassifie classe, void* classe_user,
                                          OwlTcpPoolSertDp sert_dp,
                                          OwlTcpFluxDrain drain,
                                          OwlTcpFluxDetruit detruit,
                                          OwlTcpFluxDelaiMax delaimax,
                                          void* flux_user) {
    if (!l || !sert || !classe) return NULL;

    OwlTcpPool* p = (OwlTcpPool*)calloc(1, sizeof(OwlTcpPool));
    if (!p) return NULL;

    p->l    = l;
    p->sert = sert;
    p->user = user;
    p->reacteur_mode = 1;
    p->classe        = classe;
    p->classe_user   = classe_user;
    p->idle_ms       = (cfg && cfg->reac_classe_timeout_ms > 0)
                           ? cfg->reac_classe_timeout_ms : OWL_TCPS_REAC_CLASSE_MS;
    p->sert_dp       = sert_dp;
    p->flux_drain    = drain;
    p->flux_detruit  = detruit;
    p->flux_delaimax = delaimax;
    p->flux_user     = flux_user;

    int nw = (cfg && cfg->nb_workers > 0) ? cfg->nb_workers : OWL_TCPS_WORKERS_DEFAUT;
    if (nw > OWL_TCPS_WORKERS_MAX) nw = OWL_TCPS_WORKERS_MAX;
    int cmax = (cfg && cfg->connexions_max > 0) ? cfg->connexions_max : nw;
    p->connexions_max = cmax;
    /* Concurrence data-plane = min(nb_workers, connexions_max) : un worker
     * = une connexion en service, comme le pool historique bornait par
     * `reserves`. Un worker au moins. */
    p->nb_workers = (nw < cmax ? nw : cmax);
    if (p->nb_workers < 1) p->nb_workers = 1;

    /* Reacteur cree et listener enregistre AVANT le lancement du thread :
     * l'echec d'allocation est ainsi detecte de maniere synchrone (le thread
     * ne fera plus que tick + nettoyage). */
    p->reacteur = owl_reacteur_cree();
    if (!p->reacteur) { free(p); return NULL; }
    if (!owl_reacteur_ajoute_fd(p->reacteur, l->fd, OWL_REAC_LECTURE, reac_on_listener, p)) {
        owl_reacteur_detruit(p->reacteur);
        free(p);
        return NULL;
    }

    /* Self-pipe (W4) : le pont producteur -> boucle pour le drain de flux
     * (W5). Cree et enregistre AVANT le thread (mono-thread au montage, le
     * reacteur n'est pas thread-safe). Obligatoire des que la prise en charge
     * de flux est active : c'est reac_reveil_cb, et lui seul, qui vide la
     * file des flux remis par les workers. */
    if (p->sert_dp) {
        if (!owl_reveil_cree(&p->reveil, reac_reveil_cb, p) ||
            !owl_reveil_enregistre(&p->reveil, p->reacteur)) {
            if (p->reveil.fd_lecture != OWL_INVALID_SOCKET ||
                p->reveil.fd_ecriture != OWL_INVALID_SOCKET) {
                owl_reveil_detruit(&p->reveil, p->reacteur);
            }
            owl_reacteur_retire_fd(p->reacteur, l->fd);
            owl_reacteur_detruit(p->reacteur);
            free(p);
            return NULL;
        }
        p->reveil_actif = 1;
    }

    tp_mutex_init(&p->m);
    tp_mutex_init(&p->accept_m);   /* inutilise en mode reacteur, detruit par arrete */

#ifdef _WIN32
    p->reac_thread = CreateThread(NULL, 0, tp_reacteur_thread_fn, p, 0, NULL);
    if (p->reac_thread != NULL) p->reac_lance = 1;
#else
    if (pthread_create(&p->reac_thread, NULL, tp_reacteur_thread_fn, p) == 0) p->reac_lance = 1;
#endif
    if (!p->reac_lance) {
        /* Le thread boucle n'a jamais demarre : son nettoyage (qui detruit le
         * reveil) ne tournera pas, on le fait ici. */
        if (p->reveil_actif) { owl_reveil_detruit(&p->reveil, p->reacteur); p->reveil_actif = 0; }
        owl_reacteur_retire_fd(p->reacteur, l->fd);
        owl_reacteur_detruit(p->reacteur);
        tp_mutex_detruit(&p->accept_m);
        tp_mutex_detruit(&p->m);
        free(p);
        return NULL;
    }

    for (int i = 0; i < p->nb_workers; i++) {
#ifdef _WIN32
        p->threads[i] = CreateThread(NULL, 0, tp_reac_worker_fn, p, 0, NULL);
        if (p->threads[i] == NULL) break;
#else
        if (pthread_create(&p->threads[i], NULL, tp_reac_worker_fn, p) != 0) break;
#endif
        p->nb_lances++;
    }

    if (p->nb_lances == 0) {
        /* Aucun worker : le data-plane ne serait jamais servi. Arret propre. */
        tp_verrouille(&p->m);
        p->arret = 1;
        tp_deverrouille(&p->m);
#ifdef _WIN32
        WaitForSingleObject(p->reac_thread, 30000);
        CloseHandle(p->reac_thread);
#else
        pthread_join(p->reac_thread, NULL);
#endif
        owl_reacteur_detruit(p->reacteur);
        tp_mutex_detruit(&p->accept_m);
        tp_mutex_detruit(&p->m);
        free(p);
        return NULL;
    }
    return p;
}

void owl_tcp_pool_arrete(OwlTcpPool* p) {
    if (!p) return;

    tp_verrouille(&p->m);
    p->arret = 1;
    tp_deverrouille(&p->m);

    if (p->reacteur_mode) {
        /* Reacteur d'abord (il ferme les connexions pending et cesse
         * d'alimenter la file), puis workers (ils drainent le data-plane
         * deja dispatche). */
#ifdef _WIN32
        WaitForSingleObject(p->reac_thread, 30000);
        CloseHandle(p->reac_thread);
#else
        pthread_join(p->reac_thread, NULL);
#endif
        for (int i = 0; i < p->nb_lances; i++) {
#ifdef _WIN32
            WaitForSingleObject(p->threads[i], 30000);
            CloseHandle(p->threads[i]);
#else
            pthread_join(p->threads[i], NULL);
#endif
        }
        /* Filet de securite : une RConn restee en file (worker avorte) est
         * fermee ici. En marche normale les workers ont tout draine. */
        while (p->disp_tete) {
            RConn* rc = p->disp_tete;
            p->disp_tete = rc->dnext;
            owl_tcp_conn_ferme(&rc->conn);
            free(rc);
        }
        owl_reacteur_detruit(p->reacteur);
        tp_mutex_detruit(&p->accept_m);
        tp_mutex_detruit(&p->m);
        free(p);
        return;
    }

    for (int i = 0; i < p->nb_lances; i++) {
#ifdef _WIN32
        WaitForSingleObject(p->threads[i], 30000);
        CloseHandle(p->threads[i]);
#else
        pthread_join(p->threads[i], NULL);
#endif
    }

    tp_mutex_detruit(&p->accept_m);
    tp_mutex_detruit(&p->m);
    free(p);
}

size_t owl_tcp_pool_connexions_servies(const OwlTcpPool* p) { return p ? p->servies : 0; }
size_t owl_tcp_pool_pic_simultanees(const OwlTcpPool* p)    { return p ? p->pic : 0; }
size_t owl_tcp_pool_pic_reac_tenue(const OwlTcpPool* p)     { return p ? p->reac_pic : 0; }
size_t owl_tcp_pool_nb_workers(const OwlTcpPool* p)         { return p ? (size_t)p->nb_lances : 0; }
size_t owl_tcp_pool_handshakes_echoues(const OwlTcpPool* p) { return p ? p->handshakes_echoues : 0; }

void owl_tcp_conn_ferme(OwlTcpConn* c) {
    if (c->fd == OWL_INVALID_SOCKET) return;
    /* close_notify AVANT le shutdown : l'alerte de fermeture TLS est un
     * enregistrement comme un autre, elle doit partir sur une socket
     * encore ouverte en ecriture. */
    if (c->tls) {
        owl_tls_session_ferme(c->tls);
        c->tls = NULL;
    }
    /* shutdown(envoi) puis drain a echeance : sans ca Windows repond RST
     * et le client perd la reponse qu'on vient d'ecrire. */
    shutdown(c->fd,
#ifdef _WIN32
             SD_SEND
#else
             SHUT_WR
#endif
    );
    char poubelle[1024];
    uint64_t fin = owl_tcp_server_horloge_ms() + OWL_TCPS_DRAIN_FERMETURE_MS;
    for (;;) {
        uint64_t maintenant = owl_tcp_server_horloge_ms();
        if (maintenant >= fin) break;
        if (srv_attend(c->fd, POLLIN, (int)(fin - maintenant)) != 1) break;
        int n = recv(c->fd, poubelle, (int)sizeof(poubelle), 0);
        if (n <= 0) break;   /* EOF : le pair a fini, plus rien en vol */
    }
    OWL_SRV_CLOSE(c->fd);
    c->fd = OWL_INVALID_SOCKET;
}
