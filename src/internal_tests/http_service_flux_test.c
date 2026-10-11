// SPDX-License-Identifier: AGPL-3.0-only
/* http_service_flux_test.c — 3e mode d'execution du DSL de service ( P3).
 *
 * Ticket : 
 * Design gele : Piece #1 (3e mode = sous-mode de execute_service_job, ouverture
 * paresseuse au 1er FluxEcrit) + concurrence  (yield cooperatif au push
 * de flux) + statut fige au 1er chunk ( §5).
 *
 * *** DIGUE TEST-FIRST ***
 * Ce fichier definit le contrat AVANT que le branchement flux de
 * execute_service_job (http_exec.c) n'existe. A la pose des tests, les familles
 * F0..F5 sont ROUGES POUR LA BONNE RAISON : sans le 3e mode, une route
 * Flux=fluxSSE tourne comme une route bufferisee (le corps ne streame pas, aucun
 * chunk data: sur le fil, aucun flux_demarre), et FluxEcrit leve « hors d'un
 * flux » puisque le slot ambiant n'est jamais arme cote service.
 *
 * *** HORS BATTERIE PAR DEFAUT JUSQU'AU VERT ***
 * Comme vm_concurrence_famine_test.c et --self-test-net-crash-1723
 * : le point d'entree --self-test-http-service-flux est callable et
 * cablable, mais N'EST PAS ajoute a run_all_checks.sh tant que l'impl (Piece #1)
 * n'a pas rendu ces CHECK verts — sinon `make all` casserait pour tous les
 * agents en parallele. L'implementeur cable la batterie APRES le vert, jamais
 * l'inverse, et ne modifie PAS les assertions (elles sont la digue).
 *
 * Six familles, chacune une PREUVE :
 *
 *   F0. E2E SSE MULTI-CHUNK (socket loopback) : une route Flux=fluxSSE dont
 *       surRequete fait 3 FluxEcrit -> le client recoit 3 morceaux framees
 *       `data: <payload>\n\n` en Transfer-Encoding: chunked, statut 200,
 *       Content-Type text/event-stream. Le corps streame VRAIMENT (au lieu
 *       d'un corps bufferise unique).
 *   F1. E2E OCTETS (socket loopback) : une route Flux=fluxOctets -> les
 *       morceaux partent BRUTS (aucun prefixe `data: `), concatenes sur le fil.
 *   F2. STATUT FIGE / MoiMeme-present (socket loopback) : surRequete pose
 *       MoiMeme..Reponse..CodeEtat = httpCree (201) AVANT le 1er FluxEcrit ->
 *       la ligne de statut du flux est « HTTP/1.1 201 » (fige a l'ouverture
 *       paresseuse,  §5). Couvre le chemin MoiMeme-present non exerce
 *       en P2.
 *   F3. MODE BUFFERISE INTACT (socket loopback) : une route SANS Flux garde
 *       exactement son comportement — succes bufferise 200, et un crash de
 *       handler -> 500 propre (surErreur/500 du chemin bufferise INCHANGE).
 *       Controle de non-regression du 3e mode.
 *   F4. ANTI-CROISEMENT (pool M-drains, drain direct) : deux routes flux
 *       concurrentes /flux-a (octets 'A') et /flux-b (octets 'B'), chacune
 *       poussant au-dela de la borne memoire donc PARQUEE a son point de yield
 *      . Drainees ensuite : le flux de A ne contient QUE des 'A',
 *       celui de B QUE des 'B'. Verrouille que FluxEcrit vise le flux AMBIANT
 *       (slot park/restore, P2) et JAMAIS x->jeton_courant (qu'un 2e drain a pu
 *       ecraser pendant le park).
 *   F5. ANTI-FAMINE NIVEAU SERVICE (analogue vm_concurrence_famine_test.c F1,
 *       mais via de VRAIES routes flux/service) : une route flux lente /lent
 *       parquee sur la contre-pression (jamais drainee) ne doit pas affamer une
 *       route saine bufferisee /sain deposee juste apres. Controle differentiel
 *       OWL_VM_THREADS=1 : un seul drain reste affame AVANT ET APRES l'impl.
 *
 * Montage : familles E2E via le patron joue_e2e (socket loopback + OwlMockPeer,
 * calque de http_dsl_service_test.c) ; familles concurrence via depose/attend/
 * drain direct (calque de vm_concurrence_famine_test.c), sans socket, pour un
 * controle deterministe du park.
 *
 * Execute via `owl --self-test-http-service-flux`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_exec.h"
#include "../http_server.h"
#include "../http_service_runtime.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../gc.h"

extern void compiler_reset_poo_state(void);

#ifdef _WIN32
  #include <windows.h>
  #define SF_SLEEP(ms)      Sleep((DWORD)(ms))
  #define SF_SETENV_1(nom)  _putenv_s((nom), "1")
  #define SF_UNSETENV(nom)  _putenv_s((nom), "")
#else
  #include <time.h>
  static void sf_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define SF_SLEEP(ms)      sf_sleep_ms((int)(ms))
  #define SF_SETENV_1(nom)  setenv((nom), "1", 1)
  #define SF_UNSETENV(nom)  unsetenv((nom))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define NB_FAMILLES 6
static int g_pass[NB_FAMILLES] = {0, 0, 0, 0, 0, 0};
static int g_fail[NB_FAMILLES] = {0, 0, 0, 0, 0, 0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL SVF%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Sources OWL
 * ================================================================ */

static const char* SRC_SSE =
    "ServiceFluxSSE est un ServiceREST\n"
    "    Diffuse est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"sse\"]\n"
    "            Flux    = fluxSSE\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            FluxEcrit(\"alpha\")\n"
    "            FluxEcrit(\"beta\")\n"
    "            FluxEcrit(\"gamma\")\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

static const char* SRC_OCTETS =
    "ServiceFluxOctets est un ServiceREST\n"
    "    Diffuse est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"raw\"]\n"
    "            Flux    = fluxOctets\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            FluxEcrit(\"AB\")\n"
    "            FluxEcrit(\"CD\")\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Statut fige : le CodeEtat pose AVANT le 1er FluxEcrit gouverne la ligne de
 * statut du flux (201 httpCree). */
static const char* SRC_STATUT =
    "ServiceFluxStatut est un ServiceREST\n"
    "    Diffuse est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"sse\"]\n"
    "            Flux    = fluxSSE\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpCree\n"
    "            FluxEcrit(\"un\")\n"
    "            FluxEcrit(\"deux\")\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Mode bufferise intact : une route sans Flux (succes) et une route sans Flux
 * qui crashe (-> 500 propre, chemin bufferise inchange). */
static const char* SRC_BUFFERISE =
    "ServiceBuf est un ServiceREST\n"
    "    Ok est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ok\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"bufok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Boum est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"boum\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            nA est un entier = 1\n"
    "            nB est un entier = 0\n"
    "            nC est un entier = nA / nB\n"
    "            MoiMeme..Reponse..Corps = \"jamais\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Anti-croisement : deux routes flux octets, chacune pousse ~1 Mo TROIS fois.
 * Le 2e push excede la borne memoire -> PARK ; apres reprise, le 3e FluxEcrit
 * RELIT le slot ambiant (owl_vm_flux_courant_get). C'est ce 3e push post-yield
 * qui exerce reellement le park/restore du slot : un restore casse ferait viser
 * a A le flux de B (2 pushes suffiraient a ne tester que le pointeur C local du
 * push parque, pas la relecture du slot). Repete construit le motif ~1 Mo. */
static const char* SRC_CROISEMENT =
    "ServiceCroise est un ServiceREST\n"
    "    FluxA est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"flux-a\"]\n"
    "            Flux    = fluxOctets\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sA est une chaine = Repete(\"AAAAAAAAAA\", 104858)\n"
    "            FluxEcrit(sA)\n"
    "            FluxEcrit(sA)\n"
    "            FluxEcrit(sA)\n"
    "        FIN\n"
    "    FIN\n"
    "    FluxB est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"flux-b\"]\n"
    "            Flux    = fluxOctets\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sB est une chaine = Repete(\"BBBBBBBBBB\", 104858)\n"
    "            FluxEcrit(sB)\n"
    "            FluxEcrit(sB)\n"
    "            FluxEcrit(sB)\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Anti-famine : /lent (flux octets) parque sur la contre-pression (jamais
 * draine), /sain (bufferise) doit se resoudre pendant que /lent est parque. */
static const char* SRC_FAMINE =
    "ServiceFamine est un ServiceREST\n"
    "    Lent est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin  = [\"lent\"]\n"
    "            Flux    = fluxOctets\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sL est une chaine = Repete(\"LLLLLLLLLL\", 104858)\n"
    "            FluxEcrit(sL)\n"
    "            FluxEcrit(sL)\n"
    "        FIN\n"
    "    FIN\n"
    "    Sain est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sain\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"sain-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Patron E2E socket (calque de http_dsl_service_test.c::joue_e2e)
 * ================================================================ */

typedef struct {
    int    statut;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} EchangeE2E;

static EchangeE2E joue_e2e(const char* octets, OwlHttpExecuteur* x) {
    EchangeE2E e;
    memset(&e, 0, sizeof(e));

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) return e;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                       = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                       = "127.0.0.1";
    mc.port                       = owl_tcp_listener_port(&l);
    mc.octets                     = octets;
    mc.octets_len                 = strlen(octets);
    mc.decoupe                    = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture        = 1;
    mc.timeout_lecture_ms         = 5000;
    mc.fermer_apres_ecriture      = 1;
    mc.fermer_envoi_avant_lecture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); return e; }

    e.statut = owl_http_serveur_sert_une_connexion(&l, &cfg, 5000);

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

/* Compte les occurrences non chevauchantes de `motif` dans `s`. */
static int compte_occ(const char* s, const char* motif) {
    int n = 0;
    size_t ml = strlen(motif);
    const char* p = s;
    while ((p = strstr(p, motif)) != NULL) { n++; p += ml; }
    return n;
}

static OwlHttpExecuteur* cree_service(const char* src) {
    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 16;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = src;
    cfg.service_mode       = 1;
    return owl_http_executeur_cree(&cfg);
}

/* ================================================================
 * F0 — E2E SSE multi-chunk
 * ================================================================ */

static void famille_0_sse_multichunk(void) {
    g_famille = 0;
    compiler_reset_poo_state();

    OwlHttpExecuteur* x = cree_service(SRC_SSE);
    if (!x) { CHECK(0, "sse_cree"); return; }
    CHECK(owl_http_executeur_demarre(x), "sse_demarre");

    EchangeE2E e = joue_e2e("GET /sse HTTP/1.1\r\nHost: a\r\n\r\n", x);

    CHECK(strstr(e.reponse, "HTTP/1.1 200 OK\r\n") == e.reponse, "sse_statut_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked") != NULL, "sse_chunked");
    CHECK(strstr(e.reponse, "text/event-stream") != NULL, "sse_content_type");
    /* Les 3 morceaux SSE, chacun frame `data: <payload>\n\n`. */
    CHECK(strstr(e.reponse, "data: alpha\n\n") != NULL, "sse_chunk_alpha");
    CHECK(strstr(e.reponse, "data: beta\n\n")  != NULL, "sse_chunk_beta");
    CHECK(strstr(e.reponse, "data: gamma\n\n") != NULL, "sse_chunk_gamma");
    CHECK(compte_occ(e.reponse, "data: ") == 3, "sse_exactement_3_chunks");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * F1 — E2E octets bruts
 * ================================================================ */

static void famille_1_octets(void) {
    g_famille = 1;
    compiler_reset_poo_state();

    OwlHttpExecuteur* x = cree_service(SRC_OCTETS);
    if (!x) { CHECK(0, "oct_cree"); return; }
    CHECK(owl_http_executeur_demarre(x), "oct_demarre");

    EchangeE2E e = joue_e2e("GET /raw HTTP/1.1\r\nHost: a\r\n\r\n", x);

    CHECK(strstr(e.reponse, "HTTP/1.1 200 OK\r\n") == e.reponse, "oct_statut_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked") != NULL, "oct_chunked");
    /* Octets bruts : aucun prefixe SSE, et les payloads apparaissent tels quels. */
    CHECK(strstr(e.reponse, "data: ") == NULL, "oct_pas_de_prefixe_sse");
    CHECK(strstr(e.reponse, "AB") != NULL, "oct_payload_ab");
    CHECK(strstr(e.reponse, "CD") != NULL, "oct_payload_cd");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * F2 — statut fige (MoiMeme-present) : 201 avant le 1er chunk
 * ================================================================ */

static void famille_2_statut_fige(void) {
    g_famille = 2;
    compiler_reset_poo_state();

    OwlHttpExecuteur* x = cree_service(SRC_STATUT);
    if (!x) { CHECK(0, "st_cree"); return; }
    CHECK(owl_http_executeur_demarre(x), "st_demarre");

    EchangeE2E e = joue_e2e("GET /sse HTTP/1.1\r\nHost: a\r\n\r\n", x);

    /* Le CodeEtat pose AVANT le 1er FluxEcrit gouverne la ligne de statut. */
    CHECK(strstr(e.reponse, "HTTP/1.1 201 ") == e.reponse, "st_statut_201_fige");
    CHECK(strstr(e.reponse, "data: un\n\n")   != NULL, "st_chunk_un");
    CHECK(strstr(e.reponse, "data: deux\n\n") != NULL, "st_chunk_deux");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * F3 — mode bufferise intact (succes 200 + crash 500)
 * ================================================================ */

static void famille_3_bufferise_intact(void) {
    g_famille = 3;
    compiler_reset_poo_state();

    OwlHttpExecuteur* x = cree_service(SRC_BUFFERISE);
    if (!x) { CHECK(0, "buf_cree"); return; }
    CHECK(owl_http_executeur_demarre(x), "buf_demarre");

    /* Route sans Flux : reponse bufferisee normale, PAS de chunked. */
    EchangeE2E ok = joue_e2e("GET /ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
    CHECK(strstr(ok.reponse, "HTTP/1.1 200 OK\r\n") == ok.reponse, "buf_ok_200");
    CHECK(strstr(ok.reponse, "Transfer-Encoding: chunked") == NULL, "buf_ok_pas_chunked");
    CHECK(strstr(ok.reponse, "bufok") != NULL, "buf_ok_corps");

    /* Crash de handler sans Flux : 500 propre, sans fuite du corps. */
    EchangeE2E boum = joue_e2e("GET /boum HTTP/1.1\r\nHost: a\r\n\r\n", x);
    CHECK(strstr(boum.reponse, "HTTP/1.1 500") == boum.reponse, "buf_crash_500");
    CHECK(strstr(boum.reponse, "jamais") == NULL, "buf_crash_pas_de_fuite_corps");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * F4 — anti-croisement : deux flux parques, aucun croisement
 * ================================================================ */

/* Draine un flux jusqu'a son terminal, accumule les octets et compte les
 * occurrences des deux motifs 'A'/'B'. Borne d'iterations = garde anti-hang. */
typedef struct { long nbA; long nbB; size_t total; int terminal; } BilanDrain;

static BilanDrain draine_et_compte(OwlHttpRep* flux) {
    BilanDrain b; memset(&b, 0, sizeof(b));
    for (int garde = 0; garde < 2000000; garde++) {
        char* d = NULL; size_t l = 0;
        OwlHttpFluxTire t = owl_http_rep_flux_tire(flux, &d, &l);
        if (t == OWL_HTTP_FLUX_TIRE_DONNEE) {
            for (size_t i = 0; i < l; i++) {
                if (d[i] == 'A') b.nbA++;
                else if (d[i] == 'B') b.nbB++;
            }
            b.total += l;
            free(d);
            continue;
        }
        if (t == OWL_HTTP_FLUX_TIRE_VIDE) { SF_SLEEP(1); continue; }
        /* FINI / AVORTE / MORT : plus de donnees. */
        b.terminal = (int)t;
        break;
    }
    return b;
}

static void famille_4_anti_croisement(void) {
    g_famille = 4;
    compiler_reset_poo_state();

    OwlHttpExecuteur* x = cree_service(SRC_CROISEMENT);
    if (!x) { CHECK(0, "cx_cree"); return; }
    if (!owl_http_executeur_demarre(x)) {
        CHECK(0, "cx_demarre");
        owl_http_executeur_libere(x);
        return;
    }

    OwlHttpRequete rA, rB;
    memset(&rA, 0, sizeof(rA)); snprintf(rA.methode, sizeof(rA.methode), "GET"); rA.chemin = "/flux-a";
    memset(&rB, 0, sizeof(rB)); snprintf(rB.methode, sizeof(rB.methode), "GET"); rB.chemin = "/flux-b";
    OwlHttpJeton *jA = NULL, *jB = NULL;
    owl_http_executeur_depose(x, &rA, &jA);
    owl_http_executeur_depose(x, &rB, &jB);

    /* Les deux handlers demarrent sur le pool, ouvrent leur flux, et PARQUENT
     * sur leur 2e push (rien ne draine encore). attend rend chaque flux des
     * flux_demarre (1er chunk pret). */
    OwlHttpRep *fA = NULL, *fB = NULL;
    OwlHttpExecStatut sA = owl_http_executeur_attend(x, jA, &fA);
    OwlHttpExecStatut sB = owl_http_executeur_attend(x, jB, &fB);
    int fluxA = (sA == OWL_HTTP_EXEC_OK && fA != NULL && owl_http_rep_est_flux(fA));
    int fluxB = (sB == OWL_HTTP_EXEC_OK && fB != NULL && owl_http_rep_est_flux(fB));
    CHECK(fluxA, "cx_flux_a_demarre");
    CHECK(fluxB, "cx_flux_b_demarre");

    BilanDrain bA, bB; memset(&bA, 0, sizeof(bA)); memset(&bB, 0, sizeof(bB));
    if (fluxA) { bA = draine_et_compte(fA); owl_http_rep_flux_marque_consommateur_fini(fA); owl_http_rep_flux_relache(fA); }
    else if (fA) owl_http_rep_libere(fA);
    if (fluxB) { bB = draine_et_compte(fB); owl_http_rep_flux_marque_consommateur_fini(fB); owl_http_rep_flux_relache(fB); }
    else if (fB) owl_http_rep_libere(fB);

    /* AUCUN croisement : le flux de A ne contient QUE des 'A', celui de B QUE
     * des 'B'. Un slot ambiant mal park/restore ferait fuir un push de A dans
     * le flux de B (ou l'inverse) -> nbB(A) != 0 ou nbA(B) != 0. */
    CHECK(bA.nbA > 0 && bA.nbB == 0, "cx_flux_a_que_des_A");
    CHECK(bB.nbB > 0 && bB.nbA == 0, "cx_flux_b_que_des_B");
    /* Chaque flux a livre ses TROIS pushes (~3 Mo) : le 3e est emis APRES la
     * reprise du park, donc apres relecture du slot ambiant -> preuve que le
     * restore a bien ramene CE ctx (et pas celui de l'autre flux concurrent). */
    CHECK(bA.total >= 3000000, "cx_flux_a_trois_pushes");
    CHECK(bB.total >= 3000000, "cx_flux_b_trois_pushes");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * F5 — anti-famine niveau service
 * ================================================================ */

typedef struct {
    OwlHttpExecStatut statutSain;
    uint64_t          tSain;
    int               demarre;
} ScenarioFamine;

static ScenarioFamine famine_joue(int forcer_un_thread) {
    ScenarioFamine res; memset(&res, 0, sizeof(res));
    if (forcer_un_thread) SF_SETENV_1("OWL_VM_THREADS");

    compiler_reset_poo_state();
    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 16;
    cfg.plafond_attente_ms = 400;   /* borne l'attente d'un job jamais demarre */
    cfg.source_owl         = SRC_FAMINE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    res.demarre = (x != NULL) && owl_http_executeur_demarre(x);
    if (!res.demarre) {
        if (x) owl_http_executeur_libere(x);
        res.statutSain = OWL_HTTP_EXEC_SATURE;
        if (forcer_un_thread) SF_UNSETENV("OWL_VM_THREADS");
        return res;
    }

    OwlHttpRequete rL, rS;
    memset(&rL, 0, sizeof(rL)); snprintf(rL.methode, sizeof(rL.methode), "GET"); rL.chemin = "/lent";
    memset(&rS, 0, sizeof(rS)); snprintf(rS.methode, sizeof(rS.methode), "GET"); rS.chemin = "/sain";
    OwlHttpJeton *jL = NULL, *jS = NULL;
    owl_http_executeur_depose(x, &rL, &jL);
    owl_http_executeur_depose(x, &rS, &jS);

    /* Laisse le pool entamer /lent et le parquer sur sa contre-pression avant
     * de mesurer /sain (sinon on mesurerait un retard de demarrage). */
    SF_SLEEP(150);

    OwlHttpRep* repS = NULL;
    uint64_t t0 = owl_tcp_server_horloge_ms();
    res.statutSain = owl_http_executeur_attend(x, jS, &repS);
    res.tSain      = owl_tcp_server_horloge_ms() - t0;
    if (repS) owl_http_rep_libere(repS);

    /* Teardown SANS HANG : debloque /lent (flux via essaie -> consommateur_fini)
     * AVANT arrete (dont l'attente de fin de thread est sans borne). */
    OwlHttpRep* repL = NULL;
    for (int i = 0; i < 400; i++) {
        OwlHttpEssaie es = owl_http_executeur_essaie(x, jL, &repL);
        if (es == OWL_HTTP_ESSAIE_FLUX && repL) break;
        if (es == OWL_HTTP_ESSAIE_BUFFERISE) { if (repL) { owl_http_rep_libere(repL); repL = NULL; } break; }
        if (es == OWL_HTTP_ESSAIE_SATURE) break;
        SF_SLEEP(5);
    }
    if (repL && owl_http_rep_est_flux(repL)) {
        owl_http_rep_flux_marque_consommateur_fini(repL);
        owl_http_rep_flux_relache(repL);
    } else if (repL) {
        owl_http_rep_libere(repL);
    }
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    if (forcer_un_thread) SF_UNSETENV("OWL_VM_THREADS");
    return res;
}

static void famille_5_anti_famine(void) {
    g_famille = 5;

    ScenarioFamine cible    = famine_joue(0);
    ScenarioFamine controle = famine_joue(1);

    fprintf(stderr,
            "  [f5] cible(defaut) statutSain=%s tSain=%lums | "
            "controle(OWL_VM_THREADS=1) statutSain=%s tSain=%lums\n",
            cible.statutSain    == OWL_HTTP_EXEC_OK ? "OK" : "SATURE", (unsigned long)cible.tSain,
            controle.statutSain == OWL_HTTP_EXEC_OK ? "OK" : "SATURE", (unsigned long)controle.tSain);

    CHECK(cible.demarre,    "famine_cible_demarre");
    CHECK(controle.demarre, "famine_controle_demarre");

    /* CIBLE : /sain se resout pendant que /lent est parque (yield cooperatif). */
    CHECK(cible.statutSain == OWL_HTTP_EXEC_OK, "famine_sain_se_resout_pendant_lent_parque");
    CHECK(cible.tSain < 350, "famine_sain_resout_vite_sans_attendre_le_plafond");

    /* CONTROLE differentiel : un seul drain reste affame (le thread unique est
     * bloque dans /lent, aucun autre pour servir /sain). Doit rester vrai
     * AVANT ET APRES l'impl -> rend l'assertion cible non triviale. */
    CHECK(controle.statutSain != OWL_HTTP_EXEC_OK, "famine_controle_un_seul_thread_reste_affame");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_service_flux_self_test_run(void);

int http_service_flux_self_test_run(void) {
    fprintf(stderr, "[http_service_flux] feat/1834 P3 — 3e mode d'execution flux\n");

    famille_0_sse_multichunk();
    famille_1_octets();
    famille_2_statut_fige();
    famille_3_bufferise_intact();
    famille_4_anti_croisement();
    famille_5_anti_famine();

    int total_pass = 0, total_fail = 0;
    for (int i = 0; i < NB_FAMILLES; i++) { total_pass += g_pass[i]; total_fail += g_fail[i]; }
    fprintf(stderr, "[http_service_flux] %d passes, %d echecs\n", total_pass, total_fail);
    return total_fail;
}
