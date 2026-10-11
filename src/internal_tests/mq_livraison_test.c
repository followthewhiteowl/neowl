// SPDX-License-Identifier: AGPL-3.0-only
/* mq_livraison_test.c — socle Message Queue E1 « pont de livraison »
 *.
 *
 * Contrat mesure : un message injecte par un pair, lu pendant UN tick de
 * pompe (owl_reacteur_tick), invoque une procedure OWL enregistree avec
 * le payload EXACT. E1 est agnostique du protocole : le pair emet un
 * bloc d'octets deja delimite, aucun parsing protocolaire ici.
 *
 * Trois familles :
 *   0. livraison bout-en-bout : octets -> pompe -> procedure OWL.
 *      L'assertion « handler appele » avec le payload exact tranche.
 *   1. non-regression GC : le handler abonne est tenu comme racine et
 *      survit intact a des cycles GC forces.
 *   2. idempotence de l'init : un 2e owl_mq_registre_init ne clobbere
 *      pas les abonnements actifs (contrat multi-protocole MQ).
 *
 * Execute via `owl --self-test-net` (cf. main.c). Chaque famille gere
 * son propre vm_init/vm_free : la VM est un singleton de processus.
 */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
  #include <winsock2.h>
  #define MQ_CLOSE closesocket
#else
  #include <unistd.h>
  #define MQ_CLOSE close
#endif

#include <stdio.h>
#include <string.h>

#include "../stdlib_mq.h"
#include "../net_reacteur.h"
#include "../net_tcp.h"
#include "../net_tcp_server.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../object.h"
#include "../table.h"
#include "../gc.h"

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass[3] = {0, 0, 0};
static int g_fail[3] = {0, 0, 0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille <= 2) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Source OWL du harnais.
 *
 * Le handler enregistre son argument payload dans une globale
 * observable et compte ses appels. Provisoire et jetable : convention
 * de harnais, pas une surface de langage.
 * ================================================================ */

static const char* SRC_HARNAIS =
    "harnaisMqPayload est une chaine = \"\"\n"
    "harnaisMqAppels est un entier = 0\n"
    "\n"
    "PROCEDURE harnaisMqRecoit(charge est une chaine)\n"
    "    harnaisMqAppels = harnaisMqAppels + 1\n"
    "    harnaisMqPayload = charge\n"
    "FIN\n";

/* ================================================================
 * Acces aux globales OWL depuis le C (lecture seule, pour assertion).
 * La cle ObjString est rootee pendant le lookup (skill gc-root-obj-fresh).
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

/* Recupere la Value d'une procedure top-level par son nom exact. */
static bool lit_procedure(const char* nom, Value* out) {
    return lit_globale(nom, out) && IS_FUNCTION(*out);
}

/* ================================================================
 * FAMILLE 0 — livraison bout-en-bout : octets -> pompe -> procedure OWL
 * ================================================================ */

static void famille_0_livraison(void) {
    g_famille = 0;

    const char* PAYLOAD = "bonjour-file-mq";

    vm_init();
    owl_mq_registre_init();

    InterpretResult ir = vm_interpret(SRC_HARNAIS);
    CHECK(ir == INTERPRET_OK, "harnais_compile");

    Value handler;
    CHECK(lit_procedure("harnaisMqRecoit", &handler), "procedure_handler_trouvee");

    OwlMqHandle h = owl_mq_abonne(handler);
    CHECK(h != OWL_MQ_HANDLE_NUL, "abonnement_enregistre");

    /* Pair CONNECTE face a un listener local : emet le payload en un
     * bloc puis raccroche. Le socket accepte devient lisible ; un tick
     * de pompe doit lire le bloc et livrer au handler. */
    OwlTcpListener l;
    CHECK(owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false), "listener_ouvre");

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = PAYLOAD;
    mc.octets_len            = strlen(PAYLOAD);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    CHECK(pair != NULL, "mock_peer_demarre");

    OwlTcpConn conn;
    memset(&conn, 0, sizeof(conn));
    int acc = owl_tcp_listener_accepte(&l, 5000, &conn);
    CHECK(acc == OWL_TCPS_OK, "connexion_acceptee");

    OwlReacteur* r = owl_reacteur_cree();
    CHECK(r != NULL, "reacteur_cree");
    CHECK(owl_mq_reacteur_branche(r, conn.fd, h), "fd_branche_sur_le_reacteur");

    /* UN tick de pompe : bloque jusqu'au POLLIN, lit le bloc, livre. */
    int n = owl_reacteur_tick(r, 2000);
    CHECK(n >= 1, "tick_a_dispatche_la_lecture");

    /* Le coeur du contrat E1 : le handler a recu le payload exact. */
    CHECK(lit_globale_entier("harnaisMqAppels") == 1,
          "handler_appele_une_fois");
    {
        const char* recu = lit_globale_chaine("harnaisMqPayload");
        CHECK(recu != NULL && strcmp(recu, PAYLOAD) == 0,
              "handler_recoit_le_payload_exact");
    }

    owl_reacteur_detruit(r);
    MQ_CLOSE(conn.fd);
    owl_mock_peer_joint(pair);
    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);

    owl_mq_registre_libere();
    vm_free();
}

/* ================================================================
 * FAMILLE 1 — non-regression GC : le handler abonne survit intact
 *
 * Le handler est tenu comme racine GC persistante par le registre. Des
 * cycles GC forces ne doivent ni le collecter ni corrompre le slot :
 * le registre rend le MEME objet, toujours appelable.
 * ================================================================ */

static void famille_1_gc_root(void) {
    g_famille = 1;

    vm_init();
    owl_mq_registre_init();

    InterpretResult ir = vm_interpret(SRC_HARNAIS);
    CHECK(ir == INTERPRET_OK, "harnais_compile_gc");

    Value handler;
    CHECK(lit_procedure("harnaisMqRecoit", &handler), "procedure_handler_trouvee_gc");

    OwlMqHandle h = owl_mq_abonne(handler);
    CHECK(h != OWL_MQ_HANDLE_NUL, "abonnement_enregistre_gc");

    Obj* avant = IS_OBJ(handler) ? AS_OBJ(handler) : NULL;
    CHECK(avant != NULL, "handler_est_un_objet");

    /* Trois cycles GC forces, avec de l'allocation entre les deux pour
     * bousculer le tas. Le handler ne doit pas bouger. */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 32; j++) (void)obj_copy_string("ballast", 7);
        gc_collect();
    }

    Value apres;
    CHECK(owl_mq_handler(h, &apres), "registre_rend_le_handler_apres_gc");
    CHECK(IS_FUNCTION(apres), "handler_toujours_appelable_apres_gc");
    CHECK(IS_OBJ(apres) && AS_OBJ(apres) == avant,
          "meme_objet_handler_apres_gc");

    /* Un desabonnement libere la racine ; le slot redevient NIL. */
    CHECK(owl_mq_desabonne(h), "desabonnement_ok");
    Value vide;
    CHECK(!owl_mq_handler(h, &vide), "handle_retire_inconnu");

    owl_mq_registre_libere();
    vm_free();
}

/* ================================================================
 * FAMILLE 2 — idempotence de l'init du socle
 *
 * L'init du socle MQ (owl_mq_registre_init) est desormais central et
 * client-agnostique. Le contrat protege le scenario multi-protocole :
 * MQTT + un futur STOMP/AMQP partagent le meme registre d'abonnements
 * sans le reinitialiser. Un abonnement actif doit survivre a un 2e appel
 * de l'init (avec l'ancienne semantique « wipe a chaque appel », ce slot
 * etait remis a NIL -> abonnement perdu).
 * ================================================================ */

static void famille_2_init_idempotent(void) {
    g_famille = 2;

    vm_init();
    owl_mq_registre_init();

    InterpretResult ir = vm_interpret(SRC_HARNAIS);
    CHECK(ir == INTERPRET_OK, "harnais_compile_idem");

    Value handler;
    CHECK(lit_procedure("harnaisMqRecoit", &handler), "procedure_handler_trouvee_idem");

    OwlMqHandle h = owl_mq_abonne(handler);
    CHECK(h != OWL_MQ_HANDLE_NUL, "abonnement_enregistre_idem");

    Obj* avant = IS_OBJ(handler) ? AS_OBJ(handler) : NULL;
    CHECK(avant != NULL, "handler_est_un_objet_idem");

    /* Le coeur du contrat : un 2e init (simule un 2e client MQ ou un
     * re-init defensif) ne doit PAS vider le slot deja abonne. */
    owl_mq_registre_init();

    Value apres;
    CHECK(owl_mq_handler(h, &apres), "abonnement_survit_au_reinit");
    CHECK(IS_FUNCTION(apres), "handler_toujours_appelable_apres_reinit");
    CHECK(IS_OBJ(apres) && AS_OBJ(apres) == avant,
          "meme_objet_handler_apres_reinit");

    owl_mq_registre_libere();
    vm_free();
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int mq_livraison_self_test_run(void) {
    printf("== Socle MQ — pont de livraison E1 (feat/1791) ==\n");

    owl_tcp_global_init();

    famille_0_livraison();
    famille_1_gc_root();
    famille_2_init_idempotent();

    int tp = 0, tf = 0;
    for (int i = 0; i < 3; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 livraison bout-en-bout : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 non-regression GC      : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 init idempotent        : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   TOTAL socle MQ            : %d/%d\n", tp, tp + tf);
    return tf;
}
