// SPDX-License-Identifier: AGPL-3.0-only
/* http_reacteur_etats_test.c — gates comportementaux du pivot W2
 * ( : machine a etats par connexion pilotee par le reacteur).
 *
 * Ticket : 
 * Design : note interne de concurrence du serveur HTTP (le pivot W2,
 *          risque de smuggling au reset d'etat, classes de bugs
 *          concurrence/ressource).
 *
 * DIGUE TEST-FIRST. Ces gates sont boite-noire, sur sockets loopback
 * REELLES, contre la frontiere reseau publique (owl_http_serveur_*,
 * OwlTcpPool, OwlHttpExecuteur) : aucune connaissance des etats internes
 * LECTURE_ENTETES/HANDLER_EN_ATTENTE/etc a venir n'est requise ici, et
 * aucune n'est utilisee.
 *
 * Trois familles (la famille back-pressure ecriture, faux vert
 * permanent en loopback Windows, est rattachee au lot stress
 *  via mutation-test, seul moyen fiable cross-OS) :
 *   A. Fast-path plan de controle : une route de controle (style
 *      /v1/models) ne doit pas attendre derriere une route lourde qui
 *      occupe le SEUL thread proprietaire de la VM (repro-A, §1 du
 *      design). RED aujourd'hui : les deux jobs partagent la MEME file
 *      mono-thread de http_exec.c, donc strictement serialises.
 *   C. Keep-alive + pipelining a travers le chemin executeur
 *      (handler_rep), sequentiel ET pipeline : doit rester/devenir vert
 *      a travers la reecriture (regression guard, pas une decouverte).
 *   D. Un vecteur cible sur un trou de couverture du corpus existant
 *      (http_server_corpus_test.c famille 6.1bis) : le drapeau
 *      « 100 Continue a emettre » remis a zero, mais en mode PIPELINE
 *      (un seul send(), le residu du meme tampon de lecture) plutot
 *      qu'en mode sequentiel (deux read() distincts, deja couvert par
 *      6.1bis_h). C'est precisement le mode ou §7.3 situe le risque.
 *      NE MODIFIE PAS le corpus existant : ajoute un vecteur nouveau.
 *
 * Execute via `owl --self-test-net` (cf. main.c). Pas de vm_init pour
 * les familles A/D (transport + executeur C pur, aucune VM) ; la
 * famille C non plus (l'executeur possede sa propre VM via son thread).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"
#include "../net_tcp.h"
#include "../net_tcp_server.h"

#ifdef _WIN32
  #include <windows.h>
  #include <ws2tcpip.h>
  typedef HANDLE we_thread_t;
  typedef SOCKET we_raw_fd_t;
  #define WE_INVALID_FD INVALID_SOCKET
  #define WE_THREAD_RET DWORD WINAPI
  #define WE_THREAD_0   0
  #define WE_SLEEP(ms)  Sleep((DWORD)(ms))
  #define WE_START(t, fn, arg) do { (t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL); } while (0)
  #define WE_JOIN(t)    do { if (t) { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } } while (0)
  #define WE_CLOSE(fd)  closesocket(fd)
#else
  #include <pthread.h>
  #include <time.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
  typedef pthread_t we_thread_t;
  typedef int we_raw_fd_t;
  #define WE_INVALID_FD (-1)
  #define WE_CLOSE(fd)  close(fd)
  #define WE_THREAD_RET void*
  #define WE_THREAD_0   NULL
  static void we_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define WE_SLEEP(ms)  we_sleep_ms((int)(ms))
  #define WE_START(t, fn, arg) do { if (pthread_create(&(t), NULL, (fn), (arg)) != 0) (t) = 0; } while (0)
  #define WE_JOIN(t)    do { if (t) pthread_join((t), NULL); } while (0)
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

enum { FAM_FASTPATH = 0, FAM_KEEPALIVE, FAM_SMUGGLING_GAP, FAM_NB };

static int g_pass[FAM_NB] = {0};
static int g_fail[FAM_NB] = {0};
static int g_famille = FAM_FASTPATH;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < FAM_NB) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL [%s] (ligne %d)\n", label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

static int we_compte_occurrences(const char* hay, const char* motif) {
    int n = 0;
    const char* p = hay;
    size_t ml = strlen(motif);
    while ((p = strstr(p, motif)) != NULL) { n++; p += ml; }
    return n;
}

/* ================================================================
 * Client mock minimal (net_mock_peer) — un chemin, une reponse.
 * ================================================================ */

typedef struct {
    OwlMockPeer* pair;
    char         requete[192];
} MClient;

static void mclient_prepare(MClient* c, const char* chemin) {
    memset(c, 0, sizeof(*c));
    snprintf(c->requete, sizeof(c->requete),
             "GET %s HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", chemin);
}

static void mclient_demarre(MClient* c, int port) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = port;
    mc.octets                     = c->requete;
    mc.octets_len                 = strlen(c->requete);
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = 15000;
    mc.fermer_apres_ecriture      = 1;
    c->pair = owl_mock_peer_demarre(&mc);
}

static void mclient_joint(MClient* c)  { if (c->pair) owl_mock_peer_joint(c->pair); }
static void mclient_libere(MClient* c) { if (c->pair) { owl_mock_peer_libere(c->pair); c->pair = NULL; } }
static const char* mclient_recu(const MClient* c) {
    if (!c->pair) return "";
    size_t l = 0;
    return owl_mock_peer_recu(c->pair, &l);
}

/* ================================================================
 * FAMILLE A — fast-path plan de controle (critere d'acceptation #4)
 *
 * Aujourd'hui, TOUTE requete passee a l'executeur (handler_rep) —
 * y compris une route de controle style /v1/models — traverse la MEME
 * file mono-thread : http_exec.c pose un seul thread proprietaire de la
 * VM qui traite les jobs un par un (owl_http_executeur_traite_une).
 * Design §1 : « /v1/models est enfile comme un job... serialise
 * derriere les generations... doublement puni ».
 *
 * RED aujourd'hui : la route de controle attend la fin du job lourd
 * deja en cours, quel que soit le nombre de workers reseau (le goulot
 * est la file de l'executeur, pas l'accept). VERT attendu apres W2 :
 * une route de controle repond sans attendre la fin d'un job en cours.
 * ================================================================ */

#define FA_LOURD_MS 700

static void producteur_fastpath(const OwlHttpRequete* req, OwlHttpProdCtx* pctx, void* user) {
    (void)user;
    const char* chemin = req->chemin ? req->chemin : "";
    OwlHttpRep* f;

    if (strcmp(chemin, "/v1/models") == 0) {
        static const char corps[] = "{\"object\":\"list\",\"data\":[]}";
        f = owl_http_prod_ouvre_flux(pctx, 200, "application/json", 1u << 16);
        if (f) {
            owl_http_rep_flux_pousse(f, corps, strlen(corps));
            owl_http_rep_flux_termine(f);
            owl_http_rep_flux_relache(f);
        }
        return;
    }

    /* Route lourde : occupe le seul thread VM pendant FA_LOURD_MS, comme
     * une generation LLM occupe l'executeur mono-thread (§1). */
    WE_SLEEP(FA_LOURD_MS);
    {
        static const char corps[] = "lourd-termine";
        f = owl_http_prod_ouvre_flux(pctx, 200, "text/plain", 1u << 16);
        if (f) {
            owl_http_rep_flux_pousse(f, corps, strlen(corps));
            owl_http_rep_flux_termine(f);
            owl_http_rep_flux_relache(f);
        }
    }
}

static void famille_fastpath(void) {
    g_famille = FAM_FASTPATH;

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.producteur      = producteur_fastpath;
    xc.producteur_user = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x || !owl_http_executeur_demarre(x)) {
        CHECK(0, "fastpath_executeur_demarre");
        if (x) owl_http_executeur_libere(x);
        return;
    }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "fastpath_ecoute_impossible");
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers = 2;   /* le goulot teste est la file EXEC, pas l'accept */

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) {
        CHECK(0, "fastpath_pool_demarre");
        owl_tcp_listener_ferme(&l);
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return;
    }

    int port = owl_tcp_listener_port(&l);

    /* La route lourde part en premier : elle entre dans la file EXEC
     * avant la route de controle. */
    MClient lourd;
    mclient_prepare(&lourd, "/lourd");
    mclient_demarre(&lourd, port);
    WE_SLEEP(150);   /* laisse le temps a la requete lourde d'etre deposee/prise */

    uint64_t t0 = owl_tcp_server_horloge_ms();
    MClient rapide;
    mclient_prepare(&rapide, "/v1/models");
    mclient_demarre(&rapide, port);
    mclient_joint(&rapide);
    uint64_t dt = owl_tcp_server_horloge_ms() - t0;

    CHECK(strstr(mclient_recu(&rapide), "\"object\":\"list\"") != NULL,
          "fastpath_v1_models_repond_correctement");

    /* Borne : tres en-dessous de FA_LOURD_MS. Le fast-path ne doit
     * jamais attendre la fin d'un job en cours dans l'executeur. */
    if (dt >= (uint64_t)(FA_LOURD_MS - 300)) {
        fprintf(stderr, "  [fastpath] /v1/models a attendu %ums (borne %dms)\n",
                (unsigned)dt, FA_LOURD_MS - 300);
    }
    CHECK(dt < (uint64_t)(FA_LOURD_MS - 300),
          "fastpath_v1_models_repond_sans_attendre_l_executeur");

    mclient_joint(&lourd);
    CHECK(strstr(mclient_recu(&lourd), "lourd-termine") != NULL,
          "fastpath_route_lourde_finit_aussi");

    owl_tcp_pool_arrete(pool);
    mclient_libere(&lourd);
    mclient_libere(&rapide);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE C — keep-alive + pipelining a travers un handler_rep OPAQUE
 * (critere d'acceptation #1). Regression guard : doit rester/devenir
 * vert a travers la reecriture de la boucle de service.
 *
 * Le corpus existant (http_server_corpus_test.c, famille 6) exerce le
 * keep-alive/pipelining avec un handler DIRECT (OwlHttpHandler). Cette
 * famille ajoute la combinaison non couverte : le handler OPAQUE
 * (OwlHttpHandlerRep, mode A « tampon » — owl_http_rep_cree_tampon).
 *
 * NOTE DE CONCEPTION (a documenter, pas un bug) : le mode C de
 * http_exec.c (flux/streaming via un producteur) FERME TOUJOURS la
 * connexion apres chaque reponse — http_server.c::ecrit_reponse_flux
 * l'annonce explicitement (« la longueur reste inconnue jusqu'au
 * dernier chunk »), C'EST INTENTIONNEL et hors du perimetre W2. Cette
 * famille utilise donc le mode A (tampon, longueur connue d'avance) —
 * pas d'executeur requis, owl_http_rep_cree_tampon est pur C-heap —
 * qui est la forme d'handler_rep compatible avec le keep-alive.
 * ================================================================ */

typedef struct {
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
    int    retour_serveur;
} EchangeSimple;

static OwlHttpRep* handler_rep_echo_tampon(const OwlHttpRequete* req, void* user) {
    (void)user;
    char corps[128];
    int n = snprintf(corps, sizeof(corps), "ECHO:%s", req->chemin ? req->chemin : "");
    if (n < 0) n = 0;
    return owl_http_rep_cree_tampon(200, "text/plain", corps, (size_t)n);
}

static EchangeSimple joue_avec_decoupe(const char* octets, size_t len,
                                       const OwlHttpServeurConfig* cfg,
                                       int decoupe_offset, size_t off0) {
    EchangeSimple e;
    memset(&e, 0, sizeof(e));

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) return e;

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = owl_tcp_listener_port(&l);
    mc.octets                     = octets;
    mc.octets_len                 = len;
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = 3000;
    mc.fermer_apres_ecriture      = 1;

    static size_t off[1];
    if (decoupe_offset) {
        off[0]         = off0;
        mc.decoupe     = OWL_MOCK_DECOUPE_OFFSETS;
        mc.offsets     = off;
        mc.offsets_len = 1;
        mc.delai_ms    = 120;
    }

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); return e; }

    e.retour_serveur = owl_http_serveur_sert_une_connexion(&l, cfg, 4000);

    owl_mock_peer_joint(pair);
    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(e.reponse, recu, rl);
    e.reponse[rl] = '\0';
    e.reponse_len = rl;

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);
    return e;
}

static void famille_keepalive_exec(void) {
    g_famille = FAM_KEEPALIVE;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_rep_echo_tampon;

    static char buf[512];
    const char* r1 = "GET /un HTTP/1.1\r\nHost: h\r\n\r\n";
    const char* r2 = "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n";
    size_t l1 = strlen(r1), l2 = strlen(r2);
    memcpy(buf, r1, l1);
    memcpy(buf + l1, r2, l2);

    /* ---- sequentiel : deux read() distincts ---- */
    {
        EchangeSimple e = joue_avec_decoupe(buf, l1 + l2, &cfg, 1, l1);
        int n200 = we_compte_occurrences(e.reponse, "HTTP/1.1 200");
        const char* p1 = strstr(e.reponse, "ECHO:/un");
        const char* p2 = strstr(e.reponse, "ECHO:/deux");
        CHECK(n200 == 2, "keepalive_exec_sequentiel_deux_200");
        CHECK(p1 != NULL && p2 != NULL && p1 < p2,
              "keepalive_exec_sequentiel_ordre_correct");
    }

    /* ---- pipeline : un seul send(), residu dans le meme tampon ---- */
    {
        EchangeSimple e = joue_avec_decoupe(buf, l1 + l2, &cfg, 0, 0);
        int n200 = we_compte_occurrences(e.reponse, "HTTP/1.1 200");
        const char* p1 = strstr(e.reponse, "ECHO:/un");
        const char* p2 = strstr(e.reponse, "ECHO:/deux");
        CHECK(n200 == 2, "keepalive_exec_pipeline_deux_200");
        CHECK(p1 != NULL && p2 != NULL && p1 < p2,
              "keepalive_exec_pipeline_ordre_correct");
    }
}

/* ================================================================
 * FAMILLE D — trou de couverture cible : drapeau « 100 Continue »
 * remis a zero en mode PIPELINE (critere d'acceptation #3, §7.3).
 *
 * Le corpus existant (http_server_corpus_test.c, 6.1bis_h) prouve deja
 * la remise a zero du drapeau en mode SEQUENTIEL (deux read() distincts,
 * via un delai de decoupage). Il ne prouve PAS le mode PIPELINE (un
 * seul send(), donc le residu vit dans le MEME tampon de lecture que la
 * requete avec Expect: 100-continue) — c'est precisement le mode que
 * §7.3 designe comme le plus a risque pour un reset d'etat imparfait.
 *
 * Ce vecteur NE MODIFIE PAS le corpus existant : il l'etend d'un
 * vecteur nouveau, dans ce fichier dedie au pivot W2.
 * ================================================================ */

static void handler_echo_transport(const OwlHttpRequete* req, OwlHttpReponse* rep, void* user) {
    (void)user;
    static char buf[128];
    int n = snprintf(buf, sizeof(buf), "M=%s|C=%s|TC=%u",
                     req->methode,
                     req->chemin ? req->chemin : "",
                     (unsigned)req->corps_len);
    if (n < 0) n = 0;
    rep->statut = 200;
    snprintf(rep->type_contenu, sizeof(rep->type_contenu), "text/plain");
    rep->corps     = buf;
    rep->corps_len = (size_t)n;
}

static void famille_smuggling_gap_pipeline_100_continue(void) {
    g_famille = FAM_SMUGGLING_GAP;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler = handler_echo_transport;

    static char buf[512];
    const char* r1 = "POST /un HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                     "Content-Length: 5\r\n\r\nhello";
    const char* r2 = "POST /deux HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
    size_t l1 = strlen(r1), l2 = strlen(r2);
    memcpy(buf, r1, l1);
    memcpy(buf + l1, r2, l2);

    /* decoupe_offset = 0 -> mode BLOC : les deux requetes tiennent dans
     * un seul send(), donc dans le meme tampon de lecture du serveur —
     * c'est le residu de pipelining, pas une simple succession. */
    EchangeSimple e = joue_avec_decoupe(buf, l1 + l2, &cfg, 0, 0);

    int nb_100 = we_compte_occurrences(e.reponse, "100 Continue");
    int nb_200 = we_compte_occurrences(e.reponse, "HTTP/1.1 200");
    CHECK(nb_100 == 1, "smuggling_gap_pipeline_100_continue_exactement_un");
    CHECK(nb_200 == 2, "smuggling_gap_pipeline_deux_200");

    const char* p1 = strstr(e.reponse, "M=POST|C=/un|TC=5");
    const char* p2 = strstr(e.reponse, "M=POST|C=/deux|TC=3");
    CHECK(p1 != NULL, "smuggling_gap_pipeline_premiere_exacte");
    CHECK(p2 != NULL, "smuggling_gap_pipeline_seconde_exacte_aucun_residu");
    CHECK(p1 != NULL && p2 != NULL && p1 < p2,
          "smuggling_gap_pipeline_ordre_correct");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_reacteur_etats_self_test_run(void) {
    printf("== Machine a etats service HTTP, pivot W2 (feat/1705) ==\n");

    famille_fastpath();
    famille_keepalive_exec();
    famille_smuggling_gap_pipeline_100_continue();

    int total_pass = 0, total_fail = 0;
    static const char* const noms[FAM_NB] = {
        "A fast-path controle",
        "C keep-alive/pipeline exec", "D trou pipeline 100-continue"
    };
    for (int i = 0; i < FAM_NB; i++) {
        printf("   %-30s : %d/%d\n", noms[i], g_pass[i], g_pass[i] + g_fail[i]);
        total_pass += g_pass[i];
        total_fail += g_fail[i];
    }
    printf("   TOTAL machine a etats W2    : %d/%d\n", total_pass, total_pass + total_fail);
    return total_fail;
}
