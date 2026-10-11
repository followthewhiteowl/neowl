// SPDX-License-Identifier: AGPL-3.0-only
/* http_stream_test.c — socle streaming SSE, mode C.
 *
 * Ticket : 
 * Standard : HTTP/1.1 chunked transfer-encoding (RFC 9112 §7.1).
 *
 * DIGUE TEST-FIRST (niveau C). Ces tests definissent le CONTRAT du mode C
 * (corps de reponse incremental) ; ils sont ROUGES tant que l'impl vit en
 * stub (cf. http_exec.c « STUB  »). Ils NE testent PAS le framing
 * SSE / le schema OpenAI / le LLM — tout ca est au ticket B (serve-llm) et
 * les octets des chunks restent opaques ici.
 *
 * Mecanisme gele : la VM POUSSE, le transport DRAINE (owl_http_rep_ecrit).
 * File bornee EN OCTETS (D2). Refcount producteur+transport (D3/§8).
 * Framing pose par le transport, JAMAIS derive du contenu.
 *
 * Neuf familles, chacune tournee vers une PREUVE :
 *   0. vrai flux : le drain recoit des octets AVANT la fin du producteur
 *      (ordre/timing, facon http_concurrence_test.c).
 *   1. framing chunked parse-able ; Transfer-Encoding present ; AUCUN
 *      Content-Length (bout-en-bout, mock peer).
 *   2. contre-pression : file pleine -> pousse BLOQUE (pas d'explosion RAM).
 *   3. timeout par chunk : consommateur fige -> drain coupe, producteur libere.
 *   4. erreur mi-flux : avorte apres >=1 chunk -> pas de terminal, fermeture
 *      abrupte.
 *   5. corpus chunked-out : contenu piege (\r\n, faux 0\r\n\r\n, chunk vide,
 *      tailles limites) -> cadrage intact.
 *   6. regle d'or : gc_frontiere vert, nb_threads_vm == 1 apres un flux.
 *   7. plafond neutralise : un producteur lent qui a DEMARRE != 503 (D1).
 *   8. statut sans corps refuse (D7) : cree_flux 204/304/1xx -> NULL.
 *
 * Execute via `owl --self-test-net` (cf. main.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_exec.h"
#include "../http_server.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../gc.h"

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE st_thread_t;
  #define ST_THREAD_RET DWORD WINAPI
  #define ST_THREAD_0   0
  #define ST_SLEEP(ms)  Sleep((DWORD)(ms))
  #define ST_START(t, fn, arg) do { (t) = CreateThread(NULL, 0, (fn), (arg), 0, NULL); } while (0)
  #define ST_JOIN(t)    do { if (t) { WaitForSingleObject((t), INFINITE); CloseHandle((t)); } } while (0)
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t st_thread_t;
  #define ST_THREAD_RET void*
  #define ST_THREAD_0   NULL
  static void st_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define ST_SLEEP(ms)  st_sleep_ms((int)(ms))
  #define ST_START(t, fn, arg) do { if (pthread_create(&(t), NULL, (fn), (arg)) != 0) (t) = 0; } while (0)
  #define ST_JOIN(t)    do { if (t) pthread_join((t), NULL); } while (0)
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define NB_FAMILLES 10
static int g_pass[NB_FAMILLES] = {0};
static int g_fail[NB_FAMILLES] = {0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Puits d'enregistrement : le CONSOMMATEUR cote test.
 *
 * owl_http_rep_ecrit(flux, puits) est l'appel de DRAIN du transport. Ce
 * puits horodate le 1er write (preuve d'entrelacement), accumule les
 * octets bruts (framing chunked a analyser), et peut simuler un
 * consommateur mort/fige (echoue_a_partir_de) ou lent (sleep_par_write_ms).
 * ================================================================ */

typedef struct {
    char     buf[65536];
    size_t   len;
    int      nb_writes;
    uint64_t t_premier_write;      /* 0 = aucun write encore */
    int      echoue_a_partir_de;   /* -1 = jamais ; sinon rend -1 quand nb_writes >= n */
    int      sleep_par_write_ms;
} PuitsRec;

static void puits_rec_init(PuitsRec* p) {
    memset(p, 0, sizeof(*p));
    p->echoue_a_partir_de = -1;
}

static int puits_rec_ecrit(void* user, const char* octets, size_t len) {
    PuitsRec* p = (PuitsRec*)user;
    if (p->nb_writes == 0) p->t_premier_write = owl_tcp_server_horloge_ms();
    if (p->echoue_a_partir_de >= 0 && p->nb_writes >= p->echoue_a_partir_de) {
        return -1;   /* consommateur mort / ecriture socket en timeout */
    }
    if (p->len + len < sizeof(p->buf)) { memcpy(p->buf + p->len, octets, len); p->len += len; }
    p->nb_writes++;
    if (p->sleep_par_write_ms) ST_SLEEP(p->sleep_par_write_ms);
    return 0;
}

/* ================================================================
 * Analyseur chunked (RFC 9112 §7.1)
 *
 * <taille hex>[;ext]CRLF <donnees> CRLF ... 0CRLF CRLF. La taille est
 * lue depuis le CADRAGE, jamais devinee du contenu : c'est ce qui rend
 * l'analyse insensible a un \r\n ou un faux 0\r\n\r\n DANS les donnees.
 * Concatene les donnees dans `out`, pose *out_len, *nb_data (chunks de
 * donnees), *a_terminal. Rend 1 si le cadrage est coherent de bout en bout.
 * ================================================================ */

static int parse_chunked(const char* buf, size_t len,
                         char* out, size_t out_cap, size_t* out_len,
                         int* nb_data, int* a_terminal) {
    size_t i = 0, o = 0;
    int ndata = 0, terminal = 0, ok = 1;

    for (;;) {
        if (i >= len) { ok = 0; break; }

        /* Taille en hexa, jusqu'au CRLF ou au ';' d'extension. */
        size_t taille = 0;
        int vus = 0;
        while (i < len && buf[i] != '\r' && buf[i] != ';') {
            char c = buf[i];
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else { ok = 0; break; }
            taille = taille * 16 + (size_t)d;
            vus++; i++;
        }
        if (!ok || !vus) { ok = 0; break; }

        /* Sauter une eventuelle extension jusqu'au CRLF. */
        while (i < len && buf[i] != '\r') i++;
        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') { ok = 0; break; }
        i += 2;

        if (taille == 0) { terminal = 1; break; }   /* chunk terminal */

        if (i + taille > len) { ok = 0; break; }
        if (o + taille <= out_cap) { memcpy(out + o, buf + i, taille); o += taille; }
        i += taille;

        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') { ok = 0; break; }
        i += 2;
        ndata++;
    }

    if (out_len)    *out_len    = o;
    if (nb_data)    *nb_data    = ndata;
    if (a_terminal) *a_terminal = terminal;
    return ok;
}

/* ================================================================
 * FAMILLE 0 — vrai flux : entrelacement prouve par le timing
 *
 * Un producteur pousse N morceaux espaces DANS SON THREAD ; le drain
 * (owl_http_rep_ecrit sur le thread principal) recoit le 1er morceau
 * bien AVANT que le producteur ait fini. Bufferise = le 1er write
 * n'arriverait qu'apres la fin du producteur ; l'ecart de temps tranche.
 * ================================================================ */

typedef struct {
    OwlHttpRep* flux;
    const char* chunks[64];
    size_t      lens[64];
    int         n;
    int         delai_ms;
    int         avorte;            /* 1 = avorte au lieu de termine */
    /* resultats (ecrits par le thread, lus apres join) */
    volatile int      done;
    volatile int      pousses_ok;
    volatile int      recu_abandon;
    volatile uint64_t t_fin;
} ProdDirect;

static ST_THREAD_RET prod_direct_fn(void* arg) {
    ProdDirect* p = (ProdDirect*)arg;
    for (int i = 0; i < p->n; i++) {
        OwlHttpFluxStatut s = owl_http_rep_flux_pousse(p->flux, p->chunks[i], p->lens[i]);
        if (s != OWL_HTTP_FLUX_OK) { p->recu_abandon = 1; break; }
        p->pousses_ok++;
        if (p->delai_ms) ST_SLEEP(p->delai_ms);
    }
    if (p->avorte) owl_http_rep_flux_avorte(p->flux);
    else           owl_http_rep_flux_termine(p->flux);
    p->t_fin = owl_tcp_server_horloge_ms();
    p->done  = 1;
    return ST_THREAD_0;
}

static void famille_0_vrai_flux(void) {
    g_famille = 0;

    OwlHttpRep* flux = owl_http_rep_cree_flux(200, "text/plain", 1 << 20);
    CHECK(flux != NULL, "cree_flux_ok");
    CHECK(owl_http_rep_est_flux(flux), "est_flux_vrai");
    if (!flux) return;

    static const char* morceaux[] = { "aa", "bbbb", "cc", "dddddd", "ee", "ff" };
    ProdDirect p; memset(&p, 0, sizeof(p));
    p.flux = flux; p.n = 6; p.delai_ms = 30;
    for (int i = 0; i < p.n; i++) { p.chunks[i] = morceaux[i]; p.lens[i] = strlen(morceaux[i]); }

    st_thread_t th; ST_START(th, prod_direct_fn, &p);

    /* Drain bloquant : rend la main au chunk terminal. Pendant ce temps le
     * producteur pousse au rythme du delai. */
    PuitsRec rec; puits_rec_init(&rec);
    OwlHttpPuits puits; puits.ecrit = puits_rec_ecrit; puits.user = &rec;
    int ecrit = owl_http_rep_ecrit(flux, &puits);

    ST_JOIN(th);

    CHECK(ecrit == 0, "drain_termine_proprement");

    /* Entrelacement : le 1er octet a ete drainé AVANT (et de loin) la fin
     * du producteur. 6 morceaux a 30 ms => ~180 ms ; le 1er write tombe
     * vers 0-30 ms. Marge de 60 ms pour rester insensible a la machine. */
    CHECK(rec.t_premier_write != 0 && rec.t_premier_write + 60 < p.t_fin,
          "premier_octet_avant_la_fin_du_producteur");

    /* Contenu de-chunke = concatenation exacte des morceaux, dans l'ordre. */
    char decode[4096]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(rec.buf, rec.len, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1,  "cadrage_chunked_coherent");
    CHECK(term    == 1,  "chunk_terminal_present");
    CHECK(nb      == 6,  "six_chunks_de_donnees");
    CHECK(dl == strlen("aabbbbccddddddeeff") &&
          memcmp(decode, "aabbbbccddddddeeff", dl) == 0, "contenu_de_chunke_exact");

    owl_http_rep_flux_relache(flux);
}

/* ================================================================
 * FAMILLE 1 — framing chunked bout-en-bout (mock peer)
 *
 * Transfer-Encoding: chunked present, AUCUN Content-Length, corps de-chunke
 * exact, chunk terminal. Le producteur passe par le SEAM (OwlHttpProdCtx),
 * comme le fera serve-llm. Config producteur-only : l'executeur doit
 * pouvoir demarrer sans procedure OWL (mode C).
 * ================================================================ */

typedef struct {
    const char* chunks[16];
    size_t      lens[16];
    int         n;
    int         statut;
    int         delai_ms;
} ProdParams;

static void test_producteur(const OwlHttpRequete* req, OwlHttpProdCtx* ctx, void* user) {
    (void)req;
    ProdParams* pp = (ProdParams*)user;
    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, pp->statut, "text/plain", 1 << 16);
    if (!f) return;
    for (int i = 0; i < pp->n; i++) {
        if (owl_http_rep_flux_pousse(f, pp->chunks[i], pp->lens[i]) != OWL_HTTP_FLUX_OK) {
            owl_http_rep_flux_avorte(f);
            owl_http_rep_flux_relache(f);
            return;
        }
        if (pp->delai_ms) ST_SLEEP(pp->delai_ms);
    }
    owl_http_rep_flux_termine(f);
    owl_http_rep_flux_relache(f);
}

/* Un echange e2e : requete brute -> reponse brute, executeur en flux. */
typedef struct {
    int    demarre_ok;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} EchangeStream;

static EchangeStream joue_stream(OwlHttpExecConfig* xc, const char* requete) {
    EchangeStream e; memset(&e, 0, sizeof(e));

    OwlHttpExecuteur* x = owl_http_executeur_cree(xc);
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
    mc.octets                = requete;
    mc.octets_len            = strlen(requete);
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

/* Extrait le corps (apres le premier CRLFCRLF). */
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

static void famille_1_framing_e2e(void) {
    g_famille = 1;

    ProdParams pp; memset(&pp, 0, sizeof(pp));
    pp.statut = 200; pp.n = 3;
    pp.chunks[0] = "Hello, "; pp.chunks[1] = "streaming "; pp.chunks[2] = "world";
    for (int i = 0; i < pp.n; i++) pp.lens[i] = strlen(pp.chunks[i]);

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 400;   /* borne le chemin d'echec en stub */
    xc.producteur         = test_producteur;
    xc.producteur_user    = &pp;

    EchangeStream e = joue_stream(&xc, "GET /flux HTTP/1.1\r\nHost: a\r\n\r\n");

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "ligne_statut_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked\r\n") != NULL,
          "transfer_encoding_chunked_present");
    CHECK(strstr(e.reponse, "Content-Length:") == NULL, "aucun_content_length");

    size_t cl = 0;
    const char* corps = corps_de(&e, &cl);
    char decode[4096]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1, "corps_chunked_parse_able");
    CHECK(term    == 1, "chunk_terminal_present");
    CHECK(dl == strlen("Hello, streaming world") &&
          memcmp(decode, "Hello, streaming world", dl) == 0, "corps_de_chunke_exact");
}

/* ================================================================
 * FAMILLE 2 — contre-pression : file pleine -> pousse BLOQUE
 *
 * Borne = un seul morceau. Le producteur pousse 4 morceaux ; le 2e BLOQUE
 * tant que le drain n'a pas fait de place. On observe le blocage AVANT de
 * drainer : `done` doit rester 0. Puis on draine -> le producteur finit.
 * Pas d'explosion memoire : au plus un morceau est en file a la fois.
 * ================================================================ */

static void famille_2_contre_pression(void) {
    g_famille = 2;

    const size_t taille = 8;
    OwlHttpRep* flux = owl_http_rep_cree_flux(200, "text/plain", taille);
    CHECK(flux != NULL, "cree_flux_borne_octets");
    if (!flux) return;

    static const char* morceaux[] = { "AAAAAAAA", "BBBBBBBB", "CCCCCCCC", "DDDDDDDD" };
    ProdDirect p; memset(&p, 0, sizeof(p));
    p.flux = flux; p.n = 4;
    for (int i = 0; i < p.n; i++) { p.chunks[i] = morceaux[i]; p.lens[i] = taille; }

    st_thread_t th; ST_START(th, prod_direct_fn, &p);

    /* Fenetre d'observation : le producteur est bloque sur le 2e push. Un
     * `done` deja a 1 signifierait qu'il a tout avale sans contre-pression. */
    ST_SLEEP(80);
    CHECK(p.done == 0,          "producteur_bloque_file_pleine");
    CHECK(p.pousses_ok <= 1,    "au_plus_un_morceau_en_file");

    /* On draine : la contre-pression se relache, le producteur avance. */
    PuitsRec rec; puits_rec_init(&rec);
    OwlHttpPuits puits; puits.ecrit = puits_rec_ecrit; puits.user = &rec;
    int ecrit = owl_http_rep_ecrit(flux, &puits);

    ST_JOIN(th);

    CHECK(ecrit == 0,        "drain_termine_apres_contre_pression");
    CHECK(p.done == 1,       "producteur_termine_une_fois_draine");
    CHECK(p.pousses_ok == 4, "les_quatre_morceaux_finissent_pousses");

    char decode[4096]; size_t dl = 0; int nb = 0, term = 0;
    parse_chunked(rec.buf, rec.len, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(nb == 4, "quatre_chunks_draines");
    CHECK(dl == 4 * taille, "aucun_octet_perdu_sous_contre_pression");

    owl_http_rep_flux_relache(flux);
}

/* ================================================================
 * FAMILLE 3 — timeout par chunk : consommateur fige -> coupure
 *
 * Le puits simule une ecriture socket qui echoue (consommateur fige,
 * echeance timeout_ecriture_ms depassee cote transport). Le drain doit
 * COUPER (rendre != 0), et l'abandon doit remonter au producteur via un
 * OWL_HTTP_FLUX_ABANDON pour qu'il s'arrete au lieu de generer sans fin.
 * Borne large : le producteur ne peut pas se bloquer, la seule sortie
 * propre est l'ABANDON — pas d'interblocage si l'impl regresse.
 * ================================================================ */

static void famille_3_timeout_chunk(void) {
    g_famille = 3;

    OwlHttpRep* flux = owl_http_rep_cree_flux(200, "text/plain", 1 << 20);
    CHECK(flux != NULL, "cree_flux_ok");
    if (!flux) return;

    static const char* morceaux[16];
    static char        store[16][8];
    ProdDirect p; memset(&p, 0, sizeof(p));
    p.flux = flux; p.n = 12; p.delai_ms = 20;
    for (int i = 0; i < p.n; i++) {
        memset(store[i], 'x', sizeof(store[i]));
        morceaux[i] = store[i];
        p.chunks[i] = store[i]; p.lens[i] = sizeof(store[i]);
    }
    (void)morceaux;

    st_thread_t th; ST_START(th, prod_direct_fn, &p);

    /* Consommateur fige : echoue des la 2e ecriture (mi-chunk). */
    PuitsRec rec; puits_rec_init(&rec);
    rec.echoue_a_partir_de = 1;
    OwlHttpPuits puits; puits.ecrit = puits_rec_ecrit; puits.user = &rec;
    int ecrit = owl_http_rep_ecrit(flux, &puits);

    ST_JOIN(th);

    CHECK(ecrit != 0,        "drain_coupe_sur_consommateur_fige");
    CHECK(p.recu_abandon == 1, "producteur_recoit_abandon");
    CHECK(p.pousses_ok < p.n,  "producteur_s_arrete_avant_la_fin");

    owl_http_rep_flux_relache(flux);
}

/* ================================================================
 * FAMILLE 4 — erreur mi-flux : avorte -> pas de chunk terminal
 *
 * Un push, puis avorte (pas termine). Le drain doit rendre != 0 (fermeture
 * abrupte) et le flux drainé ne doit PAS contenir le terminal 0\r\n\r\n :
 * c'est ce qui fait detecter au client une reponse chunked tronquee.
 * Mono-thread : borne large, on pousse puis on draine.
 * ================================================================ */

static void famille_4_erreur_mi_flux(void) {
    g_famille = 4;

    OwlHttpRep* flux = owl_http_rep_cree_flux(200, "text/plain", 1 << 20);
    CHECK(flux != NULL, "cree_flux_ok");
    if (!flux) return;

    CHECK(owl_http_rep_flux_pousse(flux, "premier", 7) == OWL_HTTP_FLUX_OK, "premier_chunk_pousse");
    owl_http_rep_flux_avorte(flux);

    PuitsRec rec; puits_rec_init(&rec);
    OwlHttpPuits puits; puits.ecrit = puits_rec_ecrit; puits.user = &rec;
    int ecrit = owl_http_rep_ecrit(flux, &puits);

    CHECK(ecrit != 0, "drain_abrupt_apres_avorte");

    /* Le terminal 0CRLF CRLF ne doit apparaitre NULLE PART. */
    int a_terminal = 0;
    for (size_t i = 0; i + 5 <= rec.len; i++) {
        if (memcmp(rec.buf + i, "0\r\n\r\n", 5) == 0) { a_terminal = 1; break; }
    }
    CHECK(a_terminal == 0, "pas_de_chunk_terminal_apres_avorte");

    owl_http_rep_flux_relache(flux);
}

/* ================================================================
 * FAMILLE 5 — corpus chunked-out : contenu piege, cadrage intact
 *
 * Des morceaux dont les DONNEES contiennent \r\n, un faux terminateur
 * 0\r\n\r\n, un morceau vide, et une taille limite. Le cadrage doit rester
 * pilote par le TRANSPORT (taille = longueur reelle), jamais par le
 * contenu : rien ne doit pouvoir injecter/casser le framing.
 * ================================================================ */

static void famille_5_corpus_chunked_out(void) {
    g_famille = 5;

    OwlHttpRep* flux = owl_http_rep_cree_flux(200, "application/octet-stream", 1 << 20);
    CHECK(flux != NULL, "cree_flux_ok");
    if (!flux) return;

    const char* c0 = "ab\r\ncd";       /* CRLF dans les donnees            */
    const char* c1 = "0\r\n\r\n";      /* faux terminateur dans les donnees */
    const char* c2 = "";               /* morceau vide : PAS de terminal    */
    char        c3[255]; memset(c3, 'Z', sizeof(c3));   /* taille limite 0xFF */

    CHECK(owl_http_rep_flux_pousse(flux, c0, 6)           == OWL_HTTP_FLUX_OK, "push_crlf");
    CHECK(owl_http_rep_flux_pousse(flux, c1, 5)           == OWL_HTTP_FLUX_OK, "push_faux_terminal");
    /* Un push vide ne doit pas emettre de chunk 0 (= terminal) au milieu. */
    owl_http_rep_flux_pousse(flux, c2, 0);
    CHECK(owl_http_rep_flux_pousse(flux, c3, sizeof(c3))  == OWL_HTTP_FLUX_OK, "push_taille_limite");
    owl_http_rep_flux_termine(flux);

    PuitsRec rec; puits_rec_init(&rec);
    OwlHttpPuits puits; puits.ecrit = puits_rec_ecrit; puits.user = &rec;
    int ecrit = owl_http_rep_ecrit(flux, &puits);
    CHECK(ecrit == 0, "drain_ok");

    char decode[4096]; size_t dl = 0; int nb = 0, term = 0;
    int cadrage = parse_chunked(rec.buf, rec.len, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(cadrage == 1, "cadrage_intact_malgre_contenu_piege");
    CHECK(term    == 1, "un_seul_terminal_en_fin");
    CHECK(nb      == 3, "trois_chunks_de_donnees_le_vide_ne_compte_pas");

    /* De-chunke = concatenation exacte, le faux terminal survit comme
     * DONNEES et non comme cadrage. */
    size_t attendu_len = 6 + 5 + sizeof(c3);
    char attendu[512];
    memcpy(attendu, c0, 6);
    memcpy(attendu + 6, c1, 5);
    memset(attendu + 11, 'Z', sizeof(c3));
    CHECK(dl == attendu_len && memcmp(decode, attendu, attendu_len) == 0,
          "contenu_de_chunke_octet_pour_octet");

    owl_http_rep_flux_relache(flux);
}

/* ================================================================
 * FAMILLE 6 — regle d'or : gc_frontiere vert, nb_threads_vm == 1
 *
 * Un flux servi bout-en-bout avec gc_frontiere=1 : aucune reference GC ne
 * traverse (l'octet exact survit au GC force), et un SEUL thread a touche
 * la VM malgre le transport concurrent.
 * ================================================================ */

static void famille_6_regle_d_or(void) {
    g_famille = 6;

    ProdParams pp; memset(&pp, 0, sizeof(pp));
    pp.statut = 200; pp.n = 3;
    pp.chunks[0] = "tok-A "; pp.chunks[1] = "tok-B "; pp.chunks[2] = "tok-C";
    for (int i = 0; i < pp.n; i++) pp.lens[i] = strlen(pp.chunks[i]);

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 400;
    xc.gc_frontiere       = 1;
    xc.producteur         = test_producteur;
    xc.producteur_user    = &pp;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    CHECK(x != NULL, "executeur_cree");
    if (!x) return;
    int demarre = owl_http_executeur_demarre(x) ? 1 : 0;
    CHECK(demarre, "executeur_flux_demarre");

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        CHECK(0, "ecoute_impossible");
        owl_http_executeur_libere(x);
        return;
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
    mc.octets                = "GET /or HTTP/1.1\r\nHost: a\r\n\r\n";
    mc.octets_len            = strlen(mc.octets);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 4000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); owl_http_executeur_libere(x); CHECK(0, "pair"); return; }

    owl_http_serveur_sert_une_connexion(&l, &cfg, 5000);
    owl_mock_peer_joint(pair);

    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    char rep[OWL_MOCK_PEER_MAX_RECU + 1];
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(rep, recu, rl); rep[rl] = '\0';

    /* Un SEUL thread a touche la VM (le thread executeur), malgre le
     * transport qui a tourne dans le thread principal. */
    CHECK(owl_http_executeur_nb_threads_vm(x) == 1, "un_seul_thread_a_touche_la_vm");

    /* L'octet exact a survecu au GC force aux deux frontieres. */
    EchangeStream tmp; tmp.reponse_len = rl; memcpy(tmp.reponse, rep, rl + 1);
    size_t cl = 0; const char* corps = corps_de(&tmp, &cl);
    char decode[4096]; size_t dl = 0; int nb = 0, term = 0;
    parse_chunked(corps, cl, decode, sizeof(decode), &dl, &nb, &term);
    CHECK(dl == strlen("tok-A tok-B tok-C") &&
          memcmp(decode, "tok-A tok-B tok-C", dl) == 0, "octet_exact_malgre_gc_force");

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 7 — plafond neutralise pour un flux demarre (D1)
 *
 * Un producteur qui DEMARRE puis genere plus lentement que
 * plafond_attente_ms ne doit PAS produire un 503 : l'hypothese « attente
 * longue = client parti » est fausse en streaming. Le plafond ne borne que
 * l'attente EN FILE, pas la duree d'un job deja demarre.
 * ================================================================ */

static void famille_7_plafond_neutralise(void) {
    g_famille = 7;

    ProdParams pp; memset(&pp, 0, sizeof(pp));
    pp.statut = 200; pp.n = 4; pp.delai_ms = 120;   /* 4*120 = 480 ms > plafond */
    pp.chunks[0] = "lent0"; pp.chunks[1] = "lent1"; pp.chunks[2] = "lent2"; pp.chunks[3] = "lent3";
    for (int i = 0; i < pp.n; i++) pp.lens[i] = strlen(pp.chunks[i]);

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 150;   /* plus court que la duree de generation */
    xc.producteur         = test_producteur;
    xc.producteur_user    = &pp;

    EchangeStream e = joue_stream(&xc, "GET /lent HTTP/1.1\r\nHost: a\r\n\r\n");

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 503") == NULL, "generation_lente_pas_de_503");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "generation_lente_reste_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked\r\n") != NULL,
          "generation_lente_bien_en_flux");
}

/* ================================================================
 * FAMILLE 8 — statut sans corps refuse (D7)
 *
 * cree_flux avec 204/304/1xx -> NULL (un flux implique un corps ; un
 * cadrage chunked sur un statut sans corps est incoherent). Controle
 * positif : 200 rend un vrai flux.
 * ================================================================ */

static void famille_8_statut_sans_corps(void) {
    g_famille = 8;

    OwlHttpRep* ok = owl_http_rep_cree_flux(200, "text/plain", 1 << 16);
    CHECK(ok != NULL, "statut_200_accepte");
    CHECK(ok != NULL && owl_http_rep_est_flux(ok), "statut_200_est_bien_un_flux");
    if (ok) owl_http_rep_flux_relache(ok);

    CHECK(owl_http_rep_cree_flux(204, "text/plain", 1 << 16) == NULL, "statut_204_refuse");
    CHECK(owl_http_rep_cree_flux(304, "text/plain", 1 << 16) == NULL, "statut_304_refuse");
    CHECK(owl_http_rep_cree_flux(100, "text/plain", 1 << 16) == NULL, "statut_100_refuse");
    CHECK(owl_http_rep_cree_flux(199, "text/plain", 1 << 16) == NULL, "statut_1xx_refuse");
}

/* ================================================================
 * FAMILLE 9 — HEAD sur flux : en-tetes + cadrage, ZERO corps, producteur
 *             arrete (ferme le trou de )
 *
 * Un HEAD sur une reponse en flux doit renvoyer les MEMES en-tetes qu'un
 * GET (dont Transfer-Encoding: chunked) mais AUCUN octet de corps, et
 * stopper le producteur : le transport draine avec un puits mort
 * (puits_mort_ecrit) qui pose consommateur_fini, donc la prochaine pousse
 * rend ABANDON. Le comportement existe deja (http_server.c
 * ecrit_reponse_flux, branche head) — ce test le VERROUILLE, il doit
 * passer AU VERT.
 *
 * Le producteur pousse en boucle (petit delai par pousse) jusqu'a recevoir
 * ABANDON : borne haute large + pas d'interblocage si l'impl regresse.
 * ================================================================ */

typedef struct {
    int          statut;
    volatile int recu_abandon;   /* 1 = une pousse a rendu ABANDON */
    volatile int pousses_ok;
} HeadProd;

static void head_producteur(const OwlHttpRequete* req, OwlHttpProdCtx* ctx,
                            void* user) {
    (void)req;
    HeadProd* hp = (HeadProd*)user;
    OwlHttpRep* f = owl_http_prod_ouvre_flux(ctx, hp->statut,
                                             "text/event-stream", 1 << 20);
    if (!f) return;
    /* attend_premier (cote transport) exige au moins un chunk pour committer
     * l'en-tete ; ensuite le puits mort pose consommateur_fini et la pousse
     * suivante rend ABANDON. La boucle bornee garantit l'arret meme si le
     * signal se perd (regression). */
    for (int i = 0; i < 200; i++) {
        if (owl_http_rep_flux_pousse(f, "data: x\n\n", 8) != OWL_HTTP_FLUX_OK) {
            hp->recu_abandon = 1;
            break;
        }
        hp->pousses_ok++;
        ST_SLEEP(5);
    }
    owl_http_rep_flux_avorte(f);
    owl_http_rep_flux_relache(f);
}

static void famille_9_head_sur_flux(void) {
    g_famille = 9;

    HeadProd hp; memset(&hp, 0, sizeof(hp));
    hp.statut = 200;

    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 8;
    xc.plafond_attente_ms = 2000;
    xc.producteur         = head_producteur;
    xc.producteur_user    = &hp;

    EchangeStream e = joue_stream(&xc, "HEAD /flux HTTP/1.1\r\nHost: a\r\n\r\n");

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "head_flux_statut_200");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked\r\n") != NULL,
          "head_flux_transfer_encoding_present");
    CHECK(strstr(e.reponse, "Content-Length:") == NULL,
          "head_flux_aucun_content_length");

    size_t cl = 0;
    (void)corps_de(&e, &cl);
    CHECK(cl == 0, "head_flux_aucun_octet_de_corps");

    CHECK(hp.pousses_ok >= 1, "head_flux_producteur_a_pu_pousser_le_premier");
    CHECK(hp.recu_abandon == 1, "head_flux_producteur_arrete_par_abandon");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_stream_self_test_run(void) {
    printf("== Streaming HTTP mode C (feat/1668) ==\n");

    famille_0_vrai_flux();
    famille_1_framing_e2e();
    famille_2_contre_pression();
    famille_3_timeout_chunk();
    famille_4_erreur_mi_flux();
    famille_5_corpus_chunked_out();
    famille_6_regle_d_or();
    famille_7_plafond_neutralise();
    famille_8_statut_sans_corps();
    famille_9_head_sur_flux();

    int tp = 0, tf = 0;
    for (int i = 0; i < NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 vrai flux (entrelacement) : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 framing chunked e2e       : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 contre-pression           : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 timeout par chunk         : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 erreur mi-flux            : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 corpus chunked-out        : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 regle d'or (gc + 1 thread): %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 plafond neutralise        : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   f8 statut sans corps refuse  : %d/%d\n", g_pass[8], g_pass[8] + g_fail[8]);
    printf("   f9 HEAD sur flux             : %d/%d\n", g_pass[9], g_pass[9] + g_fail[9]);
    printf("   TOTAL streaming mode C       : %d/%d\n", tp, tp + tf);
    return tf;
}
