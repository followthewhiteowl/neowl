// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_serve_llm_h
#define owl_serve_llm_h

/* ================================================================
 * serve-llm — API LLM locale compatible OpenAI.
 *
 * `owl serve-llm --model <x.gguf> [--port 11434] [--host 127.0.0.1]`
 * expose le modele local en endpoints OpenAI-compat, drop-in ollama :
 *   - POST /v1/chat/completions  (SSE token-par-token si stream:true,
 *                                 JSON single-shot si stream:false) ;
 *   - GET  /v1/models            (liste a un modele).
 *
 * Compose des briques deja decidees : le socle streaming mode C
 * (, ) fournit le transport (flux chunked-out) via le
 * seam producteur (OwlHttpProducteur) ; le moteur d'inference
 * (stdlib_llm.h) fournit le chargement + la generation par sink C. Rien
 * de nouveau cote transport (decision A : stream:false = chunked
 * single-shot, aucune touche a http_exec.c / http_server.c).
 * ================================================================ */

#include "http_exec.h"      /* OwlHttpProducteur, OwlHttpProdCtx */
#include "http_server.h"    /* OwlHttpRequete */
#include "stdlib_llm.h"     /* OwlLlmHandles, OwlLlmChatParams, ... */

/* Contexte producteur serve-llm (branche sur OwlHttpExecConfig.producteur_user).
 * Porte les handles du modele, l'id echoue au client, et surtout le SEAM de
 * generation INDIRECT : c'est la cle de testabilite model-free. En production
 * `generate` pointe sur le wrapper reel (prepare+stream du moteur) ; en test
 * un faux generateur emet une suite de tokens fixee, ce qui exerce TOUT le
 * framing / routage / mapping OpenAI sans charger le moindre modele. */
typedef struct ServeLlmCtx ServeLlmCtx;

/* Seam de generation (indirection de test). Encapsule prepare+stream :
 * drive `sink` une fois par token, pose `issue` (raison d'arret, compteurs,
 * drapeau d'erreur). 0 = succes, != 0 = echec (issue->erreur pose ; erreur
 * avant tout token = prepare echoue, apres >= 1 token = erreur mi-flux). */
typedef int (*ServeLlmGenerate)(ServeLlmCtx* ctx,
                                const OwlLlmChatParams* params,
                                OwlLlmTokenSink sink, void* sink_user,
                                OwlLlmChatIssue* issue);

struct ServeLlmCtx {
    OwlLlmHandles    handles;    /* modele charge (thread principal, decision G) */
    const char*      model_id;   /* id echoue dans /v1/models et les reponses */
    ServeLlmGenerate generate;   /* seam indirect (wrapper reel ou faux en test) */
};

/* Producteur mode C (seam ), execute dans le thread VM de
 * l'executeur. Route sur req.methode/req.chemin, parse le JSON de la requete
 * et mappe vers SSE (stream:true) ou JSON single-shot (stream:false).
 * Signature OwlHttpProducteur ; `user` = ServeLlmCtx*. Expose pour le test. */
void serve_llm_producteur(const OwlHttpRequete* req, OwlHttpProdCtx* ctx,
                          void* user);

/* Commande CLI `owl serve-llm` : parse les options, charge le modele (thread
 * principal, sans VM), monte le serveur persistant (listener + pool +
 * executeur(producteur) + arret propre SIGINT/TERM). Rend le code de sortie
 * processus. Branche dans main.c AVANT le vm_init global (a cote de `mcp`). */
int  owl_serve_llm_main(int argc, char** argv);

/* Convertit un delai d'attente en file (secondes, cote CLI --request-timeout)
 * en millisecondes pour OwlHttpExecConfig.plafond_attente_ms. sec > 0 ->
 * sec * 1000 (borne anti-overflow a INT_MAX) ; sec <= 0 -> -1 (attente
 * infinie, cf. http_exec.c). Fonction pure, exposee pour le test model-free. */
int  serve_llm_parse_timeout_sec_to_ms(int sec);

/* Parse strict de --ctx-size : strtol strict, refus des valeurs
 * non numeriques, vides, <= 0 ou en overflow ; clamp a la borne haute
 * OWL_LLM_N_CTX_MAX. 0 + *out (valeur >= 1) si valide ; != 0 + *out = 0
 * sinon. Fonction pure, exposee pour le test model-free. */
int  serve_llm_parse_ctx_size(const char* s, uint32_t* out);

#endif /* owl_serve_llm_h */
