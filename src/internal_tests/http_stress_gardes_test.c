// SPDX-License-Identifier: AGPL-3.0-only
/* http_stress_gardes_test.c — digue concurrence/ressource.
 *
 * Design : note interne de concurrence du serveur HTTP
 *   (les 4 classes de bugs qui n'apparaissent que sous charge,
 *   chantier-test transversal, metriques structurelles W6).
 * Ticket : ,
 *          dont la note coordinator rattachant le back-pressure d'ecriture
 *          (ex-critere W2, ) a la famille 2 ci-dessous.
 *
 * ETAT DU MONDE au moment ou ce fichier est ecrit : le reacteur (W1,
 * net_reacteur.c), le reveil inter-thread (W4, net_reveil.c) et le TLS
 * pilote par la boucle (W3) existent deja comme PRIMITIVES ISOLEES, mais
 * NE SERVENT PAS ENCORE le trafic HTTP reel — c'est le pool worker-par-
 * connexion (net_tcp_server.c::OwlTcpPool) qui sert toujours (W2, la
 * machine a etats HTTP sur reacteur, est en pause : ). D'ou :
 *
 *   - Gate anti-hang + famine fd : testent le POOL ACTUEL, ROUGE
 *     attendu maintenant (ils REPRODUISENT le hang §1 du design), VERT
 *     une fois le reacteur sert le trafic (W2+).
 *   - Spin POLLOUT : teste la PRIMITIVE reacteur deja construite (W1),
 *     VERT maintenant de facon SIGNIFICATIVE (le mecanisme existe et
 *     tourne reellement sur socket loopback) — pas un faux vert, cf.
 *     recette de mutation-test en tete de chaque sous-famille.
 *   - UAF close-mid-stream + fuites fd/refcount : testent le socle
 *     streaming deja livre (, http_exec.c) sous stress reel ;
 *     resultat documente au fil du texte, pas suppose a l'avance.
 *
 * Discipline anti-figement (piege n°1 du ticket) : CHAQUE client est un
 * socket a echeance dure (SO_RCVTIMEO / mock peer timeout_lecture_ms /
 * boucle bornee en horloge murale). Aucun test ne peut pendre — au pire
 * il ECHOUE proprement en un temps borne, meme AVANT tout fix.
 *
 * Execute via `owl --self-test-net` (cf. main.c). */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"
#include "../net_reacteur.h"
#include "../vm.h"
#include "../gc.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE hg_thread_t;
  #define HG_THREAD_RET DWORD WINAPI
  #define HG_THREAD_0   0
  #define HG_SLEEP(ms)  Sleep((DWORD)(ms))
  #define HG_START(t, fn, arg) do { (t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL); } while (0)
  #define HG_JOIN(t)    do { if (t) { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } } while (0)
  #define HG_CLOSESOCK  closesocket
  static void hg_non_bloquant(owl_socket_fd_t fd) { u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t hg_thread_t;
  #define HG_THREAD_RET void*
  #define HG_THREAD_0   NULL
  static void hg_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define HG_SLEEP(ms)  hg_sleep_ms((int)(ms))
  #define HG_START(t, fn, arg) do { if (pthread_create(&(t), NULL, (fn), (arg)) != 0) (t) = 0; } while (0)
  #define HG_JOIN(t)    do { if (t) pthread_join((t), NULL); } while (0)
  #define HG_CLOSESOCK  close
  static void hg_non_bloquant(owl_socket_fd_t fd) {
      int fl = fcntl(fd, F_GETFL, 0);
      if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
#endif

extern uint64_t owl_tcp_server_horloge_ms(void);

/* ================================================================
 * Comptage — 5 familles : 0 famine, 1 spin POLLOUT, 2 UAF close,
 * 3 fuites fd/refcount, 4 gate anti-hang.
 * ================================================================ */

#define HG_NB_FAMILLES 5
static int g_pass[HG_NB_FAMILLES] = {0};
static int g_fail[HG_NB_FAMILLES] = {0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < HG_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL G%d [%s] (ligne %d)\n", f, label, ligne); }
}
#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Helpers partages : client mock (famine / gate / fuites-normal)
 * ================================================================ */

typedef struct {
    OwlMockPeer* pair;
    char         requete[192];
    char         attendu[192];
} Client;

static void client_prepare(Client* c, const char* chemin) {
    memset(c, 0, sizeof(*c));
    snprintf(c->requete, sizeof(c->requete), "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", chemin);
    snprintf(c->attendu, sizeof(c->attendu), "M=GET|C=%s", chemin);
}

/* garder_ms > 0 : le client garde sa connexion ouverte cote emission
 * apres la lecture (aucun usage ici, garde la signature homogene avec
 * les autres harnais du fichier voisin). timeout_lecture_ms borne la
 * lecture : c'est la garantie anti-figement (piege n°1). */
static void client_demarre(Client* c, int port, int timeout_lecture_ms) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = port;
    mc.octets                     = c->requete;
    mc.octets_len                 = strlen(c->requete);
    mc.decoupe                    = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = timeout_lecture_ms;
    mc.fermer_apres_ecriture      = 1;
    c->pair = owl_mock_peer_demarre(&mc);
}

static void client_joint(Client* c)   { if (c->pair) owl_mock_peer_joint(c->pair); }
static void client_libere(Client* c)  { if (c->pair) { owl_mock_peer_libere(c->pair); c->pair = NULL; } }
static const char* client_recu(const Client* c) {
    if (!c->pair) return "";
    size_t l = 0;
    return owl_mock_peer_recu(c->pair, &l);
}

/* ================================================================
 * FAMILLE — Gate anti-hang (l'etoile polaire, §8.bis point 2)
 *
 * Repro AUTOMATISE du hang serve-llm (§1 du design) : le pool couple
 * `accept` a un `sert()` qui peut bloquer longtemps. W connexions
 * "lourdes" (handler qui dort LONG_MS, simulant une inference/un flux
 * en cours) saturent les W workers ; un endpoint LEGER arrivant APRES
 * doit repondre. Avec le pool actuel, les workers ne relachent jamais
 * l'accept pendant qu'ils dorment : l'endpoint leger reste dans le
 * backlog, jamais accepte, jamais servi.
 *
 * ETAT ATTENDU MAINTENANT : ROUGE (reproduit le hang du pool). Cible :
 * VERT quand le reacteur (W2+) decouple l'accept du service — un
 * endpoint leger classe par le fast-path controle/donnees ne fait plus
 * jamais la queue derriere un handler lourd.
 *
 * Anti-figement : le client leger a un timeout de lecture BORNE
 * (LIGHT_TIMEOUT_MS) tres inferieur a LONG_MS. Il echoue proprement
 * (recu vide) au lieu de pendre. Les clients lourds finissent eux
 * aussi (LONG_MS borne), donc owl_tcp_pool_arrete() ne peut pas pendre.
 * ================================================================ */

static int  g_gate_long_ms = 0;

/* Deux chemins distincts, DIFFERENCIES par prefixe :
 *   /sante  -> reponse fixe rapide (endpoint de CONTROLE : c'est la route
 *              que le reacteur servira INLINE, sans worker, cf. archi
 *              figee  §8 Q2 — /v1/models, /health, /sante).
 *   /long   -> dort g_gate_long_ms puis renvoie un corps fixe (la charge
 *              lourde qui sature les workers, simule un traitement en
 *              cours cote DATA-PLANE).
 * Tout le reste (aucun chemin utilise ci-dessous) -> 404, pas de branche
 * d'echo : la famille famine a ete recadree pour discriminer sur des
 * connexions de CONTROLE (/sante) plutot que sur des connexions data-plane
 * qui, elles, ont TOUJOURS besoin d'un worker et ne discriminent jamais
 * pool vs reacteur (note coordinator ). */
static OwlHttpRep* handler_gate(const OwlHttpRequete* req, void* user) {
    (void)user;
    if (req->chemin && strcmp(req->chemin, "/sante") == 0) {
        return owl_http_rep_cree_tampon(200, "text/plain", "OK-SANTE", 8);
    }
    if (req->chemin && strcmp(req->chemin, "/long") == 0) {
        if (g_gate_long_ms > 0) HG_SLEEP(g_gate_long_ms);
        return owl_http_rep_cree_tampon(200, "text/plain", "M=GET|C=/long", 13);
    }
    return owl_http_rep_cree_tampon(404, "text/plain", "?", 1);
}

static void famille_gate_anti_hang(void) {
    g_famille = 4;
    enum { W = 4, LONG_MS = 3000, LIGHT_TIMEOUT_MS = 900 };
    g_gate_long_ms = LONG_MS;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_gate;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, W + 4, false)) {
        CHECK(0, "gate_ecoute_impossible");
        return;
    }
    int port = owl_tcp_listener_port(&l);

    /* Les W connexions lourdes forment une rafale, connectees AVANT le
     * demarrage du pool pour garantir qu'elles saisissent les W workers
     * des le premier accept. */
    Client lourd[W];
    for (int i = 0; i < W; i++) {
        client_prepare(&lourd[i], "/long");
        client_demarre(&lourd[i], port, LONG_MS + 2000);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "gate_pool_demarre");
    if (!pool) {
        for (int i = 0; i < W; i++) { client_joint(&lourd[i]); client_libere(&lourd[i]); }
        owl_tcp_listener_ferme(&l);
        return;
    }

    /* Laisse les W workers accepter les connexions lourdes et entrer
     * dans leur sommeil avant que l'endpoint leger n'arrive. */
    HG_SLEEP(250);

    Client leger;
    client_prepare(&leger, "/sante");
    client_demarre(&leger, port, LIGHT_TIMEOUT_MS);
    client_joint(&leger);   /* borne par LIGHT_TIMEOUT_MS : ne peut pas pendre */

    const char* recu = client_recu(&leger);
    CHECK(strstr(recu, "OK-SANTE") != NULL,
          "gate_endpoint_leger_repond_malgre_W_connexions_lourdes");
    if (!strstr(recu, "OK-SANTE")) {
        fprintf(stderr, "  [gate] recu_leger_len=%u (attendu OK-SANTE, hang reproduit)\n",
                (unsigned)strlen(recu));
    }
    client_libere(&leger);

    owl_tcp_pool_arrete(pool);   /* borne : les lourds finissent a LONG_MS */
    for (int i = 0; i < W; i++) { client_joint(&lourd[i]); client_libere(&lourd[i]); }
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * FAMILLE 0 — famine de fd / equite (a l'echelle du gate)
 *
 * Reformulation (, note coordinator) : la premiere version
 * routait K connexions DATA-PLANE (/r{i}) avec un timeout GENEREUX.
 * Diagnostic pose par le test_writer : une connexion data-plane a
 * TOUJOURS besoin d'un worker pour etre servie — meme sous un futur
 * reacteur, si les workers sont tous pris elle reste ROUGE tant qu'un
 * worker ne se libere pas. /r{i} ne discrimine donc JAMAIS pool vs
 * reacteur : le pool finit par drainer le backlog une fois les /long
 * finis, et la garde passe VERTE sous le pool actuel sans qu'aucun fix
 * n'ait ete fait — elle ne teste plus rien.
 *
 * Ce qui discrimine reellement (aligne sur le gate anti-hang, mais a
 * l'echelle de K connexions au lieu d'une seule) : une connexion de
 * CONTROLE (/sante — meme route que le gate, cf. archi figee 
 * §8 Q2 : /v1/models, /health, /sante servis INLINE par la boucle, SANS
 * worker) avec un timeout SERRE, tres inferieur au temps que les /long
 * occupent les workers.
 *
 *   - Charge lourde : W connexions /long (handler dort g_gate_long_ms
 *     = LONG_MS = 3000ms, occupant les W workers). Rafale finie, pas
 *     de charge continue necessaire.
 *   - Connexions de controle : K (= 24, >= 16) connexions /sante, avec
 *     un timeout SERRE LIGHT_TIMEOUT_MS = 900ms — TRES INFERIEUR aux
 *     3000ms des lourds (meme valeur que le gate anti-hang).
 *   - Assertion : au moins 3*K/4 des K connexions /sante recoivent
 *     "OK-SANTE" DANS le timeout serre de 900ms.
 *
 * ETAT ATTENDU MAINTENANT (pool actuel) : ROUGE. Les W workers dorment
 * 3000ms sur les /long ; /sante est servi par le MEME pool de workers
 * (pas de distinction controle/donnees cote pool actuel) donc aucune
 * connexion /sante n'obtient de worker en 900ms -> 0 servie.
 *
 * Cible sous reacteur (W2+) : VERT. /sante est une route de CONTROLE
 * servie INLINE sur la boucle reacteur, SANS worker ; la boucle
 * round-robine les K connexions sans qu'aucune ne monopolise (§8 Q2 :
 * « aucune connexion n'affame la boucle »). C'est le test d'equite §8
 * Q2 a l'echelle (K connexions de controle, symetrique au gate qui
 * n'en teste qu'une seule).
 * ================================================================ */

static void famille_famine_fd(void) {
    g_famille = 0;
    enum { W = 4, K = 24, LONG_MS = 3000, LIGHT_TIMEOUT_MS = 900 };
    g_gate_long_ms = LONG_MS;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_gate;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, W + K + 4, false)) {
        CHECK(0, "famine_ecoute_impossible");
        return;
    }
    int port = owl_tcp_listener_port(&l);

    Client lourd[W];
    for (int i = 0; i < W; i++) {
        client_prepare(&lourd[i], "/long");
        client_demarre(&lourd[i], port, LONG_MS + 2000);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "famine_pool_demarre");
    if (!pool) {
        for (int i = 0; i < W; i++) { client_joint(&lourd[i]); client_libere(&lourd[i]); }
        owl_tcp_listener_ferme(&l);
        return;
    }

    HG_SLEEP(250);   /* les W lourds saisissent les workers avant l'arrivee du controle */

    /* K connexions de CONTROLE (/sante), timeout SERRE : c'est ce qui
     * discrimine pool (partage les workers, /sante en attend un comme
     * n'importe quelle requete) vs reacteur (route inline, jamais
     * derriere les /long), cf. tete de fonction. */
    Client controle[K];
    for (int i = 0; i < K; i++) {
        client_prepare(&controle[i], "/sante");
        client_demarre(&controle[i], port, LIGHT_TIMEOUT_MS);
    }
    for (int i = 0; i < K; i++) client_joint(&controle[i]);   /* borne par LIGHT_TIMEOUT_MS chacun */

    int servies = 0;
    for (int i = 0; i < K; i++) {
        if (strstr(client_recu(&controle[i]), "OK-SANTE")) servies++;
    }
    CHECK(servies >= (3 * K) / 4,
          "famine_la_plupart_des_connexions_de_controle_servies_dans_le_timeout_serre");
    if (servies < (3 * K) / 4) {
        fprintf(stderr, "  [famine] servies=%d/%d (attendu >= %d) dans %dms\n",
                servies, K, (3 * K) / 4, (int)LIGHT_TIMEOUT_MS);
    }
    for (int i = 0; i < K; i++) client_libere(&controle[i]);

    owl_tcp_pool_arrete(pool);
    for (int i = 0; i < W; i++) { client_joint(&lourd[i]); client_libere(&lourd[i]); }
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * FAMILLE 1 — spin POLLOUT (+ sous-cas back-pressure, note coordinator)
 *
 * Teste directement la PRIMITIVE reacteur DEJA CONSTRUITE (W1,
 * net_reacteur.c) sur socket loopback reelle — pas le pool, pas W2.
 * C'est une primitive isolee (§ pas encore branchee au service HTTP),
 * mais c'est du code de production reel : un vert ici est SIGNIFICATIF
 * (le mecanisme tourne vraiment), pas un faux vert.
 *
 * Deux sous-scenarios :
 *   A. drain complet -> desenregistrement -> plus aucun dispatch
 *      superflu ensuite (pas de spin residuel).
 *   B. flux pace par un producteur plus lent que le consommateur
 *      (simule par des timers du reacteur lui-meme) : l'interet
 *      ECRITURE doit etre retire TANT QUE rien n'est disponible, puis
 *      re-arme des qu'un nouveau chunk arrive — jamais de dispatch
 *      "a vide". C'est le sous-cas back-pressure rattache depuis W2
 *      (note coordinator, 2026-08-19) : la piste boite-noire sur TCP
 *      loopback s'est averee un faux vert permanent sous Windows (pas
 *      de CI Linux ici) ; le mutation-test sur cette primitive est la
 *      voie fiable retenue a la place.
 *
 * RECETTE DE MUTATION-TEST (a appliquer par le reviewer/implementer,
 * PAS committee ici) : dans fd_cb_drain_complet / fd_cb_pace, commenter
 * l'appel a owl_reacteur_modifie_interet(r, fd, 0) qui retire l'interet
 * ECRITURE une fois qu'il n'y a plus rien a ecrire IMMEDIATEMENT.
 * Rebuild + relancer ce fichier seul : g_a_dispatch_a_vide /
 * g_b_appels_inutiles doivent devenir > 0 et l'assertion correspondante
 * DOIT rougir — sinon l'assertion ne discrimine pas et doit etre
 * renforcee (verifie manuellement par le test_writer au depot, cf.
 * rapport de ticket).
 * ================================================================ */

static int hg_make_pair(owl_socket_fd_t* pa, owl_socket_fd_t* pb) {
    owl_tcp_global_init();
    owl_socket_fd_t lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == OWL_INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(lst, (struct sockaddr*)&addr, sizeof(addr)) != 0) { HG_CLOSESOCK(lst); return -1; }
    socklen_t al = sizeof(addr);
    if (getsockname(lst, (struct sockaddr*)&addr, &al) != 0) { HG_CLOSESOCK(lst); return -1; }
    if (listen(lst, 1) != 0) { HG_CLOSESOCK(lst); return -1; }

    owl_socket_fd_t cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == OWL_INVALID_SOCKET) { HG_CLOSESOCK(lst); return -1; }
    if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        HG_CLOSESOCK(cli); HG_CLOSESOCK(lst); return -1;
    }
    owl_socket_fd_t srv = accept(lst, NULL, NULL);
    HG_CLOSESOCK(lst);
    if (srv == OWL_INVALID_SOCKET) { HG_CLOSESOCK(cli); return -1; }

    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    setsockopt(srv, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
    hg_non_bloquant(cli);
    hg_non_bloquant(srv);

    *pa = cli;
    *pb = srv;
    return 0;
}

/* ---- Sous-scenario A : drain complet, pas de spin residuel ---- */

typedef struct {
    owl_socket_fd_t fd;
    const char*     payload;
    size_t          total;
    size_t          envoye;
    int             retire_appele;
    int             dispatch_apres_retrait;   /* mesure de non-regression */
} EtatDrainA;

static void fd_cb_drain_complet(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    EtatDrainA* e = (EtatDrainA*)user;
    if (!(evts & OWL_REAC_ECRITURE)) return;
    if (e->retire_appele) { e->dispatch_apres_retrait++; return; }
    if (e->envoye < e->total) {
        int n = send(fd, e->payload + e->envoye, (int)(e->total - e->envoye), 0);
        if (n > 0) e->envoye += (size_t)n;
    }
    if (e->envoye >= e->total) {
        owl_reacteur_modifie_interet(r, fd, 0);   /* <- ligne cible du mutation-test A */
        e->retire_appele = 1;
    }
}

static void spin_a_drain_complet(void) {
    owl_socket_fd_t srv, cli;
    if (hg_make_pair(&srv, &cli) != 0) { CHECK(0, "spinA_paire_loopback"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    CHECK(r != NULL, "spinA_reacteur_cree");
    if (!r) { HG_CLOSESOCK(srv); HG_CLOSESOCK(cli); return; }

    EtatDrainA e; memset(&e, 0, sizeof(e));
    e.fd = srv; e.payload = "PAYLOAD-32-OCTETS-POUR-LE-DRAIN"; e.total = strlen(e.payload);

    CHECK(owl_reacteur_ajoute_fd(r, srv, OWL_REAC_ECRITURE, fd_cb_drain_complet, &e),
          "spinA_ajoute_fd_ecriture");

    uint64_t t0 = owl_tcp_server_horloge_ms();
    while (!e.retire_appele && owl_tcp_server_horloge_ms() - t0 < 2000) {
        owl_reacteur_tick(r, 100);
    }
    CHECK(e.retire_appele, "spinA_interet_retire_apres_drain_complet");

    /* Fenetre d'observation bornee : plus aucun dispatch ne doit
     * survenir (l'interet a ete retire). Le client (cli) ne lit jamais
     * les 32 octets : sans importance, ils tiennent dans le tampon
     * socket OS et n'empechent pas l'observation. */
    uint64_t t1 = owl_tcp_server_horloge_ms();
    int ticks = 0;
    while (owl_tcp_server_horloge_ms() - t1 < 300) {
        owl_reacteur_tick(r, 50);
        ticks++;
    }
    CHECK(e.dispatch_apres_retrait == 0,
          "spinA_aucun_dispatch_apres_retrait_interet");
    CHECK(ticks >= 4, "spinA_fenetre_observation_a_bien_tourne_plusieurs_ticks");
    if (e.dispatch_apres_retrait != 0) {
        fprintf(stderr, "  [spinA] dispatch_apres_retrait=%d (spin CPU probable)\n",
                e.dispatch_apres_retrait);
    }

    owl_reacteur_detruit(r);
    HG_CLOSESOCK(srv);
    HG_CLOSESOCK(cli);
}

/* ---- Sous-scenario B : producteur pace par timers (back-pressure) ---- */

typedef struct {
    OwlReacteur*    r;
    owl_socket_fd_t fd;
    int             chunk_dispo;
    int             chunks_envoyes;
    int             total_chunks;
    int             retraits;
    int             rearmements;
    int             termine;
    int             appels_inutiles;   /* dispatch ECRITURE sans rien a envoyer : bug */
} EtatPace;

static void fd_cb_pace(OwlReacteur* r, owl_socket_fd_t fd, short evts, void* user) {
    EtatPace* e = (EtatPace*)user;
    if (!(evts & OWL_REAC_ECRITURE)) return;
    if (!e->chunk_dispo) { e->appels_inutiles++; return; }

    char chunk[16]; memset(chunk, 'c', sizeof(chunk));
    send(fd, chunk, sizeof(chunk), 0);
    e->chunk_dispo = 0;
    e->chunks_envoyes++;

    if (e->chunks_envoyes >= e->total_chunks) {
        owl_reacteur_modifie_interet(r, fd, 0);   /* <- retrait final */
        e->termine = 1;
    } else {
        owl_reacteur_modifie_interet(r, fd, 0);   /* <- ligne cible du mutation-test B */
        e->retraits++;
    }
}

static void timer_cb_pace(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)id;
    EtatPace* e = (EtatPace*)user;
    if (e->termine) return;
    e->chunk_dispo = 1;
    e->rearmements++;
    owl_reacteur_modifie_interet(r, e->fd, OWL_REAC_ECRITURE);
}

static void spin_b_backpressure_pace(void) {
    owl_socket_fd_t srv, cli;
    if (hg_make_pair(&srv, &cli) != 0) { CHECK(0, "spinB_paire_loopback"); return; }

    OwlReacteur* r = owl_reacteur_cree();
    CHECK(r != NULL, "spinB_reacteur_cree");
    if (!r) { HG_CLOSESOCK(srv); HG_CLOSESOCK(cli); return; }

    EtatPace e; memset(&e, 0, sizeof(e));
    e.r = r; e.fd = srv; e.chunk_dispo = 1; e.total_chunks = 3;

    CHECK(owl_reacteur_ajoute_fd(r, srv, OWL_REAC_ECRITURE, fd_cb_pace, &e),
          "spinB_ajoute_fd_ecriture");
    /* Le producteur "pousse" un 2e puis un 3e chunk a +150ms/+300ms : le
     * reacteur doit rester silencieux entre les deux (interet retire),
     * pas re-tenter d'ecrire a vide. */
    owl_reacteur_arme_timer(r, 150, timer_cb_pace, &e);
    owl_reacteur_arme_timer(r, 300, timer_cb_pace, &e);

    uint64_t t0 = owl_tcp_server_horloge_ms();
    int tick_calls = 0;
    while (!e.termine && owl_tcp_server_horloge_ms() - t0 < 1500) {
        owl_reacteur_tick(r, 1000);   /* plafond genereux : bloque jusqu'au prochain evt/timer */
        tick_calls++;
    }

    CHECK(e.termine, "spinB_les_trois_chunks_livres_malgre_le_paceur");
    CHECK(e.retraits == 2, "spinB_interet_retire_a_chaque_creux_de_production");
    CHECK(e.rearmements == 2, "spinB_interet_rearme_a_chaque_nouveau_chunk");
    CHECK(e.appels_inutiles == 0, "spinB_aucun_dispatch_ecriture_sans_donnee");
    /* Bilan de spin : sous discipline correcte, une poignee de ticks
     * suffit (3 dispatchs fd + 2 timers). Un interet jamais retire
     * ferait dispatcher a chaque poll immediatement pret -> des
     * centaines/milliers de tick_calls dans la meme fenetre. */
    CHECK(tick_calls <= 20, "spinB_peu_de_ticks_donc_pas_de_boucle_cpu");
    if (!e.termine || e.appels_inutiles != 0 || tick_calls > 20) {
        fprintf(stderr, "  [spinB] termine=%d retraits=%d rearmements=%d"
                " appels_inutiles=%d tick_calls=%d\n",
                e.termine, e.retraits, e.rearmements, e.appels_inutiles, tick_calls);
    }

    owl_reacteur_detruit(r);
    HG_CLOSESOCK(srv);
    HG_CLOSESOCK(cli);
}

static void famille_spin_pollout(void) {
    g_famille = 1;
    spin_a_drain_complet();
    spin_b_backpressure_pace();
}

/* ================================================================
 * FAMILLE 2 — use-after-free / close mid-stream (le plus dur, §8 Q2)
 *
 * Le socle streaming (, http_exec.c) est deja livre : refcount
 * producteur+transport, `consommateur_fini`, handshake
 * `transport_detache`. Cette famille le stresse REELLEMENT : des
 * clients RAW (pas le mock peer) coupent leur connexion en pleine
 * reception d'un flux (RST via SO_LINGER{on,0}, pas une fermeture
 * propre), en rafales concurrentes, a repetition.
 *
 * ETAT ATTENDU MAINTENANT : ce mecanisme ne depend PAS du reacteur
 * (W2+) — il est deja en service aujourd'hui via le pool. Un vert ici
 * est donc un vert SIGNIFICATIF sur le code actuel. Le danger (§8 Q2)
 * est le heisenbug : un crash sous cette charge serait la preuve d'un
 * probleme reel de discipline refcount, a remonter IMMEDIATEMENT — pas
 * a masquer. Reduit a N=160 (au lieu de "milliers") pour le budget CI
 * de la batterie ; meme mecanisme, meme chemin de code.
 * ================================================================ */

static void producteur_uaf(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    if (req->chemin && strcmp(req->chemin, "/petit") == 0) {
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/plain", 1 << 12);
        if (!f) return;
        owl_http_rep_flux_pousse(f, "ENCORE-VIVANT", 13);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
        return;
    }
    /* /flux : pousse jusqu'a des milliers de chunks ou jusqu'a ce que le
     * consommateur (fermeture RST cote client) soit detecte ABANDON. */
    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "application/octet-stream", 1 << 16);
    if (!f) return;
    char chunk[64]; memset(chunk, 'u', sizeof(chunk));
    for (int i = 0; i < 8000; i++) {
        if (owl_http_rep_flux_pousse(f, chunk, sizeof(chunk)) == OWL_HTTP_FLUX_ABANDON) {
            owl_http_rep_flux_relache(f);
            return;
        }
    }
    owl_http_rep_flux_termine(f);
    owl_http_rep_flux_relache(f);
}

/* Client RAW : connecte, emet une requete valide, lit un peu, puis
 * COUPE brutalement (RST, pas de FIN propre) sans jamais tout lire.
 * Bornee : timeout de reception court, pas d'attente indefinie. */
static HG_THREAD_RET uaf_client_thread(void* arg) {
    int port = *(int*)arg;
    owl_socket_fd_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == OWL_INVALID_SOCKET) return HG_THREAD_0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) { HG_CLOSESOCK(s); return HG_THREAD_0; }

    const char* req = "GET /flux HTTP/1.1\r\nHost: h\r\n\r\n";
    send(s, req, (int)strlen(req), 0);

#ifdef _WIN32
    DWORD tv = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 300 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
    char buf[256];
    recv(s, buf, sizeof(buf), 0);   /* borne par le SO_RCVTIMEO ci-dessus */

    /* RST immediat : SO_LINGER {on, 0} fait sauter la fermeture propre. */
    struct linger lg; lg.l_onoff = 1; lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
    HG_CLOSESOCK(s);
    return HG_THREAD_0;
}

static void famille_uaf_close_mid_stream(void) {
    g_famille = 2;
    enum { W = 4, N_TOTAL = 160, BATCH = 8 };

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 8000;
    xc.producteur         = producteur_uaf;
    xc.producteur_user    = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    CHECK(x != NULL, "uaf_executeur_cree");
    if (!x) return;
    CHECK(owl_http_executeur_demarre(x), "uaf_executeur_demarre");

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, W + BATCH + 4, false)) {
        CHECK(0, "uaf_ecoute_impossible");
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return;
    }
    int port = owl_tcp_listener_port(&l);

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W + BATCH;   /* laisse passer une rafale sans 503 */

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "uaf_pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); owl_http_executeur_arrete(x); owl_http_executeur_libere(x); return; }

    int lots = N_TOTAL / BATCH;
    for (int b = 0; b < lots; b++) {
        hg_thread_t th[BATCH];
        int ports[BATCH];
        for (int i = 0; i < BATCH; i++) {
            ports[i] = port;
            HG_START(th[i], uaf_client_thread, &ports[i]);
        }
        for (int i = 0; i < BATCH; i++) HG_JOIN(th[i]);
    }

    /* Marqueur "ligne atteinte" legitime (note reviewer ) : un
     * vrai UAF/crash tuerait le process AVANT cette ligne (SIGSEGV/abort
     * sous les N_TOTAL fermetures brutales ci-dessus), donc il n'y a pas
     * de condition C a exprimer autrement qu'en constatant qu'on est
     * encore vivant ici. Le CHECK(1, ...) documente le point de passage,
     * pas une tautologie a eviter : sa valeur de preuve est le crash
     * process qu'il NE capture PAS lui-meme (le harness / la batterie
     * capte l'echec via le code de sortie, pas via ce booleen). */
    CHECK(1, "uaf_process_survit_a_N_fermetures_brutales_mi_flux");
    CHECK(owl_http_executeur_jobs_pris(x) >= 1, "uaf_au_moins_un_job_traite");

    /* Controle positif : l'executeur/pool servent encore normalement
     * apres le chaos — pas d'etat corrompu residuel. */
    Client controle;
    client_prepare(&controle, "/petit");
    client_demarre(&controle, port, 4000);
    client_joint(&controle);
    CHECK(strstr(client_recu(&controle), "ENCORE-VIVANT") != NULL,
          "uaf_controle_positif_encore_servi_apres_le_chaos");
    client_libere(&controle);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 3 — fuites fd / refcount (soak)
 *
 * N cycles de creation/destruction complete (listener + pool [+
 * executeur en flux]) : le nombre de handles systeme doit revenir a la
 * baseline, a une tolerance pres. Une fuite d'1 fd/cycle serait
 * noyee sous la tolerance a 1 cycle mais PAS sur M cycles — c'est
 * l'interet du soak.
 * ================================================================ */

#ifdef _WIN32
static size_t hg_compte_handles(void) {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return (size_t)n;
}
#else
#include <dirent.h>
static size_t hg_compte_handles(void) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    size_t n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) n++;
    closedir(d);
    return n;
}
#endif

static OwlHttpRep* handler_soak_echo(const OwlHttpRequete* req, void* user) {
    (void)user;
    char corps[64];
    int n = snprintf(corps, sizeof(corps), "M=%s|C=%s", req->methode, req->chemin ? req->chemin : "");
    if (n < 0) n = 0;
    return owl_http_rep_cree_tampon(200, "text/plain", corps, (size_t)n);
}

static void cycle_soak_normal(void) {
    enum { W = 2, K = 5 };
    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_soak_echo;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, K + 4, false)) return;
    int port = owl_tcp_listener_port(&l);

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    Client cl[K];
    char chemin[32];
    for (int i = 0; i < K; i++) {
        snprintf(chemin, sizeof(chemin), "/s%d", i);
        client_prepare(&cl[i], chemin);
        client_demarre(&cl[i], port, 3000);
    }
    for (int i = 0; i < K; i++) client_joint(&cl[i]);
    for (int i = 0; i < K; i++) client_libere(&cl[i]);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

static void cycle_soak_flux_aborte(void) {
    enum { W = 2, BATCH = 3 };
    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 4;
    xc.plafond_attente_ms = 4000;
    xc.producteur         = producteur_uaf;
    xc.producteur_user    = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) return;
    if (!owl_http_executeur_demarre(x)) { owl_http_executeur_libere(x); return; }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, W + BATCH + 4, false)) {
        owl_http_executeur_arrete(x); owl_http_executeur_libere(x); return;
    }
    int port = owl_tcp_listener_port(&l);

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W + BATCH;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (pool) {
        hg_thread_t th[BATCH];
        int ports[BATCH];
        for (int i = 0; i < BATCH; i++) { ports[i] = port; HG_START(th[i], uaf_client_thread, &ports[i]); }
        for (int i = 0; i < BATCH; i++) HG_JOIN(th[i]);
        owl_tcp_pool_arrete(pool);
    }
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

static void famille_fuites_fd_refcount(void) {
    g_famille = 3;
    enum { M_NORMAL = 20, M_FLUX = 20, TOLERANCE = 12 };

    HG_SLEEP(50);
    size_t base = hg_compte_handles();
    /* Pas d'assertion sur `base` seul : size_t non signe, "> 0 || == 0"
     * est vrai par construction (tautologie, note reviewer ).
     * `base == 0` reste possible (plateforme sans sonde de handles) —
     * gere plus bas via `if (base > 0)` avant chaque delta substantiel. */

    for (int i = 0; i < M_NORMAL; i++) cycle_soak_normal();
    HG_SLEEP(50);
    size_t apres_normal = hg_compte_handles();
    if (base > 0) {
        long delta = (long)apres_normal - (long)base;
        CHECK(delta <= TOLERANCE, "fuites_pas_de_derive_apres_cycles_normaux");
        if (delta > TOLERANCE) {
            fprintf(stderr, "  [fuites] normal: base=%u apres=%u delta=%ld (tol=%d)\n",
                    (unsigned)base, (unsigned)apres_normal, delta, TOLERANCE);
        }
    }

    for (int i = 0; i < M_FLUX; i++) cycle_soak_flux_aborte();
    HG_SLEEP(50);
    size_t apres_flux = hg_compte_handles();
    if (base > 0) {
        long delta2 = (long)apres_flux - (long)apres_normal;
        CHECK(delta2 <= TOLERANCE, "fuites_pas_de_derive_apres_cycles_flux_avortes");
        if (delta2 > TOLERANCE) {
            fprintf(stderr, "  [fuites] flux: apres_normal=%u apres_flux=%u delta=%ld (tol=%d)\n",
                    (unsigned)apres_normal, (unsigned)apres_flux, delta2, TOLERANCE);
        }
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_stress_gardes_self_test_run(void) {
    printf("== Gardes stress concurrence/ressource (feat/1709) ==\n"); fflush(stdout);

    famille_gate_anti_hang();
    famille_famine_fd();
    famille_spin_pollout();
    famille_uaf_close_mid_stream();
    famille_fuites_fd_refcount();

    int tp = 0, tf = 0;
    for (int i = 0; i < HG_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   gate anti-hang (etoile polaire) : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   famine fd / equite              : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   spin POLLOUT (+ back-pressure)  : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   UAF close mid-stream            : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   fuites fd/refcount (soak)       : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   TOTAL gardes stress             : %d/%d\n", tp, tp + tf);
    return tf;
}
