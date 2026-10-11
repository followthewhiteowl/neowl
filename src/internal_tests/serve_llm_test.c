// SPDX-License-Identifier: AGPL-3.0-only
/* serve_llm_test.c — API LLM locale compatible OpenAI.
 *
 * Ticket : 
 * Standard : API OpenAI (POST /v1/chat/completions SSE + GET /v1/models),
 *            transporte par le socle streaming mode C (, chunked).
 *
 * DIGUE TEST-FIRST (niveau C). Ces tests definissent le CONTRAT de serve-llm
 * (routage OpenAI, framing SSE `chat.completion.chunk`, single-shot
 * `chat.completion` + usage, /v1/models, erreurs) ; ils sont ROUGES tant que
 * l'impl vit en stub (cf. serve_llm.c « STUB  » -> flux 500).
 *
 * CLE DE TESTABILITE : le producteur genere via ServeLlmCtx.generate, un seam
 * INDIRECT. En test on y branche un FAUX generateur qui emet une suite de
 * pieces fixee (naturel / max-tokens / erreur mi-flux) exactement comme le
 * fera owl_llm_chat_stream sur un vrai modele. Tout le framing / routage /
 * mapping OpenAI se verifie donc SANS charger le moindre GGUF. Le seam C
 * lui-meme (owl_llm_*) est teste a part, gate par OWL_LLAMA_TEST_GGUF.
 *
 * Dix familles :
 *   0. POST /v1/chat/completions stream:true -> 200 text/event-stream chunked,
 *      1re frame delta.role=assistant, deltas content ordonnes, finale
 *      finish_reason:"stop", data: [DONE].
 *   1. POST JSON malforme -> 400 propre, pas de crash.
 *   2. POST stream:false -> UN chunk JSON chat.completion + usage (decision A).
 *   3. GET /v1/models -> {object:list, data[0].id/object/owned_by}.
 *   4. finish_reason:"length" (faux generateur MAX_TOKENS).
 *   5. erreur mi-flux (faux generateur erreur apres 2 tokens) -> avorte, pas
 *      de chunk terminal, pas de [DONE].
 *   6. route/methode inconnue -> 404.
 *   7. Authorization present ET absent -> comportement identique (ignore).
 *   8. seam C (gate OWL_LLAMA_TEST_GGUF) + contrat model-free (chemin absent).
 *   9. UTF-8 partiel (decision D) : un caractere multi-octet coupe en frontiere
 *      de token (2/3/4 octets, coupure au milieu) doit etre RETENU puis emis
 *      reconstitue -- jamais un demi-caractere dans une frame (JSON invalide).
 *      Chaque delta.content isole est de l'UTF-8 bien forme, et la
 *      concatenation des content == la chaine complete attendue.
 *
 * Cinq familles tool-calling (, DIGUE test-first, ROUGES tant que le
 * routage tool-calling vit en stub -- cf. serve_llm.c « STUB  » qui
 * IGNORE `tools` et retombe sur le chemin texte) :
 *  10. tools + stream:false -> message.tool_calls (name + arguments string
 *      echappee), finish_reason:"tool_calls", content:null.
 *  11. tools + stream:true  -> deltas SSE tool_calls (id + function.name puis
 *      function.arguments), frame finale finish_reason:"tool_calls", [DONE].
 *  12. tool_choice auto/required/none/named : auto+none avec faux gen texte
 *      restent en texte (pas d'appel) ; required+named avec faux gen d'appel
 *      forcent un tool_call (le differenciateur « force »).
 *  13. message role:"tool" (multi-tours) accepte -- pas de 400 ni crash.
 *  14. `tools` ABSENT -> chemin chat inchange (aucun tool_calls).
 * Le faux generateur `faux_gen_toolcall` pousse une JSON d'appel fixe
 * ({"name":"get_weather","arguments":{"location":"Paris"}}) en un seul morceau
 * termine=1, comme un modele contraint par la GBNF d'outil.
 *
 * Execute via `owl --self-test-net` (cf. main.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../serve_llm.h"     /* ServeLlmCtx, serve_llm_producteur, seam */
#include "../http_exec.h"
#include "../http_server.h"
#include "../net_mock_peer.h"
#include "../sentinels.h"     /* OWL_STOP_NATUREL / OWL_STOP_MAX_TOKENS */

/* ================================================================
 * Comptage
 * ================================================================ */

#define NB_FAMILLES 18
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
 * Faux generateurs — injectes dans ServeLlmCtx.generate
 *
 * Ils dispensent le producteur de tout modele : ils drivent `sink` avec une
 * suite de pieces fixee, exactement comme owl_llm_chat_stream sur un vrai
 * modele (contrat : `termine`=0 pendant le flux, `termine`=1 sur le DERNIER
 * morceau), puis posent l'issue (raison d'arret + compteurs). Un retour du
 * sink != 0 = consommateur parti -> arret propre (decision B).
 * ================================================================ */

/* Pieces communes : "Hello", " ", "world" -> concatene "Hello world". */
static const char* const FAUX_TOKS[] = { "Hello", " ", "world" };
#define FAUX_NB_TOKS 3

/* Arret naturel : les 3 pieces, la derniere avec termine=1. */
static int faux_gen_naturel(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                            OwlLlmTokenSink sink, void* sink_user,
                            OwlLlmChatIssue* issue) {
    (void)ctx; (void)params;
    for (int i = 0; i < FAUX_NB_TOKS; i++) {
        int termine = (i == FAUX_NB_TOKS - 1);
        if (sink(FAUX_TOKS[i], strlen(FAUX_TOKS[i]), termine, sink_user) != 0) {
            if (issue) { memset(issue, 0, sizeof(*issue)); issue->stop_reason = OWL_STOP_NATUREL; }
            return 0;   /* consommateur parti : arret propre, pas une erreur */
        }
    }
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->stop_reason   = OWL_STOP_NATUREL;
        issue->input_tokens  = 5;
        issue->output_tokens = FAUX_NB_TOKS;
        issue->erreur        = 0;
    }
    return 0;
}

/* Plafond atteint : memes pieces, raison MAX_TOKENS -> finish_reason "length". */
static int faux_gen_maxtokens(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                              OwlLlmTokenSink sink, void* sink_user,
                              OwlLlmChatIssue* issue) {
    (void)ctx; (void)params;
    for (int i = 0; i < FAUX_NB_TOKS; i++) {
        int termine = (i == FAUX_NB_TOKS - 1);
        if (sink(FAUX_TOKS[i], strlen(FAUX_TOKS[i]), termine, sink_user) != 0) {
            if (issue) { memset(issue, 0, sizeof(*issue)); issue->stop_reason = OWL_STOP_MAX_TOKENS; }
            return 0;
        }
    }
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->stop_reason   = OWL_STOP_MAX_TOKENS;
        issue->input_tokens  = 5;
        issue->output_tokens = FAUX_NB_TOKS;
        issue->erreur        = 0;
    }
    return 0;
}

/* Erreur mi-flux : 2 pieces poussees (termine=0), puis echec -> le producteur
 * doit AVORTER le flux deja committe (pas de terminal, pas de [DONE]). */
static int faux_gen_erreur(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                           OwlLlmTokenSink sink, void* sink_user,
                           OwlLlmChatIssue* issue) {
    (void)ctx; (void)params;
    for (int i = 0; i < 2; i++) {
        if (sink(FAUX_TOKS[i], strlen(FAUX_TOKS[i]), 0, sink_user) != 0) {
            if (issue) { memset(issue, 0, sizeof(*issue)); issue->erreur = 1; }
            return 1;
        }
    }
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->output_tokens = 2;
        issue->erreur        = 1;   /* erreur apres >= 1 token = mi-flux */
    }
    return 1;   /* != 0 = echec */
}

/* Faux generateur UTF-8 partiel (famille 9). La liste de pieces est fixee par
 * cas via set_utf8_pieces() : chaque piece est poussee telle quelle (termine=0),
 * la DERNIERE avec termine=1 -- exactement le decoupage token que produirait un
 * vrai modele quand un caractere multi-octet tombe a cheval sur deux tokens. */
static const char* g_utf8_pieces[8];
static size_t      g_utf8_lens[8];
static int         g_utf8_nb = 0;

static void set_utf8_pieces(const char* const* pieces, int nb) {
    g_utf8_nb = nb;
    for (int i = 0; i < nb; i++) {
        g_utf8_pieces[i] = pieces[i];
        g_utf8_lens[i]   = strlen(pieces[i]);   /* aucun octet NUL dans nos pieces */
    }
}

static int faux_gen_utf8(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                         OwlLlmTokenSink sink, void* sink_user,
                         OwlLlmChatIssue* issue) {
    (void)ctx; (void)params;
    for (int i = 0; i < g_utf8_nb; i++) {
        int termine = (i == g_utf8_nb - 1);
        if (sink(g_utf8_pieces[i], g_utf8_lens[i], termine, sink_user) != 0) {
            if (issue) { memset(issue, 0, sizeof(*issue)); issue->stop_reason = OWL_STOP_NATUREL; }
            return 0;
        }
    }
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->stop_reason   = OWL_STOP_NATUREL;
        issue->input_tokens  = 4;
        issue->output_tokens = g_utf8_nb;
        issue->erreur        = 0;
    }
    return 0;
}

/* Faux generateur d'APPEL D'OUTIL (familles 10-12, ). Pousse la JSON
 * d'appel complete en UN SEUL morceau (termine=1) -- exactement ce que produit
 * un modele quand la GBNF d'outil le contraint a emettre l'appel d'un coup. Le
 * routage tool-calling reel accumulera cette JSON et la reframera en tool_calls
 * (message.tool_calls non-stream / deltas SSE stream). Tant que ce routage est
 * un stub, serve_chat traite ces octets comme du texte -> familles 10-12 rouges. */
static const char FAUX_TOOLCALL_JSON[] =
    "{\"name\":\"get_weather\",\"arguments\":{\"location\":\"Paris\"}}";

static int faux_gen_toolcall(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                             OwlLlmTokenSink sink, void* sink_user,
                             OwlLlmChatIssue* issue) {
    (void)ctx; (void)params;
    if (sink(FAUX_TOOLCALL_JSON, sizeof(FAUX_TOOLCALL_JSON) - 1, 1, sink_user) != 0) {
        if (issue) { memset(issue, 0, sizeof(*issue)); issue->stop_reason = OWL_STOP_NATUREL; }
        return 0;
    }
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->stop_reason   = OWL_STOP_NATUREL;
        issue->input_tokens  = 7;
        issue->output_tokens = 1;
        issue->erreur        = 0;
    }
    return 0;
}

/* Invite trop longue (famille 15, ). Reproduit ce que
 * owl_llm_chat_stream pose quand l'invite tokenisee depasse n_batch/n_ctx : le
 * garde rejette AVANT le premier decode -> AUCUN token emis (sink jamais appele,
 * donc s.flux reste NULL cote stream, aucun header SSE engage), rc -1, issue
 * marquee prompt_too_long en plus du drapeau erreur generique. X=9999 jetons
 * d'invite, Y=2048 limite de contexte : le mapping HTTP doit rendre 413 avec
 * ces deux nombres dans le message. */
static int faux_gen_prompt_too_long(ServeLlmCtx* ctx, const OwlLlmChatParams* params,
                                    OwlLlmTokenSink sink, void* sink_user,
                                    OwlLlmChatIssue* issue) {
    (void)ctx; (void)params; (void)sink; (void)sink_user;
    if (issue) {
        memset(issue, 0, sizeof(*issue));
        issue->erreur          = 1;
        issue->prompt_too_long = 1;
        issue->input_tokens    = 9999;
        issue->context_limit   = 2048;
    }
    return -1;   /* echec avant tout token : le producteur doit mapper 413 */
}

/* Sink de test du seam C (famille 8, gate modele). */
typedef struct { int nb; size_t total; int vu_termine; } SeamSink;
static int seam_sink(const char* piece, size_t len, int termine, void* user) {
    SeamSink* s = (SeamSink*)user;
    (void)piece;
    s->nb++;
    s->total += len;
    if (termine) s->vu_termine = 1;
    return 0;
}

/* ================================================================
 * Analyseur chunked (RFC 9112 §7.1) — identique au socle (http_stream_test).
 *
 * La taille est lue depuis le CADRAGE, jamais devinee du contenu : l'analyse
 * reste insensible a un \r\n ou un faux 0\r\n\r\n DANS les donnees (une frame
 * SSE contient des \n, et le corps JSON du single-shot des accolades).
 * ================================================================ */

static int parse_chunked(const char* buf, size_t len,
                         char* out, size_t out_cap, size_t* out_len,
                         int* nb_data, int* a_terminal) {
    size_t i = 0, o = 0;
    int ndata = 0, terminal = 0, ok = 1;

    for (;;) {
        if (i >= len) { ok = 0; break; }

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

        while (i < len && buf[i] != '\r') i++;
        if (i + 1 >= len || buf[i] != '\r' || buf[i + 1] != '\n') { ok = 0; break; }
        i += 2;

        if (taille == 0) { terminal = 1; break; }

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
 * Harnais e2e : requete brute -> reponse brute, executeur en flux avec
 * producteur = serve_llm_producteur (calque de joue_stream du socle).
 * ================================================================ */

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

/* Corps de la reponse (apres le premier CRLFCRLF). */
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

/* De-chunke le corps de la reponse dans `out` (NUL-termine) pour strstr. */
static int dechunk_reponse(const EchangeStream* e, char* out, size_t out_cap,
                           int* nb_data, int* a_terminal) {
    size_t cl = 0;
    const char* corps = corps_de(e, &cl);
    size_t dl = 0;
    int ok = parse_chunked(corps, cl, out, out_cap - 1, &dl, nb_data, a_terminal);
    out[dl < out_cap ? dl : out_cap - 1] = '\0';
    return ok;
}

/* 1 si toutes les aiguilles apparaissent dans `foin`, DANS L'ORDRE. */
static int en_ordre(const char* foin, const char* const* aiguilles, int n) {
    const char* p = foin;
    for (int i = 0; i < n; i++) {
        const char* q = strstr(p, aiguilles[i]);
        if (!q) return 0;
        p = q + strlen(aiguilles[i]);
    }
    return 1;
}

/* Le terminal chunked 0CRLF CRLF est-il present quelque part dans le brut ? */
static int a_chunk_terminal(const EchangeStream* e) {
    for (size_t i = 0; i + 5 <= e->reponse_len; i++) {
        if (memcmp(e->reponse + i, "0\r\n\r\n", 5) == 0) return 1;
    }
    return 0;
}

/* Valide qu'un buffer est de l'UTF-8 BIEN FORME : aucune sequence tronquee, ni
 * octet de continuation orphelin, ni octet de tete invalide. C'est le controle
 * qui distingue un demi-caractere emis (0xC3 seul) d'un caractere complet. */
static int utf8_bien_forme(const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if      ((c & 0x80) == 0x00) need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return 0;                       /* octet de tete/continuation orphelin */
        if (i + need > n) return 0;           /* sequence tronquee en fin de buffer */
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        }
        i += need;
    }
    return 1;
}

/* Parcourt le corps SSE de-chunke : pour chaque delta.content, decode sa valeur
 * JSON, verifie qu'elle est de l'UTF-8 bien forme (aucun octet partiel dans la
 * frame) et la concatene dans `out`. Retourne le nombre de content vus, ou -1
 * des qu'un content porte de l'UTF-8 tronque (= frame JSON invalide = BUG). */
static int collecte_contents(const char* sse, char* out, size_t out_cap, size_t* out_len) {
    static const char MARQ[] = "\"content\":\"";
    size_t ml = sizeof(MARQ) - 1;
    size_t o = 0;
    int n = 0;
    const char* p = sse;
    for (;;) {
        const char* q = strstr(p, MARQ);
        if (q == NULL) break;
        q += ml;

        char   val[512];
        size_t vl = 0;
        while (*q != '\0' && *q != '"') {
            char d;
            if (*q == '\\' && q[1] != '\0') {   /* echappement JSON (aucun attendu ici) */
                switch (q[1]) {
                    case 'n': d = '\n'; break;
                    case 'r': d = '\r'; break;
                    case 't': d = '\t'; break;
                    case 'b': d = '\b'; break;
                    case 'f': d = '\f'; break;
                    default:  d = q[1]; break;
                }
                q += 2;
            } else {
                d = *q;
                q++;
            }
            if (vl < sizeof(val)) val[vl++] = d;
        }
        if (!utf8_bien_forme(val, vl)) return -1;   /* demi-caractere emis -> BUG */
        if (o + vl <= out_cap) { memcpy(out + o, val, vl); o += vl; }
        n++;
        p = q;
    }
    if (out_len) *out_len = o;
    return n;
}

/* ---------- Batisseurs de requetes ---------- */

static void bati_post(char* out, size_t cap, const char* chemin,
                      const char* corps, int avec_auth) {
    snprintf(out, cap,
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "%s"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "\r\n"
        "%s",
        chemin,
        avec_auth ? "Authorization: Bearer sk-owl-bidon\r\n" : "",
        (unsigned)strlen(corps), corps);
}

static void bati_get(char* out, size_t cap, const char* chemin, int avec_auth) {
    snprintf(out, cap,
        "GET %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "%s"
        "\r\n",
        chemin,
        avec_auth ? "Authorization: Bearer sk-owl-bidon\r\n" : "");
}

static void config_producteur(OwlHttpExecConfig* xc, ServeLlmCtx* ctx) {
    owl_http_exec_config_defaut(xc);
    xc->file_profondeur    = 8;
    xc->plafond_attente_ms = 2000;
    xc->producteur         = serve_llm_producteur;
    xc->producteur_user    = ctx;
}

#define CORPS_MODELE "owl-modele-test"

/* Bloc `tools` reutilise par les familles tool-calling : un outil get_weather a
 * un parametre string `location` requis. */
#define TOOLS_WEATHER \
    "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}]"

static void ctx_defaut(ServeLlmCtx* ctx, ServeLlmGenerate gen) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->model_id = CORPS_MODELE;
    ctx->generate = gen;
}

/* ================================================================
 * FAMILLE 0 — POST /v1/chat/completions stream:true
 * ================================================================ */

static void famille_0_chat_stream(void) {
    g_famille = 0;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"stream\":true}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");
    CHECK(strstr(e.reponse, "text/event-stream") != NULL, "content_type_event_stream");
    CHECK(strstr(e.reponse, "Transfer-Encoding: chunked\r\n") != NULL, "transfer_encoding_chunked");
    CHECK(strstr(e.reponse, "Content-Length:") == NULL, "aucun_content_length");

    char sse[8192]; int nb = 0, term = 0;
    int cadrage = dechunk_reponse(&e, sse, sizeof(sse), &nb, &term);
    CHECK(cadrage == 1, "corps_chunked_parse_able");
    CHECK(term    == 1, "chunk_terminal_present");

    CHECK(strstr(sse, "\"object\":\"chat.completion.chunk\"") != NULL, "objet_chunk");
    CHECK(strstr(sse, "\"role\":\"assistant\"") != NULL, "premiere_frame_delta_role");
    CHECK(strstr(sse, "\"content\":\"Hello\"") != NULL, "delta_content_hello");
    CHECK(strstr(sse, "\"content\":\"world\"") != NULL, "delta_content_world");
    CHECK(strstr(sse, "\"finish_reason\":\"stop\"") != NULL, "finish_reason_stop");
    CHECK(strstr(sse, "data: [DONE]") != NULL, "sentinelle_done");

    const char* seq[] = {
        "\"role\":\"assistant\"", "\"content\":\"Hello\"", "\"content\":\"world\"",
        "\"finish_reason\":\"stop\"", "data: [DONE]"
    };
    CHECK(en_ordre(sse, seq, 5), "frames_ordonnees_role_content_finish_done");
}

/* ================================================================
 * FAMILLE 1 — POST JSON malforme -> 400 propre, pas de crash
 * ================================================================ */

static void famille_1_json_malforme(void) {
    g_famille = 1;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[512];
    /* Accolade non fermee + cle sans valeur : JSON invalide. */
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\", \"messages\": [ ", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(e.reponse_len > 0, "reponse_recue_pas_de_crash");
    CHECK(strstr(e.reponse, "HTTP/1.1 400") == e.reponse, "statut_400");
    CHECK(strstr(e.reponse, "data: [DONE]") == NULL, "pas_de_flux_sse_reussi");
}

/* ================================================================
 * FAMILLE 2 — POST stream:false -> UN chunk JSON chat.completion + usage
 * ================================================================ */

static void famille_2_stream_false(void) {
    g_famille = 2;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"stream\":false}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");

    char body[8192]; int nb = 0, term = 0;
    int cadrage = dechunk_reponse(&e, body, sizeof(body), &nb, &term);
    CHECK(cadrage == 1, "corps_chunked_parse_able");
    CHECK(nb == 1, "un_seul_chunk_single_shot");

    /* chat.completion (PAS le .chunk du mode SSE) + message assemble + usage. */
    CHECK(strstr(body, "\"object\":\"chat.completion\"") != NULL, "objet_chat_completion");
    CHECK(strstr(body, "\"object\":\"chat.completion.chunk\"") == NULL, "pas_d_objet_chunk");
    CHECK(strstr(body, "\"role\":\"assistant\"") != NULL, "message_role_assistant");
    CHECK(strstr(body, "Hello world") != NULL, "contenu_assemble");
    CHECK(strstr(body, "\"finish_reason\":\"stop\"") != NULL, "finish_reason_stop");
    CHECK(strstr(body, "\"usage\"") != NULL, "bloc_usage");
    CHECK(strstr(body, "\"total_tokens\"") != NULL, "usage_total_tokens");
    CHECK(strstr(body, "data: [DONE]") == NULL, "pas_de_sentinelle_sse");
}

/* ================================================================
 * FAMILLE 3 — GET /v1/models -> liste a un modele
 * ================================================================ */

static void famille_3_models(void) {
    g_famille = 3;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[512];
    bati_get(req, sizeof(req), "/v1/models", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");

    char body[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, body, sizeof(body), &nb, &term);

    CHECK(strstr(body, "\"object\":\"list\"") != NULL, "enveloppe_object_list");
    CHECK(strstr(body, "\"id\":\"" CORPS_MODELE "\"") != NULL, "data_id_modele");
    CHECK(strstr(body, "\"object\":\"model\"") != NULL, "data_object_model");
    CHECK(strstr(body, "\"owned_by\":\"owl\"") != NULL, "data_owned_by_owl");
}

/* ================================================================
 * FAMILLE 4 — finish_reason:"length" (faux generateur MAX_TOKENS)
 * ================================================================ */

static void famille_4_finish_length(void) {
    g_famille = 4;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_maxtokens);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"max_tokens\":3,\"stream\":true}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");

    char sse[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, sse, sizeof(sse), &nb, &term);

    CHECK(strstr(sse, "\"finish_reason\":\"length\"") != NULL, "finish_reason_length");
    CHECK(strstr(sse, "\"finish_reason\":\"stop\"") == NULL, "pas_de_finish_reason_stop");
    CHECK(strstr(sse, "data: [DONE]") != NULL, "sentinelle_done");
}

/* ================================================================
 * FAMILLE 5 — erreur mi-flux -> avorte (pas de terminal, pas de [DONE])
 * ================================================================ */

static void famille_5_erreur_mi_flux(void) {
    g_famille = 5;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_erreur);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"stream\":true}", 0);

    EchangeStream e = joue_stream(&xc, req);

    /* L'en-tete a ete committe (>=1 token pousse) : 200 puis coupure abrupte. */
    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200_deja_committe");

    /* Les 2 premiers deltas ont bien ete streames avant l'erreur. */
    char sse[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, sse, sizeof(sse), &nb, &term);
    CHECK(strstr(sse, "\"content\":\"Hello\"") != NULL, "premiers_deltas_streames");

    /* Avorte = pas de terminal chunked, et surtout pas de [DONE] final. */
    CHECK(a_chunk_terminal(&e) == 0, "pas_de_chunk_terminal_apres_avorte");
    CHECK(strstr(e.reponse, "data: [DONE]") == NULL, "pas_de_done_apres_erreur");
}

/* ================================================================
 * FAMILLE 6 — route / methode inconnue -> 404
 * ================================================================ */

static void famille_6_route_inconnue(void) {
    g_famille = 6;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[512];

    /* Chemin non route. */
    bati_get(req, sizeof(req), "/v1/inexistant", 0);
    EchangeStream e1 = joue_stream(&xc, req);
    CHECK(strstr(e1.reponse, "HTTP/1.1 404") == e1.reponse, "chemin_inconnu_404");

    /* Methode non routee sur un chemin connu. */
    bati_get(req, sizeof(req), "/v1/chat/completions", 0);   /* GET au lieu de POST */
    EchangeStream e2 = joue_stream(&xc, req);
    CHECK(strstr(e2.reponse, "HTTP/1.1 404") == e2.reponse, "methode_inconnue_404");
}

/* ================================================================
 * FAMILLE 7 — Authorization present ET absent -> identique (ignore)
 * ================================================================ */

static void famille_7_authorization(void) {
    g_famille = 7;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[512];

    bati_get(req, sizeof(req), "/v1/models", 1);   /* avec Authorization */
    EchangeStream avec = joue_stream(&xc, req);
    CHECK(strstr(avec.reponse, "HTTP/1.1 200") == avec.reponse, "avec_auth_200");
    CHECK(strstr(avec.reponse, "\"object\":\"list\"") != NULL, "avec_auth_liste");

    bati_get(req, sizeof(req), "/v1/models", 0);   /* sans Authorization */
    EchangeStream sans = joue_stream(&xc, req);
    CHECK(strstr(sans.reponse, "HTTP/1.1 200") == sans.reponse, "sans_auth_200");
    CHECK(strstr(sans.reponse, "\"object\":\"list\"") != NULL, "sans_auth_liste");
}

/* ================================================================
 * FAMILLE 8 — seam C : contrat model-free + generation reelle gatee GGUF
 * ================================================================ */

static void famille_8_seam_c(void) {
    g_famille = 8;

    /* Contrat model-free : un chemin absent echoue proprement (retour != 0),
     * et owl_llm_libere est idempotent sur des handles jamais charges. */
    OwlLlmHandles h; memset(&h, 0, sizeof(h));
    CHECK(owl_llm_charge_handles("__owl_inexistant__.gguf", 0, &h) != 0,
          "charge_handles_chemin_absent_echoue");
    owl_llm_libere(&h);

    const char* gguf = getenv("OWL_LLAMA_TEST_GGUF");
    if (!gguf || !gguf[0]) {
        printf("   (seam C : SKIP generation reelle -- fournir OWL_LLAMA_TEST_GGUF)\n");
        return;
    }

    OwlLlmHandles hh; memset(&hh, 0, sizeof(hh));
    int rc = owl_llm_charge_handles(gguf, 0, &hh);
    CHECK(rc == 0, "charge_handles_modele_reel");
    if (rc != 0) return;

    OwlLlmChatMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Reponds par un seul mot.";

    OwlLlmPrepared prep; memset(&prep, 0, sizeof(prep));
    int rp = owl_llm_chat_prepare(&hh, msgs, 1, &prep);
    CHECK(rp == 0, "chat_prepare_ok");
    CHECK(rp == 0 && prep.nb_tokens > 0, "invite_tokenisee_non_vide");

    if (rp == 0) {
        OwlLlmChatParams params; memset(&params, 0, sizeof(params));
        params.messages    = msgs;
        params.nb_messages = 1;
        params.temperature = 0.0;
        params.max_tokens  = 16;

        SeamSink s; memset(&s, 0, sizeof(s));
        OwlLlmChatIssue issue; memset(&issue, 0, sizeof(issue));
        int rs = owl_llm_chat_stream(&hh, &prep, &params, seam_sink, &s, &issue);
        CHECK(rs == 0, "chat_stream_ok");
        CHECK(s.nb >= 1, "au_moins_un_token_par_sink");
        CHECK(s.vu_termine == 1, "dernier_token_marque_termine");
        CHECK(issue.erreur == 0, "issue_sans_erreur");
        owl_llm_prepared_libere(&prep);
    }
    owl_llm_libere(&hh);
}

/* ================================================================
 * FAMILLE 9 — UTF-8 partiel coupe en frontiere de token (decision D)
 * ================================================================ */

/* Joue un echange stream:true avec les pieces UTF-8 courantes, puis prouve
 * l'invariant : chaque delta.content isole est de l'UTF-8 bien forme, et la
 * concatenation des content reconstitue EXACTEMENT `attendu`. */
static void utf8_cas(OwlHttpExecConfig* xc, const char* etiquette,
                     const char* attendu, size_t attendu_len) {
    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"stream\":true}", 0);

    EchangeStream e = joue_stream(xc, req);

    char lbl[96];
    snprintf(lbl, sizeof(lbl), "%s.statut_200", etiquette);
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, lbl);

    char sse[8192]; int nb = 0, term = 0;
    int cadrage = dechunk_reponse(&e, sse, sizeof(sse), &nb, &term);
    snprintf(lbl, sizeof(lbl), "%s.corps_chunked_parse_able", etiquette);
    CHECK(cadrage == 1, lbl);
    snprintf(lbl, sizeof(lbl), "%s.chunk_terminal_present", etiquette);
    CHECK(term == 1, lbl);

    char   concat[512];
    size_t cl = 0;
    int nd = collecte_contents(sse, concat, sizeof(concat), &cl);

    /* Coeur de l'invariant : nd == -1 ssi un content porte de l'UTF-8 tronque,
     * c-a-d qu'un octet partiel a fuite dans une frame SSE. */
    snprintf(lbl, sizeof(lbl), "%s.chaque_content_utf8_bien_forme", etiquette);
    CHECK(nd >= 0, lbl);
    snprintf(lbl, sizeof(lbl), "%s.au_moins_un_content", etiquette);
    CHECK(nd >= 1, lbl);

    /* Le codepoint coupe est bien reapparu COMPLET une fois bufferise. */
    snprintf(lbl, sizeof(lbl), "%s.contenu_reconstitue_complet", etiquette);
    CHECK(cl == attendu_len && memcmp(concat, attendu, cl) == 0, lbl);
}

static void famille_9_utf8_partiel(void) {
    g_famille = 9;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_utf8);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    /* Cas A — 2 octets : "e accent aigu" (U+00E9 = 0xC3 0xA9) coupe entre les
     * tokens "caf\xC3" et "\xA9". Attendu : "cafe" avec le e accentue. */
    {
        static const char* const pieces[] = { "caf\xC3", "\xA9" };
        set_utf8_pieces(pieces, 2);
        static const char attendu[] = "caf\xC3\xA9";
        utf8_cas(&xc, "2o_e_accent", attendu, sizeof(attendu) - 1);
    }

    /* Cas B — 3 octets : ideogramme CJK (U+65E5 = 0xE6 0x97 0xA5) coupe entre
     * "A\xE6\x97" et "\xA5""B", ASCII de part et d'autre. */
    {
        static const char* const pieces[] = { "A\xE6\x97", "\xA5" "B" };
        set_utf8_pieces(pieces, 2);
        static const char attendu[] = "A\xE6\x97\xA5" "B";
        utf8_cas(&xc, "3o_cjk", attendu, sizeof(attendu) - 1);
    }

    /* Cas C — 4 octets : emoji (U+1F600 = 0xF0 0x9F 0x98 0x80) coupe AU MILIEU,
     * reparti sur 3 tokens ("\xF0", "\x9F\x98", "\x80"). La sequence partielle
     * doit s'accumuler cote sink sur deux tokens avant d'etre emise. */
    {
        static const char* const pieces[] = { "\xF0", "\x9F\x98", "\x80" };
        set_utf8_pieces(pieces, 3);
        static const char attendu[] = "\xF0\x9F\x98\x80";
        utf8_cas(&xc, "4o_emoji_milieu", attendu, sizeof(attendu) - 1);
    }
}

/* ================================================================
 * FAMILLE 10 — tools + stream:false -> message.tool_calls
 * ================================================================ */

static void famille_10_toolcall_non_stream(void) {
    g_famille = 10;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_toolcall);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Meteo a Paris ?\"}],"
              TOOLS_WEATHER ","
              "\"stream\":false}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");

    char body[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, body, sizeof(body), &nb, &term);

    CHECK(strstr(body, "\"object\":\"chat.completion\"") != NULL, "objet_chat_completion");
    CHECK(strstr(body, "\"tool_calls\"") != NULL, "message_tool_calls");
    CHECK(strstr(body, "\"name\":\"get_weather\"") != NULL, "tool_call_name");
    /* arguments = string JSON echappee : `"arguments":"{\"location\":...}"`. */
    CHECK(strstr(body, "\"arguments\":\"") != NULL, "arguments_string");
    CHECK(strstr(body, "\"arguments\":\"{\\\"location\\\"") != NULL, "arguments_json_echappee");
    CHECK(strstr(body, "\"finish_reason\":\"tool_calls\"") != NULL, "finish_reason_tool_calls");
    CHECK(strstr(body, "\"content\":null") != NULL, "content_null");
}

/* ================================================================
 * FAMILLE 11 — tools + stream:true -> deltas SSE tool_calls
 * ================================================================ */

static void famille_11_toolcall_stream(void) {
    g_famille = 11;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_toolcall);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Meteo a Paris ?\"}],"
              TOOLS_WEATHER ","
              "\"stream\":true}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");
    CHECK(strstr(e.reponse, "text/event-stream") != NULL, "content_type_event_stream");

    char sse[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, sse, sizeof(sse), &nb, &term);

    /* delta1 : id `call...` + function.name (arguments encore vide). */
    CHECK(strstr(sse, "\"tool_calls\"") != NULL, "deltas_tool_calls");
    CHECK(strstr(sse, "\"id\":\"call") != NULL, "delta_id_call");
    CHECK(strstr(sse, "\"name\":\"get_weather\"") != NULL, "delta_function_name");
    /* delta2 : function.arguments = string JSON echappee. */
    CHECK(strstr(sse, "\"arguments\":\"{\\\"location\\\"") != NULL, "delta_arguments_echappee");
    /* frame finale + sentinelle. */
    CHECK(strstr(sse, "\"finish_reason\":\"tool_calls\"") != NULL, "finish_reason_tool_calls");
    CHECK(strstr(sse, "data: [DONE]") != NULL, "sentinelle_done");

    const char* seq[] = {
        "\"tool_calls\"", "\"name\":\"get_weather\"", "\"arguments\":\"",
        "\"finish_reason\":\"tool_calls\"", "data: [DONE]"
    };
    CHECK(en_ordre(sse, seq, 5), "frames_ordonnees_toolcall");
}

/* ================================================================
 * FAMILLE 12 — tool_choice auto / required / none / named
 * ================================================================ */

static void famille_12_tool_choice(void) {
    g_famille = 12;

    /* auto + faux gen TEXTE : auto peut repondre en texte (pas d'appel force). */
    {
        ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
        OwlHttpExecConfig xc; config_producteur(&xc, &ctx);
        char req[1024];
        bati_post(req, sizeof(req), "/v1/chat/completions",
                  "{\"model\":\"" CORPS_MODELE "\","
                  "\"messages\":[{\"role\":\"user\",\"content\":\"Salut\"}],"
                  TOOLS_WEATHER ",\"tool_choice\":\"auto\",\"stream\":false}", 0);
        EchangeStream e = joue_stream(&xc, req);
        char body[8192]; int nb = 0, term = 0;
        dechunk_reponse(&e, body, sizeof(body), &nb, &term);
        CHECK(strstr(body, "Hello world") != NULL, "auto_texte_possible");
        CHECK(strstr(body, "\"tool_calls\"") == NULL, "auto_pas_de_toolcall");
    }

    /* required + faux gen d'APPEL : un tool_call DOIT sortir (forcage). */
    {
        ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_toolcall);
        OwlHttpExecConfig xc; config_producteur(&xc, &ctx);
        char req[1024];
        bati_post(req, sizeof(req), "/v1/chat/completions",
                  "{\"model\":\"" CORPS_MODELE "\","
                  "\"messages\":[{\"role\":\"user\",\"content\":\"Meteo Paris\"}],"
                  TOOLS_WEATHER ",\"tool_choice\":\"required\",\"stream\":false}", 0);
        EchangeStream e = joue_stream(&xc, req);
        char body[8192]; int nb = 0, term = 0;
        dechunk_reponse(&e, body, sizeof(body), &nb, &term);
        CHECK(strstr(body, "\"tool_calls\"") != NULL, "required_force_appel");
        CHECK(strstr(body, "\"finish_reason\":\"tool_calls\"") != NULL, "required_finish_tool_calls");
    }

    /* none + faux gen TEXTE : jamais d'appel, texte seul. */
    {
        ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
        OwlHttpExecConfig xc; config_producteur(&xc, &ctx);
        char req[1024];
        bati_post(req, sizeof(req), "/v1/chat/completions",
                  "{\"model\":\"" CORPS_MODELE "\","
                  "\"messages\":[{\"role\":\"user\",\"content\":\"Salut\"}],"
                  TOOLS_WEATHER ",\"tool_choice\":\"none\",\"stream\":false}", 0);
        EchangeStream e = joue_stream(&xc, req);
        char body[8192]; int nb = 0, term = 0;
        dechunk_reponse(&e, body, sizeof(body), &nb, &term);
        CHECK(strstr(body, "\"tool_calls\"") == NULL, "none_pas_de_toolcall");
        CHECK(strstr(body, "Hello world") != NULL, "none_texte");
    }

    /* named (specifique) + faux gen d'APPEL : l'outil nomme doit sortir. */
    {
        ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_toolcall);
        OwlHttpExecConfig xc; config_producteur(&xc, &ctx);
        char req[1024];
        bati_post(req, sizeof(req), "/v1/chat/completions",
                  "{\"model\":\"" CORPS_MODELE "\","
                  "\"messages\":[{\"role\":\"user\",\"content\":\"Meteo Paris\"}],"
                  TOOLS_WEATHER ","
                  "\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"get_weather\"}},"
                  "\"stream\":false}", 0);
        EchangeStream e = joue_stream(&xc, req);
        char body[8192]; int nb = 0, term = 0;
        dechunk_reponse(&e, body, sizeof(body), &nb, &term);
        CHECK(strstr(body, "\"tool_calls\"") != NULL, "named_force_appel");
        CHECK(strstr(body, "\"name\":\"get_weather\"") != NULL, "named_bon_outil");
    }
}

/* ================================================================
 * FAMILLE 13 — message role:"tool" (multi-tours) accepte
 * ================================================================ */

static void famille_13_role_tool(void) {
    g_famille = 13;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    /* Tour precedent : l'assistant a appele get_weather, on renvoie le resultat
     * via un message role:"tool" (tool_call_id + content). Le serveur doit
     * ACCEPTER cet historique sans 400 ni crash. */
    char req[2048];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":["
                "{\"role\":\"user\",\"content\":\"Meteo Paris\"},"
                "{\"role\":\"assistant\",\"content\":null,"
                  "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
                  "\"function\":{\"name\":\"get_weather\","
                  "\"arguments\":\"{\\\"location\\\":\\\"Paris\\\"}\"}}]},"
                "{\"role\":\"tool\",\"tool_call_id\":\"call_1\","
                  "\"content\":\"{\\\"temp\\\":20}\"}"
              "],"
              TOOLS_WEATHER ",\"stream\":false}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(e.demarre_ok == 1, "executeur_flux_demarre");
    CHECK(e.reponse_len > 0, "reponse_recue_pas_de_crash");
    CHECK(strstr(e.reponse, "HTTP/1.1 400") == NULL, "pas_de_400_sur_role_tool");
    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");
}

/* ================================================================
 * FAMILLE 14 — `tools` ABSENT -> chemin chat inchange
 * ================================================================ */

static void famille_14_tools_absent(void) {
    g_famille = 14;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_naturel);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Bonjour\"}],"
              "\"stream\":false}", 0);

    EchangeStream e = joue_stream(&xc, req);

    CHECK(strstr(e.reponse, "HTTP/1.1 200") == e.reponse, "statut_200");

    char body[8192]; int nb = 0, term = 0;
    dechunk_reponse(&e, body, sizeof(body), &nb, &term);

    CHECK(strstr(body, "\"object\":\"chat.completion\"") != NULL, "objet_chat_completion");
    CHECK(strstr(body, "Hello world") != NULL, "contenu_texte");
    CHECK(strstr(body, "\"finish_reason\":\"stop\"") != NULL, "finish_reason_stop");
    CHECK(strstr(body, "\"tool_calls\"") == NULL, "aucun_tool_calls");
}

/* ================================================================
 * FAMILLE 15 — invite trop longue -> HTTP 413 invalid_request_error
 *
 * Le garde anti-crash (llm_generate_core) rejette une invite depassant
 * n_batch/n_ctx AVANT le premier decode : plus de GGML_ASSERT/abort. Le faux
 * generateur pose issue.prompt_too_long (rc -1, aucun token). Le producteur doit
 * distinguer ce cas des autres erreurs et rendre 413 « invalid_request_error »
 * (au lieu du 500 generique), AVANT tout header SSE, dans les 3 modes.
 * ================================================================ */

static void attend_413_prompt_too_long(const EchangeStream* e, const char* etiq) {
    char lbl[96];
    snprintf(lbl, sizeof(lbl), "%s.statut_413", etiq);
    CHECK(strstr(e->reponse, "HTTP/1.1 413") == e->reponse, lbl);
    snprintf(lbl, sizeof(lbl), "%s.type_invalid_request", etiq);
    CHECK(strstr(e->reponse, "invalid_request_error") != NULL, lbl);
    snprintf(lbl, sizeof(lbl), "%s.mentionne_X_jetons", etiq);
    CHECK(strstr(e->reponse, "9999") != NULL, lbl);
    snprintf(lbl, sizeof(lbl), "%s.mentionne_Y_limite", etiq);
    CHECK(strstr(e->reponse, "2048") != NULL, lbl);
    /* Aucun flux SSE engage : pas de sentinelle [DONE], pas de crash. */
    snprintf(lbl, sizeof(lbl), "%s.pas_de_done", etiq);
    CHECK(strstr(e->reponse, "data: [DONE]") == NULL, lbl);
    snprintf(lbl, sizeof(lbl), "%s.pas_de_500", etiq);
    CHECK(strstr(e->reponse, "HTTP/1.1 500") != e->reponse, lbl);
}

static void famille_15_prompt_too_long(void) {
    g_famille = 15;

    ServeLlmCtx ctx; ctx_defaut(&ctx, faux_gen_prompt_too_long);
    OwlHttpExecConfig xc; config_producteur(&xc, &ctx);

    char req[1024];

    /* Mode stream:true — s.flux reste NULL (aucun token), branche erreur pre-flux. */
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"invite gigantesque\"}],"
              "\"stream\":true}", 0);
    EchangeStream es = joue_stream(&xc, req);
    CHECK(es.demarre_ok == 1, "stream.executeur_flux_demarre");
    CHECK(es.reponse_len > 0, "stream.reponse_recue_pas_de_crash");
    attend_413_prompt_too_long(&es, "stream");

    /* Mode single-shot (stream:false) — branche else du handler. */
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"invite gigantesque\"}],"
              "\"stream\":false}", 0);
    EchangeStream ess = joue_stream(&xc, req);
    CHECK(ess.reponse_len > 0, "single.reponse_recue_pas_de_crash");
    attend_413_prompt_too_long(&ess, "single");

    /* Mode tool (serve_chat_finish_tool) — meme distinction 413. */
    bati_post(req, sizeof(req), "/v1/chat/completions",
              "{\"model\":\"" CORPS_MODELE "\","
              "\"messages\":[{\"role\":\"user\",\"content\":\"Meteo Paris ?\"}],"
              TOOLS_WEATHER ","
              "\"stream\":false}", 0);
    EchangeStream et = joue_stream(&xc, req);
    CHECK(et.reponse_len > 0, "tool.reponse_recue_pas_de_crash");
    attend_413_prompt_too_long(&et, "tool");
}

/* ================================================================
 * FAMILLE 16 — parse du delai d'attente CLI (--request-timeout)
 *
 * Coeur testable model-free de  : la conversion secondes -> ms qui
 * alimente OwlHttpExecConfig.plafond_attente_ms. Contrat :
 *   sec > 0  -> sec * 1000 (borne anti-overflow a INT_MAX) ;
 *   sec <= 0 -> -1 (attente infinie, cf. http_exec.c:54).
 * Le 503-sous-charge reel (timing + serveur + modele) n'est PAS reproductible
 * ici ; on verrouille la logique de conversion, le cablage sur xc et les
 * defauts (300 s / 32) etant de la config validee dev/testeur.
 * ================================================================ */

static void famille_16_parse_timeout(void) {
    g_famille = 16;

    /* Valeurs positives : conversion secondes -> millisecondes. */
    CHECK(serve_llm_parse_timeout_sec_to_ms(1)   == 1000,   "timeout.1s_1000ms");
    CHECK(serve_llm_parse_timeout_sec_to_ms(30)  == 30000,  "timeout.30s_30000ms");
    CHECK(serve_llm_parse_timeout_sec_to_ms(300) == 300000, "timeout.300s_300000ms");

    /* 0 et negatif -> attente infinie (-1). */
    CHECK(serve_llm_parse_timeout_sec_to_ms(0)   == -1, "timeout.0_infini");
    CHECK(serve_llm_parse_timeout_sec_to_ms(-1)  == -1, "timeout.neg_infini");
    CHECK(serve_llm_parse_timeout_sec_to_ms(-42) == -1, "timeout.neg42_infini");

    /* Borne anti-overflow : sec * 1000 > INT_MAX -> cap a INT_MAX (pas d'UB). */
    CHECK(serve_llm_parse_timeout_sec_to_ms(INT_MAX) == INT_MAX,
          "timeout.overflow_cap_intmax");
    CHECK(serve_llm_parse_timeout_sec_to_ms(INT_MAX / 1000) == (INT_MAX / 1000) * 1000,
          "timeout.max_exact_sans_overflow");
}

/* ================================================================
 * FAMILLE 17 — parse --ctx-size (opt-in fenetre de contexte, )
 *
 * serve_llm_parse_ctx_size est la fonction pure de validation/normalisation
 * de --ctx-size : strtol strict, refus <= 0 et non numerique, clamp a la
 * borne haute de securite OWL_LLM_N_CTX_MAX (anti-alloc KV delirante). Le
 * chargement reel avec un n_ctx custom passe par le seam gate GGUF ; ici on
 * verrouille uniquement la logique model-free de parse.
 * ================================================================ */

static void famille_17_parse_ctx_size(void) {
    g_famille = 17;
    uint32_t out;

    /* Valeurs valides : parse exact, out pose. */
    out = 12345;
    CHECK(serve_llm_parse_ctx_size("8192", &out) == 0 && out == 8192u,
          "ctx.8192_ok");
    out = 0;
    CHECK(serve_llm_parse_ctx_size("16384", &out) == 0 && out == 16384u,
          "ctx.16384_ok");
    out = 0;
    CHECK(serve_llm_parse_ctx_size("1", &out) == 0 && out == 1u, "ctx.1_ok");

    /* Refus : <= 0. out remis a 0 sur erreur. */
    out = 999;
    CHECK(serve_llm_parse_ctx_size("0", &out) != 0 && out == 0u, "ctx.0_refuse");
    out = 999;
    CHECK(serve_llm_parse_ctx_size("-5", &out) != 0 && out == 0u, "ctx.neg_refuse");

    /* Refus : non numerique / suffixe / vide / NULL. */
    CHECK(serve_llm_parse_ctx_size("abc", &out) != 0, "ctx.non_num_refuse");
    CHECK(serve_llm_parse_ctx_size("16k", &out) != 0, "ctx.suffixe_refuse");
    CHECK(serve_llm_parse_ctx_size("", &out) != 0, "ctx.vide_refuse");
    CHECK(serve_llm_parse_ctx_size(NULL, &out) != 0, "ctx.null_refuse");

    /* Borne haute de securite : clamp a OWL_LLM_N_CTX_MAX. */
    out = 0;
    CHECK(serve_llm_parse_ctx_size("131072", &out) == 0 && out == OWL_LLM_N_CTX_MAX,
          "ctx.max_exact");
    out = 0;
    CHECK(serve_llm_parse_ctx_size("999999999", &out) == 0 && out == OWL_LLM_N_CTX_MAX,
          "ctx.au_dessus_borne_clampe");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int serve_llm_self_test_run(void) {
    printf("== serve-llm OpenAI-compat (feat/1669 + tool-calling feat/1671) ==\n");

    famille_0_chat_stream();
    famille_1_json_malforme();
    famille_2_stream_false();
    famille_3_models();
    famille_4_finish_length();
    famille_5_erreur_mi_flux();
    famille_6_route_inconnue();
    famille_7_authorization();
    famille_8_seam_c();
    famille_9_utf8_partiel();
    famille_10_toolcall_non_stream();
    famille_11_toolcall_stream();
    famille_12_tool_choice();
    famille_13_role_tool();
    famille_14_tools_absent();
    famille_15_prompt_too_long();
    famille_16_parse_timeout();
    famille_17_parse_ctx_size();

    int tp = 0, tf = 0;
    for (int i = 0; i < NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 chat stream:true (SSE)    : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 JSON malforme -> 400      : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 stream:false single-shot  : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 GET /v1/models            : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 finish_reason length      : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 erreur mi-flux -> avorte  : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 route/methode inconnue    : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 Authorization ignore      : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   f8 seam C (gate GGUF)        : %d/%d\n", g_pass[8], g_pass[8] + g_fail[8]);
    printf("   f9 UTF-8 partiel bufferise   : %d/%d\n", g_pass[9], g_pass[9] + g_fail[9]);
    printf("   f10 toolcall non-stream      : %d/%d\n", g_pass[10], g_pass[10] + g_fail[10]);
    printf("   f11 toolcall stream deltas   : %d/%d\n", g_pass[11], g_pass[11] + g_fail[11]);
    printf("   f12 tool_choice a/r/n/named  : %d/%d\n", g_pass[12], g_pass[12] + g_fail[12]);
    printf("   f13 role:tool multi-tours    : %d/%d\n", g_pass[13], g_pass[13] + g_fail[13]);
    printf("   f14 tools absent -> chat     : %d/%d\n", g_pass[14], g_pass[14] + g_fail[14]);
    printf("   f15 invite trop longue -> 413: %d/%d\n", g_pass[15], g_pass[15] + g_fail[15]);
    printf("   f16 parse --request-timeout  : %d/%d\n", g_pass[16], g_pass[16] + g_fail[16]);
    printf("   f17 parse --ctx-size         : %d/%d\n", g_pass[17], g_pass[17] + g_fail[17]);
    printf("   TOTAL serve-llm              : %d/%d\n", tp, tp + tf);
    return tf;
}
