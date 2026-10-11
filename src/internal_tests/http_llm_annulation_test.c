// SPDX-License-Identifier: AGPL-3.0-only
/* http_llm_annulation_test.c — annulation de generation a la deconnexion
 * client.
 *
 * Ticket : 
 *
 * CONSTAT TESTEUR (verbatim, chantier B serve-llm) : un client qui se
 * deconnecte en pleine generation ne coupe PAS le producteur — la
 * generation continue jusqu'a ..MaxTokens et occupe l'executeur
 * mono-thread. Mesure reelle : apres 6 generations abandonnees, une
 * petite generation (5 jetons) met 175 s au lieu de ~30 s a vide.
 *
 * RE-CADRAGE 2026-08-22 (reponse testeur) : OpenCode RACCROCHE — il ne
 * "s'arrete de lire sans fermer" jamais. Pool keep-alive : requetes
 * finies/errees -> FIN propre (fermeture cote client) ; abandon dur
 * (kill/Ctrl-C/timeout harness) -> RST. L'abandon REEL qui produit les
 * zombies decrits par le testeur est un abandon DUR (kill/Ctrl-C/timeout),
 * donc un RST cote TCP, pas une fermeture FIN propre.
 *
 * RE-CADRAGE 2026-08-22 bis (mesure loopback) : sur loopback Windows, un
 * close() propre (FIN) est INDISTINGUABLE d'un half-close cote serveur —
 * POLLHUP + recv()==0 sont les memes evenements dans les deux cas, aucun
 * signal fiable ne permet de les discriminer. Le cas FIN-propre n'est donc
 * pas testable physiquement sur cette plateforme. Ce fichier fait fermer
 * le client de test en RST (SO_LINGER{on,0}, meme pattern que
 * http_llm_annulation_crash_test.c) : c'est a la fois le seul signal
 * detectable de maniere fiable ET le motif de l'abandon reel qui cree les
 * zombies (kill/Ctrl-C/timeout -> RST, confirme testeur).
 *
 * CE QUI RESTE MANQUANT (constat re-cadrage, priorite 2 du ticket) :
 * la chaine de detection existe (owl_http_rep_flux_pousse rend ABANDON
 * a l'echec d'ecriture, cf. http_server.c) et est deja verifiee PROMPTE
 * sur le chemin FLUX DIRECT (producteur de test / handler_rep bloquant,
 * cf. famille UAF close-mid-stream de http_stress_gardes_test.c,
 * ). Mais la production reelle (serve_llm.c:1173) sert le
 * data-plane via cfg.executeur_async (chemin W7 : le reacteur possede la
 * connexion, le producteur tourne dans le thread executeur, la
 * resolution passe par owl_http_executeur_depose/essaie) — un chemin
 * DIFFERENT que la garde UAF ne couvre PAS. Rien aujourd'hui ne prouve
 * que la fermeture cliente propage l'ABANDON jusqu'au producteur sur CE
 * chemin precis. Ce fichier le monte donc EXACTEMENT comme serve_llm.c
 * (cfg.executeur_async pose, cf. montage_demarre ci-dessous), la ou la
 * version precedente de ce test montait le producteur en handler_rep
 * bloquant simple (chemin different de la production, gap de couverture
 * corrige ici).
 *
 * DISCIPLINE ANTI-FIGEMENT (piege n°1) : chaque generation simulee est
 * bornee par un nombre de chunks fixe (GEN_MAX_CHUNKS) ET une cadence
 * fixe (GEN_PACE_MS) -> duree maximale connue a l'avance, jamais
 * d'attente indefinie MEME SANS FIX (le producteur epuise sa borne haute
 * et rend la main). Le client ferme sa socket au bout d'un temps fixe,
 * puis chaque boucle d'attente cote test est bornee dans le temps.
 * Execute via `owl --self-test-net` (cf. main.c).
 */

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

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE la_thread_t;
  #define LA_THREAD_RET DWORD WINAPI
  #define LA_THREAD_0   0
  #define LA_START(t, fn, arg) do { (t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL); } while (0)
  #define LA_JOIN(t)    do { if (t) { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } } while (0)
  #define LA_SLEEP(ms)  Sleep((DWORD)(ms))
  #define LA_CLOSESOCK  closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t la_thread_t;
  #define LA_THREAD_RET void*
  #define LA_THREAD_0   NULL
  #define LA_START(t, fn, arg) do { if (pthread_create(&(t), NULL, (fn), (arg)) != 0) (t) = 0; } while (0)
  #define LA_JOIN(t)    do { if (t) pthread_join((t), NULL); } while (0)
  static void la_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define LA_SLEEP(ms)  la_sleep_ms((int)(ms))
  #define LA_CLOSESOCK  close
#endif

extern uint64_t owl_tcp_server_horloge_ms(void);

/* ================================================================
 * Comptage — 2 familles : 0 promptitude (discriminant), 1 pas de
 * spirale (repro inversee du constat testeur : N fermetures ne
 * degradent pas la requete suivante).
 * ================================================================ */

enum { FAM_PROMPTITUDE = 0, FAM_PAS_DE_SPIRALE, FAM_NB };

static int g_pass[FAM_NB] = {0};
static int g_fail[FAM_NB] = {0};
static int g_famille = FAM_PROMPTITUDE;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < FAM_NB) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL [%s] (ligne %d)\n", label, ligne); }
}
#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Producteur de test : simule une generation LLM PACEE (un "jeton"
 * toutes les GEN_PACE_MS), sans le moindre GGUF. Chemin /generation.
 * /petit : generation courte (2 jetons) qui sert de sonde de latence
 * de l'executeur mono-thread apres une fermeture.
 * ================================================================ */

#define GEN_MAX_CHUNKS 250   /* borne haute simulant ..MaxTokens */
#define GEN_PACE_MS    20    /* cadence simulee : ~5 s si jamais coupe */

/* Dernier resultat de generation observe (l'executeur est mono-thread :
 * un seul producteur tourne a la fois, pas de concurrence a proteger). */
static volatile long g_dernier_chunks_pousses = -1;
static volatile long g_dernier_stoppe_par_abandon = 0;

static void producteur_llm_paced(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    const char* chemin = req->chemin ? req->chemin : "";

    if (strcmp(chemin, "/petit") == 0) {
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1 << 16);
        if (!f) return;
        owl_http_rep_flux_pousse(f, "data: tok0\n\n", 12);
        owl_http_rep_flux_pousse(f, "data: [DONE]\n\n", 14);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
        return;
    }

    /* /generation : pousse jusqu'a GEN_MAX_CHUNKS jetons paces, ou
     * jusqu'a ABANDON si le consommateur est detecte parti. */
    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1 << 20);
    if (!f) return;

    char buf[40];
    int  pousses  = 0;
    int  abandon  = 0;
    for (int i = 0; i < GEN_MAX_CHUNKS; i++) {
        int n = snprintf(buf, sizeof(buf), "data: tok%d\n\n", i);
        if (n < 0) n = 0;
        if (owl_http_rep_flux_pousse(f, buf, (size_t)n) == OWL_HTTP_FLUX_ABANDON) {
            abandon = 1;
            break;
        }
        pousses++;
        LA_SLEEP(GEN_PACE_MS);
    }
    g_dernier_chunks_pousses      = pousses;
    g_dernier_stoppe_par_abandon  = abandon;

    if (!abandon) {
        owl_http_rep_flux_termine(f);
    }
    owl_http_rep_flux_relache(f);
}

/* ================================================================
 * Client RAW : connecte, envoie la requete, lit UN chunk (le temps de
 * savoir que la generation a bien demarre), puis COUPE EN RST
 * (SO_LINGER{on,0} avant closesocket, meme pattern que
 * http_llm_annulation_crash_test.c) — motif de l'abandon DUR reel
 * (kill/Ctrl-C/timeout du pool cote OpenCode, confirme testeur
 * 2026-08-22). Le cas FIN-propre n'est PAS teste ici : sur loopback
 * Windows il est indistinguable d'un half-close cote serveur (POLLHUP +
 * recv()==0 dans les deux cas), donc non discriminant / non testable.
 * Borne par SO_RCVTIMEO sur la seule lecture faite ici : ne peut pas
 * pendre.
 * ================================================================ */

#ifdef _WIN32
  typedef SOCKET la_sock_t;
  #define LA_INVALID INVALID_SOCKET
#else
  typedef int la_sock_t;
  #define LA_INVALID (-1)
#endif

typedef struct {
    int port;
} FermetureArg;

static LA_THREAD_RET la_client_ferme_run(void* arg) {
    FermetureArg* fa = (FermetureArg*)arg;
    int port = fa->port;

    la_sock_t s = (la_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (s == LA_INVALID) return LA_THREAD_0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        LA_CLOSESOCK(s); return LA_THREAD_0;
    }

    const char* req = "GET /generation HTTP/1.1\r\nHost: h\r\n\r\n";
    send(s, req, (int)strlen(req), 0);

#ifdef _WIN32
    DWORD tv = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
    /* Une seule lecture, bornee : juste de quoi savoir que la generation
     * a demarre (en-tete + 1er chunk). Puis fermeture PROPRE immediate —
     * le motif "raccroche" reel, pas un maintien de socket morte. */
    char buf[512];
    recv(s, buf, sizeof(buf), 0);

    /* RST immediat : SO_LINGER {on, 0} fait sauter la fermeture propre —
     * seul signal detectable de maniere fiable sur loopback, et motif de
     * l'abandon dur reel (kill/Ctrl-C/timeout) confirme par le testeur. */
    struct linger lg; lg.l_onoff = 1; lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
    LA_CLOSESOCK(s);
    return LA_THREAD_0;
}

/* ================================================================
 * Client de sonde (mock peer) : requete courte, mesure le temps de
 * bout en bout. Sert a prouver que l'executeur s'est bien libere.
 * ================================================================ */

typedef struct {
    OwlMockPeer* pair;
    char         requete[96];
} Sonde;

static void sonde_prepare(Sonde* c, const char* chemin) {
    memset(c, 0, sizeof(*c));
    snprintf(c->requete, sizeof(c->requete), "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", chemin);
}

static void sonde_demarre(Sonde* c, int port, int timeout_lecture_ms) {
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

static void sonde_joint(Sonde* c)  { if (c->pair) owl_mock_peer_joint(c->pair); }
static void sonde_libere(Sonde* c) { if (c->pair) { owl_mock_peer_libere(c->pair); c->pair = NULL; } }
static const char* sonde_recu(const Sonde* c) {
    if (!c->pair) return "";
    size_t l = 0;
    return owl_mock_peer_recu(c->pair, &l);
}

/* ================================================================
 * Montage commun executeur + pool + reacteur, chemin de PRODUCTION reel
 * (cfg.executeur_async pose, exactement comme serve_llm.c:1173 — le
 * data-plane est servi par le reacteur/executeur asynchrone W7, pas par
 * un handler_rep bloquant. C'est le chemin que la garde UAF 
 * ne couvre pas, cf. commentaire de tete du fichier).
 * ================================================================ */

typedef struct {
    OwlHttpExecuteur*    x;
    OwlTcpListener       l;
    OwlTcpPool*          pool;
    int                  port;
    /* Le pool ne COPIE pas la config, il en garde le pointeur (meme
     * contrat que tous les harnais voisins, qui declarent cfg dans la
     * fonction qui garde le pool en vie) : elle doit donc survivre a la
     * duree du montage, pas etre une locale de montage_demarre(). */
    OwlHttpServeurConfig cfg;
} Montage;

static int montage_demarre(Montage* m) {
    memset(m, 0, sizeof(*m));

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 4;
    xc.plafond_attente_ms = 20000;   /* neutralise des qu'un job flux a demarre (D1) */
    xc.producteur         = producteur_llm_paced;
    xc.producteur_user    = NULL;

    m->x = owl_http_executeur_cree(&xc);
    if (!m->x) return 0;
    if (!owl_http_executeur_demarre(m->x)) { owl_http_executeur_libere(m->x); m->x = NULL; return 0; }

    if (!owl_tcp_listener_ouvre(&m->l, "127.0.0.1", 0, 16, false)) {
        owl_http_executeur_arrete(m->x);
        owl_http_executeur_libere(m->x);
        m->x = NULL;
        return 0;
    }
    m->port = owl_tcp_listener_port(&m->l);

    owl_http_serveur_config_defaut(&m->cfg);
    m->cfg.handler_rep      = owl_http_exec_handler_rep;
    m->cfg.handler_rep_user = m->x;
    m->cfg.executeur_async  = m->x;   /* chemin reel : data-plane via le reacteur (W7) */

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = 2;
    pc.connexions_max = 8;

    m->pool = owl_http_serveur_pool_demarre(&m->l, &m->cfg, &pc);
    if (!m->pool) {
        owl_tcp_listener_ferme(&m->l);
        owl_http_executeur_arrete(m->x);
        owl_http_executeur_libere(m->x);
        m->x = NULL;
        return 0;
    }
    return 1;
}

static void montage_arrete(Montage* m) {
    if (m->pool) owl_tcp_pool_arrete(m->pool);
    owl_tcp_listener_ferme(&m->l);
    if (m->x) { owl_http_executeur_arrete(m->x); owl_http_executeur_libere(m->x); }
    memset(m, 0, sizeof(*m));
}

/* ================================================================
 * FAMILLE 0 — discriminant de promptitude.
 *
 * Un client coupe en RST (abandon dur, cf. la_client_ferme_run) une
 * generation en cours. On mesure combien de chunks le producteur a
 * effectivement pousses avant de detecter le depart. ROUGE attendu sans
 * le fix : rien ne cable la deconnexion detectee cote reacteur/transport
 * jusqu'au producteur sur le chemin executeur-async, le producteur peut
 * donc pousser un nombre de chunks bien superieur a une poignee avant que
 * l'echec d'ecriture ne remonte -- jusqu'a potentiellement GEN_MAX_CHUNKS.
 * VERT attendu avec le fix : quelques chunks maximum (le temps qu'UNE
 * pousse echoue sur ECONNRESET).
 * ================================================================ */

/* Borne "prompt" : un chiffre a un digit de marge sur la mecanique
 * TCP locale (loopback, aucune latence reseau reelle), tres en-dessous
 * de GEN_MAX_CHUNKS. Choisie pour discriminer un arret rapide d'une
 * course jusqu'a la borne haute, pas pour capturer l'exactitude au
 * chunk pres (mecanique OS non deterministe a l'octet pres). */
#define BORNE_CHUNKS_PROMPTS 15

static void famille_0_promptitude(void) {
    g_famille = FAM_PROMPTITUDE;

    Montage m;
    if (!montage_demarre(&m)) { CHECK(0, "promptitude_montage"); return; }

    g_dernier_chunks_pousses     = -1;
    g_dernier_stoppe_par_abandon = 0;

    FermetureArg fa; fa.port = m.port;
    la_thread_t th;
    LA_START(th, la_client_ferme_run, &fa);
    LA_JOIN(th);

    /* Le producteur tourne dans le thread executeur : on lui laisse le
     * temps de constater le depart et de sortir de sa boucle. Bornee :
     * GEN_MAX_CHUNKS * GEN_PACE_MS est le plafond dur (~5 s), on
     * attend large mais fini (8 s) puis on constate l'etat. */
    uint64_t t0 = owl_tcp_server_horloge_ms();
    while (g_dernier_chunks_pousses < 0 &&
           owl_tcp_server_horloge_ms() - t0 < 8000) {
        LA_SLEEP(20);
    }

    CHECK(g_dernier_chunks_pousses >= 0, "promptitude_generation_terminee_dans_les_8s");
    CHECK(g_dernier_stoppe_par_abandon == 1, "promptitude_generation_stoppee_par_abandon_pas_par_epuisement");
    CHECK(g_dernier_chunks_pousses <= BORNE_CHUNKS_PROMPTS,
          "promptitude_peu_de_chunks_perdus_avant_detection");
    if (g_dernier_chunks_pousses < 0 || g_dernier_chunks_pousses > BORNE_CHUNKS_PROMPTS ||
        !g_dernier_stoppe_par_abandon) {
        fprintf(stderr, "  [f0] chunks_pousses=%ld abandon=%ld (borne=%d, max=%d)\n",
                g_dernier_chunks_pousses, g_dernier_stoppe_par_abandon,
                BORNE_CHUNKS_PROMPTS, GEN_MAX_CHUNKS);
    }

    montage_arrete(&m);
}

/* ================================================================
 * FAMILLE 1 — pas de spirale (repro inversee du constat testeur).
 *
 * N generations abandonnees en RST (abandon dur, cf. la_client_ferme_run),
 * TIREES L'UNE APRES L'AUTRE (motif OpenCode abandon+retry reel : le pool
 * raccroche puis retente), puis une petite generation. Mesure le temps DE
 * BOUT EN BOUT depuis la 1ere fermeture jusqu'a la reponse de la petite
 * generation -- exactement l'axe du repro testeur ("6 générations
 * abandonnées -> 175 s" est une mesure TOTALE, pas isolee a la derniere
 * requete). ROUGE attendu sans le fix : chaque generation dont le client
 * abandonne, non coupee tot (famille 0), occupe l'executeur mono-thread
 * pendant GEN_MAX_CHUNKS * GEN_PACE_MS avant de rendre la main -- N
 * generations pleines dominent largement le total. VERT attendu avec le
 * fix : chaque abandon libere l'executeur en une poignee de chunks, le
 * total reste proche du temps a vide.
 * ================================================================ */

#define N_ABANDONS   3
/* Borne : sous fix, N fermetures prompts (quelques ms chacune, cf.
 * famille 0) + une petite generation instantanee tiennent tres large
 * dans 1 s. Tres en-dessous d'UNE SEULE generation non coupee menee a
 * son terme (GEN_MAX_CHUNKS * GEN_PACE_MS ~ 5000 ms), a fortiori de
 * N_ABANDONS. */
#define BORNE_TOTAL_MS 3000

static void famille_1_pas_de_spirale(void) {
    g_famille = FAM_PAS_DE_SPIRALE;

    Montage m;
    if (!montage_demarre(&m)) { CHECK(0, "spirale_montage"); return; }

    uint64_t t_debut = owl_tcp_server_horloge_ms();

    for (int i = 0; i < N_ABANDONS; i++) {
        FermetureArg fa; fa.port = m.port;
        la_thread_t th;
        LA_START(th, la_client_ferme_run, &fa);
        LA_JOIN(th);
        /* Laisse cette generation abandonnee rendre la main (coupee tot
         * sous fix, ou epuiser sa borne haute sans fix) avant la
         * fermeture suivante -- reproduit le motif "abandon, puis
         * nouvelle requete" d'OpenCode. Bornee : le plafond dur
         * (GEN_MAX_CHUNKS * GEN_PACE_MS) empeche toute attente
         * indefinie. */
        uint64_t t0 = owl_tcp_server_horloge_ms();
        g_dernier_chunks_pousses = -1;
        while (g_dernier_chunks_pousses < 0 &&
               owl_tcp_server_horloge_ms() - t0 < 8000) {
            LA_SLEEP(20);
        }
    }

    Sonde petite;
    sonde_prepare(&petite, "/petit");
    sonde_demarre(&petite, m.port, 8000);
    sonde_joint(&petite);

    uint64_t duree_totale = owl_tcp_server_horloge_ms() - t_debut;

    CHECK(strstr(sonde_recu(&petite), "[DONE]") != NULL, "spirale_petite_generation_bien_servie");
    CHECK(duree_totale < BORNE_TOTAL_MS,
          "spirale_pas_de_ralentissement_cumulatif_sur_N_abandons");
    if (duree_totale >= BORNE_TOTAL_MS) {
        fprintf(stderr, "  [f1] duree_totale=%llums (borne=%dms) sur %d fermetures + 1 petite generation\n",
                (unsigned long long)duree_totale, BORNE_TOTAL_MS, N_ABANDONS);
    }

    sonde_libere(&petite);
    montage_arrete(&m);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_llm_annulation_self_test_run(void) {
    printf("== Annulation generation a la deconnexion client (fix/1723) ==\n");

    famille_0_promptitude();
    famille_1_pas_de_spirale();

    int tp = 0, tf = 0;
    for (int i = 0; i < FAM_NB; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 promptitude (chunks perdus bornes) : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 pas de spirale (N fermetures)       : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   TOTAL annulation generation (fix/1723) : %d/%d\n", tp, tp + tf);
    return tf;
}
