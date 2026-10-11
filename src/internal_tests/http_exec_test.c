// SPDX-License-Identifier: AGPL-3.0-only
/* http_exec_test.c — executeur et file du serveur HTTP (, A0.2).
 *
 * Spec : contrat de transport HTTP interne (regle d'or, file, politique
 *        de saturation).
 *
 * Six familles, dans l'ordre ou elles doivent etre lues :
 *
 *   0. LA question du lot : la VM est-elle reutilisable APRES un
 *      INTERPRET_RUNTIME_ERROR ? Si non, un seul handler fautif tue le
 *      worker et tout le modele « un executeur qui enchaine les
 *      requetes » s'effondre. Mesuree, pas deduite.
 *   1. File bornee — deterministe, SANS thread, via traite_une().
 *   2. Saturation (503 sans reveiller l'executeur) et plafond d'attente.
 *   3. Bout-en-bout : octets sur socket -> procedure OWL -> socket.
 *   4. Erreur runtime dans le handler -> 500, pas un crash. Abandon (§8).
 *   5. Absence de fuite GC a la frontiere, en DEUX mesures.
 *
 * Execute via `owl --self-test-net` (cf. main.c). Chaque famille gere
 * son propre cycle vm_init/vm_free : `vm` est un singleton de processus,
 * deux familles ne peuvent pas en detenir une en meme temps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_exec.h"
#include "../http_server.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../gc.h"
#include "../object.h"
#include "../table.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <windows.h>
  #define HX_SLEEP(ms) Sleep((DWORD)(ms))
#else
  #include <time.h>
  static void hx_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define HX_SLEEP(ms) hx_sleep_ms((int)(ms))
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass[6] = {0,0,0,0,0,0};
static int g_fail[6] = {0,0,0,0,0,0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille <= 5) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Source OWL du harnais.
 *
 * PROVISOIRE ET JETABLE : ces globales sont une convention de harnais
 * (cf. http_exec.h), pas une surface de langage. Elles ne sont
 * enregistrees nulle part et sont remplacees par le DSL de la phase B.
 * ================================================================ */

static const char* SRC_HARNAIS =
    "harnaisHttpEntreeMethode est une chaine = \"\"\n"
    "harnaisHttpEntreeChemin est une chaine = \"\"\n"
    "harnaisHttpEntreeCorps est une chaine = \"\"\n"
    "harnaisHttpSortieStatut est un entier = 0\n"
    "harnaisHttpSortieType est une chaine = \"\"\n"
    "harnaisHttpSortieCorps est une chaine = \"\"\n"
    "harnaisCompteurAppels est un entier = 0\n"
    "harnaisDiviseur est un entier = 0\n"
    "\n"
    "PROCEDURE harnaisHttpTraite()\n"
    "    harnaisCompteurAppels = harnaisCompteurAppels + 1\n"
    "    harnaisHttpSortieStatut = 200\n"
    "    harnaisHttpSortieType = \"text/plain\"\n"
    "    harnaisHttpSortieCorps = \"M=\" + harnaisHttpEntreeMethode + \"|C=\" +"
    " harnaisHttpEntreeChemin + \"|TC=\" + harnaisHttpEntreeCorps\n"
    "FIN\n";

/* Handler fautif : division par zero au runtime (E_RT_DIV_BY_ZERO). Le
 * diviseur passe par une globale pour que le compilateur ne puisse pas
 * replier la division a la compilation. */
static const char* SRC_HARNAIS_FAUTIF =
    "harnaisHttpEntreeMethode est une chaine = \"\"\n"
    "harnaisHttpEntreeChemin est une chaine = \"\"\n"
    "harnaisHttpEntreeCorps est une chaine = \"\"\n"
    "harnaisHttpSortieStatut est un entier = 0\n"
    "harnaisHttpSortieType est une chaine = \"\"\n"
    "harnaisHttpSortieCorps est une chaine = \"\"\n"
    "harnaisCompteurAppels est un entier = 0\n"
    "harnaisDiviseur est un entier = 0\n"
    "\n"
    "PROCEDURE harnaisHttpTraite()\n"
    "    harnaisCompteurAppels = harnaisCompteurAppels + 1\n"
    "    n est un entier = 10 / harnaisDiviseur\n"
    "    harnaisHttpSortieStatut = 200\n"
    "    harnaisHttpSortieCorps = \"jamais atteint\"\n"
    "FIN\n";

/* Alterne : appel pair -> succes, appel impair -> erreur runtime. Sert a
 * prouver qu'une erreur ne laisse aucun residu pour l'appel suivant. */
static const char* SRC_ALTERNE =
    "harnaisSortie est une chaine = \"\"\n"
    "harnaisDiviseur est un entier = 0\n"
    "\n"
    "PROCEDURE harnaisOk()\n"
    "    harnaisSortie = \"vivant\"\n"
    "FIN\n"
    "\n"
    "PROCEDURE harnaisBoum()\n"
    "    n est un entier = 10 / harnaisDiviseur\n"
    "    harnaisSortie = \"jamais\"\n"
    "FIN\n";

/* ================================================================
 * Acces aux globales OWL depuis le C (lecture seule, pour assertion)
 * ================================================================ */

static bool lit_globale(const char* nom, Value* out) {
    ObjString* k = obj_copy_string(nom, (int)strlen(nom));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&vm.globals, k, out);
    vm_pop();
    return ok;
}

static const char* lit_globale_chaine(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return NULL;
    if (!IS_STRING(v)) return NULL;
    return AS_CSTRING(v);
}

static long lit_globale_entier(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return -1;
    if (IS_NUMBER(v)) return (long)AS_NUMBER(v);
    return -1;
}

/* ================================================================
 * Silence de stderr : runtime_error() ecrit une trace complete, et les
 * familles 0 et 4 en provoquent volontairement. Sans ce filtre, la
 * sortie du self-test devient illisible et un vrai FAIL s'y noie.
 * ================================================================ */

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #define HX_DUP _dup
  #define HX_DUP2 _dup2
  #define HX_CLOSE _close
  #define HX_FILENO _fileno
#else
  #include <unistd.h>
  #define HX_DUP dup
  #define HX_DUP2 dup2
  #define HX_CLOSE close
  #define HX_FILENO fileno
#endif

static char  g_stderr_chemin[128];
static int   g_stderr_sauve = -1;
static FILE* g_stderr_tmp   = NULL;

static void stderr_muet(void) {
    fflush(stderr);
    fixture_unique(g_stderr_chemin, sizeof(g_stderr_chemin),
                   "_owl_http_exec_stderr_", ".tmp");
    g_stderr_sauve = HX_DUP(HX_FILENO(stderr));
    g_stderr_tmp   = fopen(g_stderr_chemin, "w+");
    if (g_stderr_tmp && g_stderr_sauve >= 0) {
        HX_DUP2(HX_FILENO(g_stderr_tmp), HX_FILENO(stderr));
    }
}

static void stderr_rend(void) {
    fflush(stderr);
    if (g_stderr_sauve >= 0) {
        HX_DUP2(g_stderr_sauve, HX_FILENO(stderr));
        HX_CLOSE(g_stderr_sauve);
        g_stderr_sauve = -1;
    }
    if (g_stderr_tmp) { fclose(g_stderr_tmp); g_stderr_tmp = NULL; }
    remove(g_stderr_chemin);
}

/* ================================================================
 * FAMILLE 0 — la VM survit-elle a une erreur runtime ?
 *
 * MESURE, pas deduction. Trois observables :
 *   - vm.frameCount et vm.stackTop remis a plat apres l'erreur ;
 *   - un appel ulterieur rend INTERPRET_OK et produit sa valeur ;
 *   - 50 alternances succes/erreur sans derive de la pile.
 * ================================================================ */

static void famille_0_vm_apres_erreur(void) {
    g_famille = 0;

    vm_init();

    InterpretResult r = vm_interpret(SRC_ALTERNE);
    CHECK(r == INTERPRET_OK, "harnais_alterne_compile");

    Value* pile_base = vm.stack;

    /* 1er appel : nominal. */
    InterpretResult a1 = vm_call_global("harnaisOk");
    CHECK(a1 == INTERPRET_OK, "appel_nominal_ok");
    {
        const char* s = lit_globale_chaine("harnaisSortie");
        CHECK(s != NULL && strcmp(s, "vivant") == 0, "appel_nominal_valeur_exacte");
    }

    /* 2e appel : erreur runtime volontaire. */
    stderr_muet();
    InterpretResult a2 = vm_call_global("harnaisBoum");
    stderr_rend();
    CHECK(a2 == INTERPRET_RUNTIME_ERROR, "erreur_runtime_signalee");
    CHECK(vm.frameCount == 0,            "frameCount_remis_a_zero");
    CHECK(vm.stackTop == pile_base,      "pile_remise_a_plat");

    /* 3e appel : LA question. La VM doit encore repondre. */
    obj_globals_set("harnaisSortie", OBJ_VAL(obj_copy_string("", 0)));
    InterpretResult a3 = vm_call_global("harnaisOk");
    CHECK(a3 == INTERPRET_OK, "vm_reutilisable_apres_erreur");
    {
        const char* s = lit_globale_chaine("harnaisSortie");
        CHECK(s != NULL && strcmp(s, "vivant") == 0,
              "vm_reutilisable_valeur_exacte");
    }

    /* 50 alternances : pas de derive de pile, pas d'accumulation de
     * frames. C'est ce qui autorise un worker a enchainer les requetes. */
    {
        int erreurs = 0, succes = 0, derive = 0;
        for (int i = 0; i < 50; i++) {
            stderr_muet();
            InterpretResult e = vm_call_global("harnaisBoum");
            stderr_rend();
            if (e == INTERPRET_RUNTIME_ERROR) erreurs++;
            InterpretResult o = vm_call_global("harnaisOk");
            if (o == INTERPRET_OK) succes++;
            if (vm.stackTop != pile_base || vm.frameCount != 0) derive++;
        }
        CHECK(erreurs == 50, "50_erreurs_toutes_signalees");
        CHECK(succes  == 50, "50_appels_suivants_tous_ok");
        CHECK(derive  == 0,  "aucune_derive_de_pile_sur_50_cycles");
    }

    vm_free();
}

/* ================================================================
 * Fabrication d'une requete sans reseau
 * ================================================================ */

static void fabrique_requete(OwlHttpRequete* r, const char* methode,
                             const char* chemin, const char* corps) {
    memset(r, 0, sizeof(*r));
    snprintf(r->methode, sizeof(r->methode), "%s", methode);
    /* Pointeurs empruntes : cette requete n'est jamais passee a
     * libere_requete(). L'executeur COPIE tout au depot (§2), donc
     * l'emprunt s'arrete au retour de depose(). */
    r->cible  = (char*)chemin;
    r->chemin = r->cible;
    r->version_majeure = 1;
    r->version_mineure = 1;
    r->corps     = (char*)corps;
    r->corps_len = corps ? strlen(corps) : 0;
    snprintf(r->pair, sizeof(r->pair), "127.0.0.1:0");
}

/* Puits memoire pour lire le corps d'une reponse opaque. */
typedef struct { char d[4096]; size_t len; } Sac;

static int sac_ecrit(void* user, const char* octets, size_t len) {
    Sac* s = (Sac*)user;
    if (s->len + len >= sizeof(s->d)) return -1;
    memcpy(s->d + s->len, octets, len);
    s->len += len;
    s->d[s->len] = '\0';
    return 0;
}

static void vide_reponse(const OwlHttpRep* rep, Sac* sac) {
    OwlHttpPuits p; p.ecrit = sac_ecrit; p.user = sac;
    memset(sac, 0, sizeof(*sac));
    owl_http_rep_ecrit(rep, &p);
}

/* ================================================================
 * FAMILLE 1 — file bornee, deterministe, SANS thread
 * ================================================================ */

static void famille_1_file_bornee(void) {
    g_famille = 1;

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 2;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_HARNAIS;
    cfg.procedure          = "harnaisHttpTraite";

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "cree_sans_thread_ni_vm");
    if (!x) return;

    /* cree() n'a ni thread ni VM : la file est vide et rien n'a ete pris. */
    CHECK(owl_http_executeur_profondeur(x) == 0, "file_vide_a_la_creation");
    CHECK(owl_http_executeur_jobs_pris(x) == 0,  "aucun_job_pris_a_la_creation");

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "harnais_compile_dans_la_vm_du_thread");

    OwlHttpRequete r1, r2;
    fabrique_requete(&r1, "GET",  "/un",   NULL);
    fabrique_requete(&r2, "POST", "/deux", "abc");

    OwlHttpJeton *j1 = NULL, *j2 = NULL;
    CHECK(owl_http_executeur_depose(x, &r1, &j1) == OWL_HTTP_EXEC_OK, "depot_1_ok");
    CHECK(owl_http_executeur_depose(x, &r2, &j2) == OWL_HTTP_EXEC_OK, "depot_2_ok");
    CHECK(owl_http_executeur_profondeur(x) == 2, "file_a_deux_entrees");
    CHECK(owl_http_executeur_jobs_pris(x) == 0,  "rien_traite_avant_traite_une");

    CHECK(owl_http_executeur_traite_une(x, 0) == 1, "traite_une_prend_le_premier");
    CHECK(owl_http_executeur_jobs_pris(x) == 1,     "un_seul_job_pris");
    CHECK(owl_http_executeur_profondeur(x) == 1,    "file_redescend_a_un");

    OwlHttpRep* rep1 = NULL;
    CHECK(owl_http_executeur_attend(x, j1, &rep1) == OWL_HTTP_EXEC_OK, "attend_1_ok");
    CHECK(rep1 != NULL, "reponse_1_rendue");
    if (rep1) {
        Sac s; vide_reponse(rep1, &s);
        CHECK(owl_http_rep_statut(rep1) == 200, "reponse_1_statut_200");
        CHECK(strcmp(owl_http_rep_type_contenu(rep1), "text/plain") == 0,
              "reponse_1_type_exact");
        CHECK(strcmp(s.d, "M=GET|C=/un|TC=") == 0, "reponse_1_corps_exact");
        CHECK(owl_http_rep_longueur(rep1) == strlen("M=GET|C=/un|TC="),
              "reponse_1_longueur_exacte");
        owl_http_rep_libere(rep1);
    }

    CHECK(owl_http_executeur_traite_une(x, 0) == 1, "traite_une_prend_le_second");
    OwlHttpRep* rep2 = NULL;
    CHECK(owl_http_executeur_attend(x, j2, &rep2) == OWL_HTTP_EXEC_OK, "attend_2_ok");
    if (rep2) {
        Sac s; vide_reponse(rep2, &s);
        CHECK(strcmp(s.d, "M=POST|C=/deux|TC=abc") == 0, "reponse_2_corps_exact");
        owl_http_rep_libere(rep2);
    }

    /* L'ordre est FIFO et la file est vide : traite_une n'a plus rien. */
    CHECK(owl_http_executeur_traite_une(x, 0) == 0, "file_vide_traite_une_rend_zero");
    CHECK(owl_http_executeur_jobs_pris(x) == 2,     "exactement_deux_jobs_pris");
    CHECK(lit_globale_entier("harnaisCompteurAppels") == 2,
          "procedure_owl_appelee_deux_fois");

    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 2 — saturation (§7) et plafond d'attente
 * ================================================================ */

static void famille_2_saturation(void) {
    g_famille = 2;

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 2;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_HARNAIS;
    cfg.procedure          = "harnaisHttpTraite";

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "cree"); return; }

    OwlHttpRequete r;
    fabrique_requete(&r, "GET", "/plein", NULL);

    OwlHttpJeton *j1 = NULL, *j2 = NULL, *j3 = NULL;
    CHECK(owl_http_executeur_depose(x, &r, &j1) == OWL_HTTP_EXEC_OK,     "depot_1");
    CHECK(owl_http_executeur_depose(x, &r, &j2) == OWL_HTTP_EXEC_OK,     "depot_2");
    CHECK(owl_http_executeur_depose(x, &r, &j3) == OWL_HTTP_EXEC_SATURE, "depot_3_sature");
    CHECK(j3 == NULL, "jeton_nul_quand_sature");

    /* LA preuve du §7 : le 503 est produit par le transport SEUL. Aucun
     * executeur n'a tourne — ni thread, ni traite_une — et le compteur
     * jobs_pris, incremente uniquement dans traite_une, vaut zero. */
    CHECK(owl_http_executeur_jobs_pris(x) == 0, "sature_sans_reveiller_l_executeur");
    CHECK(owl_http_executeur_rejets(x)    == 1, "un_rejet_comptabilise");
    CHECK(owl_http_executeur_profondeur(x) == 2, "file_reste_a_sa_borne");

    /* Plafond d'attente : echeance ABSOLUE posee au depot. Le job trop
     * vieux est retire de la file sans etre execute. */
    {
        OwlHttpExecConfig c2;
        owl_http_exec_config_defaut(&c2);
        c2.file_profondeur    = 4;
        c2.plafond_attente_ms = 20;
        c2.source_owl         = SRC_HARNAIS;
        c2.procedure          = "harnaisHttpTraite";

        OwlHttpExecuteur* y = owl_http_executeur_cree(&c2);
        if (y) {
            vm_init();
            owl_http_executeur_prepare_vm(y);

            OwlHttpJeton* jv = NULL;
            CHECK(owl_http_executeur_depose(y, &r, &jv) == OWL_HTTP_EXEC_OK,
                  "depot_avant_expiration");
            HX_SLEEP(80);
            CHECK(owl_http_executeur_traite_une(y, 0) == -1,
                  "job_expire_abandonne_par_traite_une");
            CHECK(owl_http_executeur_jobs_pris(y) == 0,
                  "job_expire_jamais_execute");
            CHECK(lit_globale_entier("harnaisCompteurAppels") == 0,
                  "procedure_owl_jamais_appelee_sur_job_expire");

            OwlHttpRep* rv = NULL;
            CHECK(owl_http_executeur_attend(y, jv, &rv) == OWL_HTTP_EXEC_SATURE,
                  "attente_expiree_rend_sature");
            CHECK(rv == NULL, "aucune_reponse_sur_expiration");

            vm_free();
            owl_http_executeur_libere(y);
        } else {
            CHECK(0, "cree_plafond");
        }
    }

    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 3 — bout en bout : octets -> procedure OWL -> socket
 *
 * L'executeur tourne dans SON thread et possede la VM ; le transport
 * tourne dans le thread principal, qui n'y touche jamais.
 * ================================================================ */

typedef struct {
    int    statut;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} EchangeE2E;

static EchangeE2E joue_e2e(const char* octets, OwlHttpExecuteur* x,
                           int lire_reponse) {
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
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = octets;
    mc.octets_len            = strlen(octets);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture   = lire_reponse;
    mc.timeout_lecture_ms    = 5000;
    mc.fermer_apres_ecriture = 1;

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

static void famille_3_bout_en_bout(void) {
    g_famille = 3;

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_HARNAIS;
    cfg.procedure          = "harnaisHttpTraite";

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "thread_executeur_demarre");

    {
        EchangeE2E e = joue_e2e("GET /bonjour HTTP/1.1\r\nHost: a\r\n\r\n", x, 1);
        CHECK(e.statut == 200, "e2e_statut_200");
        CHECK(strstr(e.reponse, "HTTP/1.1 200 OK\r\n") == e.reponse,
              "e2e_ligne_de_statut");
        CHECK(strstr(e.reponse, "Content-Type: text/plain\r\n") != NULL,
              "e2e_type_contenu_du_handler_owl");
        CHECK(strstr(e.reponse, "\r\n\r\nM=GET|C=/bonjour|TC=") != NULL,
              "e2e_corps_produit_par_la_procedure_owl");
        /* 20 = strlen("M=GET|C=/bonjour|TC="), la valeur assertee juste
         * au-dessus. La premiere version de ce test annoncait 22 : une
         * erreur de comptage a la main, cote TEST et non cote impl. */
        CHECK(strstr(e.reponse, "Content-Length: 20\r\n") != NULL,
              "e2e_content_length_exact");
    }

    {
        EchangeE2E e = joue_e2e(
            "POST /envoi HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello", x, 1);
        CHECK(e.statut == 200, "e2e_post_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nM=POST|C=/envoi|TC=hello") != NULL,
              "e2e_post_corps_traverse_la_frontiere");
    }

    CHECK(owl_http_executeur_jobs_pris(x) == 2, "deux_jobs_traites_par_le_thread");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 4 — erreur runtime -> 500 ; abandon (§8)
 * ================================================================ */

static void famille_4_erreur_et_abandon(void) {
    g_famille = 4;

    /* 4a — un handler qui leve une erreur runtime produit un 500, et le
     * worker SURVIT : la requete suivante est servie normalement. */
    {
        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = SRC_HARNAIS_FAUTIF;
        cfg.procedure          = "harnaisHttpTraite";

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "cree_fautif"); return; }

        stderr_muet();
        CHECK(owl_http_executeur_demarre(x), "thread_demarre_harnais_fautif");
        EchangeE2E e1 = joue_e2e("GET /boum HTTP/1.1\r\nHost: a\r\n\r\n", x, 1);
        EchangeE2E e2 = joue_e2e("GET /boum HTTP/1.1\r\nHost: a\r\n\r\n", x, 1);
        owl_http_executeur_arrete(x);
        stderr_rend();

        CHECK(e1.statut == 500, "erreur_runtime_produit_500");
        CHECK(strstr(e1.reponse, "HTTP/1.1 500 ") == e1.reponse,
              "500_ecrit_sur_la_socket");
        CHECK(e2.statut == 500, "worker_survit_a_un_handler_fautif");
        CHECK(owl_http_executeur_jobs_pris(x) == 2,
              "les_deux_jobs_ont_ete_pris_malgre_l_erreur");

        owl_http_executeur_libere(x);
    }

    /* 4b — abandon (§8) : le handler est execute QUAND MEME, la reponse
     * est jetee. Pas de crash, pas de fuite. Verifiable sans reseau. */
    {
        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 4;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = SRC_HARNAIS;
        cfg.procedure          = "harnaisHttpTraite";

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "cree_abandon"); return; }

        vm_init();
        owl_http_executeur_prepare_vm(x);

        OwlHttpRequete r;
        fabrique_requete(&r, "GET", "/parti", NULL);
        OwlHttpJeton* j = NULL;
        owl_http_executeur_depose(x, &r, &j);

        owl_http_executeur_abandonne(x, j);   /* le client est parti */

        CHECK(owl_http_executeur_traite_une(x, 0) == 1,
              "handler_execute_malgre_l_abandon");
        CHECK(owl_http_executeur_jobs_pris(x) == 1, "job_bien_compte");
        CHECK(lit_globale_entier("harnaisCompteurAppels") == 1,
              "procedure_owl_bien_appelee_une_fois");

        vm_free();
        owl_http_executeur_libere(x);
    }

    /* 4c — le pair se deconnecte sans lire : le transport ecrit dans le
     * vide, l'executeur a quand meme tourne, rien ne casse. */
    {
        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 4;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = SRC_HARNAIS;
        cfg.procedure          = "harnaisHttpTraite";

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "cree_deco"); return; }
        owl_http_executeur_demarre(x);

        EchangeE2E e = joue_e2e("GET /coupe HTTP/1.1\r\nHost: a\r\n\r\n", x, 0);
        CHECK(e.statut == 200,  "deconnexion_pendant_le_handler_pas_de_crash");
        CHECK(e.reponse_len == 0, "aucune_reponse_lue_par_le_pair_parti");
        CHECK(owl_http_executeur_jobs_pris(x) == 1,
              "handler_termine_quand_meme_reponse_jetee");

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
    }
}

/* ================================================================
 * FAMILLE 5 — absence de fuite GC a la frontiere, en DEUX mesures
 *
 * Mesure 1 : garde d'allocation. Le chemin transport (depot de la
 *   requete, puis rendu de la reponse) ne doit faire varier
 *   gc_bytesAllocated d'AUCUN octet. Si quelqu'un remplace une copie
 *   par un AS_CSTRING ou construit un objet OWL cote transport, le
 *   compteur bouge et cette assertion casse. C'est le but.
 *   La mesure exclut volontairement traite_une : c'est le seul segment
 *   ou la VM travaille, et elle y alloue evidemment.
 *
 * Mesure 2 : GC force AUX DEUX frontieres (gc_frontiere=1). Toute
 *   reference ayant fuite serait pendante ; l'assertion porte sur
 *   l'OCTET EXACT du corps rendu, pas sur « ca n'a pas plante ».
 * ================================================================ */

static void famille_5_anti_gc(void) {
    g_famille = 5;

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_HARNAIS;
    cfg.procedure          = "harnaisHttpTraite";
    cfg.gc_frontiere       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "cree_gc"); return; }

    vm_init();
    owl_http_executeur_prepare_vm(x);

    OwlHttpRequete r;
    fabrique_requete(&r, "PUT", "/gc", "charge-utile");

    /* --- Mesure 1a : le depot n'alloue rien cote GC. --- */
    size_t avant_depot = gc_bytesAllocated;
    OwlHttpJeton* j = NULL;
    OwlHttpExecStatut sd = owl_http_executeur_depose(x, &r, &j);
    size_t apres_depot = gc_bytesAllocated;
    CHECK(sd == OWL_HTTP_EXEC_OK, "depot_gc_ok");
    CHECK(apres_depot == avant_depot, "depot_n_alloue_aucun_octet_gc");

    /* Le seul segment ou la VM travaille. gc_frontiere y force un cycle
     * complet a l'entree comme a la sortie. */
    CHECK(owl_http_executeur_traite_une(x, 0) == 1, "traite_une_avec_gc_force");

    /* --- Mesure 1b : rendre la reponse au transport n'alloue rien. --- */
    size_t avant_rendu = gc_bytesAllocated;
    OwlHttpRep* rep = NULL;
    OwlHttpExecStatut sa = owl_http_executeur_attend(x, j, &rep);
    Sac sac; memset(&sac, 0, sizeof(sac));
    int statut = 0; size_t longueur = 0;
    if (rep) {
        statut   = owl_http_rep_statut(rep);
        longueur = owl_http_rep_longueur(rep);
        vide_reponse(rep, &sac);
    }
    size_t apres_rendu = gc_bytesAllocated;

    CHECK(sa == OWL_HTTP_EXEC_OK, "attend_gc_ok");
    CHECK(apres_rendu == avant_rendu, "rendu_de_la_reponse_n_alloue_aucun_octet_gc");

    /* --- Mesure 2 : l'octet exact survit au GC force. --- */
    CHECK(statut == 200, "gc_force_statut_exact");
    CHECK(strcmp(sac.d, "M=PUT|C=/gc|TC=charge-utile") == 0,
          "gc_force_corps_octet_pour_octet");
    CHECK(longueur == strlen("M=PUT|C=/gc|TC=charge-utile"),
          "gc_force_longueur_exacte");

    /* Un GC supplementaire APRES la remise au transport : la reponse est
     * hors du graphe de la VM, elle doit rester lisible. */
    gc_collect();
    if (rep) {
        Sac s2; vide_reponse(rep, &s2);
        CHECK(strcmp(s2.d, "M=PUT|C=/gc|TC=charge-utile") == 0,
              "reponse_survit_a_un_gc_posterieur");
        owl_http_rep_libere(rep);
    } else {
        CHECK(0, "reponse_rendue");
    }

    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_exec_self_test_run(void) {
    printf("== Executeur HTTP (feat/1366, phase A0.2) ==\n");

    famille_0_vm_apres_erreur();
    famille_1_file_bornee();
    famille_2_saturation();
    famille_3_bout_en_bout();
    famille_4_erreur_et_abandon();
    famille_5_anti_gc();

    int tp = 0, tf = 0;
    for (int i = 0; i < 6; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 vm apres erreur runtime : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 file bornee             : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 saturation + plafond    : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 bout en bout            : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 erreur runtime + abandon: %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 anti-fuite GC           : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   TOTAL executeur HTTP       : %d/%d\n", tp, tp + tf);
    return tf;
}
