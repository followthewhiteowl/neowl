// SPDX-License-Identifier: AGPL-3.0-only
/* http_flux_ecrit_test.c — native FluxÉcrit + slot ambiant « flux courant »
 * + park/restore,  P2.
 *
 * DIGUE TEST-FIRST (niveau C). Ces tests définissent le CONTRAT de la première
 * pièce runtime du DSL de service streaming :
 *
 *   F0. Émission SSE : un flux ouvert + le slot ambiant posé sur son ProdCtx,
 *       une source OWL qui appelle FluxEcrit("x") -> le transport draine
 *       EXACTEMENT `data: x\n\n` (framing SSE, patron serve_llm.c).
 *   F1. Émission octets : mode fluxOctets -> le morceau part BRUT (aucun
 *       préfixe SSE), drainé octet pour octet.
 *   F2. Hors flux : FluxEcrit sans slot posé -> exception (erreur programmeur
 *       rattrapable), vm_interpret rend INTERPRET_RUNTIME_ERROR.
 *   F3. Park/restore anti-croisement : deux contextes parkés à leur yield, le
 *       slot restauré est LE BON (jamais celui de l'autre) — l'analogue du
 *       test anti-croisement `jeton_courant`.
 *
 * Le slot est posé/testé via le seam producteur déterministe (le pont service
 * réel = P3). Aucun ObjString ne traverse la frontière : FluxEcrit extrait les
 * octets (value_as_bytes) dans un tampon C, le transport COPIE (§2 ).
 *
 * Exécuté via `owl --self-test-net` (cf. main.c). --self-test-net n'initialise
 * PAS la VM globale : F1/F3 la montent/démontent eux-mêmes (vm_init/vm_free) ;
 * F0/F1-émission passent par l'exécuteur qui possède sa propre VM (thread).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_exec.h"
#include "../http_server.h"
#include "../net_mock_peer.h"
#include "../vm.h"

#ifdef _WIN32
  #include <windows.h>
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define NB_FAMILLES 8
static int g_pass[NB_FAMILLES] = {0};
static int g_fail[NB_FAMILLES] = {0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL FE%d [%s] (ligne %d)\n", f, label, ligne); }
}
#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Analyseur chunked (RFC 9112 §7.1) — copie autonome du patron
 * http_stream_test.c : la taille est lue depuis le CADRAGE, jamais du contenu.
 * ================================================================ */

static int parse_chunked(const char* buf, size_t len,
                         char* out, size_t out_cap, size_t* out_len,
                         int* nb_data, int* a_terminal) {
    size_t i = 0, o = 0;
    int ndata = 0, terminal = 0, ok = 1;
    for (;;) {
        if (i >= len) { ok = 0; break; }
        size_t taille = 0; int vus = 0;
        while (i < len && buf[i] != '\r' && buf[i] != ';') {
            char c = buf[i]; int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else { ok = 0; break; }
            taille = taille * 16 + (size_t)d; vus++; i++;
        }
        if (!ok || !vus) { ok = 0; break; }
        while (i < len && buf[i] != '\r') i++;
        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') { ok = 0; break; }
        i += 2;
        if (taille == 0) { terminal = 1; break; }
        if (i + taille > len) { ok = 0; break; }
        if (o + taille <= out_cap) { memcpy(out + o, buf + i, taille); o += taille; }
        i += taille;
        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') { ok = 0; break; }
        i += 2; ndata++;
    }
    if (out_len)    *out_len    = o;
    if (nb_data)    *nb_data    = ndata;
    if (a_terminal) *a_terminal = terminal;
    return ok;
}

/* ================================================================
 * Producteur de test : pose le mode + le slot ambiant, exécute une source OWL
 * qui appelle FluxEcrit, puis termine + relâche le flux ouvert en paresseux.
 * Tourne dans le thread VM de l'exécuteur (GIL tenu).
 * ================================================================ */

typedef struct {
    const char*  source;        /* p.ex. `FluxEcrit("x")` */
    int          mode;          /* OWL_HTTP_FLUX_MODE_* */
    int          pre_deconnecte;/* P4 : ouvrir + marquer consommateur_fini AVANT
                                 * le vm_interpret -> le prochain FluxEcrit rend
                                 * ABANDON (client parti mi-flux). */
    volatile int interpret_ok;
} FluxEcritParams;

static void flux_ecrit_producteur(const OwlHttpRequete* req, OwlHttpProdCtx* ctx,
                                  void* user) {
    (void)req;
    FluxEcritParams* pp = (FluxEcritParams*)user;
    owl_http_prod_flux_mode_set(ctx, pp->mode);
    owl_vm_flux_courant_set(ctx);
    if (pp->pre_deconnecte) {
        /* Simule la deconnexion mi-flux : le flux est ouvert (publie au
         * transport) puis marque consommateur_fini. La prochaine pousse (dans
         * pp->source) rendra ABANDON -> exception DECONNECTE. */
        OwlHttpRep* f0 = owl_http_prod_flux_assure(ctx);
        if (f0) owl_http_rep_flux_marque_consommateur_fini(f0);
    }
    InterpretResult r = vm_interpret(pp->source);
    owl_vm_flux_courant_set(NULL);
    pp->interpret_ok = (r == INTERPRET_OK) ? 1 : 0;
    /* La native a ouvert le flux en paresseux et l'a mis en cache dans ctx.
     * On clôt proprement (fin de procédure = fin du flux). */
    OwlHttpRep* f = owl_http_prod_flux_courant(ctx);
    if (f) {
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
    }
}

/* Un échange e2e : requête brute -> réponse brute, exécuteur en flux. */
typedef struct {
    int    demarre_ok;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} EchangeStream;

static EchangeStream joue_stream(FluxEcritParams* pp) {
    EchangeStream e; memset(&e, 0, sizeof(e));

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 800;
    xc.producteur         = flux_ecrit_producteur;
    xc.producteur_user    = pp;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (!x) return e;
    e.demarre_ok = owl_http_executeur_demarre(x) ? 1 : 0;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        owl_http_executeur_libere(x);
        return e;
    }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = "GET /flux HTTP/1.1\r\nHost: a\r\n\r\n";
    mc.octets_len            = strlen(mc.octets);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 4000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); owl_http_executeur_libere(x); return e; }

    owl_http_serveur_sert_une_connexion(&l, &cfg, 5000);
    owl_mock_peer_joint(pair);

    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(e.reponse, recu, rl);
    e.reponse[rl]  = '\0';
    e.reponse_len  = rl;

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_libere(x);
    return e;
}

static const char* corps_de(const EchangeStream* e, size_t* out_len) {
    const char* sep = "\r\n\r\n";
    for (size_t i = 0; i + 4 <= e->reponse_len; i++) {
        if (memcmp(e->reponse + i, sep, 4) == 0) {
            *out_len = e->reponse_len - (i + 4);
            return e->reponse + i + 4;
        }
    }
    *out_len = 0;
    return e->reponse + e->reponse_len;
}

/* ================================================================
 * F0 — émission SSE : FluxEcrit("x") -> `data: x\n\n` drainé exact
 * ================================================================ */

static void famille_0_emission_sse(void) {
    g_famille = 0;

    FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
    pp.source = "FluxEcrit(\"x\")";
    pp.mode   = OWL_HTTP_FLUX_MODE_SSE;

    EchangeStream e = joue_stream(&pp);

    CHECK(e.demarre_ok == 1, "sse_executeur_demarre");
    CHECK(pp.interpret_ok == 1, "sse_flux_ecrit_sans_exception");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "sse_statut_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked\r\n") != NULL,
          "sse_transfer_encoding_chunked");

    size_t cl = 0;
    const char* corps = corps_de(&e, &cl);
    char decode[512]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1, "sse_cadrage_coherent");
    /* LA preuve : le framing SSE exact `data: x\n\n`. */
    CHECK(dl == 9 && memcmp(decode, "data: x\n\n", 9) == 0,
          "sse_framing_data_x_exact");
}

/* ================================================================
 * F1 — émission octets : mode fluxOctets -> morceau brut, sans préfixe SSE
 * ================================================================ */

static void famille_1_emission_octets(void) {
    g_famille = 1;

    FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
    pp.source = "FluxEcrit(\"raw\")";
    pp.mode   = OWL_HTTP_FLUX_MODE_OCTETS;

    EchangeStream e = joue_stream(&pp);

    CHECK(e.demarre_ok == 1, "octets_executeur_demarre");
    CHECK(pp.interpret_ok == 1, "octets_flux_ecrit_sans_exception");

    size_t cl = 0;
    const char* corps = corps_de(&e, &cl);
    char decode[512]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1, "octets_cadrage_coherent");
    /* Octets bruts : AUCUN préfixe `data: `, le morceau part tel quel. */
    CHECK(dl == 3 && memcmp(decode, "raw", 3) == 0, "octets_bruts_exacts");
}

/* ================================================================
 * F2 — hors flux : FluxEcrit sans slot -> exception rattrapable
 * ================================================================ */

static void famille_2_hors_flux(void) {
    g_famille = 2;

    vm_init();
    owl_vm_flux_courant_set(NULL);   /* slot vide : appel hors flux */
    InterpretResult r = vm_interpret("FluxEcrit(\"x\")");
    /* Valeur exacte : appel hors flux = exception non rattrapée au top-level. */
    CHECK(r == INTERPRET_RUNTIME_ERROR, "hors_flux_leve_exception");
    vm_free();
}

/* ================================================================
 * F3 — park/restore anti-croisement : le slot restauré est LE BON
 * ================================================================ */

static void famille_3_park_restore(void) {
    g_famille = 3;

    vm_init();

    void* fluxA = (void*)0x1111;
    void* fluxB = (void*)0x2222;

    owl_vm_flux_courant_set(fluxA);
    CHECK(owl_vm_flux_courant_get() == fluxA, "slot_pose_A");

    void* parkA = owl_vm_exec_park();
    /* Après park : baseline vide, le prochain détenteur démarre propre. */
    CHECK(owl_vm_flux_courant_get() == NULL, "slot_baseline_vide_apres_park");

    owl_vm_flux_courant_set(fluxB);
    void* parkB = owl_vm_exec_park();
    CHECK(owl_vm_flux_courant_get() == NULL, "slot_baseline_vide_apres_2e_park");

    /* Bruit : un 3e détenteur pose son propre flux avant les restores. */
    owl_vm_flux_courant_set((void*)0x3333);

    owl_vm_exec_restore(parkB);
    CHECK(owl_vm_flux_courant_get() == fluxB, "restore_B_retrouve_son_flux");

    owl_vm_exec_restore(parkA);
    /* LA preuve anti-croisement : A retrouve SON flux, pas celui de B. */
    CHECK(owl_vm_flux_courant_get() == fluxA, "restore_A_pas_de_croisement");

    vm_free();
}

/* ================================================================
 * F4 — hors contexte : le code canon dédié est rattrapable + filtrable (P4)
 * ================================================================ */

/* Source OWL : rattrape l'exception et NE la propage QUE si le code ne matche
 * pas. Rattrapée avec le bon code -> vm_interpret rend INTERPRET_OK ; mauvais
 * code (ou code vide pré-impl) -> ExceptionPropage -> INTERPRET_RUNTIME_ERROR.
 * C'est la preuve exacte : le filtrage SI ExceptionInfo(errCode) = <code> marche. */
static const char* SRC_FILTRE_HORS_CONTEXTE =
    "QUAND EXCEPTION DANS\n"
    "    FluxEcrit(\"x\")\n"
    "FAIRE\n"
    /* Filtrage par la constante publique (pas la string brute) : le handler
     * ne rattrape que si le code EST httpFluxHorsContexte, et propage sinon.
     * Contrôle discriminant : httpFluxDeconnecte NE matche PAS cette exception,
     * donc le OU ci-dessous ne rattrape jamais à tort. */
    "    SI ExceptionInfo(errCode) <> httpFluxHorsContexte\n"
    "       OU ExceptionInfo(errCode) = httpFluxDeconnecte ALORS\n"
    "        ExceptionPropage()\n"
    "    FIN\n"
    "FIN\n";

static void famille_4_hors_contexte_code(void) {
    g_famille = 4;

    vm_init();
    owl_vm_flux_courant_set(NULL);   /* slot vide : appel hors flux */
    InterpretResult r = vm_interpret(SRC_FILTRE_HORS_CONTEXTE);
    /* Rattrapée par le code canon dédié -> aucune propagation -> INTERPRET_OK. */
    CHECK(r == INTERPRET_OK, "hors_contexte_code_canon_rattrapable_et_filtrable");
    vm_free();
}

/* ================================================================
 * F5 — déconnexion mi-flux : ABANDON -> exception DECONNECTE filtrable (P4)
 * ================================================================ */

static const char* SRC_FILTRE_DECONNECTE =
    "QUAND EXCEPTION DANS\n"
    "    FluxEcrit(\"x\")\n"
    "FAIRE\n"
    /* Filtrage par la constante publique httpFluxDeconnecte : rattrape le départ
     * client, propage tout le reste. Contrôle discriminant : httpFluxHorsContexte
     * NE matche PAS cette déconnexion, sinon le OU propagerait à tort. */
    "    SI ExceptionInfo(errCode) <> httpFluxDeconnecte\n"
    "       OU ExceptionInfo(errCode) = httpFluxHorsContexte ALORS\n"
    "        ExceptionPropage()\n"
    "    FIN\n"
    "FIN\n";

static void famille_5_deconnecte_code(void) {
    g_famille = 5;

    FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
    pp.source         = SRC_FILTRE_DECONNECTE;
    pp.mode           = OWL_HTTP_FLUX_MODE_OCTETS;
    pp.pre_deconnecte = 1;   /* flux ouvert puis consommateur_fini avant le push */

    EchangeStream e = joue_stream(&pp);

    CHECK(e.demarre_ok == 1, "deconnecte_executeur_demarre");
    /* Le push rend ABANDON -> exception DECONNECTE -> rattrapée par le bon code
     * (sinon ExceptionPropage -> interpret_ok = 0). Preuve exacte du code canon. */
    CHECK(pp.interpret_ok == 1, "deconnecte_code_canon_rattrapable_et_filtrable");
}

/* ================================================================
 * F6 — auto-sérialisation « tout » : une valeur non chaîne/buffer est
 * sérialisée en JSON MINIFIÉ (mono-ligne) sur le même chemin que RENVOYER,
 * puis émise (framée SSE). Honore la signature `donnée : tout` (BUG 2).
 * ================================================================ */

static void famille_6_auto_serialise(void) {
    g_famille = 6;

    /* (a) Entier -> JSON "42" -> framing SSE `data: 42\n\n`. */
    {
        FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
        pp.source = "FluxEcrit(42)";
        pp.mode   = OWL_HTTP_FLUX_MODE_SSE;

        EchangeStream e = joue_stream(&pp);
        CHECK(pp.interpret_ok == 1, "auto_entier_sans_exception");

        size_t cl = 0;
        const char* corps = corps_de(&e, &cl);
        char decode[512]; size_t dl = 0; int nb = 0, term = 0;
        int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
        CHECK(cadrage == 1, "auto_entier_cadrage_coherent");
        CHECK(dl == 10 && memcmp(decode, "data: 42\n\n", 10) == 0,
              "auto_entier_json_42");
    }

    /* (b) Structure -> JSON MINIFIÉ `{"a":1,"b":2}` (mono-ligne : prouve la
     *     minification, une sortie pretty aurait des retours à la ligne). */
    {
        FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
        pp.source =
            "MaStruct est une Structure\n"
            "\tmontant est un entier\n"
            "\ttotal est un entier\n"
            "FIN\n"
            "s est un MaStruct\n"
            "s.montant = 1\n"
            "s.total = 2\n"
            "FluxEcrit(s)\n";
        pp.mode = OWL_HTTP_FLUX_MODE_SSE;

        EchangeStream e = joue_stream(&pp);
        CHECK(pp.interpret_ok == 1, "auto_struct_sans_exception");

        size_t cl = 0;
        const char* corps = corps_de(&e, &cl);
        char decode[512]; size_t dl = 0; int nb = 0, term = 0;
        int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
        CHECK(cadrage == 1, "auto_struct_cadrage_coherent");
        const char* attendu = "data: {\"montant\":1,\"total\":2}\n\n";
        size_t la = strlen(attendu);
        CHECK(dl == la && memcmp(decode, attendu, la) == 0,
              "auto_struct_json_minifie");
    }
}

/* ================================================================
 * F7 — framing SSE multi-ligne : un payload contenant des `\n` internes est
 * re-préfixé `data: ` sur CHAQUE ligne (spec SSE), terminé par `\n\n` (BUG 3).
 * ================================================================ */

static void famille_7_framing_multiligne(void) {
    g_famille = 7;

    FluxEcritParams pp; memset(&pp, 0, sizeof(pp));
    /* Backtick OWL : chaîne brute qui préserve le vrai saut de ligne. */
    pp.source = "FluxEcrit(`a\nb`)";
    pp.mode   = OWL_HTTP_FLUX_MODE_SSE;

    EchangeStream e = joue_stream(&pp);
    CHECK(pp.interpret_ok == 1, "multiligne_sans_exception");

    size_t cl = 0;
    const char* corps = corps_de(&e, &cl);
    char decode[512]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1, "multiligne_cadrage_coherent");
    /* LA preuve : chaque ligne re-préfixée. `a\nb` -> `data: a\ndata: b\n\n`. */
    const char* attendu = "data: a\ndata: b\n\n";
    size_t la = strlen(attendu);
    CHECK(dl == la && memcmp(decode, attendu, la) == 0,
          "multiligne_reprefixe_chaque_ligne");
}

/* ================================================================
 * Point d'entrée
 * ================================================================ */

int http_flux_ecrit_self_test_run(void) {
    printf("== Native FluxEcrit + slot flux courant (feat/1834 P2/P4) ==\n");

    famille_0_emission_sse();
    famille_1_emission_octets();
    famille_2_hors_flux();
    famille_3_park_restore();
    famille_4_hors_contexte_code();
    famille_5_deconnecte_code();
    famille_6_auto_serialise();
    famille_7_framing_multiligne();

    int tp = 0, tf = 0;
    for (int i = 0; i < NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 emission SSE (data: x)      : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 emission octets bruts       : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 hors flux -> exception      : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 park/restore anti-croisement: %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 hors contexte code canon    : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 deconnecte code canon       : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 auto-serialise JSON (tout)  : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 framing SSE multi-ligne     : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   TOTAL FluxEcrit                : %d/%d\n", tp, tp + tf);
    return tf;
}
