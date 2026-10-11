// SPDX-License-Identifier: AGPL-3.0-only
/* http_reacteur_stream_test.c — flux SSE dans la boucle reacteur (W5).
 *
 * Ticket : 
 * Design : note interne de concurrence du serveur HTTP (W5).
 *
 * DIGUE TEST-FIRST. Etat actuel (note coordinator post-W2, ) : le
 * reacteur classe controle/donnees mais dispatche TOUT le data-plane — flux
 * SSE COMPRIS — a un worker qui appelle p->sert() en BLOQUANT (le drain de
 * flux bloque sur sa condvar interne, cf. commentaire tp_reac_worker dans
 * net_tcp_server.c). Un flux qui reste ouvert longtemps retient donc son
 * worker pour toute sa duree, exactement comme le pool historique.
 *
 * Le test est model-free : un handler_rep DIRECT (owl_http_rep_cree_flux +
 * un thread producteur de test qui pousse des trames a intervalles reguliers,
 * PAS de VM/executeur, PAS de LLM) simule N connexions de streaming actives.
 * Le producteur ne touche plus `flux` apres avoir appele
 * owl_http_rep_flux_termine (dernier acces) : le transport, seul proprietaire
 * (refcount cree a 1 par owl_http_rep_cree_flux), le libere ensuite sans
 * course — meme discipline que le seam producteur de http_exec.c.
 *
 * FAMILLE 0 — discriminant : N >= nb_workers flux actifs retiennent les
 * workers -> une requete data-plane COURTE arrivee apres doit attendre
 * qu'un worker se libere. ROUGE aujourd'hui (attente >= une duree de flux
 * complete) ; VERT attendu apres W5 (le reacteur draine les flux via
 * POLLOUT sans jamais dispatcher un worker pour eux, donc les workers
 * restent tous disponibles pour le plan de donnees non-streaming).
 *
 * FAMILLE 1 — pas de starvation : les N flux ET la requete courte finissent
 * TOUS par etre servis avec leur contenu exact, quelle que soit l'archi
 * (avant/apres W5). Un test qui ne prouverait que la lenteur sans prouver
 * que tout le monde progresse ne discriminerait pas un vrai hang d'une
 * simple lenteur — cf. repro-A du design (§1).
 *
 * Hors perimetre de ce fichier (deja couverts ailleurs, NE PAS dupliquer) :
 *   - contre-pression bornee (cap 1 Mo) : http_stream_test.c famille 2.
 *   - framing SSE / serve-llm (data:, [DONE], terminaison) : familles
 *     serve_llm_self_test_run (147/147), inchangees par ce ticket.
 *   - UAF close mid-stream : garde stress 
 *     (http_stress_gardes_test.c) — refcount/relache y est deja durci.
 *   - anti-spin POLLOUT (buffer vide -> desenregistrement de l'interet) :
 *     PAS testable boite-noire depuis ce niveau (compteur d'iterations
 *     internes au reacteur non expose). A verrouiller par mutation-test au
 *     moment de l'impl W5, meme methode que le spin POLLOUT de  :
 *     watchdog d'iterations/tick sur une connexion oisive POLLOUT-enregistree.
 *
 * Execute via `owl --self-test-net` (cf. main.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"
#include "../net_tcp_server.h"

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE rs_thread_t;
  #define RS_THREAD_RET DWORD WINAPI
  #define RS_THREAD_0   0
  #define RS_SLEEP(ms)  Sleep((DWORD)(ms))
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t rs_thread_t;
  #define RS_THREAD_RET void*
  #define RS_THREAD_0   NULL
  static void rs_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define RS_SLEEP(ms)  rs_sleep_ms((int)(ms))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

enum { FAM_DISCRIMINANT = 0, FAM_SANS_STARVATION, FAM_EXEC_ASYNC, FAM_DELAI_MAX, FAM_NB };

static int g_pass[FAM_NB] = {0};
static int g_fail[FAM_NB] = {0};
static int g_famille = FAM_DISCRIMINANT;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < FAM_NB) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL [%s] (ligne %d)\n", label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Producteur de test : pousse des trames SSE a intervalles reguliers,
 * PUIS s'arrete de toucher `flux` (dernier acces = flux_termine). Le
 * transport (refcount 1, seul proprietaire) libere ensuite sans course.
 * ================================================================ */

typedef struct {
    OwlHttpRep* flux;
    int         n_chunks;
    int         pas_ms;
} StreamProd;

static RS_THREAD_RET stream_prod_fn(void* arg) {
    StreamProd* sp = (StreamProd*)arg;
    char buf[40];
    for (int i = 0; i < sp->n_chunks; i++) {
        int n = snprintf(buf, sizeof(buf), "data: tick%d\n\n", i);
        if (n < 0) n = 0;
        if (owl_http_rep_flux_pousse(sp->flux, buf, (size_t)n) != OWL_HTTP_FLUX_OK) break;
        RS_SLEEP(sp->pas_ms);
    }
    owl_http_rep_flux_termine(sp->flux);   /* dernier acces a sp->flux */
    free(sp);
    return RS_THREAD_0;
}

#define RS_STREAM_CHUNKS 15
#define RS_STREAM_PAS_MS 100   /* ~1500 ms de duree de flux totale */

static OwlHttpRep* handler_w5(const OwlHttpRequete* req, void* user) {
    (void)user;
    const char* chemin = req->chemin ? req->chemin : "";

    if (strncmp(chemin, "/stream", 7) == 0) {
        OwlHttpRep* f = owl_http_rep_cree_flux(200, "text/event-stream", 1 << 16);
        if (!f) return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);

        StreamProd* sp = (StreamProd*)malloc(sizeof(StreamProd));
        if (!sp) { owl_http_rep_libere(f); return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0); }
        sp->flux = f; sp->n_chunks = RS_STREAM_CHUNKS; sp->pas_ms = RS_STREAM_PAS_MS;

        /* Fire-and-forget : PAS de join ici. Rejoindre le thread rendrait ce
         * handler_rep bloquant de bout en bout, ce qui invaliderait le test
         * (cf. commentaire de tete — le drain doit rester concurrent de la
         * production, comme le seam producteur reel de http_exec.c). */
#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, stream_prod_fn, sp, 0, NULL);
        if (th) CloseHandle(th);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, stream_prod_fn, sp) == 0) pthread_detach(th);
#endif
        return f;
    }

    return owl_http_rep_cree_tampon(200, "text/plain", "court-ok", 8);
}

/* ================================================================
 * Client mock minimal : un chemin, une lecture bornee.
 * ================================================================ */

typedef struct {
    OwlMockPeer* pair;
    char         requete[96];
} MP;

static void mp_prepare(MP* c, const char* chemin) {
    memset(c, 0, sizeof(*c));
    snprintf(c->requete, sizeof(c->requete), "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", chemin);
}

static void mp_demarre(MP* c, int port, int timeout_lecture_ms) {
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
    mc.timeout_lecture_ms         = timeout_lecture_ms;   /* borne : jamais de blocage figeant la batterie */
    mc.fermer_apres_ecriture      = 1;
    c->pair = owl_mock_peer_demarre(&mc);
}

static void mp_joint(MP* c)  { if (c->pair) owl_mock_peer_joint(c->pair); }
static void mp_libere(MP* c) { if (c->pair) { owl_mock_peer_libere(c->pair); c->pair = NULL; } }
static const char* mp_recu(const MP* c) {
    if (!c->pair) return "";
    size_t l = 0;
    return owl_mock_peer_recu(c->pair, &l);
}

/* ================================================================
 * FAMILLE 0 — discriminant : N >= W flux actifs, une requete courte
 * arrivee APRES doit quand meme etre servie vite.
 * ================================================================ */

#define RS_W 4
#define RS_N 6
/* Borne large sous la duree d'un flux (~1500 ms) : sous l'archi actuelle,
 * la courte attend qu'un worker se libere d'un flux complet -> ROUGE. Sous
 * W5, aucun worker n'est jamais retenu par un flux -> la courte est servie
 * en quelques dizaines de ms, tres en-dessous de la borne. */
#define RS_BORNE_COURTE_MS 700

static void famille_0_discriminant(void) {
    g_famille = FAM_DISCRIMINANT;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_w5;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, RS_N + 8, false)) {
        CHECK(0, "ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers = RS_W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    int port = owl_tcp_listener_port(&l);

    /* N flux longs demarres D'ABORD : sous l'archi actuelle ils remplissent
     * les RS_W workers (et le surplus attend en file de dispatch). */
    MP flux[RS_N];
    char chemin[32];
    for (int i = 0; i < RS_N; i++) {
        snprintf(chemin, sizeof(chemin), "/stream%d", i);
        mp_prepare(&flux[i], chemin);
        mp_demarre(&flux[i], port, 8000);
    }

    /* Fenetre de reglage : laisse les connexions se classer et se
     * dispatcher AVANT que la courte n'arrive, pour que l'ordre FIFO soit
     * sans ambiguite (bien plus courte que la duree d'un flux). */
    RS_SLEEP(150);

    MP courte;
    mp_prepare(&courte, "/court");
    uint64_t t0 = owl_tcp_server_horloge_ms();
    mp_demarre(&courte, port, 8000);
    mp_joint(&courte);
    uint64_t duree_courte = owl_tcp_server_horloge_ms() - t0;

    CHECK(strstr(mp_recu(&courte), "court-ok") != NULL, "courte_recoit_bien_sa_reponse");
    CHECK(duree_courte < RS_BORNE_COURTE_MS,
          "courte_servie_sans_attendre_un_worker_libere_par_un_flux");
    if (duree_courte >= RS_BORNE_COURTE_MS) {
        fprintf(stderr, "  [f0] duree_courte=%llums (borne=%dms)\n",
                (unsigned long long)duree_courte, RS_BORNE_COURTE_MS);
    }

    for (int i = 0; i < RS_N; i++) mp_joint(&flux[i]);
    for (int i = 0; i < RS_N; i++) mp_libere(&flux[i]);
    mp_libere(&courte);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * FAMILLE 1 — pas de starvation : tout le monde finit, contenu exact.
 *
 * Reprend le meme scenario (N flux + 1 courte) mais verifie que CHAQUE
 * flux recoit son cadrage chunked complet (Transfer-Encoding, dernier
 * chunk 0\r\n\r\n) et que la courte recoit son corps exact. Une impl qui
 * "gagnerait" la famille 0 en abandonnant des connexions (au lieu de les
 * servir sans retenir de worker) serait attrapee ici.
 * ================================================================ */

static void famille_1_sans_starvation(void) {
    g_famille = FAM_SANS_STARVATION;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_w5;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, RS_N + 8, false)) {
        CHECK(0, "ecoute_impossible");
        return;
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers = RS_W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "pool_demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); return; }

    int port = owl_tcp_listener_port(&l);

    MP flux[RS_N];
    char chemin[32];
    for (int i = 0; i < RS_N; i++) {
        snprintf(chemin, sizeof(chemin), "/stream%d", i);
        mp_prepare(&flux[i], chemin);
        /* Borne large : meme sous l'archi actuelle (retention de worker),
         * un flux doit finir par etre servi — aucune connexion n'est
         * censee etre abandonnee, seulement retardee. */
        mp_demarre(&flux[i], port, 15000);
    }

    MP courte;
    mp_prepare(&courte, "/court");
    mp_demarre(&courte, port, 15000);

    for (int i = 0; i < RS_N; i++) mp_joint(&flux[i]);
    mp_joint(&courte);

    int flux_complets = 0;
    for (int i = 0; i < RS_N; i++) {
        const char* r = mp_recu(&flux[i]);
        int a_entete   = strstr(r, "Transfer-Encoding: chunked\r\n") != NULL;
        int a_terminal = strstr(r, "0\r\n\r\n") != NULL;
        int a_donnees  = strstr(r, "data: tick0") != NULL;
        if (a_entete && a_terminal && a_donnees) flux_complets++;
    }
    CHECK(flux_complets == RS_N, "tous_les_flux_arrivent_a_terme_avec_leur_contenu");

    CHECK(strstr(mp_recu(&courte), "court-ok") != NULL, "courte_finit_aussi_par_etre_servie");

    size_t servies = owl_tcp_pool_connexions_servies(pool);
    CHECK(servies == (size_t)(RS_N + 1), "aucune_connexion_perdue_en_route");
    if (flux_complets != RS_N || servies != (size_t)(RS_N + 1)) {
        fprintf(stderr, "  [f1] flux_complets=%d/%d servies=%u\n",
                flux_complets, RS_N, (unsigned)servies);
    }

    for (int i = 0; i < RS_N; i++) mp_libere(&flux[i]);
    mp_libere(&courte);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * FAMILLE 2 — flux via l'EXECUTEUR async (W7)
 *
 * C'est le chemin de PRODUCTION serve-llm : un producteur (mode C) tourne
 * dans le thread executeur, le worker DEPOSE + remet le jeton au reacteur
 * (executeur_async) sans jamais attendre, et le reacteur resout via essaie
 * (-> FLUX) puis draine les chunks. Les familles 0/1 exercaient un flux
 * DIRECT (handler_rep rendant un flux pret) — le chemin async essaie->FLUX
 * n'etait couvert par aucun test. Ici on le valide de bout en bout.
 *
 * L'executeur etant mono-thread (llama_decode mono-ctx, irreductible), les
 * flux sont PRODUITS en serie ; le gain W7 n'est pas le parallelisme de
 * generation mais que le PLAN DE CONTROLE ne soit jamais affame (aucun
 * worker retenu) pendant qu'un flux se produit. On mesure les deux :
 * contenu SSE exact des N flux + reponse de controle prompte.
 * ================================================================ */

#define RS_EA_N        3
#define RS_EA_CHUNKS   3
#define RS_EA_PAS_MS   20    /* ~60 ms/flux nominal ; l'executeur mono-thread
                              * les SERIALISE, le dernier flux attend le plus.
                              * Timeout client large (RS_EA_LECTURE_MS) : le test
                              * mesure la CORRECTION, pas la latence — il ne doit
                              * pas flaker sous la contention CPU d'un voisin. */
#define RS_EA_LECTURE_MS 15000

/* Producteur executeur (mode C). Route-aware comme serve_llm_producteur :
 * une route de controle rend une reponse courte (fast-path inline), le
 * data-plane rend un flux SSE. Tourne dans le thread executeur. */
static void exec_stream_prod(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    const char* path = req->chemin ? req->chemin : "";

    if (owl_http_route_est_controle(path)) {
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/plain", 1u << 10);
        if (!f) return;
        owl_http_rep_flux_pousse(f, "sante-ok", 8);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
        return;
    }

    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1u << 16);
    if (!f) return;
    char buf[40];
    for (int i = 0; i < RS_EA_CHUNKS; i++) {
        int n = snprintf(buf, sizeof(buf), "data: tick%d\n\n", i);
        if (n < 0) n = 0;
        if (owl_http_rep_flux_pousse(f, buf, (size_t)n) != OWL_HTTP_FLUX_OK) break;
        RS_SLEEP(RS_EA_PAS_MS);
    }
    owl_http_rep_flux_termine(f);
    owl_http_rep_flux_relache(f);   /* dernier acces : le transport libere ensuite */
}

static void famille_2_exec_async(void) {
    g_famille = FAM_EXEC_ASYNC;

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 16;
    xc.plafond_attente_ms = 8000;
    xc.producteur         = exec_stream_prod;
    xc.producteur_user    = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) { CHECK(0, "ea_executeur_cree"); return; }
    if (!owl_http_executeur_demarre(x)) {
        CHECK(0, "ea_executeur_demarre");
        owl_http_executeur_libere(x);
        return;
    }

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, RS_EA_N + 4, false)) {
        CHECK(0, "ea_ecoute_impossible");
        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        return;
    }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;   /* controle inline */
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;                            /* data-plane async (W7) */

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = RS_W;
    pc.connexions_max = RS_W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { CHECK(0, "ea_pool_demarre"); owl_tcp_listener_ferme(&l);
                 owl_http_executeur_arrete(x); owl_http_executeur_libere(x); return; }
    int port = owl_tcp_listener_port(&l);

    MP flux[RS_EA_N];
    char chemin[32];
    for (int i = 0; i < RS_EA_N; i++) {
        snprintf(chemin, sizeof(chemin), "/gen%d", i);
        mp_prepare(&flux[i], chemin);
        mp_demarre(&flux[i], port, RS_EA_LECTURE_MS);
    }

    /* Un flux est en cours de production sur le thread executeur mono-thread ;
     * une requete de CONTROLE arrivee maintenant ne doit PAS attendre — elle
     * est servie inline sur le reacteur, aucun worker n'etant retenu (W7). */
    RS_SLEEP(60);
    MP contr;
    mp_prepare(&contr, "/sante");
    uint64_t t0 = owl_tcp_server_horloge_ms();
    mp_demarre(&contr, port, RS_EA_LECTURE_MS);
    mp_joint(&contr);
    uint64_t duree_contr = owl_tcp_server_horloge_ms() - t0;

    CHECK(strstr(mp_recu(&contr), "sante-ok") != NULL, "ea_controle_recoit_sa_reponse");
    CHECK(duree_contr < RS_BORNE_COURTE_MS,
          "ea_controle_prompt_pendant_qu_un_flux_se_produit");
    if (duree_contr >= RS_BORNE_COURTE_MS) {
        fprintf(stderr, "  [f2] duree_controle=%llums (borne=%dms)\n",
                (unsigned long long)duree_contr, RS_BORNE_COURTE_MS);
    }

    for (int i = 0; i < RS_EA_N; i++) mp_joint(&flux[i]);

    int flux_complets = 0;
    for (int i = 0; i < RS_EA_N; i++) {
        const char* r = mp_recu(&flux[i]);
        int a_entete   = strstr(r, "Transfer-Encoding: chunked\r\n") != NULL;
        int a_terminal = strstr(r, "0\r\n\r\n") != NULL;
        int a_donnees  = strstr(r, "data: tick0") != NULL;
        if (a_entete && a_terminal && a_donnees) flux_complets++;
    }
    CHECK(flux_complets == RS_EA_N,
          "ea_tous_les_flux_async_arrivent_a_terme_avec_leur_contenu");

    size_t servies = owl_tcp_pool_connexions_servies(pool);
    CHECK(servies == (size_t)(RS_EA_N + 1), "ea_aucune_connexion_perdue");
    if (flux_complets != RS_EA_N || servies != (size_t)(RS_EA_N + 1)) {
        fprintf(stderr, "  [f2] flux_complets=%d/%d servies=%u\n",
                flux_complets, RS_EA_N, (unsigned)servies);
    }

    for (int i = 0; i < RS_EA_N; i++) mp_libere(&flux[i]);
    mp_libere(&contr);

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 3 — DelaiMax : borne applicative de DUREE du flux ( P5)
 *
 * DIGUE TEST-FIRST. Le socle transport ne borne que la MEMOIRE (contre-pression
 * bornee en octets) ; P5 ajoute une borne de DUREE cote reacteur : un timer
 * dedie par flux (DISTINCT de l'echeance W7 de synthese 503) arme a la
 * resolution du flux dans le drain, qui a echeance marque le consommateur fini
 * (comme une mort de socket) -> la prochaine pousse rend ABANDON, et le
 * transport est libere A L'HEURE.
 *
 * Chemin exerce = EXECUTEUR async (comme famille 2, le patron serve-llm /
 * service DSL) : le producteur ouvre le flux via owl_http_prod_ouvre_flux (donc
 * l'estampillage delai_max_ms + la resolution jeton->FLUX + la query reacteur
 * sont tous parcourus), le reacteur draine et arme/coupe le timer.
 *
 * ROUGE tant que net_tcp_server.c ne consomme PAS le delai (pas de timer) :
 * le flux long court jusqu'au bout (terminal 0CRLF CRLF present, aucun ABANDON).
 * VERT une fois le timer DelaiMax cable : coupe abrupte a ~N ms, pas de
 * terminal, le producteur voit ABANDON.
 * ================================================================ */

static struct {
    int          delai_ms;      /* borne posee sur le flux (0 = illimite) */
    int          n_chunks;      /* borne dure du producteur (evite tout hang) */
    int          pas_ms;
    volatile int pousses_ok;    /* nb de pousses acceptees avant fin/ABANDON */
    volatile int abandonne;     /* une pousse a rendu ABANDON (coupe transport) */
} g_delai;

/* Producteur executeur (mode C) : estampille le delai sur le ProdCtx AVANT
 * d'ouvrir (owl_http_prod_ouvre_flux stampe alors le flux), puis pousse des
 * trames a intervalle regulier jusqu'a n_chunks OU jusqu'a un ABANDON. */
static void delai_stream_prod(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user; (void)req;
    owl_http_prod_flux_delai_max_set(ctx, g_delai.delai_ms);
    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/event-stream", 1u << 16);
    if (!f) return;
    char buf[40];
    for (int i = 0; i < g_delai.n_chunks; i++) {
        int n = snprintf(buf, sizeof(buf), "data: tick%d\n\n", i);
        if (n < 0) n = 0;
        if (owl_http_rep_flux_pousse(f, buf, (size_t)n) != OWL_HTTP_FLUX_OK) {
            g_delai.abandonne = 1;
            break;
        }
        g_delai.pousses_ok++;
        RS_SLEEP(g_delai.pas_ms);
    }
    owl_http_rep_flux_termine(f);
    owl_http_rep_flux_relache(f);   /* ref producteur : le transport a la sienne */
}

typedef struct {
    uint64_t duree_ms;
    int      a_terminal;   /* "0\r\n\r\n" recu (fin nominale chunked) */
    int      a_tick0;      /* au moins la 1re trame recue */
    int      a_dernier;    /* la derniere trame attendue recue */
    int      pousses_ok;
    int      abandonne;
} DelaiRes;

/* Monte un serveur executeur avec UN flux et mesure son sort cote client. */
static DelaiRes run_delai_scenario(int delai_ms, int n_chunks, int pas_ms,
                                   const char* dernier_tick) {
    DelaiRes res; memset(&res, 0, sizeof(res));
    g_delai.delai_ms = delai_ms;
    g_delai.n_chunks = n_chunks;
    g_delai.pas_ms   = pas_ms;
    g_delai.pousses_ok = 0;
    g_delai.abandonne  = 0;

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 8000;
    xc.producteur         = delai_stream_prod;
    xc.producteur_user    = NULL;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) { CHECK(0, "dm_executeur_cree"); return res; }
    if (!owl_http_executeur_demarre(x)) {
        CHECK(0, "dm_executeur_demarre"); owl_http_executeur_libere(x); return res;
    }

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 6, false)) {
        CHECK(0, "dm_ecoute_impossible");
        owl_http_executeur_arrete(x); owl_http_executeur_libere(x); return res;
    }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = RS_W;
    pc.connexions_max = RS_W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { CHECK(0, "dm_pool_demarre"); owl_tcp_listener_ferme(&l);
                 owl_http_executeur_arrete(x); owl_http_executeur_libere(x); return res; }
    int port = owl_tcp_listener_port(&l);

    MP flux;
    mp_prepare(&flux, "/gen");
    uint64_t t0 = owl_tcp_server_horloge_ms();
    mp_demarre(&flux, port, 8000);
    mp_joint(&flux);
    res.duree_ms = owl_tcp_server_horloge_ms() - t0;

    const char* r = mp_recu(&flux);
    res.a_terminal = strstr(r, "0\r\n\r\n") != NULL;
    res.a_tick0    = strstr(r, "data: tick0") != NULL;
    res.a_dernier  = dernier_tick ? (strstr(r, dernier_tick) != NULL) : 1;

    mp_libere(&flux);
    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_arrete(x);   /* joint le thread executeur : g_delai stable */
    owl_http_executeur_libere(x);

    res.pousses_ok = g_delai.pousses_ok;
    res.abandonne  = g_delai.abandonne;
    return res;
}

static void famille_3_delai_max(void) {
    g_famille = FAM_DELAI_MAX;

    /* 3d — accesseur : verrou de valeur EXACTE (independant du reacteur). */
    OwlHttpRep* f = owl_http_rep_cree_flux(200, "text/event-stream", 1 << 16);
    CHECK(f != NULL, "dm_accessor_cree_flux");
    if (f) {
        owl_http_rep_flux_delai_max_set(f, 1234);
        CHECK(owl_http_rep_flux_delai_max_ms(f) == 1234, "dm_accessor_valeur_exacte");
        owl_http_rep_flux_delai_max_set(f, 0);
        CHECK(owl_http_rep_flux_delai_max_ms(f) == 0, "dm_accessor_zero");
        owl_http_rep_flux_delai_max_set(f, -5);
        CHECK(owl_http_rep_flux_delai_max_ms(f) == 0, "dm_accessor_negatif_rabat_zero");
        owl_http_rep_flux_marque_consommateur_fini(f);
        owl_http_rep_flux_relache(f);
    }

    /* 3a — DELAI ATTEINT : flux long (30 x 100 ms = ~3 s), coupe a ~500 ms.
     * Le transport doit etre libere A L'HEURE et le producteur voir ABANDON. */
    DelaiRes hit = run_delai_scenario(500, 30, 100, NULL);
    CHECK(hit.a_tick0, "dm_hit_recoit_au_moins_la_premiere_trame");
    CHECK(!hit.a_terminal, "dm_hit_coupe_abrupte_sans_terminal");
    CHECK(hit.duree_ms >= 350 && hit.duree_ms < 1800, "dm_hit_coupe_a_environ_le_delai");
    CHECK(hit.abandonne == 1, "dm_hit_producteur_voit_abandon");
    CHECK(hit.pousses_ok >= 1 && hit.pousses_ok < 30, "dm_hit_coupe_en_plein_flux");
    if (hit.duree_ms >= 1800 || hit.a_terminal) {
        fprintf(stderr, "  [f3a] duree=%llums terminal=%d pousses=%d abandon=%d\n",
                (unsigned long long)hit.duree_ms, hit.a_terminal, hit.pousses_ok, hit.abandonne);
    }

    /* 3b — DANS LES CLOUS : flux court (3 x 50 ms) sous un delai large (5 s).
     * Complete normal, timer annule (aucun faux positif apres coup). */
    DelaiRes ok = run_delai_scenario(5000, 3, 50, "data: tick2");
    CHECK(ok.a_terminal, "dm_within_complete_avec_terminal");
    CHECK(ok.a_dernier, "dm_within_recoit_toutes_les_trames");
    CHECK(ok.abandonne == 0, "dm_within_aucun_abandon");
    CHECK(ok.pousses_ok == 3, "dm_within_toutes_les_pousses_ok");
    CHECK(ok.duree_ms < 5000, "dm_within_termine_avant_le_delai");

    /* 3c — DELAI = 0 (illimite) : aucun timer arme. Flux ~600 ms complete
     * entierement (un timer a 0 ms le couperait immediatement -> discriminant). */
    DelaiRes illim = run_delai_scenario(0, 8, 75, "data: tick7");
    CHECK(illim.a_terminal, "dm_illim_complete_avec_terminal");
    CHECK(illim.a_dernier, "dm_illim_recoit_toutes_les_trames");
    CHECK(illim.abandonne == 0, "dm_illim_aucun_abandon");
    CHECK(illim.pousses_ok == 8, "dm_illim_toutes_les_pousses_ok");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_reacteur_stream_self_test_run(void) {
    printf("== Flux SSE dans la boucle reacteur (feat/1706, W5) ==\n");

    famille_0_discriminant();
    famille_1_sans_starvation();
    famille_2_exec_async();
    famille_3_delai_max();

    int tp = 0, tf = 0;
    for (int i = 0; i < FAM_NB; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 discriminant (worker non retenu) : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 pas de starvation                : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 flux via executeur async (W7)    : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 DelaiMax (borne de duree, P5)    : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   TOTAL flux SSE reacteur (W5)         : %d/%d\n", tp, tp + tf);
    return tf;
}
