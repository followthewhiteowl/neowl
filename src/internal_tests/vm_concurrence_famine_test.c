// SPDX-License-Identifier: AGPL-3.0-only
/* vm_concurrence_famine_test.c — digue anti-famine VM,  (LOT 0).
 *
 * Ticket : 
 * Contrat verrouille :  §3 (« un handler s'execute atomiquement
 * entre deux points de yield explicites ; interleaving UNIQUEMENT aux
 * yields ; jamais de preemption ; zero data race user-land »).
 *
 * CE FICHIER EST UNE DIGUE TEST-FIRST : elle definit le contrat AVANT que
 * le mecanisme qui le rend vrai (pool N-threads exécuteur + GIL coopératif
 * relache aux points de yield, LOTs 1-4 du ticket) n'existe. Aujourd'hui
 * `owl_http_executeur_demarre` lance un seul thread (hx_thread_fn, voir
 * http_exec.c) : les deux familles ci-dessous sont donc ROUGES POUR LA
 * BONNE RAISON (un handler qui attend affame le reste), et c'est voulu.
 *
 * *** CABLAGE BATTERIE : DIFFERE AU LOT 4 de  ***
 * Ce fichier compile et peut etre execute manuellement (cf. rapport
 * test_writer), mais N'EST PAS appele par `--self-test-net` ni par aucune
 * autre entree de `main.c` : le mecanisme qu'il verifie n'existe pas
 * encore, le cabler dans la batterie maintenant romprait `make all` /
 * `--self-test-net` pour tous les agents en parallele sur ce depot.
 * Precedent : `--self-test-net-crash-1723`, delibrement hors de
 * la liste `net_familles`. L'implementeur (LOTs 1-4) fait passer les CHECK
 * ci-dessous au vert PUIS ajoute l'appel dans `main.c` (`net_familles`),
 * jamais l'inverse, et ne modifie PAS les assertions pour les faire passer
 * (elles sont la digue, pas un detail d'implementation).
 *
 * Deux familles, chacune une PREUVE :
 *
 *   F1. Anti-famine (analogue VM du gate anti-hang reacteur ) :
 *       un handler S « lent » pousse un flux au-dela de sa capacite alors
 *       que rien ne le draine -> il bloque sur `non_plein`
 *       (owl_http_rep_flux_pousse, http_exec.c). Un handler H « sain »,
 *       depose juste apres dans la MEME file, doit se resoudre PENDANT
 *       que S reste parque. Controle differentiel (style C2,
 *       http_concurrence_test.c) : la meme sequence forcee a UN SEUL
 *       thread executeur (arbitrage A4, `OWL_VM_THREADS=1`) doit rester
 *       affamee AVANT ET APRES l'implementation — sinon l'assertion
 *       principale ne prouverait rien (un pool qui « resout » H sans
 *       jamais vraiment serialiser personne ne serait pas credible).
 *
 *   F2. Contrat de determinisme  §3 : deux handlers touchant un
 *       meme etat partage. Le second (B) doit pouvoir progresser PENDANT
 *       que le premier (A) est parque a son point de yield, et A ne doit
 *       voir le changement de B qu'APRES avoir repris — jamais avant,
 *       jamais « au milieu » d'un calcul (ici, il n'y a structurellement
 *       qu'un seul point de yield par handler : la lecture "avant" et la
 *       lecture "apres" sont les deux seules occasions d'observer l'etat,
 *       ce qui suffit a exprimer le contrat sans instrumenter le
 *       bytecode).
 *
 * Montage volontairement au niveau C `http_exec` (seam producteur,
 * `OwlHttpProdCtx` / `owl_http_prod_ouvre_flux` / `owl_http_rep_flux_pousse`),
 * PAS la surface OWL `FluxÉcrit` (, n'existe pas encore) : c'est
 * exactement le seam documente en tete de http_exec.h (« le pont OWL
 * viendra en phase B ; ce seam est le point de test C »).
 *
 * Aucune modification de vm.c / http_exec.c / gc.c : uniquement l'API
 * publique deja exportee par http_exec.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_exec.h"
#include "../http_server.h"

#ifdef _WIN32
  #include <windows.h>
  #define CC_SLEEP(ms) Sleep((DWORD)(ms))
  #define CC_SETENV_1(nom) _putenv_s((nom), "1")
  #define CC_UNSETENV(nom) _putenv_s((nom), "")
#else
  #include <time.h>
  static void cc_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define CC_SLEEP(ms) cc_sleep_ms((int)(ms))
  #define CC_SETENV_1(nom) setenv((nom), "1", 1)
  #define CC_UNSETENV(nom) unsetenv((nom))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define NB_FAMILLES 2
static int g_pass[NB_FAMILLES] = {0, 0};
static int g_fail[NB_FAMILLES] = {0, 0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL VMF%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

static void requete_simple(OwlHttpRequete* req, const char* chemin) {
    memset(req, 0, sizeof(*req));
    snprintf(req->methode, sizeof(req->methode), "GET");
    req->chemin = chemin;
}

/* ================================================================
 * F1 — anti-famine : S parque sur son 2e push, H doit progresser
 * ================================================================ */

static volatile OwlHttpRep* g_s_flux           = NULL;
static volatile int         g_s_flux_ready     = 0;
static volatile int         g_s_apres_1er_push = 0;
static volatile int         g_s_apres_2e_push  = 0;

static void f1_dispatch(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    if (req->chemin && strcmp(req->chemin, "/lent") == 0) {
        /* Handler S : cap minuscule (4 octets). Le 1er push passe toujours
         * (regle D2 : file vide -> un chunk surdimensionne passe quand
         * meme). Le 2e EXCEDE la cap et RIEN ne draine jamais ce flux dans
         * ce test : il bloque sur `non_plein` jusqu'a ce que le teardown
         * appelle owl_http_rep_flux_marque_consommateur_fini. C'est LE
         * point de yield qu' §3 doit un jour relacher. */
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/plain", 4);
        if (!f) return;
        g_s_flux       = f;
        g_s_flux_ready = 1;
        owl_http_rep_flux_pousse(f, "AAAA", 4);
        g_s_apres_1er_push = 1;
        owl_http_rep_flux_pousse(f, "BBBB", 4);   /* BLOQUE ICI */
        g_s_apres_2e_push = 1;
        owl_http_rep_flux_avorte(f);
        owl_http_rep_flux_relache(f);
        return;
    }
    if (req->chemin && strcmp(req->chemin, "/sain") == 0) {
        /* Handler H : reponse triviale, sans aucune dependance a un
         * consommateur externe (payload << cap, jamais de contre-pression).
         * Doit se resoudre immediatement, meme pendant que S est parque. */
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/plain", 1 << 16);
        if (!f) return;
        owl_http_rep_flux_pousse(f, "ok", 2);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
        return;
    }
}

typedef struct {
    OwlHttpExecStatut statutH;
    uint64_t          tH;
    int               s_encore_parque;   /* 1 = g_s_apres_2e_push valait 0 a la mesure */
    int               executeur_demarre;
} ScenarioF1;

/* Joue S puis H sur un executeur frais, mesure H, et TEARDOWN SANS RISQUE
 * DE HANG : debloque S (marque_consommateur_fini) AVANT d'appeler
 * owl_http_executeur_arrete, dont l'attente de fin de thread est SANS
 * borne en POSIX (pthread_join) — un S encore parque y bloquerait a
 * jamais. `forcer_un_thread` force OWL_VM_THREADS=1 (arbitrage A4) : sans
 * effet aujourd'hui (l'env n'est pas encore lu), mais fige le controle
 * differentiel pour apres l'implementation. */
static ScenarioF1 f1_joue_scenario(int forcer_un_thread) {
    ScenarioF1 res;
    memset(&res, 0, sizeof(res));

    g_s_flux = NULL; g_s_flux_ready = 0;
    g_s_apres_1er_push = 0; g_s_apres_2e_push = 0;

    if (forcer_un_thread) CC_SETENV_1("OWL_VM_THREADS");

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.producteur         = f1_dispatch;
    xc.plafond_attente_ms = 400;   /* borne l'attente d'un job jamais demarre (§7/D1) */

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    res.executeur_demarre = (x != NULL) && owl_http_executeur_demarre(x);
    if (!res.executeur_demarre) {
        if (x) owl_http_executeur_libere(x);
        res.statutH          = OWL_HTTP_EXEC_SATURE;
        res.s_encore_parque  = 1;
        if (forcer_un_thread) CC_UNSETENV("OWL_VM_THREADS");
        return res;
    }

    OwlHttpRequete rS, rH;
    requete_simple(&rS, "/lent");
    requete_simple(&rH, "/sain");
    OwlHttpJeton *jS = NULL, *jH = NULL;
    owl_http_executeur_depose(x, &rS, &jS);
    owl_http_executeur_depose(x, &rH, &jH);

    /* Laisse le thread executeur reellement entamer S (1er push) avant de
     * mesurer H — sinon on mesurerait un simple retard de demarrage. */
    for (int i = 0; i < 200 && !g_s_apres_1er_push; i++) CC_SLEEP(5);

    OwlHttpRep* repH = NULL;
    uint64_t t0 = owl_tcp_server_horloge_ms();
    res.statutH = owl_http_executeur_attend(x, jH, &repH);
    res.tH      = owl_tcp_server_horloge_ms() - t0;
    res.s_encore_parque = (g_s_apres_2e_push == 0);
    if (repH) owl_http_rep_libere(repH);

    /* ---- Teardown : debloque S d'abord, arrete ensuite ---- */
    for (int i = 0; i < 400 && !g_s_flux_ready; i++) CC_SLEEP(5);
    if (g_s_flux_ready) owl_http_rep_flux_marque_consommateur_fini((OwlHttpRep*)g_s_flux);
    for (int i = 0; i < 400 && !g_s_apres_2e_push; i++) CC_SLEEP(5);
    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    if (forcer_un_thread) CC_UNSETENV("OWL_VM_THREADS");
    return res;
}

static void famille_f1_anti_famine(void) {
    g_famille = 0;

    ScenarioF1 cible    = f1_joue_scenario(0);
    ScenarioF1 controle = f1_joue_scenario(1);

    fprintf(stderr,
            "  [f1] preuve-rouge locale (executeur mono-thread actuel) : "
            "cible(defaut) statutH=%s tH=%lums s_parque=%d | "
            "controle(OWL_VM_THREADS=1) statutH=%s tH=%lums s_parque=%d\n",
            cible.statutH    == OWL_HTTP_EXEC_OK ? "OK" : "SATURE", (unsigned long)cible.tH,    cible.s_encore_parque,
            controle.statutH == OWL_HTTP_EXEC_OK ? "OK" : "SATURE", (unsigned long)controle.tH, controle.s_encore_parque);

    CHECK(cible.executeur_demarre,    "f1_executeur_cible_demarre");
    CHECK(controle.executeur_demarre, "f1_executeur_controle_demarre");

    /* ---- Assertions CIBLES (LOT 4,  §3) : ROUGES aujourd'hui ----
     * Le pool N-threads + GIL n'existent pas : owl_http_executeur_demarre
     * lance TOUJOURS un seul thread. H starve donc derriere S jusqu'au
     * plafond -> statutH == SATURE et tH ~= plafond_attente_ms. C'est la
     * digue : l'implementeur fait passer ces deux CHECK au vert, il ne les
     * modifie pas. */
    CHECK(cible.statutH == OWL_HTTP_EXEC_OK, "f1_h_se_resout_pendant_que_s_est_parque");
    CHECK(cible.tH < 300, "f1_h_resout_vite_sans_attendre_le_plafond_de_s");

    /* ---- Controle differentiel (style C2) : force a un seul thread ----
     * Doit rester affame AVANT ET APRES l'implementation : un seul thread
     * executeur ne peut pas faire progresser H pendant qu'il est bloque
     * dans S, quel que soit le mecanisme de GIL. Si ce CHECK devient faux
     * un jour, OWL_VM_THREADS=1 n'est plus respecte — regression, pas
     * progres. C'est ce qui rend l'assertion cible non triviale. */
    CHECK(controle.statutH != OWL_HTTP_EXEC_OK, "f1_controle_un_seul_thread_reste_affame");

    /* ---- Invariant valable dans les DEUX configs : S ne progresse jamais
     * pendant qu'on observe H (sinon H n'aurait fait qu'attendre son tour
     * normalement, ce qui ne prouverait rien). ---- */
    CHECK(cible.s_encore_parque    == 1, "f1_s_reste_parque_pendant_la_mesure_cible");
    CHECK(controle.s_encore_parque == 1, "f1_s_reste_parque_pendant_la_mesure_controle");
}

/* ================================================================
 * F2 — contrat §3 : interleaving SEULEMENT aux points de yield
 * ================================================================ */

static volatile long         g_partagee            = 0;
static volatile OwlHttpRep*  g_a_flux              = NULL;
static volatile int          g_a_flux_ready        = 0;
static volatile long         g_a_avant             = -1;
static volatile long         g_a_apres             = -1;
static volatile int          g_a_second_push_fait  = 0;
static volatile int          g_b_fait              = 0;

static void f2_dispatch(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)user;
    if (req->chemin && strcmp(req->chemin, "/yield-lit") == 0) {
        /* Handler A : lit AVANT le yield, se pare au meme point de blocage
         * que S (F1), puis relit APRES avoir repris. Structurellement, il
         * n'y a que deux occasions d'observer l'etat partage — avant et
         * apres — ce qui suffit a exprimer « jamais au milieu ». */
        g_a_avant = g_partagee;
        OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, 200, "text/plain", 4);
        if (!f) return;
        g_a_flux       = f;
        g_a_flux_ready = 1;
        owl_http_rep_flux_pousse(f, "AAAA", 4);
        owl_http_rep_flux_pousse(f, "BBBB", 4);   /* BLOQUE = le point de yield */
        g_a_apres            = g_partagee;
        g_a_second_push_fait = 1;
        owl_http_rep_flux_avorte(f);
        owl_http_rep_flux_relache(f);
        return;
    }
    if (req->chemin && strcmp(req->chemin, "/increment") == 0) {
        /* Handler B : n'ouvre aucun flux -> execute_job renvoie NULL et le
         * job est marque `fait` sans corps ; c'est suffisant, seul l'effet
         * sur `g_partagee` nous interesse ici. */
        g_partagee += 1;
        g_b_fait = 1;
        return;
    }
}

static void famille_f2_contrat_yield(void) {
    g_famille = 1;

    g_partagee = 0; g_a_flux = NULL; g_a_flux_ready = 0;
    g_a_avant = -1; g_a_apres = -1; g_a_second_push_fait = 0; g_b_fait = 0;

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.producteur         = f2_dispatch;
    xc.plafond_attente_ms = 400;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x || !owl_http_executeur_demarre(x)) {
        CHECK(0, "f2_executeur_demarre");
        if (x) owl_http_executeur_libere(x);
        return;
    }

    OwlHttpRequete rA, rB;
    requete_simple(&rA, "/yield-lit");
    requete_simple(&rB, "/increment");
    OwlHttpJeton *jA = NULL, *jB = NULL;
    owl_http_executeur_depose(x, &rA, &jA);
    owl_http_executeur_depose(x, &rB, &jB);

    for (int i = 0; i < 200 && !g_a_flux_ready; i++) CC_SLEEP(5);
    CHECK(g_a_avant == 0, "f2_a_lit_zero_avant_le_yield");

    /* Fenetre d'observation bornee : sous le contrat cible, B doit se
     * terminer PENDANT que A reste parque a son point de yield.
     * Aujourd'hui (executeur mono-thread), B ne peut pas demarrer avant
     * que A rende la main -> il reste a 0 durant toute la fenetre. */
    uint64_t t0 = owl_tcp_server_horloge_ms();
    while (!g_b_fait && g_a_second_push_fait == 0 &&
           owl_tcp_server_horloge_ms() - t0 < 300) {
        CC_SLEEP(5);
    }
    int b_fini_pendant_que_a_parque = (g_b_fait == 1 && g_a_second_push_fait == 0);

    fprintf(stderr,
            "  [f2] preuve-rouge locale : b_fait=%d a_2e_push_fait=%d avant=%ld apres_pas_encore=%ld\n",
            g_b_fait, g_a_second_push_fait, g_a_avant, g_a_apres);

    /* ---- Assertion CIBLE (LOT 4) : ROUGE aujourd'hui. B doit pouvoir
     * progresser et finir PENDANT que A est parque a son yield — c'est la
     * definition meme de la cession cooperative ( §3). */
    CHECK(b_fini_pendant_que_a_parque == 1, "f2_b_progresse_pendant_que_a_est_parque_au_yield");

    /* Debloque A pour qu'il reprenne et relise la globale. */
    if (g_a_flux_ready) owl_http_rep_flux_marque_consommateur_fini((OwlHttpRep*)g_a_flux);
    for (int i = 0; i < 400 && !g_a_second_push_fait; i++) CC_SLEEP(5);

    /* Laisse B, s'il n'a pas encore tourne (cas mono-thread aujourd'hui),
     * le temps de s'executer une fois A rendu le seul thread. */
    for (int i = 0; i < 200 && !g_b_fait; i++) CC_SLEEP(5);

    /* ---- Assertion CIBLE : A ne voit le changement qu'APRES avoir repris
     * a son point de yield — jamais avant, jamais « au milieu » d'un
     * calcul. ROUGE aujourd'hui : B ne peut s'executer qu'APRES que A ait
     * deja fini (mono-thread), donc `g_a_apres` a ete lu avant l'increment. */
    CHECK(g_a_apres == 1, "f2_a_voit_le_changement_seulement_apres_avoir_repris_du_yield");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Point d'entree — NON CABLE (cf. en-tete de fichier)
 * ================================================================ */

int vm_concurrence_famine_self_test_run(void) {
    printf("== Digue anti-famine VM (feat/2093, LOT 0 — non cablee, cf. en-tete) ==\n");

    famille_f1_anti_famine();
    famille_f2_contrat_yield();

    int tp = 0, tf = 0;
    for (int i = 0; i < NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f1 anti-famine (S parque / H sain) : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f2 contrat yield (§3 ADR_0218)      : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   TOTAL digue concurrence VM          : %d/%d\n", tp, tp + tf);
    return tf;
}
