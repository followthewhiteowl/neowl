// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* net_tls_server.c — TLS serveur non bloquant (, phase A1.2).
 * Cf. l'en-tete pour le pourquoi de la propriete par worker. */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "net_tls_server.h"
#include "net_tcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define OWL_TLSS_ERRNO  WSAGetLastError()
  #define OWL_TLSS_EAGAIN WSAEWOULDBLOCK
  #define OWL_TLSS_EINTR  WSAEINTR
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define OWL_TLSS_ERRNO  errno
  #define OWL_TLSS_EAGAIN EAGAIN
  #define OWL_TLSS_EINTR  EINTR
#endif

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>

/* ================================================================
 * Instrument : tours des boucles TLS (cf. en-tete pour le pourquoi de
 * cette grandeur plutot que le nombre de reveils poll)
 * ================================================================ */

static size_t g_tours_boucle = 0;

size_t owl_tls_serveur_tours_boucle(void)     { return g_tours_boucle; }
void   owl_tls_serveur_tours_boucle_raz(void) { g_tours_boucle = 0; }

/* ================================================================
 * Materiel immuable
 * ================================================================ */

struct OwlTlsServeurMateriel {
    /* Les longueurs INCLUENT le NUL final : mbedtls_x509_crt_parse et
     * mbedtls_pk_parse_key ne reconnaissent le PEM qu'a cette
     * condition. Un octet de moins et tout part en DER, qui echoue. */
    unsigned char* cert;
    size_t         cert_len;
    unsigned char* cle;
    size_t         cle_len;
};

static void tlss_err(char* err, size_t err_len, const char* quoi, int rc) {
    if (!err || err_len == 0) return;
    char buf[160];
    mbedtls_strerror(rc, buf, sizeof(buf));
    snprintf(err, err_len, "%s: %s (code -0x%04x)", quoi, buf, -rc);
}

OwlTlsServeurMateriel* owl_tls_serveur_materiel_cree(const char* cert_pem,
                                                     const char* cle_pem,
                                                     char* err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!cert_pem || !cert_pem[0] || !cle_pem || !cle_pem[0]) {
        if (err && err_len) snprintf(err, err_len, "certificat ou cle absent");
        return NULL;
    }

    OwlTlsServeurMateriel* m = (OwlTlsServeurMateriel*)calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cert_len = strlen(cert_pem) + 1;
    m->cle_len  = strlen(cle_pem) + 1;
    m->cert = (unsigned char*)malloc(m->cert_len);
    m->cle  = (unsigned char*)malloc(m->cle_len);
    if (!m->cert || !m->cle) { owl_tls_serveur_materiel_libere(m); return NULL; }
    memcpy(m->cert, cert_pem, m->cert_len);
    memcpy(m->cle,  cle_pem,  m->cle_len);

    /* Validation immediate. Un materiel qui ne parse pas n'est pas un
     * materiel : sans ce controle, « TLS configure » se reduirait a
     * « un pointeur non nul », et le garde-fou de bind s'assouplirait
     * sur une promesse vide. */
    {
        mbedtls_x509_crt          crt;
        mbedtls_pk_context        pk;
        mbedtls_entropy_context   ent;
        mbedtls_ctr_drbg_context  drbg;
        int                       rc;
        bool                      ok = false;

        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&pk);
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);

        rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                   (const unsigned char*)"owl-tls-srv", 11);
        if (rc != 0) { tlss_err(err, err_len, "amorcage du generateur", rc); goto valide_fin; }
        rc = mbedtls_x509_crt_parse(&crt, m->cert, m->cert_len);
        if (rc != 0) { tlss_err(err, err_len, "certificat illisible", rc); goto valide_fin; }
        rc = mbedtls_pk_parse_key(&pk, m->cle, m->cle_len, NULL, 0,
                                  mbedtls_ctr_drbg_random, &drbg);
        if (rc != 0) { tlss_err(err, err_len, "cle privee illisible", rc); goto valide_fin; }
        ok = true;
valide_fin:
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&ent);
        mbedtls_pk_free(&pk);
        mbedtls_x509_crt_free(&crt);
        if (!ok) { owl_tls_serveur_materiel_libere(m); return NULL; }
    }
    return m;
}

void owl_tls_serveur_materiel_libere(OwlTlsServeurMateriel* m) {
    if (!m) return;
    free(m->cert);
    free(m->cle);
    free(m);
}

/* ================================================================
 * Contexte possede par un worker
 * ================================================================ */

struct OwlTlsServeurCtx {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       pk;
    mbedtls_x509_crt         crt;
    mbedtls_ssl_config       conf;
};

OwlTlsServeurCtx* owl_tls_serveur_ctx_cree(const OwlTlsServeurMateriel* m) {
    if (!m) return NULL;

    OwlTlsServeurCtx* c = (OwlTlsServeurCtx*)calloc(1, sizeof(*c));
    if (!c) return NULL;

    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_pk_init(&c->pk);
    mbedtls_x509_crt_init(&c->crt);
    mbedtls_ssl_config_init(&c->conf);

    /* Amorcage PROPRE a ce worker : c'est ce qui remplace
     * MBEDTLS_THREADING_C. Le sel differencie les instances sans
     * dependre d'un compteur partage. */
    char sel[48];
    snprintf(sel, sizeof(sel), "owl-tls-srv-%p", (void*)c);
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char*)sel, strlen(sel)) != 0) goto echec;
    if (mbedtls_x509_crt_parse(&c->crt, m->cert, m->cert_len) != 0) goto echec;
    if (mbedtls_pk_parse_key(&c->pk, m->cle, m->cle_len, NULL, 0,
                             mbedtls_ctr_drbg_random, &c->drbg) != 0) goto echec;

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto echec;
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    /* mTLS hors-scope de ce lot : on ne demande pas de certificat client. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    if (mbedtls_ssl_conf_own_cert(&c->conf, &c->crt, &c->pk) != 0) goto echec;

    return c;

echec:
    owl_tls_serveur_ctx_libere(c);
    return NULL;
}

void owl_tls_serveur_ctx_libere(OwlTlsServeurCtx* c) {
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->crt);
    mbedtls_pk_free(&c->pk);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}

/* ================================================================
 * Session : BIO non blocant + entrelacement avec poll()
 * ================================================================ */

struct OwlTlsSession {
    mbedtls_ssl_context ssl;
    owl_socket_fd_t     fd;
};

static void tlss_non_blocant(owl_socket_fd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Les constantes MBEDTLS_ERR_NET_* vivent dans mbedtls/net_sockets.h,
 * qu'on n'inclut pas (on a notre propre couche TCP). Meme convention
 * que net_tcp_tls.c : INTERNAL_ERROR pour « transport echec », et
 * PEER_CLOSE_NOTIFY pour un EOF propre. */
static int tlss_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    OwlTlsSession* s = (OwlTlsSession*)ctx;
    for (;;) {
        int n = recv(s->fd, (char*)buf, (int)len, 0);
        if (n > 0)  return n;
        if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
        if (OWL_TLSS_ERRNO == OWL_TLSS_EINTR)  continue;
        if (OWL_TLSS_ERRNO == OWL_TLSS_EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

static int tlss_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    OwlTlsSession* s = (OwlTlsSession*)ctx;
    for (;;) {
        int n = send(s->fd, (const char*)buf, (int)len, 0);
        if (n > 0) return n;
        if (OWL_TLSS_ERRNO == OWL_TLSS_EINTR)  continue;
        if (OWL_TLSS_ERRNO == OWL_TLSS_EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

/* Re-poll sur le BON SENS, avec ce qui RESTE de l'echeance absolue.
 * Deux proprietes, chacune indispensable :
 *   - le sens : attendre POLLIN sur un WANT_WRITE, c'est dormir jusqu'a
 *     l'echeance sur un evenement qui n'arrivera pas ;
 *   - l'echeance absolue : un pair qui distille un octet toutes les
 *     100 ms ne doit pas repousser indefiniment la coupure.
 * OWL_TCPS_OK / _TIMEOUT / _ERREUR. */
static int tlss_attend(owl_socket_fd_t fd, int lecture, uint64_t fin) {
    uint64_t maintenant = owl_tcp_server_horloge_ms();
    if (maintenant >= fin) return OWL_TCPS_TIMEOUT;

    int r = owl_tcp_attend(fd, lecture, (int)(fin - maintenant));
    if (r == 0) return OWL_TCPS_TIMEOUT;
    if (r < 0)  return OWL_TCPS_ERREUR;
    return OWL_TCPS_OK;
}

/* Libere la session SANS close_notify. Chemin d'abandon (handshake
 * jamais abouti) et brique interne d'owl_tls_session_ferme. */
static void tlss_detruit(OwlTlsSession* s) {
    mbedtls_ssl_free(&s->ssl);
    free(s);
}

/* ================================================================
 * API EVENT-DRIVEN (chantier W3) — un pas mbedTLS, WANT_* rendu a
 * l'appelant. Les fonctions blocantes plus bas sont des BOUCLES par-
 * dessus celles-ci (poll sur le bon sens + echeance). Voir l'en-tete.
 *
 * PARTAGE DU conf / DE LA CLE PRIVEE PAR THREAD DE BOUCLE — POURQUOI
 * C'EST SUR.
 * Un thread de boucle reacteur multiplexe N connexions sur UN seul
 * OwlTlsServeurCtx (donc un seul ssl_config, une seule cle privee).
 * mbedTLS mute cette cle a chaque signature (cache d'aveuglement RSA
 * rsa.c:1296-1337, table de precalcul EC grp->T ecp.c:527-528) et mute
 * aussi le DRBG du conf a chaque tirage. Ce serait une course SI deux
 * pas de handshake s'executaient en meme temps sur le meme ctx. Ils ne
 * le peuvent pas : la boucle reacteur est SEQUENTIELLE — elle dispatche
 * un callback fd a la fois, chaque callback fait AU PLUS un pas mbedTLS
 * puis rend la main. Aucune reentrance, aucune concurrence intra-thread
 * -> une seule signature en vol a tout instant. C'est exactement la
 * meme garantie que le pool worker-par-connexion (un ctx par worker),
 * transposee au reacteur (un ctx par thread de boucle). Le partage
 * ENTRE threads de boucle reste interdit ; il n'y a qu'un thread de
 * boucle aujourd'hui (design §7.2). Si un jour la boucle est shardee
 * sur plusieurs threads, chaque shard doit posseder SON ctx (comme un
 * worker) — a re-verifier a ce moment-la.
 * ================================================================ */

static OwlTlsPas tlss_want(int rc) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ)  return OWL_TLS_PAS_WANT_LECTURE;
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return OWL_TLS_PAS_WANT_ECRITURE;
    return OWL_TLS_PAS_ERREUR;
}

OwlTlsSession* owl_tls_session_cree(OwlTlsServeurCtx* ctx, owl_socket_fd_t fd) {
    if (!ctx) return NULL;

    OwlTlsSession* s = (OwlTlsSession*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fd = fd;
    mbedtls_ssl_init(&s->ssl);

    if (mbedtls_ssl_setup(&s->ssl, &ctx->conf) != 0) {
        tlss_detruit(s);
        return NULL;
    }
    tlss_non_blocant(fd);
    mbedtls_ssl_set_bio(&s->ssl, s, tlss_bio_send, tlss_bio_recv, NULL);
    return s;
}

owl_socket_fd_t owl_tls_session_fd(const OwlTlsSession* s) {
    return s->fd;
}

OwlTlsPas owl_tls_serveur_handshake_pas(OwlTlsSession* s) {
    g_tours_boucle++;
    int rc = mbedtls_ssl_handshake(&s->ssl);
    if (rc == 0) return OWL_TLS_PAS_OK;
    return tlss_want(rc);   /* WANT_* ou, pour tout autre code, ERREUR */
}

OwlTlsPas owl_tls_session_lit_pas(OwlTlsSession* s, void* buf, size_t len,
                                  size_t* out_lu) {
    if (out_lu) *out_lu = 0;
    g_tours_boucle++;
    int n = mbedtls_ssl_read(&s->ssl, (unsigned char*)buf, len);
    if (n > 0) { if (out_lu) *out_lu = (size_t)n; return OWL_TLS_PAS_OK; }
    if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return OWL_TLS_PAS_FERME;
    return tlss_want(n);
}

OwlTlsPas owl_tls_session_ecrit_pas(OwlTlsSession* s, const void* buf, size_t len,
                                    size_t* out_ecrit) {
    if (out_ecrit) *out_ecrit = 0;
    g_tours_boucle++;
    int n = mbedtls_ssl_write(&s->ssl, (const unsigned char*)buf, len);
    if (n > 0) { if (out_ecrit) *out_ecrit = (size_t)n; return OWL_TLS_PAS_OK; }
    return tlss_want(n);
}

/* ================================================================
 * Chemin BLOCANT (pool worker-par-connexion) : boucle de pas + poll sur
 * le bon sens, echeance absolue. Rien de plus qu'un pilote synchrone de
 * l'API event-driven ci-dessus.
 * ================================================================ */

/* poll pour un pas WANT_* : traduit OwlTlsPas -> sens d'attente. */
static int tlss_attend_pas(owl_socket_fd_t fd, OwlTlsPas p, uint64_t fin) {
    return tlss_attend(fd, p == OWL_TLS_PAS_WANT_LECTURE ? 1 : 0, fin);
}

int owl_tls_serveur_handshake(OwlTlsServeurCtx* ctx, owl_socket_fd_t fd,
                              int timeout_ms, OwlTlsSession** out) {
    *out = NULL;
    OwlTlsSession* s = owl_tls_session_cree(ctx, fd);
    if (!s) return OWL_TCPS_ERREUR;

    uint64_t fin = owl_tcp_server_horloge_ms() +
                   (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        OwlTlsPas p = owl_tls_serveur_handshake_pas(s);
        if (p == OWL_TLS_PAS_OK) break;
        if (p != OWL_TLS_PAS_WANT_LECTURE && p != OWL_TLS_PAS_WANT_ECRITURE) {
            tlss_detruit(s);   /* handshake jamais abouti : pas de close_notify */
            return OWL_TCPS_ERREUR;
        }
        int a = tlss_attend_pas(fd, p, fin);
        if (a != OWL_TCPS_OK) { tlss_detruit(s); return a; }
    }

    *out = s;
    return OWL_TCPS_OK;
}

int owl_tls_session_lit(OwlTlsSession* s, void* buf, size_t len, int timeout_ms) {
    if (!s) return OWL_TCPS_ERREUR;
    uint64_t fin = owl_tcp_server_horloge_ms() +
                   (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        size_t lu = 0;
        OwlTlsPas p = owl_tls_session_lit_pas(s, buf, len, &lu);
        if (p == OWL_TLS_PAS_OK)    return (int)lu;
        if (p == OWL_TLS_PAS_FERME) return OWL_TCPS_FERME;
        if (p != OWL_TLS_PAS_WANT_LECTURE && p != OWL_TLS_PAS_WANT_ECRITURE) {
            return OWL_TCPS_ERREUR;
        }
        int a = tlss_attend_pas(s->fd, p, fin);
        if (a != OWL_TCPS_OK) return a;
    }
}

int owl_tls_session_ecrit(OwlTlsSession* s, const void* buf, size_t len, int timeout_ms) {
    if (!s) return OWL_TCPS_ERREUR;
    const unsigned char* p = (const unsigned char*)buf;
    size_t   ecrit = 0;
    uint64_t fin   = owl_tcp_server_horloge_ms() +
                     (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    while (ecrit < len) {
        size_t n = 0;
        OwlTlsPas pas = owl_tls_session_ecrit_pas(s, p + ecrit, len - ecrit, &n);
        if (pas == OWL_TLS_PAS_OK) { ecrit += n; continue; }
        if (pas != OWL_TLS_PAS_WANT_LECTURE && pas != OWL_TLS_PAS_WANT_ECRITURE) {
            return OWL_TCPS_ERREUR;
        }
        int a = tlss_attend_pas(s->fd, pas, fin);
        if (a != OWL_TCPS_OK) return a;
    }
    return OWL_TCPS_OK;
}

void owl_tls_session_ferme(OwlTlsSession* s) {
    if (!s) return;
    /* close_notify : deux essais au plus (doc mbedTLS), et sans jamais
     * bloquer — la socket est non blocante, un pair muet rendrait
     * WANT_WRITE indefiniment. Le descripteur, lui, est ferme par la
     * couche TCP qui l'a ouvert. */
    for (int i = 0; i < 2; i++) {
        int rc = mbedtls_ssl_close_notify(&s->ssl);
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    tlss_detruit(s);
}
