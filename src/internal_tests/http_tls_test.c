// SPDX-License-Identifier: AGPL-3.0-only
/* http_tls_test.c — TLS serveur (, phase A1.2).
 *
 * Spec : conception du serveur HTTP interne et son contrat de transport.
 *
 * Ce lot ferme la couche transport : le serveur sait chiffrer. Six
 * familles, chacune tournee vers une PREUVE, pas vers un « ca marche » —
 * cinq precedents sur ce chantier ont montre des verts trompeurs (un
 * correctif qui ne corrigeait rien, un serveur qui ne servait rien, un
 * serveur qui fermait toujours, un serveur qui n'acceptait rien).
 *
 *   T1. POSITIF   — apres handshake, une vraie requete HTTP sur le canal
 *                   chiffre rend 200 et le corps EXACT. Un serveur qui
 *                   refuse tout est rouge ici.
 *   T2. NEGATIF   — un client EN CLAIR contre le port TLS n'obtient
 *                   jamais 200. Sans lui, « le handshake marche » ne
 *                   prouve pas que le serveur EXIGE TLS.
 *   T3. ECHEANCE  — un client qui ouvre le TCP puis se tait est coupe
 *                   dans l'echeance, et le worker est LIBERE : une
 *                   seconde connexion est servie ensuite, ce
 *                   qu'owl_tcp_pool_connexions_servies atteste.
 *   T4. BOUCLE    — compteur de TOURS des boucles TLS. Sur 300 ms de
 *                   stagnation on attend une poignee de tours, pas des
 *                   milliers. Compter les verts ne prouverait rien ici ;
 *                   cette mesure si.
 *   T5. N SIMULT. — N connexions TLS ouvertes en meme temps, servies sans
 *                   melange, parallelisme MESURE. Le dispositif d'A1.1b
 *                   etendu, pas affaibli.
 *   T6. GARDE-FOU — le refus de bind non-loopback ne s'assouplit que
 *                   lorsqu'un materiel TLS est REELLEMENT pose.
 *
 * Choix de harnais (instruits avant ecriture, cf. ticket) :
 *
 *  - le pair mock N'APPREND PAS TLS. Son contrat est « quels octets,
 *    decoupes comment » sur socket OS brute ; sous TLS le decoupage se
 *    joue au niveau enregistrement et DECOUPE_OCTET ne controle plus
 *    rien de ce que le serveur voit. Il reste utilise TEL QUEL pour le
 *    controle negatif (T2) et pour le client muet (T3, T4) ;
 *  - le client chiffre est le chemin client existant (owl_tls_connect,
 *    VERIFY_NONE). Ce chemin s'appuie sur le DRBG GLOBAL de
 *    net_tcp_tls.c : N handshakes clients concurrents seraient une course
 *    DANS LE HARNAIS. Le harnais garde donc tout le cote client sur le
 *    thread principal — les handshakes sont serialises par construction,
 *    puis les N connexions restent ouvertes simultanement pour la mesure
 *    de parallelisme. C'est une demonstration du sujet connexe (les deux
 *    paires g_entropy/g_drbg dupliquees), pas son contournement ;
 *  - le certificat est genere EN MEMOIRE par mbedTLS (EC secp256r1,
 *    auto-signe). Pas de fixture sur disque, pas d'openssl dans la
 *    chaine — il est present dans le shell mais non vendore, donc
 *    interdit comme dependance.
 *
 * Execute via `owl --self-test-net` (cf. main.c). Pas de vm_init : le
 * transport ne connait pas la VM, c'est l'invariant du contrat. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_tls_server.h"
#include "../net_tcp_tls.h"
#include "../net_mock_peer.h"

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/x509_crt.h>

#ifdef _WIN32
  #include <windows.h>
  #define TT_SLEEP(ms) Sleep((DWORD)(ms))
#else
  #include <time.h>
  static void tt_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define TT_SLEEP(ms) tt_sleep_ms((int)(ms))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define TT_NB_FAMILLES 6

static int g_pass[TT_NB_FAMILLES];
static int g_fail[TT_NB_FAMILLES];
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < TT_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL T%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Materiel de test : certificat auto-signe genere en memoire
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
                              (const unsigned char*)"owl-tls-test", 12) != 0) goto fin;
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

/* ================================================================
 * Handler d'echo REENTRANT (aucun etat global : le corps est bati sur
 * la pile du thread reseau puis COPIE dans la reponse opaque). Un
 * handler a tampon global melangerait deux connexions concurrentes —
 * ce que T5 doit pouvoir attraper.
 * ================================================================ */

static int g_handler_delai_ms = 0;

static OwlHttpRep* handler_echo_rep(const OwlHttpRequete* req, void* user) {
    (void)user;
    char corps[256];
    int n = snprintf(corps, sizeof(corps), "M=%s|C=%s", req->methode,
                     req->chemin ? req->chemin : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(corps)) n = (int)sizeof(corps) - 1;
    if (g_handler_delai_ms > 0) TT_SLEEP(g_handler_delai_ms);
    return owl_http_rep_cree_tampon(200, "text/plain", corps, (size_t)n);
}

/* ================================================================
 * Client chiffre (thread principal uniquement — cf. en-tete)
 * ================================================================ */

typedef struct {
    OwlTlsSocket s;
    bool         ouvert;
    char         recu[8192];
    size_t       recu_len;
} ClientTls;

static bool ctls_ouvre(ClientTls* c, int port) {
    memset(c, 0, sizeof(*c));
    owl_tls_init(&c->s);
    if (!owl_tls_connect(&c->s, "127.0.0.1", port, OWL_TLS_VERIFY_NONE, 5000, NULL)) {
        fprintf(stderr, "  [tls] connect echoue : %s\n", owl_tls_last_error(&c->s));
        return false;
    }
    c->ouvert = true;
    return true;
}

static bool ctls_demande(ClientTls* c, const char* chemin) {
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                     chemin);
    return owl_tls_send(&c->s, req, (size_t)n) == n;
}

/* Lit jusqu'a la fermeture du pair. Le serveur annonce Connection:
 * close, donc la lecture se termine d'elle-meme. */
static void ctls_lit_tout(ClientTls* c) {
    for (;;) {
        size_t place = sizeof(c->recu) - 1 - c->recu_len;
        if (place == 0) break;
        int n = owl_tls_recv(&c->s, c->recu + c->recu_len, place);
        if (n <= 0) break;
        c->recu_len += (size_t)n;
        c->recu[c->recu_len] = '\0';
    }
}

static void ctls_ferme(ClientTls* c) {
    if (!c->ouvert) return;
    owl_tls_close(&c->s);
    c->ouvert = false;
}

/* ================================================================
 * Client muet : ouvre le TCP et ne dit rien. Le pair mock brut suffit
 * — il lit jusqu'a son echeance sans jamais rien emettre avant.
 * ================================================================ */

static OwlMockPeer* muet_demarre(int port, int duree_ms) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                = "127.0.0.1";
    mc.port                = port;
    mc.octets              = "z";
    mc.octets_len          = 1;
    /* Lecture prealable qui ne sera JAMAIS satisfaite : le pair reste
     * connecte, muet, jusqu'a son echeance de lecture. C'est le
     * slowloris de handshake. */
    mc.octets_min_lecture  = 65000;
    mc.timeout_lecture_ms  = duree_ms;
    mc.fermer_apres_ecriture = 1;
    return owl_mock_peer_demarre(&mc);
}

/* Attend que le pool ait servi `vise` connexions, au plus `max_ms`. */
static uint64_t attend_servies(const OwlTcpPool* p, size_t vise, int max_ms) {
    uint64_t t0 = owl_tcp_server_horloge_ms();
    for (;;) {
        if (owl_tcp_pool_connexions_servies(p) >= vise) break;
        if (owl_tcp_server_horloge_ms() - t0 >= (uint64_t)max_ms) break;
        TT_SLEEP(2);
    }
    return owl_tcp_server_horloge_ms() - t0;
}

static void config_serveur(OwlHttpServeurConfig* cfg) {
    owl_http_serveur_config_defaut(cfg);
    cfg->handler_rep = handler_echo_rep;
}

/* ================================================================
 * T1 — controle POSITIF
 * ================================================================ */

static void famille_t1(void) {
    g_famille = 0;
    g_handler_delai_ms = 0;

    OwlHttpServeurConfig cfg;
    config_serveur(&cfg);

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "t1_ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 2;
    pc.connexions_max           = 2;
    pc.tls                      = g_materiel;
    pc.tls_handshake_timeout_ms = 5000;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "t1_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    ClientTls c;
    bool ouvert = ctls_ouvre(&c, owl_tcp_listener_port(&l));
    CHECK(ouvert, "t1_handshake_tls_aboutit");
    if (ouvert) {
        CHECK(strcmp(c.s.version_chosen, "TLSv1.2") == 0 ||
              strcmp(c.s.version_chosen, "TLSv1.3") == 0,
              "t1_version_tls_negociee_renseignee");
        CHECK(c.s.cipher_chosen[0] != '\0', "t1_suite_cryptographique_negociee");

        CHECK(ctls_demande(&c, "/chiffre"), "t1_requete_emise_sur_le_canal_chiffre");
        ctls_lit_tout(&c);

        /* La valeur EXACTE, pas « quelque chose est arrive ». */
        CHECK(strstr(c.recu, "HTTP/1.1 200 ") == c.recu, "t1_statut_200_en_tete_de_reponse");
        /* 16 = strlen("M=GET|C=/chiffre"). La premiere version de cette
         * assertion annoncait 15 : erreur d'arithmetique cote TEST,
         * corrigee par la mesure — le corps recu est bien celui-la, ce
         * que l'assertion suivante verifie a l'octet. */
        CHECK(strstr(c.recu, "Content-Length: 16\r\n") != NULL, "t1_cadrage_content_length_exact");
        CHECK(strstr(c.recu, "\r\n\r\nM=GET|C=/chiffre") != NULL, "t1_corps_exact_apres_les_entetes");
        if (!strstr(c.recu, "M=GET|C=/chiffre")) {
            fprintf(stderr, "  [t1] recu=%.200s\n", c.recu);
        }
        ctls_ferme(&c);
    }

    attend_servies(pool, 1, 8000);
    CHECK(owl_tcp_pool_connexions_servies(pool) == 1, "t1_une_connexion_servie");
    CHECK(owl_tcp_pool_handshakes_echoues(pool) == 0, "t1_aucun_handshake_echoue");

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * T2 — controle NEGATIF : le serveur EXIGE TLS
 * ================================================================ */

static void famille_t2(void) {
    g_famille = 1;
    g_handler_delai_ms = 0;

    OwlHttpServeurConfig cfg;
    config_serveur(&cfg);

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "t2_ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 2;
    pc.connexions_max           = 2;
    pc.tls                      = g_materiel;
    pc.tls_handshake_timeout_ms = 2000;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "t2_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    /* Une requete HTTP parfaitement valide... en clair. Le premier octet
     * ('G') n'est pas un type d'enregistrement TLS : le handshake doit
     * echouer, et AUCUNE reponse applicative ne doit sortir. */
    static const char* CLAIR =
        "GET /clair HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = owl_tcp_listener_port(&l);
    mc.octets                     = CLAIR;
    mc.octets_len                 = strlen(CLAIR);
    mc.decoupe                    = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = 3000;
    mc.fermer_apres_ecriture      = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    CHECK(pair != NULL, "t2_client_en_clair_demarre");
    if (!pair) { owl_tcp_pool_arrete(pool); owl_tcp_listener_ferme(&l); return; }

    attend_servies(pool, 1, 8000);
    owl_mock_peer_joint(pair);

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(pair, &rlen);

    CHECK(strstr(recu, "200") == NULL, "t2_jamais_de_200_en_clair");
    CHECK(strstr(recu, "HTTP/1.1") == NULL, "t2_aucune_reponse_http_emise");
    CHECK(strstr(recu, "M=GET|C=/clair") == NULL, "t2_le_handler_n_a_rien_rendu");
    CHECK(owl_tcp_pool_handshakes_echoues(pool) >= 1, "t2_handshake_compte_comme_echoue");

    owl_mock_peer_libere(pair);
    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * T3 — echeance de handshake, et worker REELLEMENT libere
 *
 * Un seul worker, un seul emplacement : si le handshake immobilise le
 * worker, la seconde connexion n'est jamais servie. C'est ce qui rend
 * l'assertion discriminante — un serveur qui coupe sans liberer
 * passerait « le muet est coupe » et echouerait ici.
 * ================================================================ */

static void famille_t3(void) {
    g_famille = 2;
    g_handler_delai_ms = 0;

    OwlHttpServeurConfig cfg;
    config_serveur(&cfg);

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "t3_ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 1;
    pc.connexions_max           = 1;
    pc.tls                      = g_materiel;
    pc.tls_handshake_timeout_ms = 300;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "t3_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    /* Le muet tient la connexion BIEN plus longtemps que l'echeance de
     * handshake : c'est le serveur qui doit trancher, pas lui. */
    OwlMockPeer* muet = muet_demarre(owl_tcp_listener_port(&l), 4000);
    CHECK(muet != NULL, "t3_client_muet_demarre");

    uint64_t t0 = owl_tcp_server_horloge_ms();
    attend_servies(pool, 1, 3000);
    uint64_t coupe_en = owl_tcp_server_horloge_ms() - t0;

    CHECK(owl_tcp_pool_connexions_servies(pool) == 1, "t3_le_muet_est_relache");
    CHECK(owl_tcp_pool_handshakes_echoues(pool) == 1, "t3_coupe_par_echeance_de_handshake");
    /* Coupe DANS l'echeance, pas au bout de l'echeance du client (4 s)
     * ni du timeout de lecture HTTP (5 s). La borne haute est ce qui
     * distingue une echeance de handshake d'un autre mecanisme. */
    CHECK(coupe_en < 2000, "t3_coupe_dans_l_echeance_pas_apres");

    /* PREUVE que le worker est libere : une seconde connexion, chiffree
     * celle-la, est servie ensuite sur le MEME unique worker. */
    ClientTls c;
    bool ouvert = ctls_ouvre(&c, owl_tcp_listener_port(&l));
    CHECK(ouvert, "t3_seconde_connexion_handshake_ok");
    if (ouvert) {
        CHECK(ctls_demande(&c, "/apres"), "t3_seconde_requete_emise");
        ctls_lit_tout(&c);
        CHECK(strstr(c.recu, "HTTP/1.1 200 ") == c.recu, "t3_seconde_connexion_rend_200");
        CHECK(strstr(c.recu, "M=GET|C=/apres") != NULL, "t3_seconde_connexion_corps_exact");
        ctls_ferme(&c);
    }

    attend_servies(pool, 2, 8000);
    CHECK(owl_tcp_pool_connexions_servies(pool) == 2, "t3_deux_connexions_traversees_au_total");
    fprintf(stderr, "  [t3] coupe_en=%ums echoues=%u servies=%u\n",
            (unsigned)coupe_en,
            (unsigned)owl_tcp_pool_handshakes_echoues(pool),
            (unsigned)owl_tcp_pool_connexions_servies(pool));

    if (muet) { owl_mock_peer_joint(muet); owl_mock_peer_libere(muet); }
    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * T4 — absence de boucle occupee
 *
 * Un handshake en attente doit DORMIR dans poll(), pas tourner. La
 * mesure est le nombre de TOURS des boucles TLS (un par appel a
 * mbedtls_ssl_handshake / _read / _write) pendant une stagnation de
 * 300 ms. Un `continue` sur WANT_READ sans attente en ferait des
 * centaines de milliers ; un poll sur le bon sens en fait un.
 *
 * Pourquoi ce compteur et pas les reveils poll() : une implementation
 * qui boucle sans jamais attendre n'appelle PAS poll, donc afficherait
 * zero reveil tout en brulant un coeur. Le tour de boucle, lui, est
 * incontournable. Premiere version de cette famille : elle comptait les
 * reveils, mesurait 0 — un vert exact et pourtant non discriminant.
 * ================================================================ */

static void famille_t4(void) {
    g_famille = 3;
    g_handler_delai_ms = 0;

    OwlHttpServeurConfig cfg;
    config_serveur(&cfg);

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "t4_ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 1;
    pc.connexions_max           = 1;
    pc.tls                      = g_materiel;
    /* Echeance LARGE : on veut mesurer la stagnation, pas la coupure. */
    pc.tls_handshake_timeout_ms = 5000;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "t4_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    OwlMockPeer* muet = muet_demarre(owl_tcp_listener_port(&l), 3000);
    CHECK(muet != NULL, "t4_client_muet_demarre");

    /* Laisse le worker entrer dans le handshake, puis remet le compteur
     * a zero : on ne mesure QUE la stagnation. */
    TT_SLEEP(150);
    owl_tls_serveur_tours_boucle_raz();
    TT_SLEEP(300);
    size_t tours = owl_tls_serveur_tours_boucle();

    CHECK(tours <= 10, "t4_une_poignee_de_tours_pas_des_milliers");
    fprintf(stderr, "  [t4] tours de boucle sur 300 ms de stagnation = %u\n", (unsigned)tours);

    /* Controle : le compteur n'est pas mort. Une connexion chiffree
     * reelle DOIT le faire bouger, sinon « tours <= 10 » serait vert
     * pour la pire des raisons — un compteur qui ne compte rien. */
    owl_tcp_pool_arrete(pool);
    if (muet) { owl_mock_peer_joint(muet); owl_mock_peer_libere(muet); }

    OwlTcpPoolConfig pc2 = pc;
    pc2.tls_handshake_timeout_ms = 5000;
    OwlTcpPool* p2 = owl_http_serveur_pool_demarre(&l, &cfg, &pc2);
    if (p2) {
        owl_tls_serveur_tours_boucle_raz();
        ClientTls c;
        if (ctls_ouvre(&c, owl_tcp_listener_port(&l))) {
            ctls_demande(&c, "/vivant");
            ctls_lit_tout(&c);
            ctls_ferme(&c);
        }
        attend_servies(p2, 1, 8000);
        size_t apres = owl_tls_serveur_tours_boucle();
        CHECK(apres >= 1, "t4_le_compteur_bouge_sur_un_echange_reel");
        fprintf(stderr, "  [t4] tours de boucle sur un echange complet = %u\n", (unsigned)apres);
        owl_tcp_pool_arrete(p2);
    } else {
        CHECK(0, "t4_pool_controle_demarre");
    }

    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * T5 — N connexions TLS simultanees, sans melange, parallelisme mesure
 * ================================================================ */

static void famille_t5(void) {
    g_famille = 4;
    enum { N = 4, W = 4 };
    g_handler_delai_ms = 60;

    OwlHttpServeurConfig cfg;
    config_serveur(&cfg);

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
        CHECK(0, "t5_ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = W;
    pc.connexions_max           = W;
    pc.tls                      = g_materiel;
    pc.tls_handshake_timeout_ms = 5000;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "t5_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    /* Handshakes SERIALISES (thread principal), puis N connexions
     * gardees ouvertes ensemble : c'est cette seconde phase qui mesure
     * le parallelisme cote serveur. */
    ClientTls cl[N];
    char chemin[N][32];
    int ouverts = 0;
    for (int i = 0; i < N; i++) {
        snprintf(chemin[i], sizeof(chemin[i]), "/n%d", i);
        if (ctls_ouvre(&cl[i], owl_tcp_listener_port(&l))) ouverts++;
    }
    CHECK(ouverts == N, "t5_les_quatre_handshakes_aboutissent");

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int emises = 0;
    for (int i = 0; i < N; i++) {
        if (cl[i].ouvert && ctls_demande(&cl[i], chemin[i])) emises++;
    }
    CHECK(emises == N, "t5_les_quatre_requetes_emises");

    for (int i = 0; i < N; i++) {
        if (cl[i].ouvert) ctls_lit_tout(&cl[i]);
    }
    uint64_t duree = owl_tcp_server_horloge_ms() - t0;

    size_t pic = owl_tcp_pool_pic_simultanees(pool);

    /* Non-melange : chaque connexion recoit SON echo, et aucune trace
     * d'un autre. Un tampon partage se verrait ici et nulle part
     * ailleurs. */
    int propres = 0, pollues = 0;
    for (int i = 0; i < N; i++) {
        char attendu[64];
        snprintf(attendu, sizeof(attendu), "M=GET|C=%s", chemin[i]);
        if (strstr(cl[i].recu, attendu)) propres++;
        for (int k = 0; k < N; k++) {
            if (k == i) continue;
            char autre[64];
            snprintf(autre, sizeof(autre), "M=GET|C=%s", chemin[k]);
            if (strstr(cl[i].recu, autre)) { pollues++; break; }
        }
    }
    CHECK(propres == N, "t5_chaque_connexion_recoit_son_echo_exact");
    CHECK(pollues == 0, "t5_aucune_reponse_ne_contient_celle_d_un_autre");

    /* Parallelisme MESURE : 4 services de 60 ms en sequentiel coutent au
     * moins 240 ms ; avec 4 workers, ~60 ms. Seuil a la moitie du
     * sequentiel pour rester insensible a la machine. */
    CHECK(duree < (uint64_t)(N * g_handler_delai_ms / 2),
          "t5_duree_tres_inferieure_au_service_sequentiel");
    CHECK(pic >= 2, "t5_au_moins_deux_connexions_chiffrees_en_vol");
    CHECK(pic <= (size_t)W, "t5_jamais_plus_que_le_plafond_en_vol");
    fprintf(stderr, "  [t5] duree=%ums pic=%u\n", (unsigned)duree, (unsigned)pic);

    for (int i = 0; i < N; i++) ctls_ferme(&cl[i]);
    attend_servies(pool, N, 8000);
    CHECK(owl_tcp_pool_handshakes_echoues(pool) == 0, "t5_aucun_handshake_echoue");

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    g_handler_delai_ms = 0;
}

/* ================================================================
 * T6 — le garde-fou de bind ne s'assouplit QUE si TLS est pose
 *
 * La signature d'owl_tcp_listener_ouvre ne change pas d'un octet — les
 * trois assertions d'A0.3 restent vertes sans etre touchees. Ce qui est
 * livre ici, c'est le SEUL point ou autoriser_non_loopback devient vrai,
 * et il le devient en fonction de la presence REELLE d'un materiel TLS.
 * ================================================================ */

static void famille_t6(void) {
    g_famille = 5;
    OwlTcpListener l;

    /* Sans materiel : le garde-fou tient, et il nomme sa raison. */
    bool sans = owl_tcp_listener_ouvre_tls(&l, "203.0.113.7", 0, 4, NULL);
    CHECK(sans == false, "t6_sans_tls_non_loopback_refuse");
    CHECK(strstr(owl_tcp_listener_erreur(&l), "non-loopback") != NULL,
          "t6_sans_tls_le_refus_nomme_la_raison");
    if (sans) owl_tcp_listener_ferme(&l);

    /* Avec materiel : le garde-fou laisse passer. Le bind echoue alors
     * pour une raison de PILE (adresse inexistante sur cette machine),
     * pas de politique — c'est l'absence du mot qui le prouve. */
    bool avec = owl_tcp_listener_ouvre_tls(&l, "203.0.113.7", 0, 4, g_materiel);
    CHECK(strstr(owl_tcp_listener_erreur(&l), "non-loopback") == NULL,
          "t6_avec_tls_le_garde_fou_est_franchi");
    if (avec) owl_tcp_listener_ferme(&l);

    /* Loopback : jamais refuse, avec ou sans materiel. */
    bool lo1 = owl_tcp_listener_ouvre_tls(&l, "127.0.0.1", 0, 4, NULL);
    CHECK(lo1 == true, "t6_loopback_autorise_sans_tls");
    if (lo1) owl_tcp_listener_ferme(&l);

    bool lo2 = owl_tcp_listener_ouvre_tls(&l, "127.0.0.1", 0, 4, g_materiel);
    CHECK(lo2 == true, "t6_loopback_autorise_avec_tls");
    if (lo2) owl_tcp_listener_ferme(&l);

    /* Un materiel invalide n'est PAS un materiel : la construction
     * echoue, donc l'appelant n'a rien a passer et le garde-fou reste
     * entier. Sans cette assertion, « TLS configure » pourrait se
     * reduire a « un pointeur non nul ». */
    char err[160] = {0};
    OwlTlsServeurMateriel* bidon =
        owl_tls_serveur_materiel_cree("pas un certificat", "pas une cle", err, sizeof(err));
    CHECK(bidon == NULL, "t6_materiel_invalide_refuse");
    CHECK(err[0] != '\0', "t6_materiel_invalide_explique_pourquoi");
    if (bidon) owl_tls_serveur_materiel_libere(bidon);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_tls_self_test_run(void) {
    printf("== TLS serveur (feat/1384, phase A1.2) ==\n");

    memset(g_pass, 0, sizeof(g_pass));
    memset(g_fail, 0, sizeof(g_fail));

    if (!genere_cert_autosigne()) {
        g_famille = 0;
        CHECK(0, "certificat_de_test_genere_en_memoire");
        printf("   TOTAL TLS serveur          : 0/1\n");
        return 1;
    }

    char err[256] = {0};
    g_materiel = owl_tls_serveur_materiel_cree(g_cert_pem, g_cle_pem, err, sizeof(err));
    if (!g_materiel) {
        g_famille = 0;
        fprintf(stderr, "  materiel TLS refuse : %s\n", err);
        CHECK(0, "materiel_tls_construit_depuis_le_pem_genere");
        printf("   TOTAL TLS serveur          : 0/1\n");
        return 1;
    }

    famille_t1();
    famille_t2();
    famille_t3();
    famille_t4();
    famille_t5();
    famille_t6();

    owl_tls_serveur_materiel_libere(g_materiel);
    g_materiel = NULL;

    int tp = 0, tf = 0;
    for (int i = 0; i < TT_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   t1 controle positif        : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   t2 controle negatif        : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   t3 echeance de handshake   : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   t4 absence de boucle       : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   t5 n connexions chiffrees  : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   t6 garde-fou de bind       : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   TOTAL TLS serveur          : %d/%d\n", tp, tp + tf);
    return tf;
}
