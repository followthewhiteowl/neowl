// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* serve_llm.c — API LLM locale compatible OpenAI.
 *
 * `owl serve-llm --model <x.gguf> [--port 11434] [--host 127.0.0.1]` expose le
 * modele local en endpoints OpenAI-compat (drop-in ollama) :
 *   - POST /v1/chat/completions  (SSE token-par-token si stream:true,
 *                                 JSON single-shot si stream:false) ;
 *   - GET  /v1/models            (liste a un modele).
 *
 * Compose des briques deja decidees : le socle streaming mode C
 * fournit le transport (flux chunked-out) via le seam producteur ; le moteur
 * (stdlib_llm.h) fournit le chargement + la generation par sink C. Aucune
 * touche au socle (http_exec.c / http_server.c) : stream:false = chunked
 * single-shot (decision A).
 *
 * REGLE D'OR : sur le chemin serve-llm, le sink ne pousse que des OCTETS C
 * COPIES dans le flux — aucun ObjString* ne traverse la frontiere.
 *
 * Tool-calling : une requete `tools` fait FORCER un appel d'outil
 * valide par grammaire (owl_tool_schema_to_gbnf, stdlib_llm.c), plus fiable que
 * l'esperance d'un JSON bien forme. La GBNF est materialisee AVANT le vm_pop de
 * la racine (regle d'or), voyage par OwlLlmChatParams.grammar, et la sortie
 * contrainte est reframee en choices[].message.tool_calls (non-stream) ou en
 * deltas SSE tool_calls (stream). Schema hors sous-ensemble couvert -> 400.
 */

#include "serve_llm.h"

#include "vm.h"            /* vm_push / vm_pop / OBJ_VAL, ObjJson */
#include "object.h"
#include "owl_sb.h"        /* SB partage */
#include "sentinels.h"     /* OWL_STOP_MAX_TOKENS */
#include "net_tcp_server.h"
#include "stdlib_llm.h"    /* owl_tool_schema_to_gbnf, OWL_TC_* (tool-calling) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
#endif

/* Parser JSON interne (stdlib_json.c), reutilise pour lire le corps de la
 * requete cote thread VM. Rend NULL sur JSON errone ou incomplet. */
extern ObjJson* json_parse_cstr(const char* src, int len);

/* ================================================================
 * Echappement JSON (decision D : UTF-8 partiel bufferise cote sink, pas ici).
 *
 * Echappe les guillemets, l'antislash, les sauts de ligne et les caracteres
 * de controle < 0x20. Les octets >= 0x20 (dont l'UTF-8 multi-octet) passent
 * tels quels. `serve_llm_json_escape` est la SEULE porte d'echappement : tout
 * texte insere dans une frame passe par ici.
 * ================================================================ */

static void serve_llm_json_escape(SB* b, const char* s, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n");  break;
            case '\r': sb_puts(b, "\\r");  break;
            case '\t': sb_puts(b, "\\t");  break;
            case '\b': sb_puts(b, "\\b");  break;
            case '\f': sb_puts(b, "\\f");  break;
            default:
                if (c < 0x20) {
                    char u[6] = { '\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 0xF] };
                    sb_putn(b, u, 6);
                } else {
                    sb_putn(b, (const char*)&c, 1);
                }
        }
    }
}

/* ================================================================
 * Bornes de troncature UTF-8 (decision D)
 *
 * Rend le nombre d'octets de FIN qui forment une sequence UTF-8 incomplete
 * (0 si le tampon se termine sur une frontiere de caractere). Ces octets sont
 * conserves cote sink jusqu'a l'arrivee de leur suite : jamais un demi-
 * caractere dans une frame (JSON invalide sinon).
 * ================================================================ */

static size_t utf8_incomplete_tail(const char* b, size_t n) {
    size_t cont = 0;
    size_t i    = n;
    while (i > 0 && cont < 4) {
        unsigned char c = (unsigned char)b[i - 1];
        if ((c & 0xC0) == 0x80) { cont++; i--; continue; }  /* octet de continuation */
        size_t need;
        if      ((c & 0x80) == 0x00) need = 1;   /* ASCII */
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                         need = 1;   /* octet de tete invalide */
        size_t have = cont + 1;
        return (have >= need) ? 0 : have;
    }
    return 0;  /* que des continuations : malforme, on ne retient rien */
}

/* ================================================================
 * Etat de sink SSE / single-shot
 *
 * En stream:true, chaque token produit une frame `chat.completion.chunk`. En
 * stream:false, le sink ACCUMULE le texte et le producteur emet un unique
 * chunk `chat.completion` apres coup. Le flux SSE est ouvert PARESSEUSEMENT
 * au premier token : tant qu'aucun octet n'est committe, une erreur avant le
 * 1er token peut encore rendre un 500 propre (en-tete pas engage).
 * ================================================================ */

typedef struct {
    OwlHttpProdCtx* pctx;
    OwlHttpRep*     flux;        /* NULL tant qu'aucune frame committee */
    const char*     model_id;
    long long       created;
    int             stream;      /* 1 = SSE par token, 0 = accumulation */
    int             tool_mode;   /* 1 = grammaire d'outil active : accumule tout,
                                    ne streame pas (reframe en tool_calls apres) */
    int             abandon;     /* 1 = consommateur parti */

    /* Bufferisation d'une sequence UTF-8 incomplete en frontiere de token. */
    char            pending[8];
    size_t          pending_len;

    /* Accumulation du texte (stream:false). */
    SB              acc;
} SseSink;

/* Prelude commun d'une frame SSE : `data: {"id":...,"object":"...","created":
 * N,"model":"M","choices":[{"index":0,"delta":{` — l'appelant ferme le delta,
 * ajoute finish_reason et le suffixe. */
static void sse_frame_prelude(SB* b, const SseSink* s) {
    sb_puts(b, "data: {\"id\":\"chatcmpl-owl\",\"object\":\"chat.completion.chunk\","
               "\"created\":");
    sb_puti(b, s->created);
    sb_puts(b, ",\"model\":\"");
    serve_llm_json_escape(b, s->model_id ? s->model_id : "", s->model_id ? strlen(s->model_id) : 0);
    sb_puts(b, "\",\"choices\":[{\"index\":0,\"delta\":{");
}

/* Pousse une frame deja composee. Un ABANDON (consommateur parti) coupe la
 * generation : on le remonte au moteur via le retour du sink. */
static int sse_push(SseSink* s, const SB* frame) {
    if (owl_http_rep_flux_pousse(s->flux, frame->p, frame->len) != OWL_HTTP_FLUX_OK) {
        s->abandon = 1;
        return -1;
    }
    return 0;
}

/* Ouvre le flux SSE (200 text/event-stream) et pousse la 1re frame
 * `delta:{role:assistant}` si ce n'est deja fait. */
static int sse_ensure_open(SseSink* s) {
    if (s->flux != NULL) return 0;
    s->flux = owl_http_prod_ouvre_flux(s->pctx, 200, "text/event-stream", 1u << 20);
    if (s->flux == NULL) { s->abandon = 1; return -1; }

    SB fb; sb_init(&fb);
    sse_frame_prelude(&fb, s);
    sb_puts(&fb, "\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n");
    int r = sse_push(s, &fb);
    sb_free(&fb);
    return r;
}

/* Pousse une frame de contenu (un delta.content). */
static int sse_push_content(SseSink* s, const char* txt, size_t len) {
    if (sse_ensure_open(s) != 0) return -1;
    SB fb; sb_init(&fb);
    sse_frame_prelude(&fb, s);
    sb_puts(&fb, "\"content\":\"");
    serve_llm_json_escape(&fb, txt, len);
    sb_puts(&fb, "\"},\"finish_reason\":null}]}\n\n");
    int r = sse_push(s, &fb);
    sb_free(&fb);
    return r;
}

/* Sink token-par-token (OwlLlmTokenSink). Octets copies uniquement (regle
 * d'or). En stream:true : emet une frame par token, en bufferisant l'UTF-8
 * partiel. En stream:false : accumule. */
static int serve_llm_sink(const char* piece, size_t len, int termine, void* user) {
    SseSink* s = (SseSink*)user;
    if (s->abandon) return 1;

    /* stream:false OU tool-calling : on accumule sans streamer. En tool_mode la
     * sortie contrainte est reframee en tool_calls (ou texte) apres coup. */
    if (!s->stream || s->tool_mode) {
        sb_putn(&s->acc, piece, len);
        return 0;
    }

    /* Combine l'eventuel reliquat UTF-8 avec le nouveau morceau. */
    const char* data = piece;
    size_t      dlen = len;
    char*       tmp  = NULL;
    if (s->pending_len > 0) {
        tmp = (char*)malloc(s->pending_len + len);
        if (tmp == NULL) { s->abandon = 1; return 1; }
        memcpy(tmp, s->pending, s->pending_len);
        if (len > 0) memcpy(tmp + s->pending_len, piece, len);
        data = tmp;
        dlen = s->pending_len + len;
        s->pending_len = 0;
    }

    size_t emit_len = dlen;
    if (!termine) {
        size_t tail = utf8_incomplete_tail(data, dlen);
        emit_len = dlen - tail;
        if (tail > 0) {
            memcpy(s->pending, data + emit_len, tail);
            s->pending_len = tail;
        }
    }

    int rc = 0;
    /* Emet une frame pour ce token (vide inclus sur le dernier morceau, ce qui
     * committe l'en-tete + la frame role meme pour une reponse vide). */
    if (emit_len > 0 || termine) {
        rc = sse_push_content(s, data, emit_len);
    }
    free(tmp);
    return rc != 0 ? 1 : 0;
}

/* ================================================================
 * Composition des reponses finales
 * ================================================================ */

/* Frame finale SSE : `delta:{}` + finish_reason (stop/length), puis la
 * sentinelle `data: [DONE]`. Termine et relache le flux. */
static void sse_finish(SseSink* s, const OwlLlmChatIssue* issue) {
    /* Reliquat UTF-8 non emis (frontiere de token) : flush en derniere frame. */
    if (s->pending_len > 0) {
        sse_push_content(s, s->pending, s->pending_len);
        s->pending_len = 0;
    }

    const char* fr = (issue->stop_reason == OWL_STOP_MAX_TOKENS) ? "length" : "stop";

    SB fb; sb_init(&fb);
    sse_frame_prelude(&fb, s);
    sb_puts(&fb, "},\"finish_reason\":\"");
    sb_puts(&fb, fr);
    sb_puts(&fb, "\"}]}\n\n");
    sse_push(s, &fb);
    sb_free(&fb);

    static const char done[] = "data: [DONE]\n\n";
    owl_http_rep_flux_pousse(s->flux, done, sizeof(done) - 1);

    owl_http_rep_flux_termine(s->flux);
    owl_http_rep_flux_relache(s->flux);
}

/* Corps JSON d'une reponse single-shot (chat.completion + usage). */
static void build_single(SB* b, const SseSink* s, const OwlLlmChatIssue* issue) {
    const char* fr = (issue->stop_reason == OWL_STOP_MAX_TOKENS) ? "length" : "stop";
    int in  = issue->input_tokens;
    int out = issue->output_tokens;

    sb_puts(b, "{\"id\":\"chatcmpl-owl\",\"object\":\"chat.completion\",\"created\":");
    sb_puti(b, s->created);
    sb_puts(b, ",\"model\":\"");
    serve_llm_json_escape(b, s->model_id ? s->model_id : "", s->model_id ? strlen(s->model_id) : 0);
    sb_puts(b, "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
               "\"content\":\"");
    serve_llm_json_escape(b, s->acc.p ? s->acc.p : "", s->acc.len);
    sb_puts(b, "\"},\"finish_reason\":\"");
    sb_puts(b, fr);
    sb_puts(b, "\"}],\"usage\":{\"prompt_tokens\":");
    sb_puti(b, in);
    sb_puts(b, ",\"completion_tokens\":");
    sb_puti(b, out);
    sb_puts(b, ",\"total_tokens\":");
    sb_puti(b, in + out);
    sb_puts(b, "}}");
}

/* Reponse d'erreur (JSON OpenAI-like) dans un flux au statut voulu. En-tete
 * PAS encore committe -> le statut choisi (400/404/500) est celui de la ligne
 * de statut. */
static void serve_error(OwlHttpProdCtx* pctx, int statut,
                        const char* type, const char* message) {
    SB b; sb_init(&b);
    sb_puts(&b, "{\"error\":{\"message\":\"");
    serve_llm_json_escape(&b, message, strlen(message));
    sb_puts(&b, "\",\"type\":\"");
    serve_llm_json_escape(&b, type, strlen(type));
    sb_puts(&b, "\"}}");

    OwlHttpRep* f = owl_http_prod_ouvre_flux(pctx, statut, "application/json", 1u << 16);
    if (f != NULL) {
        owl_http_rep_flux_pousse(f, b.p, b.len);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
    }
    sb_free(&b);
}

/* Emet un 413 invalid_request_error decrivant une invite trop longue pour la
 * fenetre de contexte du modele (X jetons d'invite > Y jetons de limite,
 * ). Distingue le cas prompt_too_long du 500 generique, AVANT tout
 * header SSE (le garde anti-abort rejette avant le 1er token). */
static void serve_error_prompt_too_long(OwlHttpProdCtx* pctx,
                                        const OwlLlmChatIssue* issue) {
    char msg[192];
    snprintf(msg, sizeof(msg),
             "invite trop longue pour la fen\xc3\xaatre de contexte du mod\xc3\xa8le "
             "(%d > %d jetons)",
             issue->input_tokens, issue->context_limit);
    serve_error(pctx, 413, "invalid_request_error", msg);
}

/* GET /v1/models : liste a un modele. */
static void serve_models(OwlHttpProdCtx* pctx, ServeLlmCtx* sctx) {
    SB b; sb_init(&b);
    sb_puts(&b, "{\"object\":\"list\",\"data\":[{\"id\":\"");
    serve_llm_json_escape(&b, sctx->model_id ? sctx->model_id : "",
                          sctx->model_id ? strlen(sctx->model_id) : 0);
    sb_puts(&b, "\",\"object\":\"model\",\"created\":0,\"owned_by\":\"owl\"}]}");

    OwlHttpRep* f = owl_http_prod_ouvre_flux(pctx, 200, "application/json", 1u << 16);
    if (f != NULL) {
        owl_http_rep_flux_pousse(f, b.p, b.len);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
    }
    sb_free(&b);
}

/* ================================================================
 * Extraction du corps /v1/chat/completions (thread VM, decision F)
 * ================================================================ */

/* Cherche un champ par cle dans un objet JSON (scan direct, sans allouer). */
static ObjJson* sj_field(ObjJson* obj, const char* key) {
    if (obj == NULL || obj->jtype != JSON_T_OBJECT) return NULL;
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        ObjString* k = obj->as.objectVal.keys[i];
        if (k != NULL && strcmp(k->chars, key) == 0)
            return obj->as.objectVal.values[i];
    }
    return NULL;
}

/* Copie C-heap d'une chaine C (NUL-terminee). */
static char* cdup(const char* s) {
    size_t n = strlen(s);
    char* d = (char*)malloc(n + 1);
    if (d != NULL) memcpy(d, s, n + 1);
    return d;
}

/* Copie C-heap d'une chaine JSON (ou "" si absente/non-chaine). */
static char* sj_dup_string(ObjJson* node) {
    const char* src = "";
    size_t      n   = 0;
    if (node != NULL && node->jtype == JSON_T_STRING && node->as.strVal != NULL) {
        src = node->as.strVal->chars;
        n   = (size_t)node->as.strVal->length;
    }
    char* d = (char*)malloc(n + 1);
    if (d == NULL) return NULL;
    memcpy(d, src, n);
    d[n] = '\0';
    return d;
}

/* Messages extraits en C (possedes), decouples du GC apres extraction. */
typedef struct {
    OwlLlmChatMsg* items;    /* role/content pointent dans `store` */
    char**         store;    /* buffers C-heap a liberer */
    size_t         store_n;
    size_t         count;
} ChatMsgs;

static void chatmsgs_free(ChatMsgs* m) {
    for (size_t i = 0; i < m->store_n; i++) free(m->store[i]);
    free(m->store);
    free(m->items);
    memset(m, 0, sizeof(*m));
}

/* Chaine C d'un champ (ou "" si absent/non-chaine), pointeur emprunte. */
static const char* sj_cstr(ObjJson* node, size_t* out_len) {
    if (node != NULL && node->jtype == JSON_T_STRING && node->as.strVal != NULL) {
        if (out_len) *out_len = (size_t)node->as.strVal->length;
        return node->as.strVal->chars;
    }
    if (out_len) *out_len = 0;
    return "";
}

/* Reconstruit le `content` d'un message assistant porteur de `tool_calls`
 * (historique) en `{"name":<nom>,"arguments":<json>}` (, decision H :
 * premier appel). Les `arguments` OpenAI sont une CHAINE JSON : son contenu
 * decode est deja du JSON valide, reinjecte tel quel. Rend une chaine C-heap. */
static char* reconstruct_assistant_toolcall(ObjJson* tool_calls) {
    SB c; sb_init(&c);
    ObjJson* first = (tool_calls != NULL && tool_calls->jtype == JSON_T_ARRAY &&
                      tool_calls->as.arrayVal.count > 0)
                         ? tool_calls->as.arrayVal.items[0] : NULL;
    ObjJson* fn = sj_field(first, "function");
    size_t nlen = 0, alen = 0;
    const char* name = sj_cstr(sj_field(fn, "name"), &nlen);
    const char* args = sj_cstr(sj_field(fn, "arguments"), &alen);
    sb_puts(&c, "{\"name\":\"");
    serve_llm_json_escape(&c, name, nlen);
    sb_puts(&c, "\",\"arguments\":");
    sb_putn(&c, alen > 0 ? args : "{}", alen > 0 ? alen : 2);
    sb_puts(&c, "}");
    return c.p ? c.p : cdup("{}");
}

/* Materialise le tableau `messages` en copies C. Applique le remap tool-calling
 * (decision F/H) : `role:"tool"` -> `role:"user"` enveloppe `<tool_response>`,
 * `role:"assistant"` porteur de `tool_calls` -> content JSON reconstruit.
 * Retourne 0 si ok. */
static int chatmsgs_extract(ObjJson* root, ChatMsgs* out) {
    memset(out, 0, sizeof(*out));
    ObjJson* arr = sj_field(root, "messages");
    if (arr == NULL || arr->jtype != JSON_T_ARRAY) return 0;  /* vide accepte */

    int n = arr->as.arrayVal.count;
    if (n <= 0) return 0;

    out->items = (OwlLlmChatMsg*)calloc((size_t)n, sizeof(OwlLlmChatMsg));
    out->store = (char**)calloc((size_t)n * 2, sizeof(char*));
    if (out->items == NULL || out->store == NULL) { chatmsgs_free(out); return -1; }

    for (int i = 0; i < n; i++) {
        ObjJson* el = arr->as.arrayVal.items[i];
        if (el == NULL || el->jtype != JSON_T_OBJECT) continue;

        size_t rlen = 0;
        const char* rolestr = sj_cstr(sj_field(el, "role"), &rlen);

        char* role    = NULL;
        char* content = NULL;

        if (strcmp(rolestr, "tool") == 0) {
            /* decision F : retour d'outil -> tour utilisateur enveloppe. */
            role = cdup("user");
            size_t idl = 0, cl = 0;
            const char* id  = sj_cstr(sj_field(el, "tool_call_id"), &idl);
            const char* cnt = sj_cstr(sj_field(el, "content"), &cl);
            SB c; sb_init(&c);
            sb_puts(&c, "<tool_response tool_call_id=\"");
            sb_putn(&c, id, idl);
            sb_puts(&c, "\">");
            sb_putn(&c, cnt, cl);
            sb_puts(&c, "</tool_response>");
            content = c.p ? c.p : cdup("");
        } else if (strcmp(rolestr, "assistant") == 0 &&
                   sj_field(el, "tool_calls") != NULL) {
            role    = cdup("assistant");
            content = reconstruct_assistant_toolcall(sj_field(el, "tool_calls"));
        } else {
            role    = sj_dup_string(sj_field(el, "role"));
            content = sj_dup_string(sj_field(el, "content"));
        }

        if (role == NULL || content == NULL) {
            free(role); free(content); chatmsgs_free(out); return -1;
        }
        out->store[out->store_n++] = role;
        out->store[out->store_n++] = content;
        out->items[out->count].role    = role;
        out->items[out->count].content = content;
        out->count++;
    }
    return 0;
}

/* ================================================================
 * Tool-calling : injection <tools>, reframe de la sortie
 * contrainte en choices[].message.tool_calls.
 * ================================================================ */

/* Writer JSON compact local (decision G) : serialise un ObjJson en JSON sans
 * espaces. Lit l'arbre, n'ecrit que des octets (aucune alloc Obj). Sert a
 * (1) l'injection compacte des schemas <tools>, (2) la re-serialisation des
 * `arguments` d'un appel en chaine JSON echappee. */
static void sj_write_compact(SB* b, ObjJson* n) {
    if (n == NULL) { sb_puts(b, "null"); return; }
    switch (n->jtype) {
        case JSON_T_NULL: sb_puts(b, "null"); break;
        case JSON_T_BOOL: sb_puts(b, n->as.boolVal ? "true" : "false"); break;
        case JSON_T_INT:  sb_puti(b, (long long)n->as.intVal); break;
        case JSON_T_REAL: {
            char t[32]; int k = snprintf(t, sizeof(t), "%.17g", n->as.realVal);
            if (k > 0) sb_putn(b, t, (size_t)k);
        } break;
        case JSON_T_STRING:
            sb_puts(b, "\"");
            serve_llm_json_escape(b, n->as.strVal ? n->as.strVal->chars : "",
                                  n->as.strVal ? (size_t)n->as.strVal->length : 0);
            sb_puts(b, "\"");
            break;
        case JSON_T_ARRAY:
            sb_puts(b, "[");
            for (int i = 0; i < n->as.arrayVal.count; i++) {
                if (i) sb_puts(b, ",");
                sj_write_compact(b, n->as.arrayVal.items[i]);
            }
            sb_puts(b, "]");
            break;
        case JSON_T_OBJECT:
            sb_puts(b, "{");
            for (int i = 0; i < n->as.objectVal.count; i++) {
                if (i) sb_puts(b, ",");
                ObjString* k = n->as.objectVal.keys[i];
                sb_puts(b, "\"");
                serve_llm_json_escape(b, k ? k->chars : "", k ? (size_t)k->length : 0);
                sb_puts(b, "\":");
                sj_write_compact(b, n->as.objectVal.values[i]);
            }
            sb_puts(b, "}");
            break;
        default: sb_puts(b, "null"); break;  /* currency/numeric : hors args d'outil */
    }
}

/* Noms d'outils, copies C avant vm_pop (valide l'appel emis apres generation). */
typedef struct { char** names; int count; } ToolNames;

static void toolnames_free(ToolNames* t) {
    for (int i = 0; i < t->count; i++) free(t->names[i]);
    free(t->names);
    memset(t, 0, sizeof(*t));
}

static int toolnames_extract(ObjJson* tools, ToolNames* out) {
    memset(out, 0, sizeof(*out));
    if (tools == NULL || tools->jtype != JSON_T_ARRAY) return 0;
    int n = tools->as.arrayVal.count;
    if (n <= 0) return 0;
    out->names = (char**)calloc((size_t)n, sizeof(char*));
    if (out->names == NULL) return -1;
    for (int i = 0; i < n; i++) {
        ObjJson* fn = sj_field(tools->as.arrayVal.items[i], "function");
        size_t nl = 0;
        const char* nm = sj_cstr(sj_field(fn, "name"), &nl);
        if (nl > 0) {
            out->names[out->count] = cdup(nm);
            if (out->names[out->count] == NULL) return -1;
            out->count++;
        }
    }
    return 0;
}

static int toolnames_has(const ToolNames* t, const char* name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->names[i], name) == 0) return 1;
    return 0;
}

/* Message systeme synthetique d'injection (bloc <tools> + consigne d'appel,
 * decision E). chatml n'a pas de slot d'outils : le bloc est generique. */
static void build_tools_system(SB* b, const char* tools_compact, int tc_mode) {
    sb_puts(b, "Outils disponibles : <tools>");
    sb_puts(b, tools_compact ? tools_compact : "[]");
    sb_puts(b, "</tools>. Pour appeler un outil, reponds UNIQUEMENT par un objet "
               "JSON {\"name\":<nom>,\"arguments\":{...}} sans aucun texte autour.");
    if (tc_mode == OWL_TC_REQUIRED || tc_mode == OWL_TC_NAMED)
        sb_puts(b, " Tu DOIS appeler un outil.");
    else
        sb_puts(b, " Sinon, reponds normalement en texte.");
}

/* Injecte le bloc <tools> (decision E : prefixe le systeme client s'il ouvre la
 * conversation, sinon insere un systeme en tete). Nouvelles chaines allouees
 * dans m->store (liberees avec ChatMsgs). Retourne 0 si ok. */
static int chatmsgs_inject_tools(ChatMsgs* m, const char* tools_compact, int tc_mode) {
    SB sys; sb_init(&sys);
    build_tools_system(&sys, tools_compact, tc_mode);

    char** ns = (char**)realloc(m->store, (m->store_n + 2) * sizeof(char*));
    if (ns == NULL) { sb_free(&sys); return -1; }
    m->store = ns;

    int lead_is_system = (m->count > 0 && m->items[0].role != NULL &&
                          strcmp(m->items[0].role, "system") == 0);

    if (lead_is_system) {
        SB merged; sb_init(&merged);
        sb_putn(&merged, sys.p ? sys.p : "", sys.len);
        sb_puts(&merged, "\n");
        sb_puts(&merged, m->items[0].content ? m->items[0].content : "");
        sb_free(&sys);
        char* c = merged.p ? merged.p : cdup("");
        if (c == NULL) return -1;
        m->store[m->store_n++] = c;   /* ownership transferee */
        m->items[0].content = c;
        return 0;
    }

    /* Insere un nouveau message systeme en tete. */
    OwlLlmChatMsg* ni = (OwlLlmChatMsg*)realloc(m->items,
                            (m->count + 1) * sizeof(OwlLlmChatMsg));
    if (ni == NULL) { sb_free(&sys); return -1; }
    m->items = ni;
    memmove(&m->items[1], &m->items[0], m->count * sizeof(OwlLlmChatMsg));

    char* role    = cdup("system");
    char* content = sys.p ? sys.p : cdup("");   /* ownership de sys.p transferee */
    if (role == NULL || content == NULL) { free(role); free(content); return -1; }
    m->store[m->store_n++] = role;
    m->store[m->store_n++] = content;
    m->items[0].role    = role;
    m->items[0].content = content;
    m->count++;
    return 0;
}

/* id d'appel `call_owl_<created>_<seq>` (compteur monotone best-effort). */
static unsigned g_toolcall_seq = 0;
static void toolcall_make_id(char* out, size_t cap, long long created) {
    snprintf(out, cap, "call_owl_%08llx_%u", (unsigned long long)created, g_toolcall_seq++);
}

/* Corps JSON single-shot d'un appel d'outil (calque build_single) :
 * message.tool_calls[1], content:null, finish_reason:"tool_calls", usage. */
static void build_single_toolcall(SB* b, const SseSink* s, const char* call_id,
                                  const char* name, const char* args_json,
                                  const OwlLlmChatIssue* issue) {
    int in  = issue->input_tokens;
    int out = issue->output_tokens;
    sb_puts(b, "{\"id\":\"chatcmpl-owl\",\"object\":\"chat.completion\",\"created\":");
    sb_puti(b, s->created);
    sb_puts(b, ",\"model\":\"");
    serve_llm_json_escape(b, s->model_id ? s->model_id : "", s->model_id ? strlen(s->model_id) : 0);
    sb_puts(b, "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,"
               "\"tool_calls\":[{\"id\":\"");
    serve_llm_json_escape(b, call_id, strlen(call_id));
    sb_puts(b, "\",\"type\":\"function\",\"function\":{\"name\":\"");
    serve_llm_json_escape(b, name, strlen(name));
    sb_puts(b, "\",\"arguments\":\"");
    serve_llm_json_escape(b, args_json, strlen(args_json));
    sb_puts(b, "\"}}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":");
    sb_puti(b, in);
    sb_puts(b, ",\"completion_tokens\":");
    sb_puti(b, out);
    sb_puts(b, ",\"total_tokens\":");
    sb_puti(b, in + out);
    sb_puts(b, "}}");
}

/* Pousse une frame SSE dont le corps `delta:{<delta_body>}` est fourni. */
static int sse_push_delta(SseSink* s, const char* delta_body) {
    SB fb; sb_init(&fb);
    sse_frame_prelude(&fb, s);
    sb_puts(&fb, delta_body);
    sb_puts(&fb, "},\"finish_reason\":null}]}\n\n");
    int r = sse_push(s, &fb);
    sb_free(&fb);
    return r;
}

/* Emet un appel d'outil en flux SSE : frame role, delta name, delta arguments,
 * frame finale finish_reason:"tool_calls", puis [DONE]. Un seul morceau
 * d'arguments = valide OpenAI (le client reassemble). */
static void sse_emit_toolcall(SseSink* s, const char* call_id, const char* name,
                              const char* args_json) {
    if (sse_ensure_open(s) != 0) return;   /* 1re frame delta:{role:assistant} */

    {   /* delta1 : id + function.name (arguments encore vide). */
        SB d; sb_init(&d);
        sb_puts(&d, "\"tool_calls\":[{\"index\":0,\"id\":\"");
        serve_llm_json_escape(&d, call_id, strlen(call_id));
        sb_puts(&d, "\",\"type\":\"function\",\"function\":{\"name\":\"");
        serve_llm_json_escape(&d, name, strlen(name));
        sb_puts(&d, "\",\"arguments\":\"\"}}]");
        sse_push_delta(s, d.p ? d.p : "");
        sb_free(&d);
    }
    {   /* delta2 : function.arguments = chaine JSON echappee. */
        SB d; sb_init(&d);
        sb_puts(&d, "\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"");
        serve_llm_json_escape(&d, args_json, strlen(args_json));
        sb_puts(&d, "\"}}]");
        sse_push_delta(s, d.p ? d.p : "");
        sb_free(&d);
    }
    {   /* frame finale : delta vide + finish_reason tool_calls. */
        SB fb; sb_init(&fb);
        sse_frame_prelude(&fb, s);
        sb_puts(&fb, "},\"finish_reason\":\"tool_calls\"}]}\n\n");
        sse_push(s, &fb);
        sb_free(&fb);
    }
    static const char done[] = "data: [DONE]\n\n";
    owl_http_rep_flux_pousse(s->flux, done, sizeof(done) - 1);
    owl_http_rep_flux_termine(s->flux);
    owl_http_rep_flux_relache(s->flux);
}

/* Emission d'un corps single-shot ou d'un flux texte accumule (chemin de
 * repli tool_mode : sortie non reconnue comme appel). */
static void serve_emit_body(OwlHttpProdCtx* pctx, const SB* b) {
    OwlHttpRep* f = owl_http_prod_ouvre_flux(pctx, 200, "application/json", 1u << 20);
    if (f != NULL) {
        owl_http_rep_flux_pousse(f, b->p, b->len);
        owl_http_rep_flux_termine(f);
        owl_http_rep_flux_relache(f);
    }
}

/* Fin de generation en tool_mode : reframe la sortie accumulee en tool_calls
 * (si `{name,arguments}` avec name connu), sinon repli sur le chemin texte.
 * Le flux SSE n'a pas ete ouvert pendant la generation (accumulation). */
static void serve_chat_finish_tool(OwlHttpProdCtx* pctx, SseSink* s,
                                   const ToolNames* tnames, int rc,
                                   const OwlLlmChatIssue* issue) {
    if (rc != 0 || issue->erreur) {
        if (issue->prompt_too_long) serve_error_prompt_too_long(pctx, issue);
        else serve_error(pctx, 500, "internal_error", "Echec de la generation");
        return;
    }

    /* Reframe : parse la sortie accumulee, cherche {name,arguments}. */
    char* name_c = NULL;
    char* args_c = NULL;
    ObjJson* parsed = json_parse_cstr(s->acc.p ? s->acc.p : "", (int)s->acc.len);
    if (parsed != NULL) {
        vm_push(OBJ_VAL(parsed));   /* root pendant l'inspection + sj_write_compact */
        if (parsed->jtype == JSON_T_OBJECT) {
            ObjJson* nm = sj_field(parsed, "name");
            ObjJson* ar = sj_field(parsed, "arguments");
            if (nm != NULL && nm->jtype == JSON_T_STRING && nm->as.strVal != NULL &&
                ar != NULL && ar->jtype == JSON_T_OBJECT &&
                toolnames_has(tnames, nm->as.strVal->chars)) {
                name_c = cdup(nm->as.strVal->chars);
                SB ab; sb_init(&ab);
                sj_write_compact(&ab, ar);
                args_c = ab.p ? ab.p : cdup("{}");
            }
        }
        vm_pop();
    }

    if (name_c != NULL && args_c != NULL) {
        char call_id[64];
        toolcall_make_id(call_id, sizeof(call_id), s->created);
        if (s->stream) {
            sse_emit_toolcall(s, call_id, name_c, args_c);
        } else {
            SB b; sb_init(&b);
            build_single_toolcall(&b, s, call_id, name_c, args_c, issue);
            serve_emit_body(pctx, &b);
            sb_free(&b);
        }
        free(name_c);
        free(args_c);
        return;
    }
    free(name_c);
    free(args_c);

    /* Pas un appel d'outil (auto -> texte, ou parse KO) : chemin texte. */
    if (s->stream) {
        if (sse_ensure_open(s) == 0) {
            if (s->acc.len > 0) sse_push_content(s, s->acc.p, s->acc.len);
            sse_finish(s, issue);
        } else if (s->flux != NULL) {
            owl_http_rep_flux_avorte(s->flux);
            owl_http_rep_flux_relache(s->flux);
        }
    } else {
        SB b; sb_init(&b);
        build_single(&b, s, issue);
        serve_emit_body(pctx, &b);
        sb_free(&b);
    }
}

/* POST /v1/chat/completions : parse, extrait en C, genere, mappe vers SSE ou
 * single-shot. */
static void serve_chat(const OwlHttpRequete* req, OwlHttpProdCtx* pctx,
                       ServeLlmCtx* sctx) {
    ObjJson* root = json_parse_cstr(req->corps ? req->corps : "",
                                    (int)req->corps_len);
    if (root == NULL) {
        serve_error(pctx, 400, "invalid_request_error",
                    "Corps JSON absent ou malforme");
        return;
    }
    vm_push(OBJ_VAL(root));   /* root pendant l'extraction (defensif) */

    ChatMsgs msgs;
    int ex = chatmsgs_extract(root, &msgs);

    int    stream = 0;
    double temperature = 0.8;
    int    max_tokens  = 512;
    {
        ObjJson* st = sj_field(root, "stream");
        if (st != NULL && st->jtype == JSON_T_BOOL) stream = st->as.boolVal ? 1 : 0;
        ObjJson* tp = sj_field(root, "temperature");
        if (tp != NULL && tp->jtype == JSON_T_REAL) temperature = tp->as.realVal;
        else if (tp != NULL && tp->jtype == JSON_T_INT) temperature = (double)tp->as.intVal;
        ObjJson* mt = sj_field(root, "max_tokens");
        if (mt != NULL && mt->jtype == JSON_T_INT) max_tokens = (int)mt->as.intVal;
        else if (mt != NULL && mt->jtype == JSON_T_REAL) max_tokens = (int)mt->as.realVal;
    }

    /* Tool-calling. TOUT ce qui lit la racine JSON (tools,
     * tool_choice, GBNF, injection, noms d'outils) DOIT etre materialise AVANT
     * le vm_pop (regle d'or) : la generation ne travaille plus qu'en C. */
    ObjJson* tools = sj_field(root, "tools");
    int have_tools = (tools != NULL && tools->jtype == JSON_T_ARRAY &&
                      tools->as.arrayVal.count > 0);

    int         tc_mode     = OWL_TC_AUTO;
    int         use_grammar = 0;
    const char* forced_name = NULL;
    if (have_tools) {
        ObjJson* tc = sj_field(root, "tool_choice");
        if (tc == NULL) {
            tc_mode = OWL_TC_AUTO; use_grammar = 1;   /* defaut = auto */
        } else if (tc->jtype == JSON_T_STRING && tc->as.strVal != NULL) {
            const char* v = tc->as.strVal->chars;
            if      (strcmp(v, "none") == 0)     { use_grammar = 0; }
            else if (strcmp(v, "required") == 0) { tc_mode = OWL_TC_REQUIRED; use_grammar = 1; }
            else                                 { tc_mode = OWL_TC_AUTO;     use_grammar = 1; }
        } else if (tc->jtype == JSON_T_OBJECT) {
            ObjJson* fn = sj_field(tc, "function");
            ObjJson* nm = sj_field(fn, "name");
            if (nm != NULL && nm->jtype == JSON_T_STRING && nm->as.strVal != NULL) {
                tc_mode = OWL_TC_NAMED; forced_name = nm->as.strVal->chars; use_grammar = 1;
            } else { tc_mode = OWL_TC_AUTO; use_grammar = 1; }
        }
    }

    char*     tool_gbnf   = NULL;
    int       tool_active = 0;
    ToolNames tnames; memset(&tnames, 0, sizeof(tnames));

    if (have_tools && use_grammar) {
        OwlToolGbnfError terr; memset(&terr, 0, sizeof(terr));
        tool_gbnf = owl_tool_schema_to_gbnf(tools, tc_mode, forced_name, &terr);
        if (tool_gbnf == NULL) {
            /* Schema hors sous-ensemble : 400 explicite (jamais de fallback muet). */
            vm_pop();
            chatmsgs_free(&msgs);
            char detail[420];
            snprintf(detail, sizeof(detail),
                     "Schema d'outil non convertible en grammaire (%s : %s)",
                     terr.where, terr.reason);
            serve_error(pctx, 400, "invalid_request_error", detail);
            return;
        }
        tool_active = 1;
        toolnames_extract(tools, &tnames);   /* copies C, valident l'appel emis */

        SB tc_compact; sb_init(&tc_compact);
        sj_write_compact(&tc_compact, tools);
        chatmsgs_inject_tools(&msgs, tc_compact.p ? tc_compact.p : "[]", tc_mode);
        sb_free(&tc_compact);
    }

    vm_pop();   /* racine JSON larguee AVANT generation (regle d'or / decision F) */

    if (ex != 0) {
        toolnames_free(&tnames);
        free(tool_gbnf);
        serve_error(pctx, 500, "internal_error", "Extraction des messages impossible");
        return;
    }

    OwlLlmChatParams params;
    memset(&params, 0, sizeof(params));
    params.messages    = msgs.items;
    params.nb_messages = msgs.count;
    params.temperature = temperature;
    params.max_tokens  = max_tokens;
    params.grammar     = tool_gbnf;   /* NULL hors tool-calling -> chemin OWL/chat inchange */

    SseSink s;
    memset(&s, 0, sizeof(s));
    s.pctx      = pctx;
    s.model_id  = sctx->model_id;
    s.created   = (long long)time(NULL);
    s.stream    = stream;
    s.tool_mode = tool_active;
    sb_init(&s.acc);

    OwlLlmChatIssue issue;
    memset(&issue, 0, sizeof(issue));

    int rc = sctx->generate(sctx, &params, serve_llm_sink, &s, &issue);

    if (tool_active) {
        /* En tool_mode la sortie a ete accumulee : reframe en tool_calls (ou
         * repli texte). Le flux SSE n'a pas ete ouvert pendant la generation. */
        serve_chat_finish_tool(pctx, &s, &tnames, rc, &issue);
    } else if (stream) {
        if (s.flux == NULL) {
            /* Aucun token committe : l'en-tete n'est pas engage. */
            if (rc != 0 || issue.erreur) {
                if (issue.prompt_too_long) serve_error_prompt_too_long(pctx, &issue);
                else serve_error(pctx, 500, "internal_error", "Echec de la generation");
            } else {
                /* Generation vide mais reussie : flux 200 + finale + [DONE]. */
                if (sse_ensure_open(&s) == 0) sse_finish(&s, &issue);
                else if (s.flux != NULL) { owl_http_rep_flux_avorte(s.flux); owl_http_rep_flux_relache(s.flux); }
            }
        } else {
            if (rc != 0 || issue.erreur) {
                /* Erreur mi-flux : fermeture abrupte, pas de terminal ni [DONE]. */
                owl_http_rep_flux_avorte(s.flux);
                owl_http_rep_flux_relache(s.flux);
            } else if (!s.abandon) {
                sse_finish(&s, &issue);
            } else {
                /* Consommateur parti : rien de plus a pousser, on relache. */
                owl_http_rep_flux_avorte(s.flux);
                owl_http_rep_flux_relache(s.flux);
            }
        }
    } else {
        /* single-shot : un unique chunk chat.completion + usage (decision A). */
        if (rc != 0 || issue.erreur) {
            if (issue.prompt_too_long) serve_error_prompt_too_long(pctx, &issue);
            else serve_error(pctx, 500, "internal_error", "Echec de la generation");
        } else {
            SB b; sb_init(&b);
            build_single(&b, &s, &issue);
            serve_emit_body(pctx, &b);
            sb_free(&b);
        }
    }

    free(tool_gbnf);
    toolnames_free(&tnames);
    sb_free(&s.acc);
    chatmsgs_free(&msgs);
}

/* ================================================================
 * Routage
 * ================================================================ */

/* Compare le chemin de la requete a une route, en ignorant une query string. */
static int path_eq(const char* chemin, const char* route) {
    if (chemin == NULL) return 0;
    size_t n = strlen(route);
    if (strncmp(chemin, route, n) != 0) return 0;
    char c = chemin[n];
    return c == '\0' || c == '?';
}

void serve_llm_producteur(const OwlHttpRequete* req, OwlHttpProdCtx* ctx,
                          void* user) {
    ServeLlmCtx* sctx = (ServeLlmCtx*)user;
    const char*  path = req->chemin;
    int is_get  = strcmp(req->methode, "GET")  == 0;
    int is_post = strcmp(req->methode, "POST") == 0;

    if (is_get && path_eq(path, "/v1/models")) {
        serve_models(ctx, sctx);
        return;
    }
    if (is_post && path_eq(path, "/v1/chat/completions")) {
        serve_chat(req, ctx, sctx);
        return;
    }
    /* Route ou methode inconnue : 404. `Authorization` n'est jamais consulte. */
    serve_error(ctx, 404, "not_found", "Route inconnue");
}

/* ================================================================
 * Generateur reel (defaut de ServeLlmCtx.generate) : prepare + stream.
 *
 * Le test ECRASE ce pointeur par un faux generateur ; ne PAS changer ce
 * mecanisme d'indirection.
 * ================================================================ */

static int serve_llm_default_generate(ServeLlmCtx* ctx,
                                      const OwlLlmChatParams* params,
                                      OwlLlmTokenSink sink, void* sink_user,
                                      OwlLlmChatIssue* issue) {
    OwlLlmPrepared prep;
    memset(&prep, 0, sizeof(prep));
    if (owl_llm_chat_prepare(&ctx->handles, params->messages, params->nb_messages,
                             &prep) != 0) {
        if (issue) { memset(issue, 0, sizeof(*issue)); issue->erreur = 1; }
        return -1;   /* prepare echoue : 0 token emis */
    }
    int rc = owl_llm_chat_stream(&ctx->handles, &prep, params, sink, sink_user, issue);
    owl_llm_prepared_libere(&prep);
    return rc;
}

/* ================================================================
 * CLI `owl serve-llm`
 * ================================================================ */

static void serve_llm_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&ts, NULL);
#endif
}

int serve_llm_parse_timeout_sec_to_ms(int sec) {
    if (sec <= 0) return -1;                 /* 0 ou negatif -> attente infinie */
    if (sec > INT_MAX / 1000) return INT_MAX; /* borne anti-overflow (sec*1000) */
    return sec * 1000;
}

int serve_llm_parse_ctx_size(const char* s, uint32_t* out) {
    if (out) *out = 0;
    if (s == NULL || s[0] == '\0') return -1;
    char* fin = NULL;
    errno = 0;
    long v = strtol(s, &fin, 10);
    if (fin == s || *fin != '\0') return -1;  /* non numerique / suffixe parasite */
    if (errno == ERANGE || v <= 0) return -1; /* 0, negatif, overflow -> refus */
    if (v > (long)OWL_LLM_N_CTX_MAX) v = (long)OWL_LLM_N_CTX_MAX; /* borne haute */
    if (out) *out = (uint32_t)v;
    return 0;
}

static void serve_llm_usage(void) {
    fprintf(stderr,
        "Usage : owl serve-llm --model <fichier.gguf> [--port 11434] [--host 127.0.0.1]\n"
        "                       [--request-timeout <sec>] [--ctx-size <n>]\n"
        "  Expose le modele local en API compatible OpenAI (SSE).\n"
        "  Endpoints : POST /v1/chat/completions, GET /v1/models.\n"
        "  --host non loopback : refuse sauf indication explicite.\n"
        "  --request-timeout <sec> : delai max d'attente en file avant 503\n"
        "                            (defaut 300 ; 0 ou negatif = illimite).\n"
        "  --ctx-size <n> : fenetre de contexte en jetons (defaut :\n"
        "                   min(contexte d'entrainement, 8192)) ; plus grand =\n"
        "                   plus de RAM (cache KV). Borne haute 131072.\n"
        "\n"
        "  Tool-calling (appel d'outil) : le champ `tools` d'une requete force,\n"
        "  par grammaire, un appel structure (choices[].message.tool_calls,\n"
        "  finish_reason \"tool_calls\"), en streaming comme en single-shot.\n"
        "  `tool_choice` : auto (defaut) | required | none | {function:{name}}.\n"
        "  Sous-ensemble de schema couvert : object/string/number/integer/boolean/\n"
        "  array + enum + required/optionnel + objet imbrique. Hors sous-ensemble\n"
        "  ($ref, allOf/anyOf/oneOf, pattern, format, bornes...) : 400 explicite.\n");
}

int owl_serve_llm_main(int argc, char** argv) {
    const char* model = NULL;
    const char* host  = "127.0.0.1";
    int         port  = 11434;
    int         host_explicite = 0;
    int         request_timeout_sec    = 0;  /* secondes ; applique si fourni */
    int         request_timeout_fourni = 0;
    uint32_t    ctx_size = 0;                 /* 0 = pas d'override (defaut inchange) */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--request-timeout") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            char* fin = NULL;
            long sec = strtol(v, &fin, 10);
            if (fin == v || *fin != '\0') {
                fprintf(stderr, "serve-llm : --request-timeout attend un entier "
                                "(secondes), recu '%s'\n", v);
                serve_llm_usage();
                return 2;
            }
            if (sec > INT_MAX) sec = INT_MAX;   /* clamp avant cast, evite l'UB */
            if (sec < INT_MIN) sec = INT_MIN;
            request_timeout_sec    = (int)sec;
            request_timeout_fourni = 1;
        } else if (strcmp(argv[i], "--ctx-size") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (serve_llm_parse_ctx_size(v, &ctx_size) != 0) {
                fprintf(stderr, "serve-llm : --ctx-size attend un entier > 0 "
                                "(jetons), recu '%s'\n", v);
                serve_llm_usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
            host_explicite = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            serve_llm_usage();
            return 0;
        } else {
            fprintf(stderr, "serve-llm : argument inconnu '%s'\n", argv[i]);
            serve_llm_usage();
            return 2;
        }
    }

    if (model == NULL || model[0] == '\0') {
        fprintf(stderr, "serve-llm : --model <fichier.gguf> est obligatoire\n");
        serve_llm_usage();
        return 2;
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "serve-llm : port invalide (%d)\n", port);
        return 2;
    }

    /* Chargement du modele sur le THREAD PRINCIPAL, sans VM/GC (decision G). */
    ServeLlmCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    if (owl_llm_charge_handles(model, ctx_size, &sctx.handles) != 0) {
        fprintf(stderr, "serve-llm : impossible de charger le modele '%s'\n", model);
        return 1;
    }
    sctx.model_id = model;
    sctx.generate = serve_llm_default_generate;

    /* Executeur mode C : le producteur possede la VM via le thread executeur. */
    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 32;      /* absorbe rafales/retries */
    xc.plafond_attente_ms = 300000;  /* 300 s : file patiente sur CPU mono-ctx */
    xc.producteur         = serve_llm_producteur;
    xc.producteur_user    = &sctx;

    /* --request-timeout override (secondes). Parse dans une locale sentinelle,
     * applique APRES lecture de tous les args (l'ordre des flags n'importe pas). */
    if (request_timeout_fourni) {
        xc.plafond_attente_ms = serve_llm_parse_timeout_sec_to_ms(request_timeout_sec);
    }

    OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
    if (x == NULL || !owl_http_executeur_demarre(x)) {
        fprintf(stderr, "serve-llm : demarrage de l'executeur impossible\n");
        if (x) owl_http_executeur_libere(x);
        owl_llm_libere(&sctx.handles);
        return 1;
    }

    /* bind : loopback autorise ; non-loopback refuse sauf --host explicite. */
    int autoriser_non_loopback = host_explicite;
    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, host, port, 64, autoriser_non_loopback != 0)) {
        fprintf(stderr, "serve-llm : ecoute impossible sur %s:%d (%s)\n",
                host, port, owl_tcp_listener_erreur(&l));
        owl_http_executeur_libere(x);
        owl_llm_libere(&sctx.handles);
        return 1;
    }

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    /* handler_rep sert le plan de CONTROLE inline (/v1/models, /health) sans
     * toucher la VM. Le data-plane (generation) passe par executeur_async :
     * le worker depose et remet le jeton au reacteur, sans jamais attendre
     * l'executeur mono-thread (W7 — fin de la retention de worker). */
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;
    cfg.executeur_async  = x;

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers     = 4;
    pc.connexions_max = 16;

    owl_tcp_server_installe_arret();   /* SIGINT / SIGTERM -> arret propre */

    OwlTcpPool* pool = owl_http_serveur_pool_demarre(&l, &cfg, &pc);
    if (pool == NULL) {
        fprintf(stderr, "serve-llm : demarrage du pool reseau impossible\n");
        owl_tcp_listener_ferme(&l);
        owl_http_executeur_libere(x);
        owl_llm_libere(&sctx.handles);
        return 1;
    }

    fprintf(stderr, "serve-llm : modele '%s' pret, ecoute sur http://%s:%d\n",
            model, host, port);
    fprintf(stderr, "  POST /v1/chat/completions | GET /v1/models  (Ctrl+C pour arreter)\n");

    /* Attente de l'arret : les workers acceptent+servent, le thread principal
     * ne fait que veiller le signal. */
    while (!owl_tcp_server_arret_demande()) {
        serve_llm_sleep_ms(200);
    }

    fprintf(stderr, "serve-llm : arret en cours...\n");
    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_http_executeur_libere(x);
    owl_llm_libere(&sctx.handles);
    return 0;
}
