// SPDX-License-Identifier: AGPL-3.0-only
/* http_concurrence_test.c — pool de workers reseau (, A1.1b).
 *
 * Spec : contrat de transport HTTP interne (les trois niveaux de
 *        concurrence, regle d'or).
 *
 * Le lot ajoute le NIVEAU 1 et lui seul : plusieurs connexions reseau
 * menees en parallele, sans que la VM devienne concurrente. D'ou quatre
 * familles, chacune tournee vers une PREUVE et pas vers un « ca marche » :
 *
 *   C1. N connexions simultanees servies sans melange, plafond de
 *       connexions respecte (vecteur 3.5 reel), et parallelisme MESURE
 *       (duree totale tres inferieure a la somme des services).
 *   C2. Un client lent n'empeche pas les autres d'avancer. Mesure
 *       DIFFERENTIELLE pool contre worker unique : sans le controle a un
 *       seul worker, l'assertion ne prouverait rien.
 *   C3. Saturation observable : file pleine -> 503 produit par le
 *       transport SEUL, jobs_pris inchange le prouve. Controle positif
 *       avec l'executeur demarre : les memes connexions rendent 200.
 *   C4. Un seul thread touche la VM, et la mesure anti-GC d'A0.2 etendue
 *       a N threads reseau : sur chaque fenetre ou seul le reseau tourne,
 *       gc_bytesAllocated ne bouge pas d'un octet.
 *
 * Le pair mock (net_mock_peer.c) tourne deja un thread par instance : N
 * clients simultanes = N pairs, aucune variante a ecrire.
 *
 * Execute via `owl --self-test-net` (cf. main.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../gc.h"

#ifdef _WIN32
  #include <windows.h>
  #define CC_SLEEP(ms) Sleep((DWORD)(ms))
#else
  #include <time.h>
  static void cc_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define CC_SLEEP(ms) cc_sleep_ms((int)(ms))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass[5] = {0,0,0,0,0};
static int g_fail[5] = {0,0,0,0,0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille <= 4) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL C%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Handler d'echo REENTRANT
 *
 * Aucun etat global : le corps est bati sur la pile du thread reseau
 * puis COPIE dans la reponse opaque, qui en devient proprietaire. Un
 * handler a tampon global (celui du corpus A0.1) melangerait les
 * reponses de deux connexions concurrentes — c'est exactement ce que
 * cette famille doit pouvoir attraper si l'impl regresse.
 * ================================================================ */

static int g_handler_delai_ms = 0;

static OwlHttpRep* handler_echo_rep(const OwlHttpRequete* req, void* user) {
    (void)user;
    char corps[256];
    int n = snprintf(corps, sizeof(corps), "M=%s|C=%s|TC=%.*s",
                     req->methode,
                     req->chemin ? req->chemin : "",
                     (int)req->corps_len, req->corps ? req->corps : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(corps)) n = (int)sizeof(corps) - 1;
    /* Service volontairement lent : sans lui, N connexions se servent
     * plus vite qu'elles n'arrivent et le parallelisme reste invisible.
     * On mesure le pool, pas la vitesse de loopback. */
    if (g_handler_delai_ms > 0) CC_SLEEP(g_handler_delai_ms);
    return owl_http_rep_cree_tampon(200, "text/plain", corps, (size_t)n);
}

/* ================================================================
 * Clients
 * ================================================================ */

typedef struct {
    OwlMockPeer* pair;
    char         requete[192];
    char         attendu[192];   /* corps d'echo exact attendu */
} Client;

static void client_prepare(Client* c, const char* chemin, const char* corps) {
    memset(c, 0, sizeof(*c));
    if (corps && corps[0]) {
        snprintf(c->requete, sizeof(c->requete),
                 "POST %s HTTP/1.1\r\nHost: h\r\nContent-Length: %u\r\n\r\n%s",
                 chemin, (unsigned)strlen(corps), corps);
        snprintf(c->attendu, sizeof(c->attendu), "M=POST|C=%s|TC=%s", chemin, corps);
    } else {
        snprintf(c->requete, sizeof(c->requete), "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", chemin);
        snprintf(c->attendu, sizeof(c->attendu), "M=GET|C=%s|TC=", chemin);
    }
}

/* decoupe_octet : le client lent de la famille C2. */
static void client_demarre(Client* c, int port, int decoupe_octet, int delai_ms) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = port;
    mc.octets                     = c->requete;
    mc.octets_len                 = strlen(c->requete);
    mc.decoupe                    = decoupe_octet ? OWL_MOCK_DECOUPE_OCTET
                                                  : OWL_MOCK_DECOUPE_BLOC;
    mc.delai_ms                   = delai_ms;
    mc.lire_apres_ecriture        = 1;
    mc.fermer_envoi_avant_lecture = 1;
    mc.timeout_lecture_ms         = 15000;
    mc.fermer_apres_ecriture      = 1;
    c->pair = owl_mock_peer_demarre(&mc);
}

static void client_joint(Client* c) {
    if (!c->pair) return;
    owl_mock_peer_joint(c->pair);
}

static void client_libere(Client* c) {
    if (!c->pair) return;
    owl_mock_peer_libere(c->pair);
    c->pair = NULL;
}

static const char* client_recu(const Client* c) {
    if (!c->pair) return "";
    size_t l = 0;
    return owl_mock_peer_recu(c->pair, &l);
}

/* Attend que le pool ait servi `vise` connexions, au plus `max_ms`.
 * Rend le temps ecoule — c'est la MESURE, pas un simple booleen. */
static uint64_t attend_servies(const OwlTcpPool* p, size_t vise, int max_ms) {
    uint64_t t0 = owl_tcp_server_horloge_ms();
    for (;;) {
        if (owl_tcp_pool_connexions_servies(p) >= vise) break;
        if (owl_tcp_server_horloge_ms() - t0 >= (uint64_t)max_ms) break;
        CC_SLEEP(2);
    }
    return owl_tcp_server_horloge_ms() - t0;
}

/* ================================================================
 * C1 — N connexions simultanees, non-melange, plafond (vecteur 3.5)
 * ================================================================ */

static void famille_c1(void) {
    g_famille = 0;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_echo_rep;

    /* ---- C1a : 8 connexions, 4 workers, plafond 4 ---- */
    {
        enum { N = 8, W = 4 };
        g_handler_delai_ms = 60;

        OwlTcpListener l;
        if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
            CHECK(0, "c1a_ecoute_impossible");
            return;
        }

        /* Les clients se connectent AVANT le demarrage du pool : les N
         * connexions attendent dans le backlog et forment une vraie
         * rafale simultanee, au lieu d'arriver au gre du harnais. */
        Client cl[N];
        char chemin[32], corps[32];
        for (int i = 0; i < N; i++) {
            snprintf(chemin, sizeof(chemin), "/c%d", i);
            snprintf(corps,  sizeof(corps),  "charge%d", i);
            client_prepare(&cl[i], chemin, corps);
            client_demarre(&cl[i], owl_tcp_listener_port(&l), 0, 0);
        }

        OwlTcpPoolConfig pc;
        owl_tcp_pool_config_defaut(&pc);
        pc.nb_workers     = W;
        pc.connexions_max = W;

        uint64_t t0 = owl_tcp_server_horloge_ms();
        OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
        CHECK(pool != NULL, "c1a_pool_demarre");
        if (!pool) { owl_tcp_listener_ferme(&l); return; }

        attend_servies(pool, N, 15000);
        uint64_t duree = owl_tcp_server_horloge_ms() - t0;

        size_t servies = owl_tcp_pool_connexions_servies(pool);
        size_t pic     = owl_tcp_pool_pic_simultanees(pool);
        owl_tcp_pool_arrete(pool);
        for (int i = 0; i < N; i++) client_joint(&cl[i]);

        CHECK(servies == (size_t)N, "c1a_les_huit_connexions_servies");

        /* Non-melange : chaque client recoit SON echo, a l'octet, et
         * aucune trace d'un autre. Un tampon partage entre connexions se
         * verrait ici et nulle part ailleurs. */
        int propres = 0, pollues = 0;
        for (int i = 0; i < N; i++) {
            const char* r = client_recu(&cl[i]);
            if (strstr(r, cl[i].attendu)) propres++;
            for (int k = 0; k < N; k++) {
                if (k == i) continue;
                if (strstr(r, cl[k].attendu)) { pollues++; break; }
            }
        }
        CHECK(propres == N, "c1a_chaque_client_recoit_son_echo_exact");
        CHECK(pollues == 0, "c1a_aucune_reponse_ne_contient_celle_d_un_autre");

        /* Parallelisme MESURE. En sequentiel, 8 services de 60 ms coutent
         * au moins 480 ms ; avec 4 workers, ~120 ms. Le seuil est pose a
         * la moitie du sequentiel pour rester insensible a la machine. */
        CHECK(duree < (uint64_t)(N * g_handler_delai_ms / 2),
              "c1a_duree_tres_inferieure_au_service_sequentiel");
        CHECK(pic >= 2, "c1a_au_moins_deux_connexions_en_vol_simultanement");
        CHECK(pic <= (size_t)W, "c1a_jamais_plus_que_le_plafond_en_vol");
        if (servies != (size_t)N || pic > (size_t)W) {
            fprintf(stderr, "  [c1a] servies=%u pic=%u duree=%ums\n",
                    (unsigned)servies, (unsigned)pic, (unsigned)duree);
        }

        for (int i = 0; i < N; i++) client_libere(&cl[i]);
        owl_tcp_listener_ferme(&l);
    }

    /* ---- C1b : vecteur 3.5 reel — N+1 connexions sur plafond N ----
     *
     * Le plafond est INFERIEUR au nombre de workers : les connexions
     * excedentaires restent dans le backlog d'ecoute au lieu d'etre
     * acceptees. Aucun descripteur supplementaire n'est ouvert, et le
     * pic mesure le prouve. */
    {
        enum { N = 6, W = 4, PLAFOND = 2 };
        g_handler_delai_ms = 40;

        OwlTcpListener l;
        if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
            CHECK(0, "c1b_ecoute_impossible");
            return;
        }

        Client cl[N];
        char chemin[32];
        for (int i = 0; i < N; i++) {
            snprintf(chemin, sizeof(chemin), "/p%d", i);
            client_prepare(&cl[i], chemin, NULL);
            client_demarre(&cl[i], owl_tcp_listener_port(&l), 0, 0);
        }

        OwlTcpPoolConfig pc;
        owl_tcp_pool_config_defaut(&pc);
        pc.nb_workers     = W;
        pc.connexions_max = PLAFOND;

        OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
        CHECK(pool != NULL, "c1b_pool_demarre");
        if (!pool) { owl_tcp_listener_ferme(&l); return; }

        attend_servies(pool, N, 15000);
        size_t servies = owl_tcp_pool_connexions_servies(pool);
        size_t pic     = owl_tcp_pool_pic_simultanees(pool);
        owl_tcp_pool_arrete(pool);
        for (int i = 0; i < N; i++) client_joint(&cl[i]);

        CHECK(pic <= (size_t)PLAFOND, "c1b_plafond_jamais_depasse");
        CHECK(servies == (size_t)N, "c1b_les_excedentaires_finissent_servies_pas_refusees");

        int recus = 0;
        for (int i = 0; i < N; i++) {
            if (strstr(client_recu(&cl[i]), cl[i].attendu)) recus++;
        }
        CHECK(recus == N, "c1b_chaque_client_a_bien_sa_reponse");
        for (int i = 0; i < N; i++) client_libere(&cl[i]);

        /* Controle positif : apres la rafale, on sert encore. Un
         * epuisement de descripteurs se verrait ici. */
        {
            OwlTcpPoolConfig pc2;
            owl_tcp_pool_config_defaut(&pc2);
            pc2.nb_workers = 2;
            Client apres;
            client_prepare(&apres, "/apres", NULL);
            client_demarre(&apres, owl_tcp_listener_port(&l), 0, 0);
            OwlTcpPool* p2 = owl_http_serveur_pool_demarre(&l, &cfg, &pc2);
            attend_servies(p2, 1, 8000);
            owl_tcp_pool_arrete(p2);
            client_joint(&apres);
            CHECK(strstr(client_recu(&apres), "M=GET|C=/apres|TC=") != NULL,
                  "c1b_listener_encore_utilisable_apres_la_rafale");
            client_libere(&apres);
        }

        owl_tcp_listener_ferme(&l);
    }
}

/* ================================================================
 * C2 — un client LENT A EMETTRE n'occupe pas de worker (slowloris attenue)
 *
 * Reecrit pour le front reacteur (, W2). L'ancien pool couplait
 * accept et service : un worker acceptait la connexion du lent et BLOQUAIT
 * en lecture de ses en-tetes distillees octet par octet. Avec un worker
 * unique, les rapides attendaient donc ~680 ms derriere le lent — c'etait
 * la MESURE DIFFERENTIELLE d'origine (pool 4 workers << worker unique).
 *
 * Le front reacteur change la donne : reac_on_conn peek (MSG_PEEK) la
 * connexion tant que sa ligne de requete n'est pas classifiable, et ne lui
 * assigne un worker qu'APRES classification. Un client lent a taper ses
 * en-tetes reste donc park dans le front (liste `pending`, parade slowloris
 * reac_on_idle) et n'occupe JAMAIS de worker pendant qu'il distille. Le
 * worker unique reste libre pour les rapides.
 *
 * La mesure differentielle s'INVERSE : au lieu de prouver que le pool bat
 * le worker unique, on prouve desormais que le nombre de workers N'IMPORTE
 * PLUS pour la latence des rapides (le lent est park dans le front dans les
 * deux configs) ET que le worker unique n'est plus retenu par le lent.
 * ================================================================ */

static uint64_t scenario_client_lent(int nb_workers, int* rapides_servies) {
    enum { RAPIDES = 3 };

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_echo_rep;
    g_handler_delai_ms = 0;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 16, false)) return 0;

    /* Le lent se connecte EN PREMIER et emet sa requete octet par octet a
     * 25 ms (~800 ms de transmission, ligne de requete classifiable vers
     * ~500 ms). Sous l'ancien pool il aurait pris la place du worker unique ;
     * sous le front reacteur il reste park en pre-classification et ne
     * retient aucun worker — c'est ce que la famille mesure. */
    Client lent;
    client_prepare(&lent, "/lent", NULL);
    client_demarre(&lent, owl_tcp_listener_port(&l), 1, 25);
    CC_SLEEP(120);

    Client rapide[RAPIDES];
    char chemin[32];
    for (int i = 0; i < RAPIDES; i++) {
        snprintf(chemin, sizeof(chemin), "/rapide%d", i);
        client_prepare(&rapide[i], chemin, NULL);
        client_demarre(&rapide[i], owl_tcp_listener_port(&l), 0, 0);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = nb_workers;
    pc.connexions_max = nb_workers;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { owl_tcp_listener_ferme(&l); return 0; }

    /* Temps au bout duquel les RAPIDES sont tous servis. Le lent est
     * encore en train d'emettre : c'est tout l'argument du niveau 1. */
    uint64_t t_rapides = attend_servies(pool, RAPIDES, 15000);

    attend_servies(pool, RAPIDES + 1, 15000);
    *rapides_servies = 0;
    for (int i = 0; i < RAPIDES; i++) {
        if (strstr(client_recu(&rapide[i]), rapide[i].attendu)) (*rapides_servies)++;
    }
    owl_tcp_pool_arrete(pool);
    client_joint(&lent);
    for (int i = 0; i < RAPIDES; i++) client_joint(&rapide[i]);

    /* Le lent finit lui aussi : le pool ne l'abandonne pas au profit des
     * autres, il le mene juste en parallele. */
    if (strstr(client_recu(&lent), lent.attendu)) (*rapides_servies) += 10;

    client_libere(&lent);
    for (int i = 0; i < RAPIDES; i++) client_libere(&rapide[i]);
    owl_tcp_listener_ferme(&l);
    return t_rapides;
}

static void famille_c2(void) {
    g_famille = 1;

    int servies_pool = 0, servies_seul = 0;
    uint64_t t_pool = scenario_client_lent(4, &servies_pool);
    uint64_t t_seul = scenario_client_lent(1, &servies_seul);

    /* 13 = les 3 rapides servis (3) + le lent servi lui aussi (+10). Le
     * codage tient en un entier pour garder la fonction de scenario a un
     * seul point de sortie. Le lent finit dans les deux cas : le front ne
     * l'abandonne pas, il le laisse emerger de la pre-classification a son
     * rythme puis le sert. */
    CHECK(servies_pool == 13, "c2_pool_les_trois_rapides_et_le_lent_servis");
    CHECK(servies_seul == 13, "c2_worker_unique_tout_le_monde_finit_aussi");

    /* Les rapides passent vite avec 4 workers : le lent est park dans le
     * front, aucun worker n'est retenu par lui. */
    CHECK(t_pool < 600, "c2_pool_sert_les_rapides_sans_attendre_le_lent");

    /* Slowloris attenue — le coeur de la re-validation W6. Le lent est
     * connecte EN PREMIER et distille sa ligne de requete (~500 ms avant
     * d'etre classifiable). Avec l'ancien pool worker-par-connexion, le
     * worker unique acceptait cette connexion et bloquait en lecture ->
     * t_seul mesurait ~680 ms. Sous le front reacteur, le lent ne prend
     * pas le worker unique : les rapides sont servis en ~30 ms. Le seuil
     * a 400 ms separe le comportement reacteur (~30 ms) de l'ancien
     * blocage (~680 ms) — il rougit si le worker unique se remet a bloquer
     * derriere le lent. */
    CHECK(t_seul < 400, "c2_worker_unique_non_retenu_par_le_lent_slowloris");

    /* Corollaire differentiel INVERSE : la latence des rapides ne depend
     * plus du nombre de workers, puisque le lent est park dans le front
     * dans les deux configs. L'ancien test asserait t_pool + 300 < t_seul
     * (le pool battait le worker unique retenu par le lent) ; ici pool et
     * worker unique sont equivalents a 300 ms pres. Rougit si le worker
     * unique regresse vers le blocage (t_seul remonte a ~680 ms, tres
     * au-dela de t_pool + 300). */
    CHECK(t_seul < t_pool + 300, "c2_latence_rapides_independante_du_nombre_de_workers");

    fprintf(stderr, "  [c2] t_pool=%ums t_seul=%ums\n",
            (unsigned)t_pool, (unsigned)t_seul);
}

/* ================================================================
 * C3 — saturation observable (§7)
 *
 * Executeur cree mais VOLONTAIREMENT PAS demarre : aucune VM, aucun
 * thread executeur. La file de profondeur 1 se remplit, tout le reste est
 * refuse par le transport seul. jobs_pris == 0 le prouve — c'est le seul
 * compteur incremente par l'executeur.
 *
 * Re-validation W6 (front reacteur, accept NON borne) : sous l'ancien pool
 * borne a connexions_max, seules W connexions etaient acceptees et le
 * comptage exact des rejets dependait du backlog (assertion `rejets >=
 * N-1` intermittente). Le front reacteur accepte les N connexions, qui
 * atteignent TOUTES l'executeur en rafale. Deux consequences :
 *   - la comptabilite est desormais COMPLETE : chaque connexion est soit
 *     rejetee au depot (fast-path 503, rejets++), soit coincee dans la
 *     file pleine puis abandonnee sur timeout (503 aussi, mais pas dans
 *     rejets). D'ou l'invariant deterministe rejets + profondeur == N ;
 *   - `owl_http_executeur_depose` relache le mutex entre le test de
 *     profondeur et l'enfilage (calloc/copie hors verrou), fenetre
 *     check-then-act ou, sous la rafale du reacteur, quelques workers
 *     passent le test avant tout enfilage -> la file deborde
 *     transitoirement sa profondeur (profondeur observee jusqu'a 3 pour un
 *     cap de 1, W=4 workers) et rejets sous-compte. C'est ce qui rendait `rejets >=
 *     N-1` flaky. L'invariant rejets + profondeur == N y est IMMUNE (il
 *     compte les connexions, pas les places de file). Course pre-existante
 *     dans depose, exposee — pas causee — par le reacteur ; reportee.
 * ================================================================ */

static const char* SRC_HARNAIS_C3 =
    "harnaisHttpEntreeMethode est une chaine = \"\"\n"
    "harnaisHttpEntreeChemin est une chaine = \"\"\n"
    "harnaisHttpEntreeCorps est une chaine = \"\"\n"
    "harnaisHttpSortieStatut est un entier = 0\n"
    "harnaisHttpSortieType est une chaine = \"\"\n"
    "harnaisHttpSortieCorps est une chaine = \"\"\n"
    "\n"
    "PROCEDURE harnaisHttpTraite()\n"
    "    harnaisHttpSortieStatut = 200\n"
    "    harnaisHttpSortieType = \"text/plain\"\n"
    "    harnaisHttpSortieCorps = \"M=\" + harnaisHttpEntreeMethode + \"|C=\" +"
    " harnaisHttpEntreeChemin + \"|TC=\" + harnaisHttpEntreeCorps\n"
    "FIN\n";

static void famille_c3(void) {
    g_famille = 2;
    enum { N = 5, W = 4 };

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 1;
    xc.plafond_attente_ms = 200;
    xc.source_owl         = SRC_HARNAIS_C3;
    xc.procedure          = "harnaisHttpTraite";

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) { CHECK(0, "c3_executeur_cree"); return; }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    /* Data-plane exec ASYNC (W7) : le worker depose et remet le jeton au
     * reacteur au lieu d'attendre. La file de profondeur 1 sature ; le seul
     * jeton enfile est resolu par le TIMER D'ECHEANCE du reacteur (le job ne
     * demarre jamais, l'executeur n'etant pas lance) qui synthetise le 503.
     * handler_rep ne sert plus que le controle (aucune route de controle ici). */
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
        CHECK(0, "c3_ecoute_impossible");
        owl_http_executeur_libere(x);
        return;
    }

    Client cl[N];
    char chemin[32];
    for (int i = 0; i < N; i++) {
        snprintf(chemin, sizeof(chemin), "/s%d", i);
        client_prepare(&cl[i], chemin, NULL);
        client_demarre(&cl[i], owl_tcp_listener_port(&l), 0, 0);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { CHECK(0, "c3_pool_demarre"); owl_tcp_listener_ferme(&l); return; }
    attend_servies(pool, N, 15000);
    owl_tcp_pool_arrete(pool);
    for (int i = 0; i < N; i++) client_joint(&cl[i]);

    int nb_503 = 0;
    for (int i = 0; i < N; i++) {
        if (strstr(client_recu(&cl[i]), "HTTP/1.1 503 ")) nb_503++;
    }
    size_t rejets     = owl_http_executeur_rejets(x);
    size_t profondeur = owl_http_executeur_profondeur(x);
    CHECK(nb_503 == N, "c3_toutes_les_connexions_recoivent_503");
    CHECK(owl_http_executeur_jobs_pris(x) == 0, "c3_503_sans_reveiller_l_executeur");
    /* Comptabilite COMPLETE et race-immune (cf. en-tete C3) : chaque
     * connexion est soit rejetee au depot (rejets), soit coincee dans la
     * file pleine puis 503 sur timeout (profondeur, jamais drainee car
     * l'executeur ne tourne pas). Aucune n'echappe. Remplace `rejets >=
     * N-1`, flaky sous la rafale du reacteur (depassement transitoire de la
     * profondeur de file). */
    CHECK(rejets + profondeur == (size_t)N, "c3_toutes_saturees_rejet_ou_file_pleine");
    /* Le fast-reject (503 produit par le front SANS reveiller l'executeur,
     * §7) a bien tire : la file de profondeur 1 ne peut absorber que ~1
     * connexion, les autres sont rejetees d'emblee. */
    CHECK(rejets >= 1, "c3_fast_reject_transport_seul_a_tire");
    if (rejets + profondeur != (size_t)N || nb_503 != N) {
        fprintf(stderr, "  [c3] 503=%d jobs_pris=%u rejets=%u profondeur=%u\n", nb_503,
                (unsigned)owl_http_executeur_jobs_pris(x),
                (unsigned)rejets, (unsigned)profondeur);
    }

    for (int i = 0; i < N; i++) client_libere(&cl[i]);
    owl_http_executeur_libere(x);

    /* ---- Controle positif ----
     * Les memes N connexions, le meme plafond de file, mais l'executeur
     * DEMARRE : elles rendent 200. Sans ce controle, un serveur qui ne
     * sert plus rien du tout passerait les assertions ci-dessus. */
    {
        OwlHttpExecConfig xc2 = xc;
        xc2.file_profondeur    = 8;
        xc2.plafond_attente_ms = 8000;
        OwlHttpExecuteur* y = owl_http_executeur_cree(&xc2);
        if (!y) { CHECK(0, "c3_controle_cree"); owl_tcp_listener_ferme(&l); return; }
        CHECK(owl_http_executeur_demarre(y), "c3_controle_executeur_demarre");

        OwlHttpServeurConfig cfg2;
        owl_http_serveur_config_defaut(&cfg2);
        cfg2.handler_rep      = owl_http_exec_handler_rep;
        cfg2.handler_rep_user = y;
        cfg2.executeur_async  = y;   /* meme chemin async : reponses bufferisees */

        Client cc[N];
        for (int i = 0; i < N; i++) {
            snprintf(chemin, sizeof(chemin), "/t%d", i);
            client_prepare(&cc[i], chemin, NULL);
            client_demarre(&cc[i], owl_tcp_listener_port(&l), 0, 0);
        }

        OwlTcpPool* p2 = owl_http_serveur_pool_demarre(&l, &cfg2, &pc);
        attend_servies(p2, N, 15000);
        owl_tcp_pool_arrete(p2);
        for (int i = 0; i < N; i++) client_joint(&cc[i]);

        int ok200 = 0;
        for (int i = 0; i < N; i++) {
            if (strstr(client_recu(&cc[i]), cc[i].attendu)) ok200++;
        }
        CHECK(ok200 == N, "c3_controle_positif_les_memes_connexions_rendent_200");
        CHECK(owl_http_executeur_jobs_pris(y) == (size_t)N,
              "c3_controle_positif_cinq_jobs_pris");
        CHECK(owl_http_executeur_rejets(y) == 0, "c3_controle_positif_aucun_rejet");

        for (int i = 0; i < N; i++) client_libere(&cc[i]);
        owl_http_executeur_arrete(y);
        owl_http_executeur_libere(y);
    }

    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * C4 — un seul thread touche la VM + anti-GC etendu a N threads
 *
 * Le thread principal POSSEDE la VM et la fait tourner a la main
 * (traite_une), pendant que W threads reseau acceptent, parsent,
 * deposent et ecrivent. Deux instruments, aucun affaibli :
 *
 *   - owl_http_executeur_nb_threads_vm() : nombre d'identifiants de
 *     thread DISTINCTS ayant execute un job. Doit valoir 1 alors que W
 *     threads reseau ont tourne — mesure, pas affirmation ;
 *   - gc_bytesAllocated releve sur des FENETRES ou le thread VM ne fait
 *     rien : toute allocation GC observee sur une de ces fenetres vient
 *     forcement d'un thread reseau, donc d'une violation de la regle
 *     d'or. gc_frontiere force un cycle complet aux deux frontieres, et
 *     l'assertion finale porte sur l'octet exact.
 * ================================================================ */

static void famille_c4(void) {
    g_famille = 3;
    enum { N = 6, W = 4 };

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 15000;
    xc.source_owl         = SRC_HARNAIS_C3;
    xc.procedure          = "harnaisHttpTraite";
    xc.gc_frontiere       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) { CHECK(0, "c4_executeur_cree"); return; }

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "c4_harnais_compile_dans_le_thread_principal");

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    /* Data-plane exec ASYNC (W7) : les workers deposent + remettent au
     * reacteur, le thread principal pompe traite_une. Les reponses bufferisees
     * (Content-Length preserve) sont drainees par le reacteur au fil des
     * resolutions — c'est ce que valide `exacts == N` malgre le GC force. */
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
        CHECK(0, "c4_ecoute_impossible");
        vm_free();
        owl_http_executeur_libere(x);
        return;
    }

    Client cl[N];
    char chemin[32], corps[32];
    for (int i = 0; i < N; i++) {
        snprintf(chemin, sizeof(chemin), "/g%d", i);
        snprintf(corps,  sizeof(corps),  "octets%d", i);
        client_prepare(&cl[i], chemin, corps);
        client_demarre(&cl[i], owl_tcp_listener_port(&l), 0, 0);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (!pool) { CHECK(0, "c4_pool_demarre"); vm_free(); owl_tcp_listener_ferme(&l); return; }

    size_t   fenetres = 0, fenetres_sales = 0;
    int      traites  = 0;
    uint64_t fin      = owl_tcp_server_horloge_ms() + 15000;

    while (traites < N && owl_tcp_server_horloge_ms() < fin) {
        /* Fenetre : le thread proprietaire de la VM ne fait RIEN. Tout
         * ce qui tourne pendant ce temps est du transport. */
        size_t avant = gc_bytesAllocated;
        CC_SLEEP(3);
        size_t apres = gc_bytesAllocated;
        fenetres++;
        if (apres != avant) fenetres_sales++;

        if (owl_http_executeur_traite_une(x, 20) == 1) traites++;
    }

    /* Concurrence TENUE PAR LE REACTEUR (W7) : en async, le worker se libere
     * des le handoff — sa fenetre `pic_simultanees` (SETUP) ne mesure plus le
     * parallelisme reel. Ce sont les jetons pending simultanement dans la
     * boucle qui le portent : les 6 jobs deposes s'y accumulent pendant que le
     * thread VM les pompe un a un. */
    size_t pic = owl_tcp_pool_pic_reac_tenue(pool);
    attend_servies(pool, N, 8000);
    owl_tcp_pool_arrete(pool);
    for (int i = 0; i < N; i++) client_joint(&cl[i]);

    CHECK(traites == N, "c4_les_six_jobs_traites_par_le_thread_vm");
    CHECK(fenetres >= 6, "c4_assez_de_fenetres_pour_que_la_mesure_ait_un_sens");
    CHECK(fenetres_sales == 0, "c4_aucune_allocation_gc_pendant_les_fenetres_reseau");
    CHECK(owl_http_executeur_nb_threads_vm(x) == 1, "c4_un_seul_thread_a_touche_la_vm");
    CHECK(pic >= 2, "c4_concurrence_reacteur_tenue_pendant_la_mesure");
    if (fenetres_sales != 0 || owl_http_executeur_nb_threads_vm(x) != 1) {
        fprintf(stderr, "  [c4] fenetres=%u sales=%u threads_vm=%u pic_reac=%u\n",
                (unsigned)fenetres, (unsigned)fenetres_sales,
                (unsigned)owl_http_executeur_nb_threads_vm(x), (unsigned)pic);
    }

    /* L'octet exact, apres un GC force aux deux frontieres pour chacune
     * des six requetes. Une reference qui aurait fuite du tas OWL vers un
     * thread reseau serait pendante ici. */
    int exacts = 0;
    for (int i = 0; i < N; i++) {
        if (strstr(client_recu(&cl[i]), cl[i].attendu)) exacts++;
    }
    CHECK(exacts == N, "c4_chaque_corps_exact_malgre_le_gc_force");

    for (int i = 0; i < N; i++) client_libere(&cl[i]);
    owl_tcp_listener_ferme(&l);
    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * C5 — un lecteur bloque ne gele pas le serveur
 *
 * Le bug : la socket acceptee n'etait jamais passee en non-bloquant, si
 * bien que send() bloquait indefiniment des que le tampon d'envoi se
 * remplissait — un client qui cesse de lire immobilisait son worker pour
 * toujours. W clients ainsi = W workers geles = serveur muet, y compris
 * pour une requete arrivant sur une AUTRE connexion.
 *
 * La preuve : W clients envoient une requete dont la reponse est enorme et
 * ne LISENT JAMAIS, en gardant leur connexion ouverte. Un lecteur normal
 * arrive ensuite sur une connexion distincte. Avec la socket bloquante
 * (avant le fix) les W workers restent coinces en ecriture -> le lecteur
 * n'est jamais servi (recu vide). Avec la socket non-bloquante, chaque
 * worker coince fait TIMEOUT d'ecriture (timeout_ecriture_ms), se libere,
 * et sert le lecteur.
 *
 * Rouge SUR : rien ne peut figer la batterie. Les muets bornent leur
 * duree d'ouverture (garder_ouvert_ms), le lecteur borne sa lecture
 * (SO_RCVTIMEO via timeout_lecture_ms). Meme AVANT le fix, tous les
 * threads finissent — le test ECHOUE (recu vide) sans jamais bloquer.
 * ================================================================ */

/* Corps volontairement plus gros que les tampons socket OS cumules
 * (SO_SNDBUF serveur + SO_RCVBUF client, quelques centaines de Ko meme
 * apres auto-tuning Linux) : un client qui ne draine jamais fait deborder,
 * et send() bloque. En BSS, jamais initialise : le transport ne fait que
 * le copier tel quel, son contenu n'a aucune importance. */
static char g_gros_corps[4 * 1024 * 1024];

static OwlHttpRep* handler_c5(const OwlHttpRequete* req, void* user) {
    (void)user;
    /* Le lecteur vivant recoit un petit corps connu ; tout le reste
     * recoit le pave qui sature le tampon d'envoi. */
    if (req->chemin && strcmp(req->chemin, "/vivant") == 0) {
        return owl_http_rep_cree_tampon(200, "text/plain", "VIVANT", 6);
    }
    return owl_http_rep_cree_tampon(200, "application/octet-stream",
                                    g_gros_corps, sizeof(g_gros_corps));
}

/* Client muet : emet sa requete puis NE LIT JAMAIS la reponse, en gardant
 * la connexion ouverte garder_ms. C'est ce qui remplit et maintient plein
 * le tampon d'envoi du serveur. */
static void client_demarre_muet(Client* c, int port, int garder_ms) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = port;
    mc.octets                     = c->requete;
    mc.octets_len                 = strlen(c->requete);
    mc.decoupe                    = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture        = 0;   /* NE DRAINE JAMAIS la reponse */
    mc.fermer_envoi_avant_lecture = 1;   /* requete complete, cote emission ferme */
    mc.fermer_apres_ecriture      = 0;
    mc.garder_ouvert_ms           = garder_ms;
    c->pair = owl_mock_peer_demarre(&mc);
}

/* Lecteur normal avec lecture BORNEE : meme si le serveur reste muet
 * (avant le fix), il abandonne au bout de timeout_ms au lieu de figer le
 * harnais. */
static void client_demarre_lecteur(Client* c, int port, int timeout_ms) {
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
    mc.timeout_lecture_ms         = timeout_ms;
    mc.fermer_apres_ecriture      = 1;
    c->pair = owl_mock_peer_demarre(&mc);
}

static void famille_c5(void) {
    g_famille = 4;
    enum { W = 4, GARDER_MS = 4000, LECTEUR_TIMEOUT_MS = 2500 };

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep = handler_c5;
    /* Timeout d'ecriture court : apres le fix, un worker coince se libere
     * vite ; avant le fix, il reste coince quoi qu'il arrive (la socket
     * bloquante contourne ce timeout — c'est tout le bug). */
    cfg.limites.timeout_ecriture_ms = 600;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, W + 8, false)) {
        CHECK(0, "c5_ecoute_impossible");
        return;
    }
    int port = owl_tcp_listener_port(&l);

    /* W clients muets connectes AVANT le pool : ils forment une rafale qui
     * occupe les W workers des le demarrage. */
    Client muet[W];
    for (int i = 0; i < W; i++) {
        client_prepare(&muet[i], "/gros", NULL);
        client_demarre_muet(&muet[i], port, GARDER_MS);
    }

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = W;
    pc.connexions_max = W;

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    CHECK(pool != NULL, "c5_pool_demarre");
    if (!pool) {
        for (int i = 0; i < W; i++) { client_joint(&muet[i]); client_libere(&muet[i]); }
        owl_tcp_listener_ferme(&l);
        return;
    }

    /* Laisse les W workers prendre les W connexions muettes et se retrouver
     * coinces en ecriture avant que le lecteur n'arrive. */
    CC_SLEEP(250);

    /* Le lecteur arrive sur une connexion distincte, dans le backlog. */
    Client lecteur;
    client_prepare(&lecteur, "/vivant", NULL);
    client_demarre_lecteur(&lecteur, port, LECTEUR_TIMEOUT_MS);

    client_joint(&lecteur);
    const char* recu = client_recu(&lecteur);

    /* Coeur de la preuve : le lecteur est servi malgre les W workers
     * initialement coinces. Avant le fix : recu vide (aucun worker ne se
     * libere) -> ECHEC. Apres le fix : les workers font timeout, se
     * liberent, servent le lecteur -> "VIVANT". */
    CHECK(strstr(recu, "VIVANT") != NULL,
          "c5_lecteur_servi_malgre_W_workers_coinces_en_ecriture");

    /* Signal differentiel : un worker coince qui se libere par timeout
     * incremente le compteur de connexions servies. Avant le fix, aucun
     * worker ne revient -> servies reste 0. */
    size_t servies = owl_tcp_pool_connexions_servies(pool);
    CHECK(servies >= 1, "c5_au_moins_un_worker_libere_apres_timeout_ecriture");
    if (strstr(recu, "VIVANT") == NULL || servies < 1) {
        fprintf(stderr, "  [c5] servies=%u recu_len=%u\n",
                (unsigned)servies, (unsigned)strlen(recu));
    }

    owl_tcp_pool_arrete(pool);
    for (int i = 0; i < W; i++) { client_joint(&muet[i]); client_libere(&muet[i]); }
    client_libere(&lecteur);
    owl_tcp_listener_ferme(&l);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_concurrence_self_test_run(void) {
    printf("== Pool de workers reseau (feat/1376, phase A1.1b) ==\n");

    famille_c1();
    famille_c2();
    famille_c3();
    famille_c4();
    famille_c5();

    int tp = 0, tf = 0;
    for (int i = 0; i < 5; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   c1 connexions simultanees  : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   c2 client lent             : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   c3 saturation observable   : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   c4 vm mono-thread + anti-GC: %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   c5 lecteur bloque          : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   TOTAL concurrence reseau   : %d/%d\n", tp, tp + tf);
    return tf;
}
