// SPDX-License-Identifier: AGPL-3.0-only
/* net_tls_reacteur_test.c — handshake + I/O TLS pilotés par la boucle
 * réacteur (chantier W3, ).
 *
 * Ce que les six familles de http_tls_test.c prouvent déjà : le chemin
 * BLOCANT (owl_tls_serveur_handshake / _session_lit / _ecrit) sert de
 * vraies connexions chiffrées de bout en bout. Depuis W3 ces trois
 * fonctions sont des BOUCLES par-dessus une API event-driven « fais un
 * pas, rends-moi WANT_LECTURE/WANT_ECRITURE » — donc elles couvrent
 * indirectement les fonctions _pas.
 *
 * Ce que CE fichier prouve en plus, et qu'aucune autre famille ne peut
 * prouver : le même handshake + le même échange chiffré menés SANS
 * jamais poller en interne, uniquement par re-pompage sur événement de
 * la boucle réacteur (W1). C'est exactement le montage que W2 (machine
 * à états HTTP) consommera.
 *
 * Montage :
 *   - un vrai client TLS (owl_tls_connect, chemin client existant,
 *     handshake BLOCANT) tourne sur un thread tiers. Sérialisé par
 *     construction — un seul client — donc pas de course sur le DRBG
 *     global du client (même raison qu'http_tls_test.c) ;
 *   - le SERVEUR vit sur le thread principal, piloté par un réacteur
 *     unique. Il n'appelle JAMAIS de fonction blocante : à chaque
 *     événement il pompe un pas (handshake / lecture / écriture) et
 *     bascule son intérêt LECTURE<->ECRITURE selon le WANT rendu ;
 *   - le certificat est généré en mémoire (EC secp256r1, auto-signé),
 *     comme http_tls_test.c — pas de fixture disque, pas d'openssl.
 *
 * Discriminance : le serveur ré-assemble la requête HTTP en clair
 * (donc a bien DÉCHIFFRÉ), le client relit l'écho EXACT (donc le
 * serveur a bien CHIFFRÉ sa réponse), et le compteur de pas de
 * handshake est > 1 (donc le handshake a bien été fait par PALIERS via
 * la boucle, pas en un seul appel blocant). Un serveur qui bloquerait
 * dans le handshake ne rendrait jamais la main au réacteur : la boucle
 * ne tournerait qu'une fois et le compteur de ticks resterait à 1.
 *
 * Exécuté via `owl --self-test-net` (cf. main.c). Pas de vm_init : le
 * transport ne connaît pas la VM. */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net_reacteur.h"
#include "../net_tcp_server.h"
#include "../net_tls_server.h"
#include "../net_tcp_tls.h"

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
  typedef HANDLE rt_thread_t;
  #define RT_CLOSE closesocket
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t rt_thread_t;
  #define RT_CLOSE close
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

static void compte(int ok, const char* label, int ligne) {
    if (ok) g_pass++;
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (ligne %d)\n", label, ligne); }
}
#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Certificat auto-signé en mémoire (même recette qu'http_tls_test.c)
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
                              (const unsigned char*)"owl-tls-reac", 12) != 0) goto fin;
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

/* ================================================================
 * Client chiffré (thread tiers) — chemin client existant, blocant
 * ================================================================ */

typedef struct {
    int    port;
    bool   handshake_ok;
    char   version[16];
    char   recu[2048];
    size_t recu_len;
} ClientArgs;

static void client_travail(ClientArgs* a) {
    OwlTlsSocket s;
    owl_tls_init(&s);
    if (!owl_tls_connect(&s, "127.0.0.1", a->port, OWL_TLS_VERIFY_NONE, 5000, NULL)) {
        fprintf(stderr, "  [reac] client connect echoue : %s\n", owl_tls_last_error(&s));
        return;
    }
    a->handshake_ok = true;
    snprintf(a->version, sizeof(a->version), "%s", s.version_chosen);

    static const char* REQ =
        "GET /r HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (owl_tls_send(&s, REQ, strlen(REQ)) != (int)strlen(REQ)) {
        owl_tls_close(&s);
        return;
    }
    for (;;) {
        size_t place = sizeof(a->recu) - 1 - a->recu_len;
        if (place == 0) break;
        int n = owl_tls_recv(&s, a->recu + a->recu_len, place);
        if (n <= 0) break;
        a->recu_len += (size_t)n;
        a->recu[a->recu_len] = '\0';
    }
    owl_tls_close(&s);
}

#ifdef _WIN32
static DWORD WINAPI client_thread_fn(LPVOID p) { client_travail((ClientArgs*)p); return 0; }
#else
static void* client_thread_fn(void* p) { client_travail((ClientArgs*)p); return NULL; }
#endif

/* ================================================================
 * Serveur piloté par le réacteur — jamais de poll interne
 * ================================================================ */

typedef enum { PH_HANDSHAKE, PH_LECTURE, PH_ECRITURE, PH_FINI } Phase;

typedef struct {
    OwlTlsSession*  sess;
    owl_socket_fd_t fd;
    Phase           phase;
    bool            erreur;

    char   req[2048];
    size_t req_len;
    char   rep[512];
    size_t rep_len;
    size_t rep_ecrit;

    int nb_pas_handshake;
    int nb_want_lecture;
    int nb_want_ecriture;
} ServEtat;

/* Bascule l'intérêt du fd selon le WANT rendu par le dernier pas.
 * C'est LE geste event-driven de W3 : traduire WANT_READ/WANT_WRITE
 * mbedTLS en OWL_REAC_LECTURE/ECRITURE et rendre la main à la boucle. */
static bool applique_want(OwlReacteur* r, ServEtat* e, OwlTlsPas p) {
    if (p == OWL_TLS_PAS_WANT_LECTURE) {
        e->nb_want_lecture++;
        owl_reacteur_modifie_interet(r, e->fd, OWL_REAC_LECTURE);
        return true;
    }
    if (p == OWL_TLS_PAS_WANT_ECRITURE) {
        e->nb_want_ecriture++;
        owl_reacteur_modifie_interet(r, e->fd, OWL_REAC_ECRITURE);
        return true;
    }
    return false;
}

static void termine(OwlReacteur* r, ServEtat* e, bool erreur) {
    e->erreur = erreur;
    e->phase  = PH_FINI;
    owl_reacteur_retire_fd(r, e->fd);
    owl_reacteur_demande_arret(r);
}

static void serveur_cb(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    (void)fd; (void)evts;
    ServEtat* e = (ServEtat*)user;

    for (;;) {
        if (e->phase == PH_HANDSHAKE) {
            OwlTlsPas p = owl_tls_serveur_handshake_pas(e->sess);
            e->nb_pas_handshake++;
            if (p == OWL_TLS_PAS_OK) {
                e->phase = PH_LECTURE;
                owl_reacteur_modifie_interet(r, e->fd, OWL_REAC_LECTURE);
                continue;
            }
            if (applique_want(r, e, p)) return;
            termine(r, e, true);
            return;
        }

        if (e->phase == PH_LECTURE) {
            size_t lu = 0;
            OwlTlsPas p = owl_tls_session_lit_pas(e->sess, e->req + e->req_len,
                                                  sizeof(e->req) - 1 - e->req_len, &lu);
            if (p == OWL_TLS_PAS_OK) {
                e->req_len += lu;
                e->req[e->req_len] = '\0';
                if (strstr(e->req, "\r\n\r\n")) {
                    /* Écho du chemin, comme le handler d'http_tls_test.c :
                     * prouve que la requête a été DÉCHIFFRÉE côté serveur. */
                    char methode[16] = {0}, chemin[64] = {0};
                    sscanf(e->req, "%15s %63s", methode, chemin);
                    char corps[128];
                    int nc = snprintf(corps, sizeof(corps), "M=%s|C=%s", methode, chemin);
                    if (nc < 0) nc = 0;
                    e->rep_len = (size_t)snprintf(e->rep, sizeof(e->rep),
                        "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s", nc, corps);
                    e->rep_ecrit = 0;
                    e->phase = PH_ECRITURE;
                    owl_reacteur_modifie_interet(r, e->fd, OWL_REAC_ECRITURE);
                    continue;
                }
                continue;   /* en-têtes incomplets : relire */
            }
            if (applique_want(r, e, p)) return;
            termine(r, e, p != OWL_TLS_PAS_FERME);   /* FERME prématuré = échec ici */
            return;
        }

        if (e->phase == PH_ECRITURE) {
            size_t ecr = 0;
            OwlTlsPas p = owl_tls_session_ecrit_pas(e->sess, e->rep + e->rep_ecrit,
                                                    e->rep_len - e->rep_ecrit, &ecr);
            if (p == OWL_TLS_PAS_OK) {
                e->rep_ecrit += ecr;
                if (e->rep_ecrit >= e->rep_len) { termine(r, e, false); return; }
                continue;
            }
            if (applique_want(r, e, p)) return;
            termine(r, e, true);
            return;
        }

        return;   /* PH_FINI */
    }
}

/* Filet de sécurité : si rien n'aboutit, on ne veut pas d'un test
 * suspendu. Le timer arrête la boucle au bout du délai. */
static void garde_temps_cb(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)id; (void)user;
    owl_reacteur_demande_arret(r);
}

/* ================================================================
 * Point d'entrée
 * ================================================================ */

int net_tls_reacteur_self_test_run(void) {
    printf("== TLS serveur event-driven / reacteur (feat/1704, W3) ==\n");
    g_pass = 0;
    g_fail = 0;

    if (!genere_cert_autosigne()) {
        CHECK(0, "certificat_de_test_genere_en_memoire");
        printf("   TOTAL TLS reacteur         : 0/1\n");
        return 1;
    }

    char err[256] = {0};
    OwlTlsServeurMateriel* mat =
        owl_tls_serveur_materiel_cree(g_cert_pem, g_cle_pem, err, sizeof(err));
    CHECK(mat != NULL, "materiel_tls_construit");
    if (!mat) {
        fprintf(stderr, "  materiel TLS refuse : %s\n", err);
        printf("   TOTAL TLS reacteur         : %d/%d\n", g_pass, g_pass + g_fail);
        return g_fail;
    }

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre_tls(&l, "127.0.0.1", 0, 8, mat)) {
        CHECK(0, "ecoute_ouverte");
        owl_tls_serveur_materiel_libere(mat);
        printf("   TOTAL TLS reacteur         : %d/%d\n", g_pass, g_pass + g_fail);
        return g_fail;
    }

    OwlTlsServeurCtx* ctx = owl_tls_serveur_ctx_cree(mat);
    CHECK(ctx != NULL, "contexte_tls_du_thread_de_boucle_construit");

    /* Un SEUL ctx (donc un seul conf, une seule clé privée) sert cette
     * connexion — et en servirait N d'affilée sur ce thread de boucle.
     * C'est sûr car la boucle est séquentielle : un pas de handshake à
     * la fois, jamais deux signatures concurrentes sur la même clé.
     * (Argumentaire crypto complet : commentaire de net_tls_server.c) */

    ClientArgs ca;
    memset(&ca, 0, sizeof(ca));
    ca.port = owl_tcp_listener_port(&l);

    rt_thread_t th;
#ifdef _WIN32
    th = CreateThread(NULL, 0, client_thread_fn, &ca, 0, NULL);
    CHECK(th != NULL, "thread_client_demarre");
#else
    int rc_th = pthread_create(&th, NULL, client_thread_fn, &ca);
    CHECK(rc_th == 0, "thread_client_demarre");
#endif

    OwlTcpConn conn;
    int acc = owl_tcp_listener_accepte(&l, 5000, &conn);
    CHECK(acc == OWL_TCPS_OK, "connexion_acceptee");

    ServEtat e;
    memset(&e, 0, sizeof(e));
    e.phase = PH_HANDSHAKE;

    if (acc == OWL_TCPS_OK && ctx) {
        e.sess = owl_tls_session_cree(ctx, conn.fd);
        CHECK(e.sess != NULL, "session_creee_sans_lancer_le_handshake");
        CHECK(e.sess == NULL || owl_tls_session_fd(e.sess) == conn.fd,
              "session_porte_le_bon_descripteur");
        e.fd = conn.fd;

        if (e.sess) {
            OwlReacteur* r = owl_reacteur_cree();
            CHECK(r != NULL, "reacteur_cree");
            if (r) {
                /* Le handshake commence par LIRE le ClientHello. */
                bool add = owl_reacteur_ajoute_fd(r, e.fd, OWL_REAC_LECTURE,
                                                  serveur_cb, &e);
                CHECK(add, "fd_serveur_enregistre_dans_le_reacteur");
                owl_reacteur_arme_timer(r, 8000, garde_temps_cb, NULL);
                owl_reacteur_boucle(r);
                owl_reacteur_detruit(r);
            }
            owl_tls_session_ferme(e.sess);
        }
        RT_CLOSE(conn.fd);
    }

#ifdef _WIN32
    if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
#else
    if (rc_th == 0) pthread_join(th, NULL);
#endif

    /* --- Assertions de fond --- */
    CHECK(!e.erreur, "serveur_a_mene_l_echange_sans_erreur");
    CHECK(e.phase == PH_FINI, "serveur_a_atteint_la_phase_finale");

    /* Le serveur a DÉCHIFFRÉ la requête : la ligne de requête en clair
     * est présente dans le tampon réassemblé. */
    CHECK(strstr(e.req, "GET /r HTTP/1.1") == e.req,
          "requete_dechiffree_ligne_exacte");

    /* Le handshake a été fait par PALIERS via la boucle, pas en un seul
     * appel : au moins deux pas (typiquement bien plus), et au moins un
     * WANT rendu au réacteur. */
    CHECK(e.nb_pas_handshake >= 2, "handshake_pompe_par_paliers_pas_en_bloc");
    CHECK(e.nb_want_lecture + e.nb_want_ecriture >= 1,
          "au_moins_un_want_rendu_a_la_boucle");

    /* Le client a bien reçu, DÉCHIFFRÉ, l'écho EXACT : preuve que le
     * serveur a chiffré sa réponse à travers l'API event-driven. */
    CHECK(ca.handshake_ok, "client_handshake_abouti");
    CHECK(strstr(ca.recu, "HTTP/1.1 200 OK") == ca.recu,
          "client_recoit_200_en_tete");
    /* 10 = strlen("M=GET|C=/r"). */
    CHECK(strstr(ca.recu, "Content-Length: 10\r\n") != NULL,
          "client_cadrage_content_length_exact");
    CHECK(strstr(ca.recu, "\r\n\r\nM=GET|C=/r") != NULL,
          "client_corps_echo_exact");

    fprintf(stderr, "  [reac] pas_handshake=%d want_lect=%d want_ecr=%d req_len=%u recu_len=%u\n",
            e.nb_pas_handshake, e.nb_want_lecture, e.nb_want_ecriture,
            (unsigned)e.req_len, (unsigned)ca.recu_len);

    if (ctx) owl_tls_serveur_ctx_libere(ctx);
    owl_tcp_listener_ferme(&l);
    owl_tls_serveur_materiel_libere(mat);

    printf("   TOTAL TLS reacteur         : %d/%d\n", g_pass, g_pass + g_fail);
    return g_fail;
}
