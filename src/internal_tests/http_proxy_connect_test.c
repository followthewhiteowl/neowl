// SPDX-License-Identifier: AGPL-3.0-only
/* http_proxy_connect_test.c — 
 *
 * Couverture du chemin CONNECT du client HTTP : `proxy_connect_tunnel`
 * (src/stdlib_http.c), second parseur de reponse HTTP ecrit a la main du
 * depot, jusqu'ici sans aucun test. Le premier avait un bug de
 * desynchronisation en production.
 *
 * ================= POURQUOI UN RIG A TROIS ETAGES =================
 *
 * Le chemin CONNECT n'est emprunte que pour une cible HTTPS (cf.
 * do_http_request : `if (u->is_https)`). Mesurer autre chose que « le
 * tunnel s'etablit vraiment » exige donc un VRAI serveur TLS derriere le
 * proxy. Le rig monte donc :
 *
 *   1. AMONT   — le serveur HTTP d'OWL en TLS, certificat
 *                auto-signe genere en memoire, handler d'echo ;
 *   2. PROXY   — un proxy CONNECT simule (ci-dessous) qui lit la requete
 *                CONNECT, emet la reponse qu'on lui dicte AVEC LE
 *                DECOUPAGE qu'on lui dicte, puis relaie les octets dans
 *                les deux sens ;
 *   3. CLIENT  — la vraie native HTTPRequete, via la VM (tout le parsing
 *                de stdlib_http.c est `static` : aucun point d'entree C
 *                direct, meme constat que ).
 *
 * ============ CE QUI PROTEGE CE FICHIER DES VERTS FAUX ============
 *
 * Un test « le tunnel ne perd pas d'octets » est trivialement vert sur
 * un tunnel qui ne s'etablit jamais. Trois garde-fous, appliques a
 * CHAQUE vecteur :
 *
 *   a. CONTROLE POSITIF (F1.1) : le tunnel nominal rend 200 et le corps
 *      EXACT de l'amont. Si cette assertion tombe, tout le reste du
 *      fichier ne mesure rien.
 *   b. Le proxy est TOUJOURS RELIE A L'AMONT, meme sur les vecteurs de
 *      statut malforme. Un refus ne peut donc venir que du parsing de la
 *      reponse CONNECT, jamais d'un amont absent. Sans ca, « refuse »
 *      serait vert pour la mauvaise raison.
 *   c. La requete CONNECT recue par le proxy est asserte non vide et
 *      bien formee : si un no_proxy d'environnement desactivait le
 *      proxy, le client joindrait l'amont EN DIRECT et rendrait 200 —
 *      un vert parfait qui n'aurait exerce aucune ligne du code sous
 *      test. L'assertion sur les octets recus par le proxy ferme ce
 *      trou.
 *   d. Les mesures de decoupage assertent le NOMBRE DE write() reellement
 *      emis : sans ca, rien ne prouve que le decoupage demande a eu lieu
 *      (la pile TCP recolle les segments — d'ou le delai entre write()).
 *
 * ========================= TROIS FAMILLES =========================
 *
 * F1 — NOMINAL ET NON-REGRESSION. Doit etre verte avant comme apres
 *      toute correction : tunnel nominal, reponse distillee octet par
 *      octet, entetes supplementaires, 407 refuse, reponse vide refusee,
 *      Proxy-Authorization emise.
 *
 * F2 — DEFAUT SUSPECTE : les octets qui suivent le CRLFCRLF sont
 *      abandonnes. Mesure differentielle — MEMES octets sur le fil,
 *      seule la frontiere de write() change. L'invariant teste est que
 *      le resultat NE DEPEND PAS de cette frontiere. C'est deliberement
 *      un invariant et non un verdict : il tient quel que soit le choix
 *      d'implementation (relayer ou refuser), et il ne tient pas
 *      aujourd'hui.
 *
 * F3 — LAXITE DE LA LIGNE DE STATUT. Vecteurs qu'une machine a etats
 *      stricte rejette et que le scan maison accepte. Chaque vecteur
 *      tourne contre un amont VIVANT : un statut accepte va donc jusqu'au
 *      200 complet, un statut refuse rend une erreur. La difference est
 *      donc bien imputable au parseur.
 *
 * Execute via `owl --self-test-net` (cf. main.c), sous vm_init.
 * ================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "../vm.h"
#include "../http_server.h"
#include "../http_exec.h"
#include "../net_tls_server.h"
#include "../net_tcp_server.h"
#include "fixture_unique.h"

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/x509_crt.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  typedef HANDLE px_thread_t;
  typedef SOCKET px_socket_t;
  #define PX_INVALID    INVALID_SOCKET
  #define PX_CLOSE      closesocket
  #define PX_SHUT_WR    SD_SEND
  #define PX_SLEEP(ms)  Sleep((DWORD)(ms))
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #define owl_putenv _putenv
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t px_thread_t;
  typedef int px_socket_t;
  #define PX_INVALID    (-1)
  #define PX_CLOSE      close
  #define PX_SHUT_WR    SHUT_WR
  static void px_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define PX_SLEEP(ms)  px_sleep_ms((int)(ms))
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #define owl_putenv putenv
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define PC_NB_FAMILLES 3

static int g_pass[PC_NB_FAMILLES];
static int g_fail[PC_NB_FAMILLES];
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < PC_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Amont : materiel TLS auto-signe + serveur d'echo
 * ================================================================ */

static char g_cert_pem[4096];
static char g_cle_pem[2048];

static bool genere_cert_autosigne(void) {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       cle;
    mbedtls_x509write_cert   crt;
    unsigned char            serie[1] = { 1 };
    bool                     ok = false;

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&cle);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-proxy-test", 14) != 0) goto fin;
    if (mbedtls_pk_setup(&cle, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto fin;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(cle),
                            mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    if (mbedtls_pk_write_key_pem(&cle, (unsigned char*)g_cle_pem,
                                 sizeof(g_cle_pem)) != 0) goto fin;

    mbedtls_x509write_crt_set_subject_key(&crt, &cle);
    mbedtls_x509write_crt_set_issuer_key(&crt, &cle);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost") != 0) goto fin;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost") != 0) goto fin;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serie, sizeof(serie)) != 0) goto fin;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000",
                                           "20350101000000") != 0) goto fin;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) != 0) goto fin;
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)g_cert_pem, sizeof(g_cert_pem),
                                  mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    ok = true;
fin:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&cle);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

static OwlTlsServeurMateriel* g_materiel = NULL;

/* Handler reentrant : le corps est bati sur la pile du thread reseau
 * puis copie dans la reponse opaque. */
static OwlHttpRep* handler_echo_rep(const OwlHttpRequete* req, void* user) {
    (void)user;
    char corps[256];
    int n = snprintf(corps, sizeof(corps), "M=%s|C=%s", req->methode,
                     req->chemin ? req->chemin : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(corps)) n = (int)sizeof(corps) - 1;
    return owl_http_rep_cree_tampon(200, "text/plain", corps, (size_t)n);
}

typedef struct {
    OwlTcpListener       l;
    OwlHttpServeurConfig cfg;
    OwlTcpPool*          pool;
    int                  port;
} Amont;

static bool amont_demarre(Amont* a) {
    memset(a, 0, sizeof(*a));
    owl_http_serveur_config_defaut(&a->cfg);
    a->cfg.handler_rep = handler_echo_rep;

    if (!owl_tcp_listener_ouvre(&a->l, "127.0.0.1", 0, 8, false)) return false;

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 2;
    pc.connexions_max           = 4;
    pc.tls                      = g_materiel;
    pc.tls_handshake_timeout_ms = 5000;

    a->pool = owl_http_serveur_pool_demarre(&a->l, &a->cfg, &pc);
    if (!a->pool) { owl_tcp_listener_ferme(&a->l); return false; }
    a->port = owl_tcp_listener_port(&a->l);
    return true;
}

static void amont_arrete(Amont* a) {
    if (a->pool) owl_tcp_pool_arrete(a->pool);
    owl_tcp_listener_ferme(&a->l);
    a->pool = NULL;
}

/* ================================================================
 * Proxy CONNECT simule
 *
 * Volontairement bete : il ne comprend PAS la requete CONNECT (il la
 * capture pour assertion), il emet les octets qu'on lui dicte, avec le
 * decoupage qu'on lui dicte, puis relaie. C'est ce qui permet de lui
 * faire produire des reponses qu'aucun proxy conforme n'emettrait.
 * ================================================================ */

#define PX_MAX_REQUETE 4096

typedef struct {
    /* --- entree --- */
    int         port_amont;        /* 0 = pas de relais */
    const char* reponse;           /* reponse au CONNECT */
    size_t      reponse_len;
    const char* extra;             /* octets emis APRES le CRLFCRLF */
    size_t      extra_len;
    int         extra_meme_write;  /* 1 = colles a la reponse (1 write) */
    int         delai_ms;          /* pause entre deux write() */
    int         decoupe_octet;     /* 1 = reponse emise octet par octet */

    /* --- sortie --- */
    int         port;
    int         pret;
    int         demarre;
    size_t      nb_ecritures;
    int         amont_joint;
    char        requete[PX_MAX_REQUETE + 1];
    size_t      requete_len;

    px_thread_t thread;
} ProxyBidon;

static void px_defaut(ProxyBidon* p) {
    memset(p, 0, sizeof(*p));
}

static int px_write_all(ProxyBidon* p, px_socket_t s, const char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = send(s, buf + total, (int)(len - total), 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    if (p) p->nb_ecritures++;
    return 0;
}

/* Lit la requete CONNECT jusqu'au CRLFCRLF (ou EOF). */
static void px_lit_requete(ProxyBidon* p, px_socket_t cli) {
    while (p->requete_len < PX_MAX_REQUETE) {
        if (p->requete_len >= 4 &&
            memcmp(p->requete + p->requete_len - 4, "\r\n\r\n", 4) == 0) return;
        int n = recv(cli, p->requete + p->requete_len,
                     (int)(PX_MAX_REQUETE - p->requete_len), 0);
        if (n <= 0) return;
        p->requete_len += (size_t)n;
        p->requete[p->requete_len] = '\0';
    }
}

/* Relais bidirectionnel jusqu'a fermeture des deux sens. La demi-fermeture
 * est propagee (shutdown WR) : sans ca le client ne verrait jamais la fin
 * du corps delimite par fermeture. */
static void px_relais(px_socket_t a, px_socket_t b) {
    int a_fini = 0, b_fini = 0;
    char buf[8192];
    while (!a_fini || !b_fini) {
        fd_set rd;
        FD_ZERO(&rd);
        if (!a_fini) FD_SET(a, &rd);
        if (!b_fini) FD_SET(b, &rd);
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        int maxfd = (int)((a > b ? a : b)) + 1;
        int n = select(maxfd, &rd, NULL, NULL, &tv);
        if (n <= 0) return;
        if (!a_fini && FD_ISSET(a, &rd)) {
            int k = recv(a, buf, (int)sizeof(buf), 0);
            if (k <= 0) { a_fini = 1; shutdown(b, PX_SHUT_WR); }
            else if (px_write_all(NULL, b, buf, (size_t)k) != 0) return;
        }
        if (!b_fini && FD_ISSET(b, &rd)) {
            int k = recv(b, buf, (int)sizeof(buf), 0);
            if (k <= 0) { b_fini = 1; shutdown(a, PX_SHUT_WR); }
            else if (px_write_all(NULL, a, buf, (size_t)k) != 0) return;
        }
    }
}

static px_socket_t px_connecte_amont(int port) {
    px_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == PX_INVALID) return PX_INVALID;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        PX_CLOSE(s); return PX_INVALID;
    }
    int un = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    return s;
}

static void px_emet_reponse(ProxyBidon* p, px_socket_t cli) {
    if (p->decoupe_octet) {
        for (size_t i = 0; i < p->reponse_len; i++) {
            if (px_write_all(p, cli, p->reponse + i, 1) != 0) return;
            if (p->delai_ms > 0 && i + 1 < p->reponse_len) PX_SLEEP(p->delai_ms);
        }
        return;
    }
    if (p->extra_len > 0 && p->extra_meme_write) {
        /* Un SEUL write() porte la reponse ET les octets suivants : c'est
         * le cas « le proxy et le flux tunnele arrivent ensemble ». */
        size_t n = p->reponse_len + p->extra_len;
        char* tampon = (char*)malloc(n);
        if (!tampon) return;
        memcpy(tampon, p->reponse, p->reponse_len);
        memcpy(tampon + p->reponse_len, p->extra, p->extra_len);
        px_write_all(p, cli, tampon, n);
        free(tampon);
        return;
    }
    px_write_all(p, cli, p->reponse, p->reponse_len);
    if (p->extra_len > 0) {
        /* MEMES octets, frontiere de write() differente. Le delai est
         * structurel : sans lui la pile TCP recolle les deux segments et
         * la mesure differentielle n'a plus d'objet. */
        if (p->delai_ms > 0) PX_SLEEP(p->delai_ms);
        px_write_all(p, cli, p->extra, p->extra_len);
    }
}

static void px_sert(ProxyBidon* p, px_socket_t cli) {
    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));

    px_lit_requete(p, cli);

    px_socket_t amont = PX_INVALID;
    if (p->port_amont > 0) {
        amont = px_connecte_amont(p->port_amont);
        p->amont_joint = (amont != PX_INVALID);
    }

    px_emet_reponse(p, cli);

    if (amont != PX_INVALID) {
        px_relais(cli, amont);
        PX_CLOSE(amont);
    }

    shutdown(cli, PX_SHUT_WR);
    char poubelle[512];
    int garde = 0;
    while (garde++ < 64) {
        int n = recv(cli, poubelle, (int)sizeof(poubelle), 0);
        if (n <= 0) break;
    }
    PX_CLOSE(cli);
}

#ifdef _WIN32
static DWORD WINAPI px_thread_fn(LPVOID arg) {
#else
static void* px_thread_fn(void* arg) {
#endif
    ProxyBidon* p = (ProxyBidon*)arg;

    px_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv != PX_INVALID) {
        int un = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&un, sizeof(un));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            socklen_t alen = sizeof(addr);
            if (getsockname(srv, (struct sockaddr*)&addr, &alen) == 0 &&
                listen(srv, 1) == 0) {
                p->port = ntohs(addr.sin_port);
                p->pret = 1;
                px_socket_t cli = accept(srv, NULL, NULL);
                PX_CLOSE(srv);
                srv = PX_INVALID;
                if (cli != PX_INVALID) px_sert(p, cli);
            }
        }
        if (srv != PX_INVALID) PX_CLOSE(srv);
    }
    p->pret = 1;   /* debloque l'attente meme en cas d'echec */
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int px_demarre(ProxyBidon* p) {
#ifdef _WIN32
    p->thread = CreateThread(NULL, 0, px_thread_fn, p, 0, NULL);
    if (p->thread == NULL) return 0;
#else
    if (pthread_create(&p->thread, NULL, px_thread_fn, p) != 0) return 0;
#endif
    p->demarre = 1;
    int attendu = 0;
    while (!p->pret && attendu < 2000) { PX_SLEEP(10); attendu += 10; }
    return p->port > 0;
}

static void px_joint(ProxyBidon* p) {
    if (!p->demarre) return;
#ifdef _WIN32
    WaitForSingleObject(p->thread, 15000);
    CloseHandle(p->thread);
#else
    pthread_join(p->thread, NULL);
#endif
    p->demarre = 0;
}

/* ================================================================
 * Capture stdout autour d'un vm_interpret
 * ================================================================ */

static char g_out[64 * 1024];

static InterpretResult capture_interpret(const char* source) {
    char TMP[128];
    fixture_unique(TMP, sizeof(TMP), "_owl_http_proxy_test_", ".tmp");
    g_out[0] = '\0';

    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return INTERPRET_RUNTIME_ERROR;

    int tmp_fd = owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return INTERPRET_RUNTIME_ERROR; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd); owl_close(saved_fd);
        return INTERPRET_RUNTIME_ERROR;
    }
    owl_close(tmp_fd);

    InterpretResult res = vm_interpret(source);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_out, 1, sizeof(g_out) - 1, r);
        g_out[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    return res;
}

static int lit_entier(const char* cle) {
    char motif[32];
    snprintf(motif, sizeof(motif), "%s=", cle);
    const char* p = strstr(g_out, motif);
    if (!p) return -1;
    return atoi(p + strlen(motif));
}

static void lit_entre_crochets(const char* cle, char* dst, size_t cap) {
    dst[0] = '\0';
    char motif[32];
    snprintf(motif, sizeof(motif), "%s=[", cle);
    const char* p = strstr(g_out, motif);
    if (!p) return;
    p += strlen(motif);
    const char* fin = strchr(p, ']');
    if (!fin) return;
    size_t n = (size_t)(fin - p);
    if (n >= cap) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* ================================================================
 * Pilotage d'une requete HTTPS a travers le proxy simule
 * ================================================================ */

typedef struct {
    int    interprete_ok;
    int    erreur;        /* ErreurDetectee */
    int    statut;        /* HTTPDernierStatut() */
    char   corps[2048];
    /* Ce que le proxy a observe / fait */
    char   requete_connect[PX_MAX_REQUETE + 1];
    size_t nb_ecritures;
    int    amont_joint;
} Resultat;

static int g_seq = 0;

/* `identifiants` : partie "user:pass@" de l'URL du proxy, "" si aucune. */
static Resultat joue(ProxyBidon* p, const Amont* amont, const char* identifiants,
                     const char* chemin) {
    Resultat r;
    memset(&r, 0, sizeof(r));

    p->port_amont = amont ? amont->port : 0;
    if (!px_demarre(p)) return r;

    int id = ++g_seq;
    char src[1024];
    snprintf(src, sizeof(src),
        /* Reinitialise d'abord : HTTPParametre est un etat GLOBAL qui
         * survit d'un vm_interpret a l'autre. */
        "HTTPParametre(httpParamReinitialise, 0)\n"
        "HTTPParametre(httpParamProxy, \"http://%s127.0.0.1:%d\")\n"
        "HTTPParametre(httpParamNoProxy, \"\")\n"
        "HTTPParametre(httpParamVerifCertificat, Faux)\n"
        "HTTPParametre(httpParamTimeoutConnexion, 8000)\n"
        "r%d est une chaine = HTTPRequete(\"https://127.0.0.1:%d%s\")\n"
        /* ErreurDetectee se lit AVANT tout autre appel : la sentinelle
         * d'erreur ne survit qu'a un seul OP_CALL. */
        "e%d est un booleen = ErreurDetectee\n"
        "Trace(\"ERREUR=\" + e%d)\n"
        "Trace(\"STATUT=\" + HTTPDernierStatut())\n"
        "Trace(\"CORPS=[\" + r%d + \"]\")\n",
        identifiants, p->port, id, amont ? amont->port : 0, chemin, id, id, id);

    r.interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    r.erreur = lit_entier("ERREUR");
    r.statut = lit_entier("STATUT");
    lit_entre_crochets("CORPS", r.corps, sizeof(r.corps));

    px_joint(p);
    memcpy(r.requete_connect, p->requete, p->requete_len);
    r.requete_connect[p->requete_len] = '\0';
    r.nb_ecritures = p->nb_ecritures;
    r.amont_joint  = p->amont_joint;
    return r;
}

/* Le tunnel a REELLEMENT servi : le proxy a vu passer un CONNECT bien
 * forme vers l'amont. Sans ce controle, un no_proxy d'environnement
 * ferait joindre l'amont en direct et rendrait un 200 mensonger. */
static int connect_bien_forme(const Resultat* r, int port_amont) {
    char attendu[128];
    snprintf(attendu, sizeof(attendu), "CONNECT 127.0.0.1:%d HTTP/1.1\r\n", port_amont);
    return strncmp(r->requete_connect, attendu, strlen(attendu)) == 0;
}

/* ================================================================
 * Reponses de proxy
 * ================================================================ */

static const char REP_200[]      = "HTTP/1.1 200 Connection established\r\n\r\n";
static const char REP_200_HDRS[] = "HTTP/1.1 200 Connection established\r\n"
                                   "Proxy-Agent: bidon/1.0\r\n"
                                   "Via: 1.1 bidon\r\n\r\n";
static const char REP_407[]      = "HTTP/1.1 407 Proxy Authentication Required\r\n"
                                   "Proxy-Authenticate: Basic realm=\"x\"\r\n\r\n";
static const char REP_VIDE[]     = "";
static const char REP_TRONQUEE[] = "HTTP/1.1 200 Conn";

/* Octets qui suivent le CRLFCRLF. Le premier ('T' = 0x54) n'est pas un
 * type d'enregistrement TLS valide : s'ils entrent dans le flux tunnele,
 * la poignee de main echoue — ce qui rend leur sort OBSERVABLE. */
static const char OCTETS_SUIVANTS[] = "TUNNEL-OCTETS-SUIVANTS";

/* ================================================================
 * F1 — nominal et non-regression
 * ================================================================ */

static void famille_f1(Amont* amont) {
    g_famille = 0;

    /* --- 1.1 CONTROLE POSITIF : sans lui, tout le reste est vide --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_200; p.reponse_len = sizeof(REP_200) - 1;
        Resultat r = joue(&p, amont, "", "/tunnel");

        CHECK(r.interprete_ok, "f1_1_programme_interprete");
        CHECK(connect_bien_forme(&r, amont->port), "f1_1_proxy_a_vu_un_connect_bien_forme");
        CHECK(strstr(r.requete_connect, "Host: 127.0.0.1:") != NULL,
              "f1_1_connect_porte_l_entete_host");
        CHECK(r.amont_joint, "f1_1_proxy_a_joint_l_amont");
        CHECK(r.erreur == 0, "f1_1_aucune_erreur");
        CHECK(r.statut == 200, "f1_1_statut_200");
        CHECK(strcmp(r.corps, "M=GET|C=/tunnel") == 0, "f1_1_corps_exact_de_l_amont");
        if (strcmp(r.corps, "M=GET|C=/tunnel") != 0)
            fprintf(stderr, "  [f1.1] corps=[%s] statut=%d\n", r.corps, r.statut);
    }

    /* --- 1.2 reponse distillee octet par octet --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_200; p.reponse_len = sizeof(REP_200) - 1;
        p.decoupe_octet = 1;
        p.delai_ms = 2;
        Resultat r = joue(&p, amont, "", "/distille");

        CHECK(r.nb_ecritures == sizeof(REP_200) - 1, "f1_2_decoupage_octet_par_octet_effectif");
        CHECK(r.erreur == 0, "f1_2_aucune_erreur");
        CHECK(r.statut == 200, "f1_2_statut_200");
        CHECK(strcmp(r.corps, "M=GET|C=/distille") == 0, "f1_2_corps_exact_de_l_amont");
    }

    /* --- 1.3 entetes supplementaires dans la reponse CONNECT --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_200_HDRS; p.reponse_len = sizeof(REP_200_HDRS) - 1;
        Resultat r = joue(&p, amont, "", "/entetes");

        CHECK(r.erreur == 0, "f1_3_aucune_erreur");
        CHECK(r.statut == 200, "f1_3_statut_200");
        CHECK(strcmp(r.corps, "M=GET|C=/entetes") == 0, "f1_3_corps_exact_de_l_amont");
    }

    /* --- 1.4 refus 407 : l'amont est VIVANT, donc le refus ne peut
     *     venir que de la ligne de statut --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_407; p.reponse_len = sizeof(REP_407) - 1;
        Resultat r = joue(&p, amont, "", "/refuse");

        CHECK(r.erreur == 1, "f1_4_erreur_signalee");
        CHECK(r.statut != 200, "f1_4_pas_de_200");
        CHECK(r.corps[0] == '\0', "f1_4_aucun_corps");
    }

    /* --- 1.5 reponse vide puis fermeture : refus sans plantage --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_VIDE; p.reponse_len = 0;
        Resultat r = joue(&p, NULL, "", "/vide");

        CHECK(r.interprete_ok, "f1_5_pas_de_plantage");
        CHECK(r.erreur == 1, "f1_5_erreur_signalee");
        CHECK(r.statut != 200, "f1_5_pas_de_200");
    }

    /* --- 1.6 reponse tronquee (pas de CRLFCRLF) puis fermeture --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_TRONQUEE; p.reponse_len = sizeof(REP_TRONQUEE) - 1;
        Resultat r = joue(&p, NULL, "", "/tronque");

        CHECK(r.interprete_ok, "f1_6_pas_de_plantage");
        CHECK(r.erreur == 1, "f1_6_erreur_signalee");
        CHECK(r.statut != 200, "f1_6_pas_de_200");
    }

    /* --- 1.7 identifiants du proxy portes par le CONNECT --- */
    {
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP_200; p.reponse_len = sizeof(REP_200) - 1;
        Resultat r = joue(&p, amont, "alice:secret@", "/auth");

        /* base64("alice:secret") — valeur EXACTE, pas « un entete existe ». */
        CHECK(strstr(r.requete_connect,
                     "Proxy-Authorization: Basic YWxpY2U6c2VjcmV0\r\n") != NULL,
              "f1_7_proxy_authorization_exacte");
        CHECK(r.statut == 200, "f1_7_statut_200");
        CHECK(strcmp(r.corps, "M=GET|C=/auth") == 0, "f1_7_corps_exact_de_l_amont");
    }
}

/* ================================================================
 * F2 — defaut suspecte : octets qui suivent le CRLFCRLF
 *
 * Mesure differentielle. MEMES octets sur le fil dans les deux vecteurs,
 * seule la frontiere de write() change. L'invariant : le resultat ne
 * depend pas de cette frontiere.
 * ================================================================ */

static void famille_f2(Amont* amont) {
    g_famille = 1;

    /* --- 2.1 reponse ET octets suivants dans le MEME write() --- */
    ProxyBidon p1; px_defaut(&p1);
    p1.reponse = REP_200; p1.reponse_len = sizeof(REP_200) - 1;
    p1.extra = OCTETS_SUIVANTS; p1.extra_len = sizeof(OCTETS_SUIVANTS) - 1;
    p1.extra_meme_write = 1;
    Resultat r1 = joue(&p1, amont, "", "/colle");

    CHECK(r1.nb_ecritures == 1, "f2_1_un_seul_write_coalesce");
    CHECK(connect_bien_forme(&r1, amont->port), "f2_1_tunnel_reellement_demande");

    /* --- 2.2 MEMES octets, DEUX write() separes par un delai --- */
    ProxyBidon p2; px_defaut(&p2);
    p2.reponse = REP_200; p2.reponse_len = sizeof(REP_200) - 1;
    p2.extra = OCTETS_SUIVANTS; p2.extra_len = sizeof(OCTETS_SUIVANTS) - 1;
    p2.extra_meme_write = 0;
    p2.delai_ms = 150;
    Resultat r2 = joue(&p2, amont, "", "/separe");

    CHECK(r2.nb_ecritures == 2, "f2_2_deux_write_distincts");
    CHECK(connect_bien_forme(&r2, amont->port), "f2_2_tunnel_reellement_demande");

    /* --- L'INVARIANT : meme flux d'octets, meme verdict --- */
    fprintf(stderr, "  [f2] coalesce : erreur=%d statut=%d corps=[%s]\n",
            r1.erreur, r1.statut, r1.corps);
    fprintf(stderr, "  [f2] separe   : erreur=%d statut=%d corps=[%s]\n",
            r2.erreur, r2.statut, r2.corps);

    CHECK(r1.erreur == r2.erreur, "f2_3_meme_verdict_erreur_quel_que_soit_le_decoupage");
    CHECK(r1.statut == r2.statut, "f2_3_meme_statut_quel_que_soit_le_decoupage");
    CHECK(strcmp(r1.corps, r2.corps) == 0, "f2_3_meme_corps_quel_que_soit_le_decoupage");

    /* Semantique retenue apres la mesure : des octets non sollicites font
     * echouer le tunnel. Assertion posee APRES coup, et deliberement PLUS
     * STRICTE que l'invariant ci-dessus — elle etait rouge sur le vecteur
     * coalesce avant la correction (statut=200), elle ne rattrape donc rien.
     * A noter, honnetement : sur le vecteur separe le refus vient de la
     * couche TLS (les octets parasites corrompent le flux), pas du parsing
     * du CONNECT. Seul le vecteur coalesce mesure le parseur. */
    CHECK(r1.erreur == 1 && r1.statut != 200, "f2_4_octets_non_sollicites_refuses_coalesces");
    CHECK(r2.erreur == 1 && r2.statut != 200, "f2_4_octets_non_sollicites_refuses_separes");
}

/* ================================================================
 * F3 — laxite de la ligne de statut
 *
 * Chaque vecteur tourne contre l'amont VIVANT : un statut accepte mene
 * au 200 complet, un statut refuse rend une erreur. La difference est
 * donc imputable au seul parsing de la reponse CONNECT.
 * ================================================================ */

static void famille_f3(Amont* amont) {
    g_famille = 2;

    /* --- 3.1 statut qui deborde un entier 32 bits et retombe sur 200.
     *     4294967496 = 2^32 + 200. Aucun statut HTTP n'a 10 chiffres. --- */
    {
        static const char REP[] = "HTTP/1.1 4294967496 Nope\r\n\r\n";
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP; p.reponse_len = sizeof(REP) - 1;
        Resultat r = joue(&p, amont, "", "/debordement");

        fprintf(stderr, "  [f3.1] erreur=%d statut=%d corps=[%s]\n",
                r.erreur, r.statut, r.corps);
        CHECK(r.statut != 200, "f3_1_statut_hors_bornes_refuse");
        CHECK(r.erreur == 1, "f3_1_erreur_signalee");
    }

    /* --- 3.2 aucune version HTTP sur la ligne de statut --- */
    {
        static const char REP[] = "PAS-UNE-VERSION 200 OK\r\n\r\n";
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP; p.reponse_len = sizeof(REP) - 1;
        Resultat r = joue(&p, amont, "", "/sansversion");

        fprintf(stderr, "  [f3.2] erreur=%d statut=%d corps=[%s]\n",
                r.erreur, r.statut, r.corps);
        CHECK(r.statut != 200, "f3_2_ligne_de_statut_sans_version_refusee");
        CHECK(r.erreur == 1, "f3_2_erreur_signalee");
    }

    /* --- 3.3 reponse informationnelle avant la reponse finale --- */
    {
        static const char REP[] = "HTTP/1.1 100 Continue\r\n\r\n"
                                  "HTTP/1.1 200 Connection established\r\n\r\n";
        ProxyBidon p; px_defaut(&p);
        p.reponse = REP; p.reponse_len = sizeof(REP) - 1;
        Resultat r = joue(&p, amont, "", "/interim");

        fprintf(stderr, "  [f3.3] erreur=%d statut=%d corps=[%s]\n",
                r.erreur, r.statut, r.corps);
        CHECK(r.statut == 200, "f3_3_reponse_informationnelle_traversee");
        CHECK(strcmp(r.corps, "M=GET|C=/interim") == 0, "f3_3_corps_exact_de_l_amont");
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_proxy_connect_self_test_run(void) {
    printf("== proxy CONNECT (fix/1390) ==\n");

    memset(g_pass, 0, sizeof(g_pass));
    memset(g_fail, 0, sizeof(g_fail));

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* L'environnement du PROCESSUS decide, et un autre self-test du meme
     * binaire y pose no_proxy=127.0.0.1 avant nous. Mesure faite : dans
     * cet etat, resolve_proxy sort par la porte no_proxy AVANT meme de
     * lire le proxy explicite, le client joint l'amont EN DIRECT et rend
     * un 200 parfaitement vert qui n'a exerce aucune ligne de
     * proxy_connect_tunnel. On repart donc d'un environnement neutre. */
    {
        static char e1[] = "http_proxy=";
        static char e2[] = "HTTP_PROXY=";
        static char e3[] = "https_proxy=";
        static char e4[] = "HTTPS_PROXY=";
        static char e5[] = "no_proxy=";
        static char e6[] = "NO_PROXY=";
        owl_putenv(e1); owl_putenv(e2); owl_putenv(e3);
        owl_putenv(e4); owl_putenv(e5); owl_putenv(e6);
    }

    if (!genere_cert_autosigne()) {
        g_famille = 0;
        CHECK(0, "certificat_de_test_genere_en_memoire");
        printf("   TOTAL proxy CONNECT        : 0/1\n");
        return 1;
    }

    char err[256] = {0};
    g_materiel = owl_tls_serveur_materiel_cree(g_cert_pem, g_cle_pem, err, sizeof(err));
    if (!g_materiel) {
        g_famille = 0;
        fprintf(stderr, "  materiel TLS refuse : %s\n", err);
        CHECK(0, "materiel_tls_construit_depuis_le_pem_genere");
        printf("   TOTAL proxy CONNECT        : 0/1\n");
        return 1;
    }

    Amont amont;
    if (!amont_demarre(&amont)) {
        g_famille = 0;
        CHECK(0, "amont_tls_demarre");
        owl_tls_serveur_materiel_libere(g_materiel);
        g_materiel = NULL;
        printf("   TOTAL proxy CONNECT        : 0/1\n");
        return 1;
    }

    famille_f1(&amont);
    famille_f2(&amont);
    famille_f3(&amont);

    amont_arrete(&amont);
    owl_tls_serveur_materiel_libere(g_materiel);
    g_materiel = NULL;

    int tp = 0, tf = 0;
    for (int i = 0; i < PC_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f1 nominal / non-regression: %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f2 octets apres l'en-tete  : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f3 laxite ligne de statut  : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   TOTAL proxy CONNECT        : %d/%d\n", tp, tp + tf);
    return tf;
}
