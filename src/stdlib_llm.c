// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_llm.c — Type `LLM` (, bataille 2 sous-chantier B).
 *
 * Surface OWL au-dessus de l'enabler ggml/llama.cpp (, ,
 * deja vendore + prouve au niveau C par llamacpp_smoketest). Deux types :
 *
 *   - LLM               : un modele GGUF charge en memoire. `.Charge(chemin)`
 *                         lit le fichier et prepare l'inference ; `.Génère(prompt,
 *                         options)` produit du texte. Detient deux handles
 *                         C-heap (llama_model* + llama_context*) liberes au
 *                         finalizer GC.
 *   - OptionsGénération : reglages d'UN appel de generation (analogie
 *                         OptionsEntraînement pour l'entrainement). Feuille
 *                         scalaire + un callback de flux `..SurToken` rappele a
 *                         chaque token genere.
 *
 * Pipeline d'inference (calque de llamacpp_smoketest, chemin prouve sur Qwen) :
 *   load -> vocab -> contexte -> tokenize prompt -> boucle
 *   { decode -> sample (chaine de samplers) -> detokenize -> callback } -> texte.
 *
 * Temperature 0 => sampler greedy (choix systematique du token le plus
 * probable), generation reproductible d'un appel a l'autre (le cache KV est
 * remis a zero au debut de chaque .Génère). Sinon : chaine top-k / top-p /
 * temp / dist(graine).
 *
 * Le logging verbeux de llama/ggml est muselé via llama_log_set (sinon spam
 * stderr au chargement).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "sentinels.h"   /*  : OWL_STOP_* (raison d'arret de .Chat) */
#include "stdlib_llm.h"  /*  : API C VM-free (seam serve-llm) */
#include "owl_sb.h"      /*  : SB partage (convertisseur schema->GBNF) */
#include "vm.h"

#include "llama.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Plafond du contexte alloue a .Charge. Le defaut effectif est pilote par le
 * modele : min(contexte d'entrainement du GGUF, ce plafond). Independant de
 * ..MaxTokens (par appel). On borne l'empreinte memoire : le cache KV croit
 * lineairement avec n_ctx (K+V, f16 => 2 octets, ~n_couches x n_embd_kv). A ce
 * plafond, le KV pese ~0,5 Go sur un 7B et ~1,5 Go sur un 14B ; 16384
 * doublerait ces chiffres. 8192 debloque un usage code/chat reel (l'ancien
 * 2048 en dur plafonnait prompt+generation a 2048 tokens, inutilisable) tout
 * en gardant la RAM par defaut raisonnable, y compris sur les modeles a tres
 * grand contexte (128k). Un knob configurable par appel = suivi separe. */
#define OWL_LLM_N_CTX_CAP 8192

/* Museler le logging llama/ggml : sans ca, le chargement d'un GGUF crache des
 * dizaines de lignes sur stderr. */
static void llm_quiet_log(enum ggml_log_level level, const char* text, void* ud) {
    (void)level; (void)text; (void)ud;
}

/* Backend llama initialise une seule fois par processus (idempotent cote
 * llama, mais le garde evite un re-log). Le logging verbeux est musele au
 * premier passage. Partage par le chemin OWL (fn_llm_charge) et le chemin C
 * VM-free (owl_llm_charge_handles). */
static void llm_backend_ensure(void) {
    static bool s_backend_ready = false;
    if (!s_backend_ready) {
        llama_log_set(llm_quiet_log, NULL);
        llama_backend_init();
        s_backend_ready = true;
    }
}

/* ================================================================
 * Point d'emission par token (seam )
 *
 * Le moteur (llm_generate_core) ne connait plus directement ..SurToken : il
 * emet chaque morceau via LlmEmit, qui dispatche vers UN consommateur :
 *   - chemin OWL   : owlCb (procedure ..SurToken), rappelee par vm_call_reentrant ;
 *   - chemin C     : cSink (serve-llm), rappele avec des octets copies.
 * ..SurToken reste INTACT : fn_llm_genere/fn_llm_chat posent LlmEmit{.owlCb}.
 * ================================================================ */

/* Retour de llm_emit_piece. */
#define LLM_EMIT_OK      0
#define LLM_EMIT_RAISED  1   /* le callback OWL a leve une exception */
#define LLM_EMIT_ABANDON 2   /* le sink C a demande l'arret (consommateur parti) */

typedef struct {
    Value           owlCb;   /* ..SurToken (NIL_VAL si chemin C) */
    OwlLlmTokenSink cSink;   /* sink C (NULL si chemin OWL) */
    void*           cUser;
} LlmEmit;

static bool llm_emit_active(const LlmEmit* e) {
    return e != NULL && (e->cSink != NULL || IS_CALLABLE(e->owlCb));
}

/* Emet un morceau (octets copies cote OWL comme cote C). Le chemin C ne
 * touche jamais la VM (VM-free, decision G) ; le chemin OWL alloue une chaine
 * et deferre l'appel a la procedure. */
static int llm_emit_piece(const LlmEmit* e, const char* piece, size_t len,
                          bool termine) {
    if (e->cSink != NULL) {
        int r = e->cSink(piece, len, termine ? 1 : 0, e->cUser);
        return r != 0 ? LLM_EMIT_ABANDON : LLM_EMIT_OK;
    }
    if (IS_CALLABLE(e->owlCb)) {
        vm_push(e->owlCb);
        vm_push(OBJ_VAL(obj_copy_string(piece, (int)len)));
        vm_push(BOOL_VAL(termine));
        if (vm_call_reentrant(e->owlCb, 2) != INTERPRET_OK) return LLM_EMIT_RAISED;
        vm_pop();  /* resultat du callback (ignore) */
    }
    return LLM_EMIT_OK;
}

/* ================================================================
 * Liberation des handles (finalizer GC + rechargement)
 * ================================================================ */

/* Callback enregistre dans object.c (obj_llm_set_backend_free) : object.c ne
 * connait pas llama, il delegue ici. Idempotent, safe sur un LLM jamais charge. */
static void llm_backend_free(ObjLLM* l) {
    if (l == NULL) return;
    if (l->ctx != NULL)   llama_free((struct llama_context*)l->ctx);
    if (l->model != NULL) llama_model_free((struct llama_model*)l->model);
    /* vocab est emprunte au modele : rien a liberer. object.c remet les trois
     * champs a NULL apres cet appel. */
}

/* ================================================================
 * Factories de declaration (`mon est un LLM`, `opts est un OptionsGénération`)
 * ================================================================ */

static Value fn_llm_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_llm());
}

static Value fn_options_generation_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_options_generation());
}

static Value fn_message_llm_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_message_llm());
}

static Value fn_reponse_llm_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_response_llm());
}

/* ================================================================
 * .Charge / .Load
 * ================================================================ */

static Value llm_charge_echec(const char* path, const char* detail) {
    owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Charge",
                          "native", "LLM.Charge",
                          "path", path, "detail", detail, NULL);
    return BOOL_VAL(false);
}

/* .Charge(chemin) / .Load : charge un GGUF, cree le contexte d'inference.
 * Rend Vrai en cas de succes, Faux (+ ErreurDétectée) sinon — jamais de crash.
 * Un chemin absent / illisible / non-GGUF echoue proprement (loader -> NULL). */
static Value fn_llm_charge(int argCount, Value* args) {
    (void)argCount;
    if (!IS_LLM(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.Charge",
                              "arg", "modele", "expected", "LLM",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.Charge",
                              "arg", "chemin", "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    ObjLLM*     l    = AS_LLM(args[0]);
    const char* path = AS_CSTRING(args[1]);

    /* Backend llama initialise une seule fois (log musele au premier passage). */
    llm_backend_ensure();

    /* Rechargement : on repart d'un etat propre (les anciens handles, s'il y en
     * a, sont liberes). Un echec ci-dessous laisse le LLM non charge. */
    obj_llm_free_handles(l);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;  /* CPU-only, coherent avec le vendoring ggml-cpu */
    struct llama_model* model = llama_model_load_from_file(path, mp);
    if (model == NULL) {
        return llm_charge_echec(path, "modele introuvable ou GGUF illisible");
    }

    struct llama_context_params cp = llama_context_default_params();
    /* Defaut model-aware : on prend le contexte d'entrainement du modele,
     * plafonne pour borner la RAM du cache KV. n_ctx_train est positif sur un
     * GGUF valide ; garde defensive si un modele exotique rend 0/negatif. */
    int32_t n_ctx_train = llama_model_n_ctx_train(model);
    uint32_t n_ctx = (n_ctx_train > 0 && n_ctx_train < OWL_LLM_N_CTX_CAP)
                         ? (uint32_t)n_ctx_train
                         : OWL_LLM_N_CTX_CAP;
    cp.n_ctx = n_ctx;
    /* .Génère decode le prompt entier en un seul batch. llama assert dur si
     * n_tokens > n_batch (defaut 2048) : sans ca un prompt > 2048 tokens
     * abort au lieu de tenir dans le contexte alloue. On aligne n_batch sur
     * n_ctx pour qu'un prompt qui rentre dans le contexte rentre aussi dans un
     * batch. n_ubatch reste au defaut (llama le borne a n_batch). */
    cp.n_batch = n_ctx;
    struct llama_context* ctx = llama_init_from_model(model, cp);
    if (ctx == NULL) {
        llama_model_free(model);
        return llm_charge_echec(path, "creation du contexte d'inference impossible");
    }

    l->model = model;
    l->ctx   = ctx;
    l->vocab = (void*)llama_model_get_vocab(model);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * .Génère / .Generate
 * ================================================================ */

/* Buffer texte croissant (C-heap) accumulant les morceaux de tokens. */
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} TextBuf;

static bool textbuf_append(TextBuf* b, const char* s, size_t n) {
    if (n == 0) return true;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap == 0 ? 256 : b->cap;
        while (b->len + n + 1 > nc) nc *= 2;
        char* nd = (char*)realloc(b->data, nc);
        if (nd == NULL) return false;
        b->data = nd;
        b->cap  = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

/* Grammaire JSON GBNF integree, appliquee quand ..ContrainteDeSortie vaut
 * "json" (constante contrainteJSON). Sous-ensemble strict mais complet du
 * JSON : tout ce qu'elle laisse produire est structurellement valide.
 * Reformulation OWL de la grammaire JSON canonique GBNF. Racine : "root". */
static const char OWL_LLM_JSON_GBNF[] =
    "root   ::= object\n"
    "value  ::= object | array | string | number | (\"true\" | \"false\" | \"null\") ws\n"
    "object ::= \"{\" ws ( string \":\" ws value (\",\" ws string \":\" ws value)* )? \"}\" ws\n"
    "array  ::= \"[\" ws ( value (\",\" ws value)* )? \"]\" ws\n"
    "string ::= \"\\\"\" ( [^\"\\\\] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) )* \"\\\"\" ws\n"
    "number ::= (\"-\"? ([0-9] | [1-9] [0-9]*)) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)? ws\n"
    "ws     ::= [ \\t\\n]*\n";

/* Construit la chaine de samplers a partir d'une GBNF DEJA resolue (NULL =
 * aucune contrainte). Un sampler grammaire est ajoute EN TETE : il masque les
 * tokens qui casseraient la structure avant l'echantillonnage final. Un GBNF
 * invalide fait rendre NULL a llama_sampler_init_grammar : on le signale via
 * *grammarError et on rend NULL (aucune chaine allouee). Cette variante
 * "brute" est le seam  : le tool-calling passe sa GBNF d'outil ici
 * sans toucher a o->constraint. Sinon : temperature <= 0 => greedy
 * deterministe ; sinon top-k -> top-p -> temp -> dist(graine). */
static struct llama_sampler* llm_build_sampler_from_gbnf(
        const ObjOptionsGeneration* o, const struct llama_vocab* vocab,
        const char* gbnf, bool* grammarError) {
    *grammarError = false;

    struct llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    struct llama_sampler* chain = llama_sampler_chain_init(sp);

    if (gbnf != NULL) {
        struct llama_sampler* g = llama_sampler_init_grammar(vocab, gbnf, "root");
        if (g == NULL) {
            /* GBNF invalide : pas de chaine partielle qui traine. */
            llama_sampler_free(chain);
            *grammarError = true;
            return NULL;
        }
        llama_sampler_chain_add(chain, g);
    }

    if (o->temperature <= 0.0) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    } else {
        if (o->topK > 0)
            llama_sampler_chain_add(chain, llama_sampler_init_top_k(o->topK));
        if (o->topP > 0.0 && o->topP < 1.0)
            llama_sampler_chain_add(chain,
                llama_sampler_init_top_p((float)o->topP, 1));
        llama_sampler_chain_add(chain,
            llama_sampler_init_temp((float)o->temperature));
        llama_sampler_chain_add(chain,
            llama_sampler_init_dist((uint32_t)o->seed));
    }
    return chain;
}

/* Wrapper chemin OWL : resout la chaine ..ContrainteDeSortie (open-enum) puis
 * delegue a llm_build_sampler_from_gbnf :
 *   - vide             -> aucune contrainte (contrainteLibre / defaut)
 *   - "json" (casse-i) -> grammaire JSON integree (OWL_LLM_JSON_GBNF)
 *   - toute autre      -> grammaire GBNF brute = la chaine elle-meme
 * *jsonConstraint vaut true UNIQUEMENT quand on a resolu vers la grammaire JSON
 * integree — le contrat anti-casse-en-silence post-generation ne s'applique
 * qu'a ce cas. */
static struct llama_sampler* llm_build_sampler(const ObjOptionsGeneration* o,
                                               const struct llama_vocab* vocab,
                                               bool* grammarError,
                                               bool* jsonConstraint) {
    *jsonConstraint = false;

    const char* gbnf = NULL;
    if (IS_STRING(o->constraint) && AS_STRING(o->constraint)->length > 0) {
        ObjString* c = AS_STRING(o->constraint);
        if (c->length == 4 && strncasecmp(c->chars, "json", 4) == 0) {
            /* Jeton reserve : une vraie GBNF contient toujours "::=". */
            gbnf            = OWL_LLM_JSON_GBNF;
            *jsonConstraint = true;
        } else {
            gbnf = c->chars;  /* GBNF brute */
        }
    }
    return llm_build_sampler_from_gbnf(o, vocab, gbnf, grammarError);
}

/* Valide une chaine avec le meme parseur que la native JSONValide (RFC 8259
 * stricte), sans le re-implementer : on resout la native dans vm.globals et on
 * l'appelle en reentrant. Rend true si `text` est un JSON structurellement
 * valide. L'appelant re-pose ou efface ErreurDétectée selon le contrat — cet
 * appel intermediaire ne fixe pas l'etat d'erreur final visible. */
static bool llm_output_is_valid_json(const char* text, int len) {
    ObjString* k = obj_copy_string("JSONValide", 10);
    vm_push(OBJ_VAL(k));                 /* root le nom pendant le lookup */
    Value fn;
    bool found = table_get(&vm.globals, k, &fn);
    vm_pop();
    if (!found || !IS_NATIVE(fn)) return false;

    vm_push(fn);
    vm_push(OBJ_VAL(obj_copy_string(text, len)));
    if (vm_call_reentrant(fn, 1) != INTERPRET_OK) return false;
    Value res = vm_pop();                /* booleen pousse par la native */
    return IS_BOOL(res) && AS_BOOL(res);
}

/* Defauts implicites d'une generation quand aucune OptionsGénération n'est
 * fournie (analogue a un `est un OptionsGénération` fraichement construit). */
static void llm_options_defaults(ObjOptionsGeneration* d) {
    memset(d, 0, sizeof(*d));
    d->temperature = 0.8; d->topK = 40; d->topP = 0.95;
    d->maxTokens = 512;   d->seed = 0;  d->onToken = NIL_VAL;
    d->constraint = NIL_VAL;
}

/* Issue d'un appel au moteur de generation partage (llm_generate_core). */
typedef struct {
    TextBuf out;            /* texte genere (l'appelant libere out.data) */
    int  outputTokens;      /* nombre de jetons generes */
    int  stopReason;        /* OWL_STOP_NATUREL / OWL_STOP_MAX_TOKENS */
    bool raised;            /* le callback a leve une exception -> propager NIL */
    bool abandon;           /* le sink C a demande l'arret (chemin serve-llm) */
    bool decodeErr;         /* echec du decodage llama */
    bool grammarError;      /* GBNF malformee (sampler NULL) */
    bool jsonConstraint;    /* contrainteJSON active (post-check completion) */
    bool jsonTruncated;     /* jsonConstraint && sortie JSON incomplete */
    bool promptTooLong;     /* invite > n_batch : rejetee AVANT decode (anti-abort) */
    int  contextLimit;      /* borne effective (n_batch == n_ctx) quand promptTooLong */
} LlmGenOutcome;

/*  ->  : marqueurs de fin de tour de chat connus (chatml /
 * llama3 / divers), stables et reserves. Un GGUF dont les metadonnees ne taguent
 * PAS son marqueur de fin de tour comme EOG ni control (symptome observe sur
 * certains 14B chatml) laisse fuiter sa surface litterale dans le contenu — et
 * pire, il peut l'emettre en TEXTE multi-tokens (<, |, im, end, |, >), jamais en
 * un seul morceau, donc invisible a un match par-morceau. On les detecte comme
 * stop-string sur la sortie ACCUMULEE (llm_stop_marker_scan), quoi que disent les
 * metadonnees. <|im_start|> (DEBUT de tour) est volontairement absent : ce n'est
 * pas un marqueur de fin. */
static const char* const kLlmEndOfTurnMarkers[] = {
    "<|im_end|>",
    "<|eot_id|>",
    "<|endoftext|>",
    "<|end|>",
};

/* Stop-string sur la sortie accumulee. Voir stdlib_llm.h pour le
 * contrat. Ne scanne que la fenetre de queue (<= longueur max marqueur) : O(1)
 * par token quelle que soit la taille de la sortie. */
int llm_stop_marker_scan(const char* buf, size_t len,
                         size_t* outMarkerStart, size_t* outHoldback) {
    if (outMarkerStart) *outMarkerStart = 0;
    if (outHoldback) *outHoldback = 0;
    if (buf == NULL || len == 0) return 0;

    const size_t n = sizeof(kLlmEndOfTurnMarkers) / sizeof(kLlmEndOfTurnMarkers[0]);

    /* 1. Marqueur COMPLET en fin de buffer (le plus long l'emporte : aucun de
     * ces marqueurs n'est suffixe d'un autre, mais on reste defensif). */
    size_t bestComplete = 0;  /* longueur du marqueur matche (0 = aucun) */
    for (size_t i = 0; i < n; i++) {
        size_t ml = strlen(kLlmEndOfTurnMarkers[i]);
        if (len >= ml && memcmp(buf + len - ml, kLlmEndOfTurnMarkers[i], ml) == 0
            && ml > bestComplete) {
            bestComplete = ml;
        }
    }
    if (bestComplete > 0) {
        if (outMarkerStart) *outMarkerStart = len - bestComplete;
        return 1;
    }

    /* 2. Plus long suffixe du buffer qui est un prefixe PROPRE (< marqueur)
     * d'un marqueur -> octets a retenir en streaming. */
    size_t bestHold = 0;
    for (size_t i = 0; i < n; i++) {
        size_t ml = strlen(kLlmEndOfTurnMarkers[i]);
        size_t maxp = ml - 1;              /* prefixe propre : strictement < ml */
        if (maxp > len) maxp = len;
        for (size_t p = maxp; p > bestHold; p--) {
            if (memcmp(buf + len - p, kLlmEndOfTurnMarkers[i], p) == 0) {
                bestHold = p;              /* descendant : 1er hit = le plus long */
                break;
            }
        }
    }
    if (outHoldback) *outHoldback = bestHold;
    return 0;
}

/* Moteur de generation partage par .Génère et .Chat. Prend un prompt DEJA
 * tokenise, applique la contrainte de sortie, streame les jetons
 * via ..SurToken(morceau, terminé) et rend l'issue dans `res`.
 *
 * Contrat du callback ..SurToken : rappele une fois par jeton,
 * `terminé=Faux` pendant le flux, `terminé=Vrai` sur le DERNIER morceau. On ne
 * connait le dernier morceau qu'au moment ou aucun jeton ne suit (EOG ou
 * plafond ..MaxTokens atteint) : on defere donc chaque rappel d'un cran via
 * pending*. Cas zero-jeton (EOG immediat / MaxTokens=0) : un appel unique
 * morceau="", terminé=Vrai.
 *
 * Sans etat : le cache KV est remis a zero au debut, pour que deux appels
 * successifs (y compris Chat->Génère->Chat) soient independants. */
static void llm_generate_core(struct llama_context* ctx,
                              const struct llama_vocab* vocab,
                              llama_token* tokens, int n_tok,
                              const ObjOptionsGeneration* o,
                              const char* gbnfOverride,
                              const LlmEmit* emit,
                              LlmGenOutcome* res) {
    memset(res, 0, sizeof(*res));
    res->stopReason = OWL_STOP_NATUREL;

    /* Garde anti-abort : llama_decode fait
     * GGML_ASSERT(n_tokens_all <= cparams.n_batch) -> abort DUR du process si
     * l'invite depasse le batch. n_batch est aligne sur n_ctx au chargement
     * (cp.n_batch = n_ctx), donc llama_n_batch(ctx) est la borne exacte de
     * l'assert. On rejette proprement AVANT tout decode (avant meme de batir le
     * sampler, pas de free a gerer) : plus aucun caller n'atteint l'assert. */
    int ctxLimit = (int)llama_n_batch(ctx);
    if (n_tok > ctxLimit) {
        res->promptTooLong = true;
        res->contextLimit  = ctxLimit;
        return;
    }

    bool grammarError = false;
    struct llama_sampler* smpl;
    if (gbnfOverride != NULL) {
        /* Seam  : GBNF imposee (tool-calling). Pas de post-check VM
         * (jsonConstraint reste faux) — le contrat anti-troncature JSON ne
         * s'applique qu'au chemin ..ContrainteDeSortie="json". */
        smpl = llm_build_sampler_from_gbnf(o, vocab, gbnfOverride, &grammarError);
    } else {
        smpl = llm_build_sampler(o, vocab, &grammarError, &res->jsonConstraint);
    }
    if (grammarError) { res->grammarError = true; return; }

    /* Reset du cache KV : reproductibilite + independance entre appels. */
    llama_memory_clear(llama_get_memory(ctx), true);

    bool  hasCallback = llm_emit_active(emit);
    int   maxTokens   = o->maxTokens > 0 ? o->maxTokens : 0;

    /* Premier decode : le prompt entier. */
    struct llama_batch batch = llama_batch_get_one(tokens, n_tok);
    if (llama_decode(ctx, batch) < 0) {
        res->decodeErr = true;
        llama_sampler_free(smpl);
        return;
    }

    /* Emission par CURSEUR d'octets sur res->out. On accumule tout le
     * texte dans res->out, puis on emet au callback les octets [emitCursor, safe)
     * ou safe = out.len - holdback : la queue de longueur `holdback` est RETENUE
     * car elle pourrait etre le debut d'un marqueur de fin de tour qui se
     * completerait au prochain token (marqueur coupe entre deux tokens). Remplace
     * le report d'un cran (pending*), qui ne differait qu'UN token — insuffisant
     * pour un marqueur emis en TEXTE multi-tokens. */
    size_t emitCursor  = 0;     /* octets de res->out deja emis au callback */
    bool   markerStop  = false;
    size_t markerStart = 0;
    bool   reachedMax  = false;

    llama_token cur = 0;
    for (int i = 0; i < maxTokens; i++) {
        cur = llama_sampler_sample(smpl, ctx, -1);
        /* Arret naturel : fin de generation (EOG) OU token de controle non-EOG.
         * Garde conserve : utile pour les GGUF bien tagges. Le
         * stop-string ci-dessous couvre en plus les GGUF qui emettent le marqueur
         * en texte (metadonnees muettes). */
        if (llama_vocab_is_eog(vocab, cur) || llama_vocab_is_control(vocab, cur)) {
            res->stopReason = OWL_STOP_NATUREL;
            break;
        }

        char piece[256];
        int  pn = llama_token_to_piece(vocab, cur, piece, (int)sizeof(piece), 0, false);
        if (pn < 0) pn = 0;

        if (!textbuf_append(&res->out, piece, (size_t)pn)) break;
        res->outputTokens++;

        /* Stop-string sur la sortie ACCUMULEE. Marqueur complet en fin -> on
         * tronque + stop (le marqueur ne doit apparaitre ni dans res->out final
         * ni dans le stream). Sinon holdback = octets de queue a retenir. */
        size_t mStart = 0, holdback = 0;
        if (llm_stop_marker_scan(res->out.data ? res->out.data : "", res->out.len,
                                 &mStart, &holdback)) {
            markerStop      = true;
            markerStart     = mStart;
            res->stopReason = OWL_STOP_NATUREL;
            break;
        }

        /* Emet [emitCursor, safe) avec terminé=Faux. safe >= emitCursor toujours :
         * les octets deja emis ont ete prouves hors d'un prefixe partiel. */
        if (hasCallback) {
            size_t safe = res->out.len - holdback;
            if (safe > emitCursor) {
                int er = llm_emit_piece(emit, res->out.data + emitCursor,
                                        safe - emitCursor, false);
                if (er != LLM_EMIT_OK) {
                    if (er == LLM_EMIT_RAISED) res->raised  = true;
                    else                       res->abandon = true;
                    llama_sampler_free(smpl);
                    return;
                }
                emitCursor = safe;
            }
        }

        /* Réinjection du token pour la suite (positions auto-suivies). */
        batch = llama_batch_get_one(&cur, 1);
        if (llama_decode(ctx, batch) < 0) {
            res->decodeErr = true;
            llama_sampler_free(smpl);
            return;
        }

        if (i == maxTokens - 1) reachedMax = true;
    }
    if (reachedMax) res->stopReason = OWL_STOP_MAX_TOKENS;

    llama_sampler_free(smpl);

    /* Marqueur complet detecte : tronque res->out a son debut (jamais dans le
     * contenu rendu ni dans le stream). */
    if (markerStop) {
        res->out.len = markerStart;
        if (res->out.data) res->out.data[markerStart] = '\0';
    }

    /* Fin de flux : emet le reliquat [emitCursor, out.len) avec terminé=Vrai. Le
     * buffer de retention est ainsi TOUJOURS vide a la fin (aucun octet perdu si
     * ce n'etait finalement pas un marqueur). Cas zero-jeton / tout retenu (rare)
     * : appel unique morceau="", terminé=Vrai. Invariant : la concat de tous les
     * `morceau` emis == res->out final (marqueur exclu). */
    if (hasCallback) {
        size_t rem = res->out.len > emitCursor ? res->out.len - emitCursor : 0;
        int er = llm_emit_piece(emit, rem > 0 ? res->out.data + emitCursor : "",
                                rem, true);
        if (er != LLM_EMIT_OK) {
            if (er == LLM_EMIT_RAISED) res->raised  = true;
            else                       res->abandon = true;
            return;
        }
    }

    /* Contrat anti-casse-en-silence (contrainteJSON uniquement). Une grammaire
     * contraint les tokens mais ne force pas la completion : si MaxTokens tombe
     * au milieu d'un objet, la sortie est un JSON tronque — signale, jamais rendu
     * en silence. GBNF brute exclue (completion invalidable). */
    if (res->jsonConstraint) {
        const char* txt = res->out.data ? res->out.data : "";
        if (!llm_output_is_valid_json(txt, (int)res->out.len))
            res->jsonTruncated = true;
    }
}

/* Tokenise `text` (longueur `len`) pour le vocab donne. Alloue le tableau
 * (l'appelant free). *nTok recoit le nombre de jetons (<= 0 = echec). */
static llama_token* llm_tokenize(const struct llama_vocab* vocab,
                                 const char* text, int len, int* nTok) {
    int n_max = len + 8;  /* borne haute : ~1 token/octet + tokens speciaux */
    llama_token* tokens = (llama_token*)malloc((size_t)n_max * sizeof(llama_token));
    if (tokens == NULL) { *nTok = 0; return NULL; }
    *nTok = llama_tokenize(vocab, text, len, tokens, n_max,
                           /*add_special*/ true, /*parse_special*/ false);
    return tokens;
}

/* .Génère(prompt, options) / .Generate : rend le texte genere (chaine). Sur un
 * LLM non charge -> soft-error (+ ErreurDétectée) et chaine vide. A chaque
 * token, si ..SurToken est affecte, rappelle la procedure avec le morceau de
 * texte du token et le drapeau terminé. Arret : EOS/EOG ou ..MaxTokens atteint. */
static Value fn_llm_genere(int argCount, Value* args) {
    if (!IS_LLM(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "arg", "modele", "expected", "LLM",
                              "actual", "type non pris en charge", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    ObjLLM* l = AS_LLM(args[0]);
    if (l->model == NULL || l->ctx == NULL) {
        /* Génération demandee avant tout .Charge reussi : echec recuperable. */
        owl_emit_native_error(E_NATIVE_NOT_FOUND, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "name", "mod\xc3\xa8le charg\xc3\xa9", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "arg", "prompt", "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* Options : instance fournie, sinon defauts implicites. */
    ObjOptionsGeneration defaults;
    ObjOptionsGeneration* o;
    if (argCount >= 3 && IS_OPTIONS_GENERATION(args[2])) {
        o = AS_OPTIONS_GENERATION(args[2]);
    } else {
        llm_options_defaults(&defaults);
        o = &defaults;
    }

    struct llama_context*     ctx   = (struct llama_context*)l->ctx;
    const struct llama_vocab* vocab = (const struct llama_vocab*)l->vocab;
    const char* prompt     = AS_CSTRING(args[1]);
    int         prompt_len = AS_STRING(args[1])->length;

    int          n_tok  = 0;
    llama_token* tokens = llm_tokenize(vocab, prompt, prompt_len, &n_tok);
    if (tokens == NULL || n_tok <= 0) {
        free(tokens);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "native", "LLM.G\xc3\xa9n\xc3\xa8re",
                              "path", "-", "detail", "tokenisation du prompt echouee", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    LlmEmit emit = { .owlCb = o->onToken, .cSink = NULL, .cUser = NULL };
    LlmGenOutcome res;
    llm_generate_core(ctx, vocab, tokens, n_tok, o, /*gbnfOverride*/ NULL, &emit, &res);
    free(tokens);

    if (res.grammarError) {
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "native", "LLM.G\xc3\xa9n\xc3\xa8re",
                              "path", "-", "detail", "grammaire GBNF invalide", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (res.raised) {
        /* Le callback a leve : l'exception se propage (frameSentinel). */
        free(res.out.data);
        return NIL_VAL;
    }
    if (res.decodeErr) {
        free(res.out.data);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "native", "LLM.G\xc3\xa9n\xc3\xa8re",
                              "path", "-", "detail", "echec du decodage llama", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (res.promptTooLong) {
        /* Invite plus longue que la fenetre de contexte : rejetee avant decode
         * (anti-abort ). Erreur recuperable, chaine vide. */
        free(res.out.data);
        char arg[160];
        snprintf(arg, sizeof(arg),
                 "invite : %d jetons > limite de contexte %d",
                 n_tok, res.contextLimit);
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "arg", arg, NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    if (res.jsonTruncated) {
        const char* txt = res.out.data ? res.out.data : "";
        Value partial = OBJ_VAL(obj_copy_string(txt, (int)res.out.len));
        char posbuf[24];
        snprintf(posbuf, sizeof(posbuf), "%d", (int)res.out.len);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "LLM.G\xc3\xa9n\xc3\xa8re",
                              "native", "LLM.G\xc3\xa9n\xc3\xa8re",
                              "position", posbuf,
                              "detail",
                              "sortie JSON tronqu\xc3\xa9""e : MaxTokens atteint "
                              "sans compl\xc3\xa9ter la structure \xe2\x80\x94 "
                              "augmentez MaxTokens", NULL);
        free(res.out.data);
        return partial;
    }

    owl_clear_error();
    Value result = OBJ_VAL(obj_copy_string(res.out.data ? res.out.data : "",
                                           (int)res.out.len));
    free(res.out.data);
    return result;
}

/* Materialise un RéponseLLM neutre (Contenu="", compteurs a zero, raison
 * OWL_STOP_NATUREL) : valeur rendue par le chemin soft-error de .Chat. */
static Value llm_response_neutral(void) {
    return OBJ_VAL(obj_new_response_llm());
}

/* Applique le gabarit de dialogue du modele aux messages et rend l'invite
 * formatee (C-heap, l'appelant free) ou NULL en cas d'echec. *outLen recoit la
 * longueur. Le gabarit du modele est recupere via llama_model_chat_template ;
 * s'il est absent (NULL) ou d'un format non reconnu, on retombe sur le gabarit
 * generique "chatml" (passer tmpl=NULL a llama_chat_apply_template le
 * selectionne). Deux passes : la premiere mesure la taille, la seconde ecrit. */
static char* llm_apply_chat_template(const struct llama_model* model,
                                     const struct llama_chat_message* msgs,
                                     size_t nMsg, int* outLen) {
    const char* tmpl = llama_model_chat_template(model, NULL);

    /* Premiere passe : mesure. add_ass=true pour amorcer la reponse assistant. */
    int need = llama_chat_apply_template(tmpl, msgs, nMsg, /*add_ass*/ true,
                                         NULL, 0);
    if (need < 0 && tmpl != NULL) {
        /* Gabarit du modele non reconnu : fallback generique chatml. */
        tmpl = NULL;
        need = llama_chat_apply_template(tmpl, msgs, nMsg, true, NULL, 0);
    }
    if (need < 0) { *outLen = 0; return NULL; }

    char* buf = (char*)malloc((size_t)need + 1);
    if (buf == NULL) { *outLen = 0; return NULL; }
    int written = llama_chat_apply_template(tmpl, msgs, nMsg, true, buf, need + 1);
    if (written < 0) { free(buf); *outLen = 0; return NULL; }
    if (written > need) written = need;
    buf[written] = '\0';
    *outLen = written;
    return buf;
}

/* .Chat(messages, options) / .Chat : dialogue multi-tours SANS etat. `messages`
 * est un tableau de MessageLLM (historique complet, repasse a chaque appel).
 * Applique le gabarit de dialogue du modele puis genere (moteur partage avec
 * .Génère). Rend un RéponseLLM (Contenu + Raison + compteurs de jetons). Sur un
 * LLM non charge -> soft-error (+ ErreurDétectée) et RéponseLLM neutre. */
static Value fn_llm_chat(int argCount, Value* args) {
    if (!IS_LLM(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.Chat",
                              "arg", "modele", "expected", "LLM",
                              "actual", "type non pris en charge", NULL);
        return llm_response_neutral();
    }
    ObjLLM* l = AS_LLM(args[0]);
    if (l->model == NULL || l->ctx == NULL) {
        /* Dialogue demande avant tout .Charge reussi : echec recuperable. */
        owl_emit_native_error(E_NATIVE_NOT_FOUND, "LLM.Chat",
                              "name", "mod\xc3\xa8le charg\xc3\xa9", NULL);
        return llm_response_neutral();
    }
    if (!IS_ARRAY(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LLM.Chat",
                              "arg", "messages", "expected", "tableau de MessageLLM",
                              "actual", "type non pris en charge", NULL);
        return llm_response_neutral();
    }

    /* Options : instance fournie, sinon defauts implicites. */
    ObjOptionsGeneration defaults;
    ObjOptionsGeneration* o;
    if (argCount >= 3 && IS_OPTIONS_GENERATION(args[2])) {
        o = AS_OPTIONS_GENERATION(args[2]);
    } else {
        llm_options_defaults(&defaults);
        o = &defaults;
    }

    ObjArray* arr  = AS_ARRAY(args[1]);
    int       nMsg = arr->elements.count;

    /* Vue llama_chat_message : role/content pointent dans les ObjString des
     * MessageLLM (rootes via args[1] sur la pile VM ; aucune alloc OWL entre
     * ici et l'application du gabarit). Un element non-MessageLLM est ignore. */
    struct llama_chat_message* msgs =
        (struct llama_chat_message*)malloc((size_t)(nMsg > 0 ? nMsg : 1)
                                           * sizeof(struct llama_chat_message));
    if (msgs == NULL) {
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Chat",
                              "native", "LLM.Chat",
                              "path", "-", "detail", "memoire insuffisante", NULL);
        return llm_response_neutral();
    }
    int used = 0;
    for (int i = 0; i < nMsg; i++) {
        Value e = arr->elements.values[i];
        if (!IS_MESSAGE_LLM(e)) continue;
        ObjMessageLLM* m = AS_MESSAGE_LLM(e);
        msgs[used].role    = IS_STRING(m->role) ? AS_CSTRING(m->role) : "";
        msgs[used].content = IS_STRING(m->content) ? AS_CSTRING(m->content) : "";
        used++;
    }

    const struct llama_model* model = (const struct llama_model*)l->model;
    int   promptLen = 0;
    char* prompt    = llm_apply_chat_template(model, msgs, (size_t)used,
                                              &promptLen);
    free(msgs);
    if (prompt == NULL || promptLen <= 0) {
        free(prompt);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Chat",
                              "native", "LLM.Chat",
                              "path", "-", "detail",
                              "application du gabarit de dialogue echouee", NULL);
        return llm_response_neutral();
    }

    struct llama_context*     ctx   = (struct llama_context*)l->ctx;
    const struct llama_vocab* vocab = (const struct llama_vocab*)l->vocab;

    int          n_tok  = 0;
    llama_token* tokens = llm_tokenize(vocab, prompt, promptLen, &n_tok);
    free(prompt);
    if (tokens == NULL || n_tok <= 0) {
        free(tokens);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Chat",
                              "native", "LLM.Chat",
                              "path", "-", "detail",
                              "tokenisation de l'invite echouee", NULL);
        return llm_response_neutral();
    }

    LlmEmit emit = { .owlCb = o->onToken, .cSink = NULL, .cUser = NULL };
    LlmGenOutcome res;
    llm_generate_core(ctx, vocab, tokens, n_tok, o, /*gbnfOverride*/ NULL, &emit, &res);
    free(tokens);

    if (res.grammarError) {
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Chat",
                              "native", "LLM.Chat",
                              "path", "-", "detail", "grammaire GBNF invalide", NULL);
        return llm_response_neutral();
    }
    if (res.raised) {
        free(res.out.data);
        return NIL_VAL;   /* exception du callback -> propagation (frameSentinel) */
    }
    if (res.decodeErr) {
        free(res.out.data);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "LLM.Chat",
                              "native", "LLM.Chat",
                              "path", "-", "detail", "echec du decodage llama", NULL);
        return llm_response_neutral();
    }
    if (res.promptTooLong) {
        /* Invite plus longue que la fenetre de contexte : rejetee avant decode
         * (anti-abort ). Erreur recuperable, RéponseLLM neutre. */
        free(res.out.data);
        char arg[160];
        snprintf(arg, sizeof(arg),
                 "invite : %d jetons > limite de contexte %d",
                 n_tok, res.contextLimit);
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "LLM.Chat",
                              "arg", arg, NULL);
        return llm_response_neutral();
    }

    /* RéponseLLM : rooter l'instance FRAICHE avant d'allouer la chaine Contenu
     * (obj_copy_string peut declencher un GC — ). */
    ObjResponseLLM* rep = obj_new_response_llm();
    vm_push(OBJ_VAL(rep));
    const char* txt = res.out.data ? res.out.data : "";
    rep->content      = OBJ_VAL(obj_copy_string(txt, (int)res.out.len));
    rep->reason       = res.stopReason;
    rep->inputTokens  = n_tok;
    rep->outputTokens = res.outputTokens;

    if (res.jsonTruncated) {
        /* contrainteJSON tronquee : ErreurDétectée + Raison = stopMaxTokens
         * (le plafond a coupe la structure). Le Contenu partiel reste
         * inspectable dans la RéponseLLM. */
        rep->reason = OWL_STOP_MAX_TOKENS;
        char posbuf[24];
        snprintf(posbuf, sizeof(posbuf), "%d", (int)res.out.len);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "LLM.Chat",
                              "native", "LLM.Chat",
                              "position", posbuf,
                              "detail",
                              "sortie JSON tronqu\xc3\xa9""e : MaxTokens atteint "
                              "sans compl\xc3\xa9ter la structure \xe2\x80\x94 "
                              "augmentez MaxTokens", NULL);
    } else {
        owl_clear_error();
    }

    free(res.out.data);
    vm_pop();
    return OBJ_VAL(rep);
}

/* ================================================================
 * Registry : types + methodes + factories
 * ================================================================ */

/* LLM : nom identique FR/EN (acronyme), aucun alias. OptionsGénération : forme
 * anglaise resolue via l'alias (comme AIModel/TrainingOptions). */
static const char* const ALIAS_llm[] = { NULL };
static const char* const ALIAS_options_gen[] = { "GenerationOptions", NULL };
static const char* const ALIAS_message_llm[] = { "LLMMessage", NULL };
static const char* const ALIAS_reponse_llm[] = { "LLMResponse", NULL };

/* Contraintes de sortie suggerees pour OptionsGénération..ContrainteDeSortie.
 * Les valeurs sont les noms des constantes contrainte* (chacune une chaine
 * marqueur langue-agnostique : "" pour libre, "json" pour la grammaire JSON
 * integree). Liee au champ ..ContrainteDeSortie via allowedBase pour
 * l'autocomplete LSP ; toute grammaire GBNF libre reste acceptee en saisie
 * directe (mirror de httpRequête..ContentType / E_typeMime). */
static const char* const E_contrainteSortie_values[] = {
    "contrainteLibre", "contrainteJSON", NULL
};
static const OwlEnum E_contrainteSortie = {
    .name="contrainteSortie", .nameAscii="contrainteSortie",
    .nameEN="outputConstraint", .category="Intelligence artificielle",
    .descFR="Contraintes de sortie possibles pour la generation. Liste "
            "indicative — toute grammaire GBNF libre reste egalement acceptee.",
    .descEN="Possible output constraints for generation. Hint list — any "
            "free-form GBNF grammar is also accepted.",
    .values=E_contrainteSortie_values, .combinable=false
};

/* Rôles suggeres pour MessageLLM..Rôle. Enum OUVERTE : les quatre constantes
 * rôle* sont proposees a l'autocomplete (allowedBase du champ), mais toute
 * chaine libre reste acceptee (rôle custom, ex "developer" — mirror de
 * E_contrainteSortie / E_typeMime). Les valeurs sont les noms des constantes. */
static const char* const E_roleLLM_values[] = {
    "r\xc3\xb4leSyst\xc3\xa8me", "r\xc3\xb4leUtilisateur",
    "r\xc3\xb4leAssistant", "r\xc3\xb4leOutil", NULL
};
static const OwlEnum E_roleLLM = {
    .name="r\xc3\xb4leLLM", .nameAscii="roleLLM",
    .nameEN="llmRole", .category="Intelligence artificielle",
    .descFR="Rôles de conversation possibles pour un MessageLLM. Liste "
            "indicative — un rôle en chaine libre reste egalement accepte.",
    .descEN="Possible conversation roles for an LLMMessage. Hint list — a "
            "free-form role string is also accepted.",
    .values=E_roleLLM_values, .combinable=false
};

/* Raisons d'arret d'une generation Chat (RéponseLLM..Raison). Enum STRICTE :
 * sortie moteur, l'utilisateur ne fait que lire. Valeurs sentinel-zonees
 * (OWL_STOP_*, sentinels.h) — voir constantes stop* plus bas. */
static const char* const E_raisonStop_values[] = {
    "stopNaturel", "stopMaxTokens", NULL
};
static const OwlEnum E_raisonStop = {
    .name="raisonStop", .nameAscii="raisonStop",
    .nameEN="stopReason", .category="Intelligence artificielle",
    .descFR="Raisons d'arret d'une generation : stopNaturel (jeton de fin emis) "
            "ou stopMaxTokens (plafond de jetons atteint, reponse coupee).",
    .descEN="Generation stop reasons: stopNatural (end token emitted) or "
            "stopMaxTokens (token cap reached, response truncated).",
    .values=E_raisonStop_values, .combinable=false
};

static const OwlEnum* const LLM_ENUMS[] = {
    &E_contrainteSortie, &E_roleLLM, &E_raisonStop
};

static const OwlField FIELDS_OptionsGeneration[] = {
    {"Temperature", "Temp\xc3\xa9rature", "r\xc3\xa9""el", "0.8",
     "temperature d'echantillonnage : 0 desactive le hasard (choix systematique du token le plus probable, generation reproductible), plus la valeur monte plus la sortie est variee",
     "sampling temperature: 0 turns off randomness (always picks the most likely token, reproducible generation), higher values yield more varied output",
     .nameEN="Temperature"},
    {"TopK", NULL, "entier", "40",
     "nombre de tokens candidats les plus probables gardes avant echantillonnage (0 = pas de coupe)",
     "number of most likely candidate tokens kept before sampling (0 = no cut)",
     .nameEN="TopK"},
    {"TopP", NULL, "r\xc3\xa9""el", "0.95",
     "seuil de masse de probabilite cumulee (echantillonnage noyau) : garde les tokens les plus probables jusqu'a atteindre cette proportion",
     "cumulative probability mass threshold (nucleus sampling): keeps the most likely tokens up to this proportion",
     .nameEN="TopP"},
    {"MaxTokens", NULL, "entier", "512",
     "nombre maximum de tokens generes avant arret (l'arret survient aussi sur un token de fin de sequence)",
     "maximum number of generated tokens before stopping (also stops on an end-of-sequence token)",
     .nameEN="MaxTokens"},
    {"Graine", NULL, "entier", "0",
     "graine du generateur aleatoire d'echantillonnage : une meme graine rend une sortie reproductible pour une temperature non nulle (sans effet en mode greedy)",
     "seed of the sampling random generator: the same seed makes output reproducible for a non-zero temperature (no effect in greedy mode)",
     .nameEN="Seed"},
    {"SurToken", "SurToken", "proc\xc3\xa9""dure", "",
     "procedure rappelee a chaque token genere, recoit le morceau de texte du token (chaine) et un booleen termine (Vrai sur le dernier morceau)",
     "procedure called for each generated token, receives the token's text piece (string) and a finished boolean (True on the last piece)",
     .nameEN="OnToken", .callbackSig="cha\xc3\xaene,bool\xc3\xa9""en"},
    {"ContrainteDeSortie", NULL, "cha\xc3\xaene", "contrainteLibre",
     "contrainte exercee sur la sortie. contrainteLibre (defaut) = aucune contrainte ; contrainteJSON = JSON structurellement valide via une grammaire integree ; toute autre chaine = grammaire GBNF libre a laquelle la sortie doit se conformer (schemas precis, formats stricts)",
     "constraint applied to the output. constraintFree (default) = no constraint; constraintJSON = structurally valid JSON via a built-in grammar; any other string = free-form GBNF grammar the output must conform to (precise schemas, strict formats)",
     .allowedBase=&E_contrainteSortie, .nameEN="OutputConstraint"},
    {0},
};

/* MessageLLM : un message de conversation (entree de .Chat). ..Rôle enum
 * ouverte (rôle* ou chaine libre), ..Contenu texte. */
static const OwlField FIELDS_MessageLLM[] = {
    {"Role", "R\xc3\xb4le", "cha\xc3\xaene", "\"\"",
     "rôle de l'auteur du message : rôleSystème, rôleUtilisateur, rôleAssistant, rôleOutil ou une chaine libre (rôle custom)",
     "author role of the message: roleSystem, roleUser, roleAssistant, roleTool or a free-form string (custom role)",
     .allowedBase=&E_roleLLM, .nameEN="Role"},
    {"Contenu", NULL, "cha\xc3\xaene", "\"\"",
     "texte du message",
     "text of the message",
     .nameEN="Content"},
    {0},
};

/* RéponseLLM : sortie de .Chat (lecture seule). Contenu + raison d'arret +
 * compteurs de jetons. */
static const OwlField FIELDS_ReponseLLM[] = {
    {"Contenu", NULL, "cha\xc3\xaene", "\"\"",
     "texte de reponse de l'assistant",
     "assistant's response text",
     .nameEN="Content"},
    {"Raison", NULL, "entier", "stopNaturel",
     "raison d'arret : stopNaturel (jeton de fin emis) ou stopMaxTokens (plafond de jetons atteint, reponse coupee)",
     "stop reason: stopNatural (end token emitted) or stopMaxTokens (token cap reached, response truncated)",
     .allowedBase=&E_raisonStop, .nameEN="Reason"},
    {"JetonsEntree", "JetonsEntr\xc3\xa9""e", "entier", "0",
     "nombre de jetons consommes par l'invite (utile pour budgeter la fenetre de contexte)",
     "number of tokens consumed by the prompt (useful to budget the context window)",
     .nameEN="InputTokens"},
    {"JetonsSortie", NULL, "entier", "0",
     "nombre de jetons generes dans la reponse",
     "number of tokens generated in the response",
     .nameEN="OutputTokens"},
    {0},
};

static const OwlTypeDef LLM_TYPES[] = {
    { .canonical      = "LLM",
      .canonicalAscii = "LLM",
      .aliases        = ALIAS_llm,
      .category       = "Intelligence artificielle",
      .descFR         = "Modele de langage charge depuis un fichier GGUF, capable "
                        "de generer du texte. `.Charge(chemin)` lit le fichier et "
                        "prepare l'inference (rend Vrai en cas de succes, Faux avec "
                        "ErreurDétectée si le chemin est absent ou le GGUF "
                        "illisible). `.Génère(prompt, options)` produit du texte a "
                        "partir d'une amorce et d'une feuille OptionsGénération. "
                        "Une temperature a 0 rend la generation reproductible "
                        "(choix systematique du token le plus probable). Voir aussi "
                        "IAModèle pour construire et entrainer un reseau de neurones.",
      .descEN         = "Language model loaded from a GGUF file, able to generate "
                        "text. `.Load(path)` reads the file and prepares inference "
                        "(returns True on success, False with ErrorDetected if the "
                        "path is missing or the GGUF is unreadable). "
                        "`.Generate(prompt, options)` produces text from a prompt "
                        "and a GenerationOptions leaf. A temperature of 0 makes "
                        "generation reproducible (always picks the most likely "
                        "token). See also AIModel to build and train a neural "
                        "network.",
      .fields         = NULL,   /* methodes single-dot (.Charge/.Génère), pas de props */
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "LLM",
      .genre          = OWL_GENRE_M,          /* un LLM */
      .defaultFactoryNative = "__LLMNouveau" },

    { .canonical      = "OptionsG\xc3\xa9n\xc3\xa9ration",  /* OptionsGénération */
      .canonicalAscii = "OptionsGeneration",
      .aliases        = ALIAS_options_gen,
      .category       = "Intelligence artificielle",
      .descFR         = "Reglages d'un appel de generation, passee a "
                        "`LLM.Génère(prompt, options)`. Proprietes reglables par "
                        "double point : `..Température` (0 = generation "
                        "reproductible), `..TopK`, `..TopP`, `..MaxTokens` (nombre "
                        "maximum de tokens generes) et `..Graine`. Un callback de "
                        "flux `..SurToken` est rappele a chaque token avec son "
                        "morceau de texte. Sortie contrainte : `..ContrainteDeSortie` "
                        "vaut contrainteLibre (defaut), contrainteJSON (JSON valide "
                        "garanti) ou une grammaire GBNF libre. Defauts : temperature "
                        "0.8, TopK 40, TopP 0.95, 512 tokens maximum, graine "
                        "aleatoire, aucune contrainte de sortie. Voir aussi IAModèle "
                        "et OptionsEntraînement.",
      .descEN         = "Settings for one generation call, passed to "
                        "`LLM.Generate(prompt, options)`. Properties set via the "
                        "double dot: `..Temperature` (0 = reproducible generation), "
                        "`..TopK`, `..TopP`, `..MaxTokens` (maximum number of "
                        "generated tokens) and `..Seed`. A streaming callback "
                        "`..OnToken` is called for each token with its text piece. "
                        "Constrained output: `..OutputConstraint` is constraintFree "
                        "(default), constraintJSON (guaranteed valid JSON) or a "
                        "free-form GBNF grammar. Defaults: temperature 0.8, TopK 40, "
                        "TopP 0.95, 512 tokens maximum, random seed, no output "
                        "constraint. See also AIModel and TrainingOptions.",
      .fields         = FIELDS_OptionsGeneration,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "GenerationOptions",
      .genre          = OWL_GENRE_F,          /* une OptionsGénération */
      .defaultFactoryNative = "__OptionsGenerationNouveau" },

    { .canonical      = "MessageLLM",
      .canonicalAscii = "MessageLLM",
      .aliases        = ALIAS_message_llm,
      .category       = "Intelligence artificielle",
      .descFR         = "Un message d'une conversation, passe dans un tableau a "
                        "`LLM.Chat(messages, options)`. Construit au litteral "
                        "d'accolade : `m est un MessageLLM = {Rôle = "
                        "rôleUtilisateur, Contenu = \"...\"}`. Proprietes au "
                        "double point : `..Rôle` (rôleSystème, rôleUtilisateur, "
                        "rôleAssistant, rôleOutil ou une chaine libre) et "
                        "`..Contenu` (le texte). Voir aussi RéponseLLM (sortie "
                        "de .Chat).",
      .descEN         = "One message of a conversation, passed in an array to "
                        "`LLM.Chat(messages, options)`. Built with a brace "
                        "literal: `m is an LLMMessage = {Role = roleUser, "
                        "Content = \"...\"}`. Double-dot properties: `..Role` "
                        "(roleSystem, roleUser, roleAssistant, roleTool or a "
                        "free-form string) and `..Content` (the text). See also "
                        "LLMResponse (output of .Chat).",
      .fields         = FIELDS_MessageLLM,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "LLMMessage",
      .genre          = OWL_GENRE_M,          /* un MessageLLM */
      .defaultFactoryNative = "__MessageLLMNouveau" },

    { .canonical      = "R\xc3\xa9ponseLLM",  /* RéponseLLM */
      .canonicalAscii = "ReponseLLM",
      .aliases        = ALIAS_reponse_llm,
      .category       = "Intelligence artificielle",
      .descFR         = "Reponse rendue par `LLM.Chat(messages, options)`. "
                        "Proprietes en lecture (double point) : `..Contenu` (le "
                        "texte de reponse de l'assistant), `..Raison` "
                        "(stopNaturel si le modele a emis son jeton de fin, "
                        "stopMaxTokens si le plafond ..MaxTokens a coupe la "
                        "reponse), `..JetonsEntrée` (jetons consommes par "
                        "l'invite) et `..JetonsSortie` (jetons generes). Voir "
                        "aussi MessageLLM (entree de .Chat).",
      .descEN         = "Response returned by `LLM.Chat(messages, options)`. "
                        "Read properties (double dot): `..Content` (the "
                        "assistant's response text), `..Reason` (stopNatural if "
                        "the model emitted its end token, stopMaxTokens if the "
                        "..MaxTokens cap truncated the response), `..InputTokens` "
                        "(tokens consumed by the prompt) and `..OutputTokens` "
                        "(generated tokens). See also LLMMessage (input of "
                        ".Chat).",
      .fields         = FIELDS_ReponseLLM,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "LLMResponse",
      .genre          = OWL_GENRE_F,          /* une RéponseLLM */
      .defaultFactoryNative = "__ReponseLLMNouveau" },
};

void stdlib_llm_init(void) {
    owl_registry_register_types(LLM_TYPES,
        (int)(sizeof(LLM_TYPES) / sizeof(LLM_TYPES[0])));
    owl_registry_register_enums(LLM_ENUMS,
        (int)(sizeof(LLM_ENUMS) / sizeof(LLM_ENUMS[0])));

    /* Constantes de ..ContrainteDeSortie (chaines marqueurs). Valeurs
     * langue-agnostiques et IMPERATIVES : "" (contrainteLibre) et "json"
     * (contrainteJSON) sont interpretees telles quelles par llm_build_sampler.
     * Comme les typeMime*, ce sont des STRINGS : la Value ne peut pas vivre
     * dans un static initializer, on la construit ici. register_consts
     * auto-enregistre le nom canon ET l'alias nameEN dans vm.globals + expose
     * au LSP/doc. */
    {
        static OwlConstDef contrConsts[2];
        Value vLibre = OBJ_VAL(obj_copy_string("", 0));
        vm_push(vLibre);                 /* root pendant la construction */
        Value vJSON  = OBJ_VAL(obj_copy_string("json", 4));
        vm_push(vJSON);
        contrConsts[0] = (OwlConstDef){
            .name="contrainteLibre", .category="Intelligence artificielle",
            .value=vLibre,
            .descFR="aucune contrainte de sortie (valeur par defaut de ..ContrainteDeSortie)",
            .descEN="no output constraint (default value of ..OutputConstraint)",
            .nameEN="constraintFree" };
        contrConsts[1] = (OwlConstDef){
            .name="contrainteJSON", .category="Intelligence artificielle",
            .value=vJSON,
            .descFR="contraint la sortie a un JSON structurellement valide (grammaire integree)",
            .descEN="constrains the output to structurally valid JSON (built-in grammar)",
            .nameEN="constraintJSON" };
        owl_registry_register_consts(contrConsts, 2);
        vm_pop(); vm_pop();
    }

    /* Constantes de rôle (MessageLLM..Rôle). Valeurs = rôles standard passes au
     * gabarit de dialogue (chaines "system"/"user"/"assistant"/"tool"). Comme
     * les contrainte*, ce sont des STRINGS construites au runtime, rootees le
     * temps de la registration. Enum ouverte : une chaine libre reste acceptee. */
    {
        static OwlConstDef roleConsts[4];
        Value vSys  = OBJ_VAL(obj_copy_string("system", 6));    vm_push(vSys);
        Value vUser = OBJ_VAL(obj_copy_string("user", 4));      vm_push(vUser);
        Value vAsst = OBJ_VAL(obj_copy_string("assistant", 9)); vm_push(vAsst);
        Value vTool = OBJ_VAL(obj_copy_string("tool", 4));      vm_push(vTool);
        roleConsts[0] = (OwlConstDef){
            .name="r\xc3\xb4leSyst\xc3\xa8me", .category="Intelligence artificielle",
            .value=vSys,
            .descFR="rôle « systeme » : instructions de cadrage de l'assistant",
            .descEN="\"system\" role: framing instructions for the assistant",
            .nameEN="roleSystem" };
        roleConsts[1] = (OwlConstDef){
            .name="r\xc3\xb4leUtilisateur", .category="Intelligence artificielle",
            .value=vUser,
            .descFR="rôle « utilisateur » : le message adresse a l'assistant",
            .descEN="\"user\" role: the message addressed to the assistant",
            .nameEN="roleUser" };
        roleConsts[2] = (OwlConstDef){
            .name="r\xc3\xb4leAssistant", .category="Intelligence artificielle",
            .value=vAsst,
            .descFR="rôle « assistant » : une reponse du modele dans l'historique",
            .descEN="\"assistant\" role: a model reply in the history",
            .nameEN="roleAssistant" };
        roleConsts[3] = (OwlConstDef){
            .name="r\xc3\xb4leOutil", .category="Intelligence artificielle",
            .value=vTool,
            .descFR="rôle « outil » : le resultat d'un appel d'outil dans l'historique",
            .descEN="\"tool\" role: the result of a tool call in the history",
            .nameEN="roleTool" };
        owl_registry_register_consts(roleConsts, 4);
        vm_pop(); vm_pop(); vm_pop(); vm_pop();
    }

    /* Constantes de raison d'arret (RéponseLLM..Raison). Enum STRICTE :
     * entiers sentinel-zonés (OWL_STOP_*), Value dans un static initializer. */
    {
        static const OwlConstDef stopConsts[] = {
            { .name="stopNaturel", .category="Intelligence artificielle",
              .value=INTEGER_VAL(OWL_STOP_NATUREL),
              .descFR="le modele a emis son jeton de fin (reponse complete)",
              .descEN="the model emitted its end token (complete response)",
              .nameEN="stopNatural" },
            { .name="stopMaxTokens", .category="Intelligence artificielle",
              .value=INTEGER_VAL(OWL_STOP_MAX_TOKENS),
              .descFR="le plafond ..MaxTokens a ete atteint : reponse coupee",
              .descEN="the ..MaxTokens cap was reached: response truncated",
              .nameEN="stopMaxTokens" },
        };
        owl_registry_register_consts(stopConsts,
            (int)(sizeof(stopConsts) / sizeof(stopConsts[0])));
    }

    /* Le finalizer GC des handles llama passe par ce callback (object.c ne
     * connait pas llama). */
    obj_llm_set_backend_free(llm_backend_free);

    /* Factories de declaration (`mon est un LLM`, `opts est un
     * OptionsGénération`, `m est un MessageLLM`, `rep est un RéponseLLM`).
     * Internes : emises par le compilateur. */
    owl_define_native("__LLMNouveau", fn_llm_nouveau, 0);
    owl_define_native("__OptionsGenerationNouveau",
                      fn_options_generation_nouveau, 0);
    owl_define_native("__MessageLLMNouveau", fn_message_llm_nouveau, 0);
    owl_define_native("__ReponseLLMNouveau", fn_reponse_llm_nouveau, 0);

    /* Methodes fluentes. .Charge : receveur + chemin (arite 2). .Génère :
     * receveur + prompt + options (arite 3). .Chat : receveur + messages +
     * options (arite 3). Bilingue (.Chat identique FR/EN). */
    owl_register_method(METHOD_TYPE_LLM, "Charge", fn_llm_charge, 2);
    owl_register_method(METHOD_TYPE_LLM, "Load", fn_llm_charge, 2);
    owl_register_method(METHOD_TYPE_LLM, "G\xc3\xa9n\xc3\xa8re", fn_llm_genere, 3);
    owl_register_method(METHOD_TYPE_LLM, "Generate", fn_llm_genere, 3);
    owl_register_method(METHOD_TYPE_LLM, "Chat", fn_llm_chat, 3);
}

/* ================================================================
 * API C VM-free (seam serve-llm, stdlib_llm.h)
 *
 * Surface plain-C au-dessus du moteur : charge des handles bruts (sans VM/GC,
 * decision G), applique le gabarit + tokenise (calque de llm_apply_chat_template
 * + llm_tokenize), et decode en driveant un OwlLlmTokenSink via le moteur
 * partage llm_generate_core (LlmEmit{.cSink}). Aucun symbole ici n'est
 * enregistre au registry ni au catalogue LSP.
 * ================================================================ */

int owl_llm_charge_handles(const char* path, uint32_t ctx_override,
                           OwlLlmHandles* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (path == NULL || out == NULL) return -1;

    llm_backend_ensure();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;  /* CPU-only, coherent avec le vendoring ggml-cpu */
    struct llama_model* model = llama_model_load_from_file(path, mp);
    if (model == NULL) return -1;

    struct llama_context_params cp = llama_context_default_params();
    /* ctx_override == 0 : defaut model-aware, plafonne pour borner la RAM du
     * cache KV (comme fn_llm_charge). ctx_override > 0 (--ctx-size, ) :
     * fenetre forcee, l'appelant assume le tradeoff RAM ; clampee a
     * OWL_LLM_N_CTX_MAX pour eviter une alloc KV delirante. */
    uint32_t n_ctx;
    if (ctx_override > 0) {
        n_ctx = ctx_override > OWL_LLM_N_CTX_MAX ? OWL_LLM_N_CTX_MAX : ctx_override;
    } else {
        int32_t n_ctx_train = llama_model_n_ctx_train(model);
        n_ctx = (n_ctx_train > 0 && n_ctx_train < OWL_LLM_N_CTX_CAP)
                    ? (uint32_t)n_ctx_train
                    : OWL_LLM_N_CTX_CAP;
    }
    cp.n_ctx   = n_ctx;
    cp.n_batch = n_ctx;
    struct llama_context* ctx = llama_init_from_model(model, cp);
    if (ctx == NULL) {
        llama_model_free(model);
        return -1;
    }

    out->model = model;
    out->ctx   = ctx;
    out->vocab = (void*)llama_model_get_vocab(model);
    return 0;
}

void owl_llm_libere(OwlLlmHandles* h) {
    if (h == NULL) return;
    if (h->ctx != NULL)   llama_free((struct llama_context*)h->ctx);
    if (h->model != NULL) llama_model_free((struct llama_model*)h->model);
    h->ctx   = NULL;
    h->model = NULL;
    h->vocab = NULL;  /* emprunte au modele : rien a liberer, juste oublier */
}

int owl_llm_chat_prepare(const OwlLlmHandles* h,
                         const OwlLlmChatMsg* msgs, size_t nb_msgs,
                         OwlLlmPrepared* out) {
    if (out) { out->tokens = NULL; out->nb_tokens = 0; }
    if (h == NULL || h->model == NULL || h->vocab == NULL || out == NULL)
        return -1;

    const struct llama_model* model = (const struct llama_model*)h->model;
    const struct llama_vocab* vocab = (const struct llama_vocab*)h->vocab;

    /* Vue llama_chat_message : role/content sont empruntes aux tampons de
     * l'appelant (OwlLlmChatMsg). */
    size_t n = nb_msgs > 0 ? nb_msgs : 1;
    struct llama_chat_message* lmsgs =
        (struct llama_chat_message*)malloc(n * sizeof(struct llama_chat_message));
    if (lmsgs == NULL) return -1;
    size_t used = 0;
    for (size_t i = 0; i < nb_msgs; i++) {
        lmsgs[used].role    = msgs[i].role    ? msgs[i].role    : "";
        lmsgs[used].content = msgs[i].content ? msgs[i].content : "";
        used++;
    }

    int   promptLen = 0;
    char* prompt    = llm_apply_chat_template(model, lmsgs, used, &promptLen);
    free(lmsgs);
    if (prompt == NULL || promptLen <= 0) { free(prompt); return -1; }

    int          n_tok  = 0;
    llama_token* tokens = llm_tokenize(vocab, prompt, promptLen, &n_tok);
    free(prompt);
    if (tokens == NULL || n_tok <= 0) { free(tokens); return -1; }

    out->tokens    = (int32_t*)tokens;   /* llama_token == int32_t */
    out->nb_tokens = n_tok;
    return 0;
}

void owl_llm_prepared_libere(OwlLlmPrepared* p) {
    if (p == NULL) return;
    free(p->tokens);
    p->tokens    = NULL;
    p->nb_tokens = 0;
}

int owl_llm_chat_stream(const OwlLlmHandles* h,
                        const OwlLlmPrepared* prompt,
                        const OwlLlmChatParams* params,
                        OwlLlmTokenSink sink, void* user,
                        OwlLlmChatIssue* issue) {
    if (issue) memset(issue, 0, sizeof(*issue));
    if (h == NULL || h->ctx == NULL || h->vocab == NULL || prompt == NULL ||
        prompt->tokens == NULL || prompt->nb_tokens <= 0 || sink == NULL) {
        if (issue) issue->erreur = 1;
        return -1;
    }

    struct llama_context*     ctx   = (struct llama_context*)h->ctx;
    const struct llama_vocab* vocab = (const struct llama_vocab*)h->vocab;

    /* Reglages plain-C -> feuille OptionsGénération sur la PILE C (non geree
     * GC, jamais tracee — comme les defauts implicites de fn_llm_genere). Le
     * seam serve-llm ignore params.messages ici : le prompt est deja tokenise. */
    ObjOptionsGeneration o;
    llm_options_defaults(&o);
    if (params != NULL) {
        o.temperature = params->temperature;
        if (params->max_tokens > 0) o.maxTokens = params->max_tokens;
    }
    o.onToken    = NIL_VAL;
    o.constraint = NIL_VAL;

    LlmEmit emit = { .owlCb = NIL_VAL, .cSink = sink, .cUser = user };

    /* Seam  : la GBNF d'outil (ou NULL) voyage par params->grammar,
     * hors de o->constraint pour ne pas toucher le chemin OWL. */
    const char* gbnfOverride = (params != NULL) ? params->grammar : NULL;

    LlmGenOutcome res;
    llm_generate_core(ctx, vocab, (llama_token*)prompt->tokens, prompt->nb_tokens,
                      &o, gbnfOverride, &emit, &res);

    if (issue) {
        issue->stop_reason     = res.stopReason;
        issue->input_tokens    = prompt->nb_tokens;
        issue->output_tokens   = res.outputTokens;
        issue->prompt_too_long = res.promptTooLong ? 1 : 0;
        issue->context_limit   = res.contextLimit;
        /* prompt_too_long est un echec en plus de son drapeau distinct : les
         * branches d'erreur existantes de serve (rc/issue.erreur) continuent de
         * fonctionner, la distinction 413 se fait sur prompt_too_long. */
        issue->erreur          = (res.decodeErr || res.grammarError || res.promptTooLong) ? 1 : 0;
    }
    /* Le texte accumule ne sert pas cote C (le sink l'a deja recu). */
    free(res.out.data);

    /* L'abandon (sink parti) est un arret GRACIEUX, pas une erreur. */
    return (res.decodeErr || res.grammarError || res.promptTooLong) ? -1 : 0;
}

/* ================================================================
 * Convertisseur schema JSON -> GBNF (tool-calling, ).
 *
 * Voie B (maison bornee) : traduit les schemas de function OpenAI d'une requete
 * `tools` en une GBNF qui FORCE le modele a emettre un appel d'outil valide.
 * Regle d'or : lit l'arbre ObjJson (thread VM) mais N'ALLOUE AUCUN Obj — il
 * n'emet que des octets C (SB). Aucun GC ne tourne entre parse et conversion.
 * Sous-ensemble couvert : object/string/number/integer/boolean/array + enum +
 * required/optionnel + objet imbrique. Tout le reste ($ref, allOf/anyOf/oneOf,
 * not, pattern, format, bornes, type tableau, type absent sans enum) -> refus
 * PROPRE : NULL + err (ok=0, reason, where), jamais de GBNF partielle.
 * ================================================================ */

/* Prelude GBNF jumeau du prelude JSON (OWL_LLM_JSON_GBNF), borne aux valeurs
 * feuilles d'un schema d'outil, avec integer/boolean en plus. Racine posee par
 * `root` (emise a part). */
static const char OWL_TOOL_GBNF_PRELUDE[] =
    "string  ::= \"\\\"\" ( [^\"\\\\] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) )* \"\\\"\" ws\n"
    "number  ::= (\"-\"? ([0-9] | [1-9] [0-9]*)) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)? ws\n"
    "integer ::= \"-\"? ([0-9] | [1-9] [0-9]*) ws\n"
    "boolean ::= (\"true\" | \"false\") ws\n"
    "ws      ::= [ \\t\\n]*\n";

/* Profondeur de recursion max du walk (garde anti-schema pathologique). */
#define OWL_TOOL_SCHEMA_MAX_DEPTH 32
#define OWL_TOOL_RULE_NAME_CAP    64

/* Compteur de regles generees (r_N / obj_N), unicite garantie. */
typedef struct { int counter; } RuleNamer;

static void tool_err(OwlToolGbnfError* err, const char* reason, const char* where) {
    if (err == NULL) return;
    err->ok = 0;
    snprintf(err->reason, sizeof(err->reason), "%s", reason);
    snprintf(err->where,  sizeof(err->where),  "%s", where ? where : "-");
}

/* Champ d'un objet JSON par cle (scan direct, sans allouer). Jumeau de
 * sj_field de serve_llm.c, local pour ne pas trainer de dependance croisee. */
static ObjJson* tj_field(ObjJson* obj, const char* key) {
    if (obj == NULL || obj->jtype != JSON_T_OBJECT) return NULL;
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        ObjString* k = obj->as.objectVal.keys[i];
        if (k != NULL && strcmp(k->chars, key) == 0)
            return obj->as.objectVal.values[i];
    }
    return NULL;
}

/* Porte UNIQUE d'echappement : emet un litteral GBLF ("...") a partir d'octets
 * bruts, en echappant ce que le parseur GBNF de llama exige. */
static void gbnf_quote(SB* out, const char* s, size_t n) {
    sb_putn(out, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
            case '"':  sb_puts(out, "\\\""); break;
            case '\\': sb_puts(out, "\\\\"); break;
            case '\n': sb_puts(out, "\\n");  break;
            case '\r': sb_puts(out, "\\r");  break;
            case '\t': sb_puts(out, "\\t");  break;
            default:   sb_putn(out, &c, 1);  break;
        }
    }
    sb_putn(out, "\"", 1);
}

/* Emet un litteral GBNF verrouille sur une chaine JSON precise : le contenu
 * `"<s>"` (guillemets JSON compris) devient le litteral GBNF `"\"<s>\""`. Sert
 * aux cles d'objet et aux noms d'outils (le modele ne peut ni les renommer ni
 * les inventer). */
static void gbnf_json_str_literal(SB* out, const char* s, size_t n) {
    SB tmp; sb_init(&tmp);
    sb_putn(&tmp, "\"", 1);
    sb_putn(&tmp, s, n);
    sb_putn(&tmp, "\"", 1);
    gbnf_quote(out, tmp.p ? tmp.p : "", tmp.len);
    sb_free(&tmp);
}

/* Mots-cles (A) STRUCTUREL : ils changent la FORME de ce qui est valide et
 * n'exposent aucun type de base emettable ($ref = reference non resolue ;
 * allOf/anyOf/oneOf/not = composition/union/negation de schemas). Leur presence
 * a n'importe quel niveau declenche un refus propre (NULL -> 400) : degrader
 * produirait un appel STRUCTURELLEMENT faux (, amende  §2).
 *
 * A l'oppose, les mots-cles (B) CONTRAINTE (pattern, format, minimum, maximum,
 * minLength, maxLength, minItems, maxItems, et les cousins exclusiveMinimum,
 * exclusiveMaximum, multipleOf) ne figurent PAS ici : ils raffinent un type de
 * base qui reste emettable. schema_emit lit `type`/`enum` et IGNORE les cles
 * inconnues -> la contrainte disparait et le type de base non borne est emis.
 * Une borne ignoree = valeur non contrainte, structurellement VALIDE (pas un
 * appel malforme). C'est le declencheur OpenCode : ses outils integres
 * (read/write/bash) portent un `minimum` -> un 400 tuait l'agent au demarrage. */
static const char* const TOOL_SCHEMA_FORBIDDEN[] = {
    "$ref", "allOf", "anyOf", "oneOf", "not"
};

static int schema_emit(SB* rules, RuleNamer* nm, ObjJson* node, int depth,
                       char* out_name, size_t out_cap, OwlToolGbnfError* err);

/* Nom d'une regle de corps d'objet : `b-<oid>-<i>-<suffix>`. SEPARATEUR TIRET
 * impose : le parseur GBNF de llama exclut '_' des noms de regle
 * (is_word_char = [A-Za-z0-9-]) — un nom en underscore casse le parse. */
static void emit_body_rule_name(SB* b, int oid, int i, const char* suffix) {
    sb_puts(b, "b-"); sb_puti(b, oid); sb_puts(b, "-");
    sb_puti(b, i);    sb_puts(b, "-"); sb_puts(b, suffix);
}

/* Objet : properties (ordre fixe) + required. Emet les regles de valeur de
 * chaque propriete, puis la machinerie de virgules body_i_first/body_i_more
 * (1er membre sans virgule, suivants avec virgule de tete ; optionnel =
 * alternation qui saute le membre). Renvoie le nom de la regle d'objet. */
static int emit_object(SB* rules, RuleNamer* nm, ObjJson* node, int depth,
                       char* out_name, size_t out_cap, OwlToolGbnfError* err) {
    ObjJson* props = tj_field(node, "properties");
    ObjJson* req   = tj_field(node, "required");

    int oid = nm->counter++;
    snprintf(out_name, out_cap, "obj-%d", oid);

    int k = (props != NULL && props->jtype == JSON_T_OBJECT)
                ? props->as.objectVal.count : 0;

    /* Noms de regle de valeur + drapeau required, par propriete (ordre du
     * schema). Les regles de valeur sont emises AVANT les body rules. */
    char (*valNames)[OWL_TOOL_RULE_NAME_CAP] = NULL;
    int*  reqFlags = NULL;
    if (k > 0) {
        valNames = malloc((size_t)k * OWL_TOOL_RULE_NAME_CAP);
        reqFlags = malloc((size_t)k * sizeof(int));
        if (valNames == NULL || reqFlags == NULL) {
            free(valNames); free(reqFlags);
            tool_err(err, "memoire insuffisante", "object");
            return 0;
        }
    }

    for (int i = 0; i < k; i++) {
        ObjJson* pv = props->as.objectVal.values[i];
        if (!schema_emit(rules, nm, pv, depth + 1,
                         valNames[i], OWL_TOOL_RULE_NAME_CAP, err)) {
            free(valNames); free(reqFlags);
            return 0;
        }
        /* required ? */
        int isReq = 0;
        ObjString* key = props->as.objectVal.keys[i];
        if (req != NULL && req->jtype == JSON_T_ARRAY && key != NULL) {
            for (int r = 0; r < req->as.arrayVal.count; r++) {
                ObjJson* rn = req->as.arrayVal.items[r];
                if (rn != NULL && rn->jtype == JSON_T_STRING && rn->as.strVal != NULL &&
                    strcmp(rn->as.strVal->chars, key->chars) == 0) { isReq = 1; break; }
            }
        }
        reqFlags[i] = isReq;
    }

    /* Regles terminales epsilon (index k+1). "" -> regle epsilon valide
     * (sequence vide + END), acceptee par le parseur GBNF vendore. */
    emit_body_rule_name(rules, oid, k + 1, "first"); sb_puts(rules, " ::= \"\"\n");
    emit_body_rule_name(rules, oid, k + 1, "more");  sb_puts(rules, " ::= \"\"\n");

    for (int i = k; i >= 1; i--) {
        ObjString* key = props->as.objectVal.keys[i - 1];
        const char* valName = valNames[i - 1];
        int isReq = reqFlags[i - 1];

        /* Fragment membre : "\"cle\"" ws ":" ws <regle_valeur> (deja suivie de ws). */
        SB member; sb_init(&member);
        gbnf_json_str_literal(&member, key ? key->chars : "", key ? (size_t)key->length : 0);
        sb_puts(&member, " ws \":\" ws ");
        sb_puts(&member, valName);

        /* b-<oid>-i-first */
        emit_body_rule_name(rules, oid, i, "first"); sb_puts(rules, " ::= ");
        sb_putn(rules, member.p, member.len);
        sb_puts(rules, " ");
        emit_body_rule_name(rules, oid, i + 1, "more");
        if (!isReq) {
            sb_puts(rules, " | ");
            emit_body_rule_name(rules, oid, i + 1, "first");
        }
        sb_puts(rules, "\n");

        /* b-<oid>-i-more (membre precede d'une virgule) */
        emit_body_rule_name(rules, oid, i, "more"); sb_puts(rules, " ::= \",\" ws ");
        sb_putn(rules, member.p, member.len);
        sb_puts(rules, " ");
        emit_body_rule_name(rules, oid, i + 1, "more");
        if (!isReq) {
            sb_puts(rules, " | ");
            emit_body_rule_name(rules, oid, i + 1, "more");
        }
        sb_puts(rules, "\n");

        sb_free(&member);
    }

    /* Regle d'objet : "{" ws body-1-first "}" ws. */
    sb_puts(rules, "obj-"); sb_puti(rules, oid);
    sb_puts(rules, " ::= \"{\" ws ");
    emit_body_rule_name(rules, oid, 1, "first");
    sb_puts(rules, " \"}\" ws\n");

    free(valNames); free(reqFlags);
    return 1;
}

/* Tableau : "[" ws ( item ("," ws item)* )? "]" ws. */
static int emit_array(SB* rules, RuleNamer* nm, ObjJson* node, int depth,
                      char* out_name, size_t out_cap, OwlToolGbnfError* err) {
    ObjJson* items = tj_field(node, "items");
    if (items == NULL) { tool_err(err, "array sans items", "items"); return 0; }

    char itemName[OWL_TOOL_RULE_NAME_CAP];
    if (!schema_emit(rules, nm, items, depth + 1, itemName, sizeof(itemName), err))
        return 0;

    int rid = nm->counter++;
    snprintf(out_name, out_cap, "r-%d", rid);
    sb_puts(rules, out_name);
    sb_puts(rules, " ::= \"[\" ws ( ");
    sb_puts(rules, itemName);
    sb_puts(rules, " ( \",\" ws ");
    sb_puts(rules, itemName);
    sb_puts(rules, " )* )? \"]\" ws\n");
    return 1;
}

/* Enum : alternation de litteraux verrouilles, groupee pour que le `ws` final
 * s'applique a TOUTES les branches (rN ::= ( "\"a\"" | "\"b\"" ) ws). */
static int emit_enum(SB* rules, RuleNamer* nm, ObjJson* en,
                     char* out_name, size_t out_cap, OwlToolGbnfError* err) {
    if (en->jtype != JSON_T_ARRAY || en->as.arrayVal.count < 1) {
        tool_err(err, "enum vide ou non tableau", "enum");
        return 0;
    }
    int rid = nm->counter++;
    snprintf(out_name, out_cap, "r-%d", rid);
    sb_puts(rules, out_name);
    sb_puts(rules, " ::= ( ");
    for (int i = 0; i < en->as.arrayVal.count; i++) {
        ObjJson* v = en->as.arrayVal.items[i];
        if (i > 0) sb_puts(rules, " | ");
        if (v == NULL) { tool_err(err, "valeur d'enum nulle", "enum"); return 0; }
        if (v->jtype == JSON_T_STRING) {
            gbnf_json_str_literal(rules, v->as.strVal ? v->as.strVal->chars : "",
                                  v->as.strVal ? (size_t)v->as.strVal->length : 0);
        } else if (v->jtype == JSON_T_INT) {
            char t[32]; int n = snprintf(t, sizeof(t), "%lld", (long long)v->as.intVal);
            gbnf_quote(rules, t, n > 0 ? (size_t)n : 0);
        } else if (v->jtype == JSON_T_BOOL) {
            gbnf_quote(rules, v->as.boolVal ? "true" : "false", v->as.boolVal ? 4 : 5);
        } else {
            tool_err(err, "valeur d'enum non supportee", "enum");
            return 0;
        }
    }
    sb_puts(rules, " ) ws\n");
    return 1;
}

/* Walk d'un noeud de schema. Rend (dans out_name) le NOM de la regle a
 * referencer pour matcher une valeur conforme. Primitifs -> regle prelude
 * partagee ; enum/array/object -> regle generee. 0 = schema non couvert. */
static int schema_emit(SB* rules, RuleNamer* nm, ObjJson* node, int depth,
                       char* out_name, size_t out_cap, OwlToolGbnfError* err) {
    if (depth > OWL_TOOL_SCHEMA_MAX_DEPTH) {
        tool_err(err, "schema trop profond", "depth");
        return 0;
    }
    if (node == NULL || node->jtype != JSON_T_OBJECT) {
        tool_err(err, "schema de propriete invalide", "schema");
        return 0;
    }

    /* Refus des constructions hors sous-ensemble (a tout niveau). */
    for (size_t f = 0; f < sizeof(TOOL_SCHEMA_FORBIDDEN) / sizeof(*TOOL_SCHEMA_FORBIDDEN); f++) {
        if (tj_field(node, TOOL_SCHEMA_FORBIDDEN[f]) != NULL) {
            tool_err(err, "construction de schema non supportee", TOOL_SCHEMA_FORBIDDEN[f]);
            return 0;
        }
    }

    /* enum a la priorite (le `type` accompagnant est ignore). */
    ObjJson* en = tj_field(node, "enum");
    if (en != NULL) return emit_enum(rules, nm, en, out_name, out_cap, err);

    ObjJson* typeNode = tj_field(node, "type");
    if (typeNode == NULL) {
        tool_err(err, "type absent sans enum", "type");
        return 0;
    }
    if (typeNode->jtype != JSON_T_STRING || typeNode->as.strVal == NULL) {
        tool_err(err, "type non scalaire (tableau de types ?)", "type");
        return 0;
    }
    const char* t = typeNode->as.strVal->chars;

    if (strcmp(t, "string")  == 0) { snprintf(out_name, out_cap, "string");  return 1; }
    if (strcmp(t, "number")  == 0) { snprintf(out_name, out_cap, "number");  return 1; }
    if (strcmp(t, "integer") == 0) { snprintf(out_name, out_cap, "integer"); return 1; }
    if (strcmp(t, "boolean") == 0) { snprintf(out_name, out_cap, "boolean"); return 1; }
    if (strcmp(t, "array")   == 0) return emit_array(rules, nm, node, depth, out_name, out_cap, err);
    if (strcmp(t, "object")  == 0) return emit_object(rules, nm, node, depth, out_name, out_cap, err);

    tool_err(err, "type de schema non supporte", t);
    return 0;
}

/* Emet la regle d'arguments d'un outil (schema de `parameters`, ou objet vide
 * si absent). Renvoie le nom de la regle dans argsName. */
static int emit_tool_args(SB* rules, RuleNamer* nm, ObjJson* params,
                          char* argsName, size_t cap, OwlToolGbnfError* err) {
    if (params != NULL && params->jtype == JSON_T_OBJECT && tj_field(params, "type") != NULL) {
        return schema_emit(rules, nm, params, 0, argsName, cap, err);
    }
    /* Pas de parametres declares : arguments = objet vide {}. */
    int oid = nm->counter++;
    snprintf(argsName, cap, "obj-%d", oid);
    sb_puts(rules, argsName);
    sb_puts(rules, " ::= \"{\" ws \"}\" ws\n");
    return 1;
}

char* owl_tool_schema_to_gbnf(struct ObjJson* tools, int tool_choice_mode,
                              const char* forced_name, OwlToolGbnfError* err) {
    OwlToolGbnfError local;
    if (err == NULL) err = &local;
    err->ok = 1; err->reason[0] = '\0'; err->where[0] = '\0';

    if (tools == NULL || tools->jtype != JSON_T_ARRAY || tools->as.arrayVal.count < 1) {
        tool_err(err, "tableau `tools` absent ou vide", "tools");
        return NULL;
    }

    SB rules; sb_init(&rules);   /* regles de valeur + regles d'appel */
    SB calls; sb_init(&calls);   /* noms de regles d'appel (pour le routeur root) */
    RuleNamer nm = { 0 };
    int ncalls = 0;

    for (int i = 0; i < tools->as.arrayVal.count; i++) {
        ObjJson* tool = tools->as.arrayVal.items[i];
        ObjJson* func = tj_field(tool, "function");
        ObjJson* nameNode = tj_field(func, "name");
        if (nameNode == NULL || nameNode->jtype != JSON_T_STRING || nameNode->as.strVal == NULL) {
            tool_err(err, "outil sans function.name (chaine)", "function.name");
            goto fail;
        }
        const char* tname = nameNode->as.strVal->chars;
        size_t      tnlen = (size_t)nameNode->as.strVal->length;

        /* NAMED : ne retenir que l'outil impose. */
        if (tool_choice_mode == OWL_TC_NAMED) {
            if (forced_name == NULL || strcmp(forced_name, tname) != 0) continue;
        }

        char argsName[OWL_TOOL_RULE_NAME_CAP];
        if (!emit_tool_args(&rules, &nm, tj_field(func, "parameters"),
                            argsName, sizeof(argsName), err))
            goto fail;

        /* Regle d'appel : { "name":"<tool>", "arguments":<args> }. Nom d'outil
         * verrouille au litteral -> le modele choisit une branche mais ne peut
         * pas fabriquer un nom. */
        sb_puts(&rules, "call-"); sb_puti(&rules, ncalls);
        sb_puts(&rules, " ::= \"{\" ws ");
        gbnf_json_str_literal(&rules, "name", 4);
        sb_puts(&rules, " ws \":\" ws ");
        gbnf_json_str_literal(&rules, tname, tnlen);
        sb_puts(&rules, " ws \",\" ws ");
        gbnf_json_str_literal(&rules, "arguments", 9);
        sb_puts(&rules, " ws \":\" ws ");
        sb_puts(&rules, argsName);
        sb_puts(&rules, " \"}\" ws\n");

        if (ncalls > 0) sb_puts(&calls, " | ");
        sb_puts(&calls, "call-"); sb_puti(&calls, ncalls);
        ncalls++;
    }

    if (ncalls == 0) {
        /* NAMED avec un forced_name introuvable, ou aucun outil valide. */
        tool_err(err, "outil impose introuvable dans `tools`",
                 forced_name ? forced_name : "tool_choice");
        goto fail;
    }

    /* Assemblage : prelude + routeur root (+ freetext si AUTO) + regles. */
    SB out; sb_init(&out);
    sb_puts(&out, OWL_TOOL_GBNF_PRELUDE);
    sb_puts(&out, "root ::= ws ( ");
    sb_putn(&out, calls.p, calls.len);
    if (tool_choice_mode == OWL_TC_AUTO) sb_puts(&out, " | freetext");
    sb_puts(&out, " )\n");
    /* AUTO (decision B) : 1er char != '{' -> texte libre ; sinon branche appel
     * verrouillee. `.` = tout caractere, supporte par le parseur GBNF vendore. */
    if (tool_choice_mode == OWL_TC_AUTO)
        sb_puts(&out, "freetext ::= [^{] .*\n");
    sb_putn(&out, rules.p, rules.len);

    sb_free(&rules);
    sb_free(&calls);
    err->ok = 1;
    return out.p;   /* C-heap, l'appelant free */

fail:
    sb_free(&rules);
    sb_free(&calls);
    return NULL;
}
