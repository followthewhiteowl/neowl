// SPDX-License-Identifier: AGPL-3.0-only
/* http_serve_test.c — `owl serve` : montage compile -> executeur -> listener
 * -> boucle, sur un VRAI port binde et un VRAI client socket.
 *
 * ROUGE ATTENDU EN L'ETAT : ce fichier N'EST PAS ENCORE CABLE dans
 * CMakeLists.txt ni dans le dispatch --self-test-http-serve de main.c
 * (perimetre test_writer : pas d'edition de src/main.c). Il documente et
 * verrouille par avance le contrat que l'implementeur doit fournir dans
 * src/owl_serve.{c,h} :
 *
 *   typedef struct {
 *       const char* bind_adr;   // NULL -> "127.0.0.1" (defaut surs)
 *       int         port;       // 0 -> port ephemere (comme owl_tcp_listener_ouvre)
 *   } OwlServeOptions;
 *
 *   typedef struct OwlServeMontage OwlServeMontage;
 *
 *   // Compile `source_owl`, exige EXACTEMENT un ServiceREST declare (0 ->
 *   // erreur claire dans err_out, retour NULL). Bind un listener REEL,
 *   // SYNCHRONE (le port/l'adresse sont connus des le retour). NULL sur
 *   // erreur (fichier sans service, port deja pris, ...) -- err_out rempli
 *   // d'un message non tronque-muet (au moins 1 octet + NUL si cap > 0).
 *   OwlServeMontage* owl_serve_prepare(const char* source_owl,
 *                                      const OwlServeOptions* opts,
 *                                      char* err_out, size_t err_out_cap);
 *
 *   int         owl_serve_montage_port(const OwlServeMontage* m);
 *   const char* owl_serve_montage_adresse(const OwlServeMontage* m);
 *
 *   // Demarre la boucle de service sur un thread dedie (meme idiome que
 *   // owl_http_executeur_demarre) : accepte, sert via l'executeur, repond,
 *   // jusqu'a owl_tcp_server_arret_demande() (Ctrl-C, mode production) OU
 *   // jusqu'a ce que `max_connexions` aient ete servies (mode test ; 0 =
 *   // illimite). `out` recoit le message de demarrage (NULL = silencieux).
 *   bool owl_serve_demarre(OwlServeMontage* m, int max_connexions, FILE* out);
 *
 *   // Bloque jusqu'a la fin du thread de boucle (max atteint / arret demande).
 *   void owl_serve_attend(OwlServeMontage* m);
 *
 *   void owl_serve_libere(OwlServeMontage* m);
 *
 * Cinq scenarios, TOUS sur un port reellement binde + un VRAI client TCP
 * (OwlMockPeer role CONNECTE -- pas un loopback in-process, un vrai socket
 * qui se connecte par le reseau local, cf. joue_e2e de http_dsl_service_test.c) :
 *
 *   0. GET /ping sur le port reellement binde -> 200 + corps "pong".
 *   1. GET /inconnu -> 404 (meme montage, meme connexion suivante).
 *   2. Defaut d'adresse : bind_adr == NULL -> le montage ecoute sur
 *      "127.0.0.1", jamais 0.0.0.0 (defaut sur, pas d'exposition par accident).
 *   3. Fichier sans ServiceREST -> erreur claire (retour NULL, message non
 *      vide), pas de crash.
 *   4. Port deja pris (un AUTRE listener bind le meme port avant) -> erreur
 *      claire (retour NULL, message non vide), pas de crash.
 *
 * Execution prevue : `owl --self-test-http-serve` (flag a cabler par
 * l'implementeur en meme temps que owl_serve.c).
 *
 * NOTE : src/owl_serve.c existe desormais et ce fichier EST
 * cable (CMakeLists.txt + dispatch --self-test-http-serve de main.c) --
 * le paragraphe ci-dessus documente l'etat de  au moment de sa
 * redaction, pas l'etat courant. Famille 7 ajoutee : isolation transport
 * (client lent a emettre vs client sain), cf. commentaire de
 * famille_7_isolation_transport_lent_vs_sain plus bas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../net_mock_peer.h"
#include "../net_tcp_server.h"
#include "../http_server.h"

/* Familles 5/6 -- tests de contrat du comportement idle/EOF du
 * serveur sur connexion keep-alive : threads cobayes + sockets client
 * manuelles, memes macros cross-platform que http_proxy_connect_test.c. */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <time.h>
#endif

/* ================================================================
 * Contrat attendu de l'implementeur -- cf. commentaire d'entete. Declare ici
 * (pas de nouveau header src/) : tant que src/owl_serve.c n'existe pas, ce
 * fichier compile (la declaration seule ne requiert aucun symbole), mais
 * n'est PAS cable dans CMakeLists.txt -- donc pas de lien casse pour les
 * autres chantiers en cours. Des que l'implementeur fournit owl_serve.c/h
 * et ajoute ce fichier a CMakeLists.txt, le lien exercera reellement le
 * contrat ci-dessous.
 * ================================================================ */

typedef struct {
    const char* bind_adr;
    int         port;
} OwlServeOptions;

typedef struct OwlServeMontage OwlServeMontage;

extern OwlServeMontage* owl_serve_prepare(const char* source_owl,
                                          const OwlServeOptions* opts,
                                          char* err_out, size_t err_out_cap);
extern int         owl_serve_montage_port(const OwlServeMontage* m);
extern const char* owl_serve_montage_adresse(const OwlServeMontage* m);
extern bool owl_serve_demarre(OwlServeMontage* m, int max_connexions, FILE* out);
extern void owl_serve_attend(OwlServeMontage* m);
extern void owl_serve_libere(OwlServeMontage* m);

/* ================================================================
 * Comptage
 * ================================================================ */

#define OWL_SERVE_NB_FAMILLES 8
static int g_pass[OWL_SERVE_NB_FAMILLES] = {0};
static int g_fail[OWL_SERVE_NB_FAMILLES] = {0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < OWL_SERVE_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Source OWL : un unique ServiceREST, une route GET /ping.
 * ================================================================ */

static const char* SRC_UN_SERVICE =
    "ServicePing est un ServiceREST\n"
    "    Pong est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ping\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"pong\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source valide OWL mais sans le moindre ServiceREST -- juste une structure
 * de donnees. Sert le cas "aucun service a servir". */
static const char* SRC_SANS_SERVICE =
    "stSansService est une Structure\n"
    "    nom est une chaine\n"
    "FIN\n";

/* ================================================================
 * Client TCP REEL : envoie `requete` sur 127.0.0.1:port et recupere la
 * reponse brute. OwlMockPeer en role CONNECTE ouvre un VRAI socket qui se
 * connecte au listener (aucun raccourci in-process) -- cf. joue_e2e dans
 * http_dsl_service_test.c, meme patron.
 * ================================================================ */

typedef struct {
    int    ok;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} ReponseClient;

static ReponseClient client_reel_envoie(const char* hote, int port, const char* requete) {
    ReponseClient r; memset(&r, 0, sizeof(r));

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = hote;
    mc.port                  = port;
    mc.octets                = requete;
    mc.octets_len            = strlen(requete);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 5000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) return r;

    owl_mock_peer_joint(pair);
    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(r.reponse, recu, rl);
    r.reponse[rl] = '\0';
    r.reponse_len = rl;
    r.ok = 1;

    owl_mock_peer_libere(pair);
    return r;
}

/* ================================================================
 * FAMILLE 0+1 -- bout en bout sur port reel : GET /ping puis GET /inconnu
 * sur le MEME montage (2 connexions, max_connexions = 2).
 * ================================================================ */

static void famille_0_ping_puis_inconnu(void) {
    g_famille = 0;

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = "127.0.0.1";
    opts.port     = 0;   /* ephemere : les tests ne fixent jamais un port en dur */

    OwlServeMontage* m = owl_serve_prepare(SRC_UN_SERVICE, &opts, err, sizeof(err));
    CHECK(m != NULL, "montage_prepare_ok");
    if (!m) return;

    int port = owl_serve_montage_port(m);
    CHECK(port > 0, "port_effectif_connu_avant_la_boucle");

    CHECK(owl_serve_demarre(m, 2, NULL), "boucle_demarree_2_connexions");

    ReponseClient r1 = client_reel_envoie("127.0.0.1", port,
        "GET /ping HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(r1.ok, "ping_echange_reussi");
    CHECK(strstr(r1.reponse, "HTTP/1.1 200") == r1.reponse, "ping_statut_200_sur_port_reel");
    CHECK(strstr(r1.reponse, "\r\n\r\npong") != NULL, "ping_corps_pong_sur_port_reel");

    g_famille = 1;
    ReponseClient r2 = client_reel_envoie("127.0.0.1", port,
        "GET /inconnu HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(r2.ok, "inconnu_echange_reussi");
    CHECK(strstr(r2.reponse, "HTTP/1.1 404") == r2.reponse, "inconnu_statut_404_sur_port_reel");

    owl_serve_attend(m);
    owl_serve_libere(m);
}

/* ================================================================
 * FAMILLE 2 -- defaut d'adresse : bind_adr NULL -> "127.0.0.1" (jamais
 * 0.0.0.0 par accident).
 * ================================================================ */

static void famille_2_defaut_localhost(void) {
    g_famille = 2;

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = NULL;   /* pas precise -> defaut sur attendu */
    opts.port     = 0;

    OwlServeMontage* m = owl_serve_prepare(SRC_UN_SERVICE, &opts, err, sizeof(err));
    CHECK(m != NULL, "montage_defaut_prepare_ok");
    if (!m) return;

    const char* adr = owl_serve_montage_adresse(m);
    CHECK(adr != NULL && strcmp(adr, "127.0.0.1") == 0, "defaut_bind_127_0_0_1");
    CHECK(adr == NULL || strcmp(adr, "0.0.0.0") != 0, "jamais_0_0_0_0_par_defaut");

    owl_serve_libere(m);
}

/* ================================================================
 * FAMILLE 3 -- fichier sans ServiceREST -> erreur claire, pas de crash.
 * ================================================================ */

static void famille_3_sans_service(void) {
    g_famille = 3;

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = "127.0.0.1";
    opts.port     = 0;

    OwlServeMontage* m = owl_serve_prepare(SRC_SANS_SERVICE, &opts, err, sizeof(err));
    CHECK(m == NULL, "sans_service_montage_refuse");
    CHECK(err[0] != '\0', "sans_service_message_erreur_non_vide");

    if (m) owl_serve_libere(m);   /* jamais atteint si le contrat tient -- filet */
}

/* ================================================================
 * FAMILLE 4 -- port deja pris par un AUTRE listener -> erreur claire.
 * ================================================================ */

static void famille_4_port_deja_pris(void) {
    g_famille = 4;

    /* Un listener temoin bind un port ephemere, on relit son port effectif,
     * puis on demande CE port EXACT au montage : collision garantie. */
    OwlTcpListener temoin;
    CHECK(owl_tcp_listener_ouvre(&temoin, "127.0.0.1", 0, 4, false),
          "listener_temoin_bind_ok");

    int port_pris = owl_tcp_listener_port(&temoin);

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = "127.0.0.1";
    opts.port     = port_pris;

    OwlServeMontage* m = owl_serve_prepare(SRC_UN_SERVICE, &opts, err, sizeof(err));
    CHECK(m == NULL, "port_pris_montage_refuse");
    CHECK(err[0] != '\0', "port_pris_message_erreur_non_vide");

    if (m) owl_serve_libere(m);
    owl_tcp_listener_ferme(&temoin);
}

/* ================================================================
 * FAMILLES 5/6 --  : tests de contrat du comportement idle/EOF
 * du serveur sur connexion keep-alive. CONTEXTE : le "hang" qui a motive
 *  n'etait PAS un bug runtime -- c'etait un artefact de harnais
 * (le patron joue_e2e ne signalait pas sa fin d'emission, d'ou un
 * standoff de lecture mutuel de 5 s par echange, cumule au-dela du
 * timeout CI). AUCUN changement runtime : keep-alive et la primitive
 * d'attente (WSAPoll/poll) sont corrects et inchanges. Ces familles
 * verrouillent le comportement serveur qui rend le fix de harnais valide :
 * sur un DEMI-CLOSE du pair (shutdown emission seule, lecture gardee
 * ouverte), le serveur detecte l'EOF sur sa relecture keep-alive et ferme
 * promptement ; sur un pair silencieux, il borne l'attente a
 * timeout_inactivite_ms.
 *
 * HYGIENE ANTI-HANG : AUCUN test ci-dessous ne doit pouvoir figer le
 * process. Chaque appel qui touche `owl_serve_attend` (join INFINITE en
 * interne, cf. owl_serve.c) ou `owl_http_serveur_sert_une_connexion` est
 * lance depuis un thread cobaye, rejoint avec un delai BORNE. Si la borne
 * expire, on CONSTATE (CHECK 0) et on abandonne le thread + les
 * ressources (fuite assumee -- preferable a figer la batterie).
 * ================================================================ */

#ifdef _WIN32
static DWORD WINAPI attente_montage_fn(LPVOID arg) {
    owl_serve_attend((OwlServeMontage*)arg);
    return 0;
}
#else
static void* attente_montage_fn(void* arg) {
    owl_serve_attend((OwlServeMontage*)arg);
    return NULL;
}
#endif

/* Rend 1 si `owl_serve_attend(m)` a rendu la main sous `borne_ms`, 0
 * sinon. Sur timeout, le thread et le montage sont VOLONTAIREMENT fuites
 * (cf. entete de section) : on ne libere jamais un montage auquel un
 * thread encore vivant pourrait toucher. */
static int attend_montage_borne(OwlServeMontage* m, int borne_ms) {
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, attente_montage_fn, m, 0, NULL);
    if (!t) return 0;
    DWORD r = WaitForSingleObject(t, (DWORD)borne_ms);
    if (r == WAIT_OBJECT_0) { CloseHandle(t); return 1; }
    return 0;
#else
    (void)borne_ms;  /* join non borne hors GNU/Linux (cf. commentaire ci-dessous) */
    pthread_t t;
    if (pthread_create(&t, NULL, attente_montage_fn, m) != 0) return 0;
    /* Pas de join borne portable simple hors GNU/Linux (pthread_timedjoin_np) ;
     * meme convention que px_joint dans http_proxy_connect_test.c. */
    pthread_join(t, NULL);
    return 1;
#endif
}

/* ================================================================
 * FAMILLE 5 -- contrat de detection d'EOF sur demi-close : un pair
 * keep-alive qui (a) envoie une requete HTTP/1.1, (b) LIT la reponse
 * complete, puis (c) fait un DEMI-CLOSE (shutdown de son sens emission
 * seulement -- il garde sa lecture ouverte, comme un vrai client qui a
 * fini d'emettre mais ecoute encore) doit voir le serveur : detecter
 * l'EOF sur sa relecture keep-alive, fermer PROPREMENT (pas de 2e
 * reponse, pas de rejet 400 "requete tronquee" -- cf.
 * http_server.c:1241-1252, `rien_recu` distingue EOF-entre-deux-requetes
 * d'une troncature), et rendre la main BORNEE (bien sous
 * timeout_inactivite_ms=5000ms par defaut). Client socket manuel (pas
 * OwlMockPeer : il faut lire la 1ere reponse PUIS shutdown(SD_SEND) sur
 * le meme socket -- deux phases que le mock ne permet pas). Cote client,
 * chaque recv() est borne par SO_RCVTIMEO, et cote serveur
 * `attend_montage_borne` borne l'attente : le test ne peut jamais figer
 * la batterie.
 * ================================================================ */

typedef struct {
    int  connecte;
    int  recu1_len;
    char recu1[2048];
    int  demiclose_ok;
    int  eof_detectee;   /* recv() a rendu 0 (EOF propre) apres le demi-close, sous le delai imparti */
    int  recu2_len;      /* octets recus apres le demi-close (attendu : 0) */
    char recu2[256];
} DemiCloseResultat;

static DemiCloseResultat client_demi_close_apres_reponse(const char* hote, int port,
                                                         int timeout_lecture_ms) {
    DemiCloseResultat r; memset(&r, 0, sizeof(r));

#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return r;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return r;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, hote, &addr.sin_addr);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return r;
    }
    r.connecte = 1;

#ifdef _WIN32
    DWORD tv = (DWORD)timeout_lecture_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_lecture_ms / 1000;
    tv.tv_usec = (timeout_lecture_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    static const char requete[] = "GET /ping HTTP/1.1\r\nHost: a\r\n\r\n";
    send(s, requete, (int)(sizeof(requete) - 1), 0);

    /* 1ere reponse : on lit jusqu'a reconnaitre le corps complet de la
     * fixture connue ("pong"), ou jusqu'a expiration de SO_RCVTIMEO
     * (borne, jamais indefiniment -- cote client, pas le shim serveur). */
    for (;;) {
        char tmp[512];
        int n = recv(s, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) break;
        size_t place = sizeof(r.recu1) - 1 - (size_t)r.recu1_len;
        size_t cp = (size_t)n < place ? (size_t)n : place;
        if (cp == 0) break;
        memcpy(r.recu1 + r.recu1_len, tmp, cp);
        r.recu1_len += (int)cp;
        if (strstr(r.recu1, "\r\n\r\npong") != NULL) break;
    }

    /* Demi-close : plus rien n'est emis par ce pair, mais sa lecture
     * reste ouverte -- le serveur doit voir l'EOF sur sa relecture
     * keep-alive et fermer (au lieu d'attendre une 2e requete). */
#ifdef _WIN32
    r.demiclose_ok = (shutdown(s, SD_SEND) == 0);
#else
    r.demiclose_ok = (shutdown(s, SHUT_WR) == 0);
#endif

    char tmp2[256];
    int n2 = recv(s, tmp2, (int)sizeof(tmp2), 0);   /* borne par SO_RCVTIMEO pose plus haut */
    if (n2 == 0) {
        r.eof_detectee = 1;
    } else if (n2 > 0) {
        size_t cp = (size_t)n2 < sizeof(r.recu2) - 1 ? (size_t)n2 : sizeof(r.recu2) - 1;
        memcpy(r.recu2, tmp2, cp);
        r.recu2_len = (int)cp;
    }
    /* n2 < 0 (timeout SO_RCVTIMEO) : ni EOF ni donnees -- le serveur
     * n'aurait pas ferme dans le delai imparti (regression a bannir). */

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return r;
}

static void famille_5_regression_demiclose_keepalive(void) {
    g_famille = 5;

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = "127.0.0.1";
    opts.port     = 0;

    OwlServeMontage* m = owl_serve_prepare(SRC_UN_SERVICE, &opts, err, sizeof(err));
    CHECK(m != NULL, "demiclose_montage_prepare_ok");
    if (!m) return;

    int port = owl_serve_montage_port(m);
    CHECK(owl_serve_demarre(m, 1, NULL), "demiclose_boucle_demarree_1_connexion");

    DemiCloseResultat r = client_demi_close_apres_reponse("127.0.0.1", port, 3000);
    CHECK(r.connecte, "demiclose_connexion_etablie");
    CHECK(r.recu1_len > 0 && strstr(r.recu1, "HTTP/1.1 200") == r.recu1,
          "demiclose_premiere_reponse_statut_200");
    CHECK(strstr(r.recu1, "pong") != NULL, "demiclose_premiere_reponse_corps_pong");
    CHECK(r.demiclose_ok, "demiclose_shutdown_emission_reussi");

    /* Le serveur doit detecter l'EOF du demi-close et fermer promptement
     * (au lieu d'attendre l'echeance d'inactivite). */
    CHECK(r.eof_detectee, "demiclose_serveur_detecte_eof_et_ferme");
    CHECK(r.recu2_len == 0, "demiclose_aucune_deuxieme_reponse");
    CHECK(strstr(r.recu2, "400") == NULL, "demiclose_pas_de_rejet_400_requete_tronquee");

    /* "Bien sous" timeout_inactivite_ms (5000 ms par defaut) : la
     * fermeture doit venir de la detection d'EOF, pas d'un eventuel
     * repli sur l'echeance d'inactivite. */
    if (attend_montage_borne(m, 4000)) {
        CHECK(1, "demiclose_boucle_rend_la_main_sous_4s");
        owl_serve_libere(m);
    } else {
        CHECK(0, "demiclose_boucle_rend_la_main_sous_4s");
    }
}

/* ================================================================
 * FAMILLE 6 -- CONTRAT ANTI-DOS (borne dure) : un pair qui se connecte
 * puis se TAIT completement (aucun octet, connexion gardee ouverte) ne
 * doit jamais figer `owl_http_serveur_sert_une_connexion` au-dela du
 * budget d'inactivite configure (5 s par defaut). Appel direct (pas
 * owl_serve) : un pair muet ne compte jamais comme "servie" au sens de
 * owl_serve.c::boucle_sert (r == 0), donc un max_connexions ne serait
 * jamais atteint -- on descend au niveau du contrat transport lui-meme.
 * ================================================================ */

typedef struct {
    OwlTcpListener*             l;
    const OwlHttpServeurConfig* cfg;
    int                         accept_timeout_ms;
    int                         statut;
} AppelServiConnexion;

#ifdef _WIN32
static DWORD WINAPI appel_sert_une_connexion_fn(LPVOID arg) {
    AppelServiConnexion* a = (AppelServiConnexion*)arg;
    a->statut = owl_http_serveur_sert_une_connexion(a->l, a->cfg, a->accept_timeout_ms);
    return 0;
}
#else
static void* appel_sert_une_connexion_fn(void* arg) {
    AppelServiConnexion* a = (AppelServiConnexion*)arg;
    a->statut = owl_http_serveur_sert_une_connexion(a->l, a->cfg, a->accept_timeout_ms);
    return NULL;
}
#endif

static void famille_6_anti_dos_pair_silencieux(void) {
    g_famille = 6;

    OwlTcpListener l;
    CHECK(owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 4, false), "antidos_listener_bind_ok");
    int port = owl_tcp_listener_port(&l);

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);   /* timeout_inactivite_ms = 5000 par defaut */

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = port;
    mc.octets                = "";
    mc.octets_len            = 0;
    mc.lire_apres_ecriture   = 0;
    mc.fermer_apres_ecriture = 0;
    mc.garder_ouvert_ms      = 6000;   /* > timeout_inactivite_ms : silence garanti sur
                                        * toute la fenetre de decision du serveur. */

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    CHECK(pair != NULL, "antidos_pair_silencieux_demarre");

    AppelServiConnexion a;
    memset(&a, 0, sizeof(a));
    a.l = &l;
    a.cfg = &cfg;
    a.accept_timeout_ms = 3000;

    int rendu = 0;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, appel_sert_une_connexion_fn, &a, 0, NULL);
    CHECK(t != NULL, "antidos_thread_appel_demarre");
    if (t) {
        DWORD wr = WaitForSingleObject(t, 20000);
        rendu = (wr == WAIT_OBJECT_0);
        CHECK(rendu, "antidos_sert_une_connexion_rend_la_main_sous_20s");
        CloseHandle(t);
    }
#else
    pthread_t t;
    int ok = pthread_create(&t, NULL, appel_sert_une_connexion_fn, &a) == 0;
    CHECK(ok, "antidos_thread_appel_demarre");
    if (ok) {
        pthread_join(t, NULL);
        rendu = 1;
        CHECK(rendu, "antidos_sert_une_connexion_rend_la_main_sous_20s");
    }
#endif

    if (rendu) {
        CHECK(a.statut == 0, "antidos_statut_0_connexion_morte_muette");
        owl_mock_peer_joint(pair);
        owl_mock_peer_libere(pair);
        owl_tcp_listener_ferme(&l);
    }
    /* Sur timeout : `l`, `cfg` et `pair` sont fuites volontairement (cf.
     * entete de section) -- un thread encore vivant pourrait les toucher. */
}

/* ================================================================
 * FAMILLE 7 -- ISOLATION TRANSPORT : un client LENT A EMETTRE
 * (transport, pas VM/handler) ne doit PAS empecher un client SAIN d'etre
 * servi PROMPTEMENT.
 *
 * Discriminant : `owl serve` sert aujourd'hui une connexion a la fois
 * (`boucle_sert`, owl_serve.c). Le budget d'entetes est une ECHEANCE
 * ABSOLUE (cf. http_server.c:1224-1257) : un pair qui envoie sa requete
 * OCTET PAR OCTET, avec une pause entre chaque octet, reste sous
 * timeout_entetes_ms (5000 ms par defaut) tant que le total reste sous ce
 * budget -- mais pendant toute cette duree, le thread mono-connexion est
 * OCCUPE a lire ce pair et ne peut PAS accepter la connexion suivante.
 * Un client sain qui se connecte pendant cette fenetre doit donc ATTENDRE
 * dans le backlog d'ecoute jusqu'a ce que le lent ait fini.
 *
 * Apres branchement sur le pool de transport (, decision Option
 * A), N threads de transport acceptent/lisent en parallele : le lent
 * occupe un worker, le sain en obtient un AUTRE immediatement -- sa
 * reponse arrive en quelques millisecondes, peu importe l'etat du lent.
 *
 * ROUGE ATTENDU AUJOURD'HUI : le sain attend la duree quasi-complete
 * d'emission du lent (~3 s), largement au-dessus du seuil de promptitude
 * (assertion `elapsed < 700 ms`) -- mais BORNE (le client sain a son
 * propre timeout de lecture, 5 s, largement suffisant pour observer une
 * reponse tardive plutot qu'un hang).
 * ================================================================ */

#ifdef _WIN32
static void isolation_dodo_ms(int ms) { Sleep((DWORD)ms); }
#else
static void isolation_dodo_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&ts, NULL);
}
#endif

/* Client "sain" a mesure precise : socket manuel (pas OwlMockPeer). La
 * raison : OwlMockPeer::mp_lit_reponse boucle jusqu'a EOF ou timeout AVANT
 * de rendre la main -- sur une connexion keep-alive (le defaut serveur),
 * le serveur ne ferme PAS apres la reponse, donc ce drain attendrait
 * bêtement le plein timeout_lecture_ms APRES avoir deja recu la reponse,
 * polluant toute mesure de promptitude avec une duree qui n'a rien a voir
 * avec le serveur. Ici on s'arrete au premier octet du corps "pong"
 * reconnu, et c'est CA qui borne `elapsed_ms`. */
typedef struct {
    int      connecte;
    int      reponse_complete;
    char     reponse[2048];
    int      reponse_len;
    uint64_t elapsed_ms;
} SainResultat;

static SainResultat client_sain_mesure(const char* hote, int port, int timeout_lecture_ms) {
    SainResultat r; memset(&r, 0, sizeof(r));

    uint64_t t0 = owl_tcp_server_horloge_ms();

#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return r;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return r;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, hote, &addr.sin_addr);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return r;
    }
    r.connecte = 1;

#ifdef _WIN32
    DWORD tv = (DWORD)timeout_lecture_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_lecture_ms / 1000;
    tv.tv_usec = (timeout_lecture_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    static const char requete[] = "GET /ping HTTP/1.1\r\nHost: a\r\n\r\n";
    send(s, requete, (int)(sizeof(requete) - 1), 0);

    for (;;) {
        char tmp[512];
        int n = recv(s, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) break;   /* EOF ou SO_RCVTIMEO -- borne, jamais indefini */
        size_t place = sizeof(r.reponse) - 1 - (size_t)r.reponse_len;
        size_t cp = (size_t)n < place ? (size_t)n : place;
        if (cp == 0) break;
        memcpy(r.reponse + r.reponse_len, tmp, cp);
        r.reponse_len += (int)cp;
        if (strstr(r.reponse, "\r\n\r\npong") != NULL) { r.reponse_complete = 1; break; }
    }

    r.elapsed_ms = owl_tcp_server_horloge_ms() - t0;

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return r;
}

static void famille_7_isolation_transport_lent_vs_sain(void) {
    g_famille = 7;

    char err[256] = {0};
    OwlServeOptions opts; memset(&opts, 0, sizeof(opts));
    opts.bind_adr = "127.0.0.1";
    opts.port     = 0;

    OwlServeMontage* m = owl_serve_prepare(SRC_UN_SERVICE, &opts, err, sizeof(err));
    CHECK(m != NULL, "isolation_montage_prepare_ok");
    if (!m) return;

    int port = owl_serve_montage_port(m);
    CHECK(owl_serve_demarre(m, 2, NULL), "isolation_boucle_demarree_2_connexions");

    /* Pair LENT (transport) : meme requete GET /ping, mais emise OCTET PAR
     * OCTET avec 100 ms entre chaque octet -- 31 octets => ~3 s de saisie,
     * bien sous le budget absolu de 5 s (timeout_entetes_ms), donc une
     * requete VALIDE (pas un rejet 408), qui occupe reellement le service
     * mono-connexion pendant toute cette duree. */
    static const char requete_ping[] = "GET /ping HTTP/1.1\r\nHost: a\r\n\r\n";

    OwlMockPeerConfig mc_lent;
    owl_mock_peer_config_defaut(&mc_lent);
    mc_lent.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc_lent.hote                  = "127.0.0.1";
    mc_lent.port                  = port;
    mc_lent.octets                = requete_ping;
    mc_lent.octets_len            = sizeof(requete_ping) - 1;
    mc_lent.decoupe                = OWL_MOCK_DECOUPE_OCTET;
    mc_lent.delai_ms              = 100;
    mc_lent.lire_apres_ecriture   = 1;
    /* Court : la connexion reste keep-alive cote serveur (pas de fermeture
     * spontanee apres la reponse), donc le pair lent attendrait sinon la
     * pleine echeance sans raison -- 1 s suffit a lire la reponse puis a
     * abandonner l'attente d'une 2e. Aucun impact sur la mesure (le pair
     * lent n'est joint qu'APRES la mesure du client sain). */
    mc_lent.timeout_lecture_ms    = 1000;
    mc_lent.fermer_apres_ecriture = 1;

    /* owl_mock_peer_demarre(role CONNECTE) ne rend la main qu'APRES connect()
     * (cf. net_mock_peer.c::mp_boucle_connecte) : au retour, le pair lent est
     * deja dans la file d'ecoute du serveur, et son emission paced tourne
     * deja sur son propre thread. */
    OwlMockPeer* pair_lent = owl_mock_peer_demarre(&mc_lent);
    CHECK(pair_lent != NULL, "isolation_pair_lent_connecte");

    /* Marge fixe (pas une boucle d'attente) : garantit que le lent a bien
     * ete accepte AVANT le sain (ordre FIFO de la file d'ecoute), sans quoi
     * le test discriminerait au hasard. */
    isolation_dodo_ms(80);

    /* Borne large (5 s) : couvre confortablement le pire cas mono-connexion
     * (~3 s d'occupation par le lent) sans jamais risquer un hang -- c'est
     * un timeout de LECTURE cote client, pas la mesure de promptitude. */
    SainResultat sain = client_sain_mesure("127.0.0.1", port, 5000);

    CHECK(sain.connecte, "isolation_client_sain_connecte");
    CHECK(strstr(sain.reponse, "HTTP/1.1 200") == sain.reponse,
          "isolation_client_sain_statut_200");
    CHECK(sain.reponse_complete, "isolation_client_sain_corps_pong");

    /* LE DISCRIMINANT : le sain doit etre servi PROMPTEMENT,
     * meme pendant que le lent occupe encore le transport. ROUGE aujourd'hui
     * (mono-connexion : le sain attend la quasi-totalite des ~3 s du lent),
     * VERT une fois le pool de transport branche (un autre worker le sert
     * tout de suite). */
    fprintf(stderr, "  [F7] isolation: client sain servi en %llu ms\n",
            (unsigned long long)sain.elapsed_ms);
    CHECK(sain.elapsed_ms < 700, "isolation_client_sain_repond_promptement_sous_700ms");

    owl_mock_peer_joint(pair_lent);
    owl_mock_peer_libere(pair_lent);

    owl_serve_attend(m);
    owl_serve_libere(m);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_serve_self_test_run(void) {
    printf("== owl serve -- montage compile->executeur->listener->boucle (feat/1900) ==\n");

    famille_0_ping_puis_inconnu();
    famille_2_defaut_localhost();
    famille_3_sans_service();
    famille_4_port_deja_pris();
    famille_5_regression_demiclose_keepalive();
    famille_6_anti_dos_pair_silencieux();
    famille_7_isolation_transport_lent_vs_sain();

    int tp = 0, tf = 0;
    for (int i = 0; i < OWL_SERVE_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 ping port reel            : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 inconnu 404 port reel      : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 defaut localhost           : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 sans ServiceREST           : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 port deja pris             : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 regression demi-close keep-alive : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 anti-DoS pair silencieux   : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 isolation transport lent/sain (feat/1950) : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   TOTAL owl serve               : %d/%d\n", tp, tp + tf);
    return tf;
}
