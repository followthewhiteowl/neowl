// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "net_mock_peer.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE mp_thread_t;
  typedef SOCKET mp_socket_t;
  #define MP_INVALID     INVALID_SOCKET
  #define MP_CLOSE       closesocket
  #define MP_SHUT_WR     SD_SEND
  #define MP_SLEEP(ms)   Sleep((DWORD)(ms))
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t mp_thread_t;
  typedef int mp_socket_t;
  #define MP_INVALID     (-1)
  #define MP_CLOSE       close
  #define MP_SHUT_WR     SHUT_WR
  static void mp_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define MP_SLEEP(ms)   mp_sleep_ms((int)(ms))
#endif

struct OwlMockPeer {
    OwlMockPeerRole    role;
    char               hote[128];
    int                port;
    int                pret;

    char*              octets;
    size_t             octets_len;

    OwlMockPeerDecoupe decoupe;
    size_t             offsets[OWL_MOCK_PEER_MAX_OFFSETS];
    size_t             offsets_len;

    int                delai_ms;

    char               sentinelle[16];
    size_t             sentinelle_len;
    size_t             octets_min_lecture;

    int                lire_apres_ecriture;
    int                fermer_envoi_avant_lecture;
    int                timeout_lecture_ms;
    int                fermer_apres_ecriture;
    int                garder_ouvert_ms;

    char               recu[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t             recu_len;
    size_t             nb_ecritures;
    size_t             nb_octets_emis;

    mp_thread_t        thread;
};

/* ================================================================
 * Emission
 * ================================================================ */

static int mp_write_all(OwlMockPeer* p, mp_socket_t s, const char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = send(s, buf + total, (int)(len - total), 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    p->nb_octets_emis += len;
    p->nb_ecritures++;
    return 0;
}

/* Emet le payload selon le decoupage demande. Le delai separe les write()
 * pour que la pile TCP ne recolle pas les segments : c'est ce qui rend la
 * fragmentation observable par le pair sous test. */
static void mp_emet(OwlMockPeer* p, mp_socket_t s) {
    if (p->octets_len == 0) return;

    switch (p->decoupe) {
        case OWL_MOCK_DECOUPE_OCTET: {
            for (size_t i = 0; i < p->octets_len; i++) {
                if (mp_write_all(p, s, p->octets + i, 1) != 0) return;
                if (p->delai_ms > 0 && i + 1 < p->octets_len) MP_SLEEP(p->delai_ms);
            }
            return;
        }
        case OWL_MOCK_DECOUPE_OFFSETS: {
            size_t debut = 0;
            for (size_t k = 0; k <= p->offsets_len; k++) {
                size_t fin = (k < p->offsets_len) ? p->offsets[k] : p->octets_len;
                if (fin > p->octets_len) fin = p->octets_len;
                if (fin <= debut) continue;
                if (mp_write_all(p, s, p->octets + debut, fin - debut) != 0) return;
                debut = fin;
                if (p->delai_ms > 0 && debut < p->octets_len) MP_SLEEP(p->delai_ms);
            }
            return;
        }
        case OWL_MOCK_DECOUPE_BLOC:
        default:
            mp_write_all(p, s, p->octets, p->octets_len);
            return;
    }
}

/* ================================================================
 * Lecture prealable
 * ================================================================ */

static int mp_recu_contient_sentinelle(const OwlMockPeer* p) {
    if (p->sentinelle_len == 0 || p->recu_len < p->sentinelle_len) return 0;
    for (size_t i = 0; i + p->sentinelle_len <= p->recu_len; i++) {
        if (memcmp(p->recu + i, p->sentinelle, p->sentinelle_len) == 0) return 1;
    }
    return 0;
}

static void mp_lit_prealable(OwlMockPeer* p, mp_socket_t s) {
    if (p->sentinelle_len == 0 && p->octets_min_lecture == 0) return;
    char tmp[1024];
    while (1) {
        if (mp_recu_contient_sentinelle(p)) return;
        if (p->octets_min_lecture > 0 && p->recu_len >= p->octets_min_lecture) return;
        int n = recv(s, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) return;
        size_t place = OWL_MOCK_PEER_MAX_RECU - p->recu_len;
        size_t cp = (size_t)n < place ? (size_t)n : place;
        if (cp > 0) {
            memcpy(p->recu + p->recu_len, tmp, cp);
            p->recu_len += cp;
            p->recu[p->recu_len] = '\0';
        }
        if (cp == 0) return;  /* buffer plein : on arrete de lire */
    }
}

/* Lecture d'apres emission : accumule la reponse du pair dans `recu`
 * jusqu'a EOF (ou timeout via SO_RCVTIMEO). Le drain de fermeture, lui,
 * jette ce qu'il lit — d'ou ce passage separe. */
static void mp_lit_reponse(OwlMockPeer* p, mp_socket_t s) {
    char tmp[1024];
    while (1) {
        int n = recv(s, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) return;
        size_t place = OWL_MOCK_PEER_MAX_RECU - p->recu_len;
        size_t cp = (size_t)n < place ? (size_t)n : place;
        if (cp == 0) return;
        memcpy(p->recu + p->recu_len, tmp, cp);
        p->recu_len += cp;
        p->recu[p->recu_len] = '\0';
    }
}

/* Fermeture propre : shutdown(envoi) puis drain de ce que le pair envoie
 * encore, sinon Windows peut repondre RST et faire perdre au pair les
 * octets qu'on vient d'ecrire. */
static void mp_ferme_proprement(mp_socket_t s) {
    shutdown(s, MP_SHUT_WR);
    char poubelle[512];
    int garde = 0;
    while (garde++ < 64) {
        int n = recv(s, poubelle, (int)sizeof(poubelle), 0);
        if (n <= 0) break;
    }
    MP_CLOSE(s);
}

/* ================================================================
 * Thread
 * ================================================================ */

static void mp_sert(OwlMockPeer* p, mp_socket_t cli) {
    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));

    if (p->timeout_lecture_ms > 0) {
#ifdef _WIN32
        DWORD tv = (DWORD)p->timeout_lecture_ms;
#else
        struct timeval tv;
        tv.tv_sec  = p->timeout_lecture_ms / 1000;
        tv.tv_usec = (p->timeout_lecture_ms % 1000) * 1000;
#endif
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    }

    mp_lit_prealable(p, cli);
    mp_emet(p, cli);
    if (p->fermer_envoi_avant_lecture) shutdown(cli, MP_SHUT_WR);
    if (p->lire_apres_ecriture) mp_lit_reponse(p, cli);

    if (p->fermer_apres_ecriture) {
        mp_ferme_proprement(cli);
        return;
    }
    if (p->garder_ouvert_ms > 0) MP_SLEEP(p->garder_ouvert_ms);
    mp_ferme_proprement(cli);
}

static void mp_boucle_ecoute(OwlMockPeer* p) {
    mp_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == MP_INVALID) return;

    int un = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&un, sizeof(un));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((unsigned short)p->port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) { MP_CLOSE(srv); return; }

    socklen_t alen = sizeof(addr);
    if (getsockname(srv, (struct sockaddr*)&addr, &alen) != 0) { MP_CLOSE(srv); return; }
    p->port = ntohs(addr.sin_port);

    if (listen(srv, 1) != 0) { MP_CLOSE(srv); return; }
    p->pret = 1;

    mp_socket_t cli = accept(srv, NULL, NULL);
    MP_CLOSE(srv);
    if (cli == MP_INVALID) return;

    mp_sert(p, cli);
}

static void mp_boucle_connecte(OwlMockPeer* p) {
    mp_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == MP_INVALID) { p->pret = 1; return; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)p->port);
    if (inet_pton(AF_INET, p->hote[0] ? p->hote : "127.0.0.1", &addr.sin_addr) != 1) {
        MP_CLOSE(s); p->pret = 1; return;
    }

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        MP_CLOSE(s); p->pret = 1; return;
    }
    p->pret = 1;
    mp_sert(p, s);
}

#ifdef _WIN32
static DWORD WINAPI mp_thread_fn(LPVOID arg) {
#else
static void* mp_thread_fn(void* arg) {
#endif
    OwlMockPeer* p = (OwlMockPeer*)arg;
    if (p->role == OWL_MOCK_ROLE_CONNECTE) mp_boucle_connecte(p);
    else                                   mp_boucle_ecoute(p);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================
 * API publique
 * ================================================================ */

void owl_mock_peer_config_defaut(OwlMockPeerConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg->decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg->fermer_apres_ecriture = 1;
}

OwlMockPeer* owl_mock_peer_demarre(const OwlMockPeerConfig* cfg) {
    OwlMockPeer* p = (OwlMockPeer*)calloc(1, sizeof(OwlMockPeer));
    if (!p) return NULL;

    p->role     = cfg->role;
    p->port     = cfg->port;
    p->decoupe  = cfg->decoupe;
    p->delai_ms = cfg->delai_ms;
    p->lire_apres_ecriture   = cfg->lire_apres_ecriture;
    p->fermer_envoi_avant_lecture = cfg->fermer_envoi_avant_lecture;
    p->timeout_lecture_ms    = cfg->timeout_lecture_ms;
    p->fermer_apres_ecriture = cfg->fermer_apres_ecriture;
    p->garder_ouvert_ms      = cfg->garder_ouvert_ms;
    p->octets_min_lecture    = cfg->octets_min_lecture;

    if (cfg->hote) {
        size_t hl = strlen(cfg->hote);
        if (hl >= sizeof(p->hote)) hl = sizeof(p->hote) - 1;
        memcpy(p->hote, cfg->hote, hl);
        p->hote[hl] = '\0';
    }

    if (cfg->sentinelle_lecture) {
        size_t sl = strlen(cfg->sentinelle_lecture);
        if (sl >= sizeof(p->sentinelle)) sl = sizeof(p->sentinelle) - 1;
        memcpy(p->sentinelle, cfg->sentinelle_lecture, sl);
        p->sentinelle_len = sl;
    }

    if (cfg->octets_len > 0) {
        p->octets = (char*)malloc(cfg->octets_len);
        if (!p->octets) { free(p); return NULL; }
        memcpy(p->octets, cfg->octets, cfg->octets_len);
        p->octets_len = cfg->octets_len;
    }

    size_t no = cfg->offsets_len;
    if (no > OWL_MOCK_PEER_MAX_OFFSETS) no = OWL_MOCK_PEER_MAX_OFFSETS;
    for (size_t i = 0; i < no; i++) p->offsets[i] = cfg->offsets[i];
    p->offsets_len = no;

#ifdef _WIN32
    p->thread = CreateThread(NULL, 0, mp_thread_fn, p, 0, NULL);
    if (p->thread == NULL) { free(p->octets); free(p); return NULL; }
#else
    if (pthread_create(&p->thread, NULL, mp_thread_fn, p) != 0) {
        free(p->octets); free(p); return NULL;
    }
#endif

    /* Attend que le listen soit en place (max 2s) pour que le port soit
     * lisible des le retour. Le role CONNECTE pose pret apres connect. */
    int attendu = 0;
    while (!p->pret && attendu < 2000) { MP_SLEEP(10); attendu += 10; }
    return p;
}

int owl_mock_peer_port(const OwlMockPeer* p) { return p->port; }
int owl_mock_peer_pret(const OwlMockPeer* p) { return p->pret; }

void owl_mock_peer_joint(OwlMockPeer* p) {
#ifdef _WIN32
    WaitForSingleObject(p->thread, 10000);
    CloseHandle(p->thread);
#else
    pthread_join(p->thread, NULL);
#endif
}

void owl_mock_peer_libere(OwlMockPeer* p) {
    free(p->octets);
    free(p);
}

const char* owl_mock_peer_recu(const OwlMockPeer* p, size_t* out_len) {
    if (out_len) *out_len = p->recu_len;
    return p->recu;
}

size_t owl_mock_peer_nb_ecritures(const OwlMockPeer* p)   { return p->nb_ecritures; }
size_t owl_mock_peer_nb_octets_emis(const OwlMockPeer* p) { return p->nb_octets_emis; }
