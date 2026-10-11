// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_llm_h
#define owl_stdlib_llm_h

/* ================================================================
 * API C VM-free du moteur d'inference (seam serve-llm, ).
 *
 * Cet en-tete expose une surface plain-C, INDEPENDANTE de la VM et de
 * llama.h, au-dessus du moteur d'inference de stdlib_llm.c. Elle sert le
 * handler C `serve-llm` (API compatible OpenAI) : charger un modele une
 * fois, appliquer le gabarit de dialogue + tokeniser, puis decoder en
 * driveant un callback C par token.
 *
 * La surface OWL publique (`LLM`, `..SurToken`, `.Chat`) reste INTACTE :
 * ce seam est un decouplage moteur <-> consommateur, pas une nouvelle
 * surface de langage. Aucun symbole ici n'est enregistre au registry ni
 * au catalogue LSP.
 *
 * Decisions gelees (spike ) :
 *   B : sink `int(piece,len,termine,user)` — retour != 0 = consommateur
 *       parti (le moteur s'arrete).
 *   C : parametres plain-C (`OwlLlmChatParams`), inference VM-independante.
 *   G : chargement modele en handles BRUTS (thread principal, sans VM/GC).
 * ================================================================ */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sink C token-par-token (decision B). Rappele une fois par token avec le
 * morceau de texte (octets copies, jamais un ObjString*) et le drapeau
 * `termine` (!= 0 sur le dernier morceau). Un retour != 0 signale que le
 * consommateur est parti : le moteur doit interrompre la generation. */
typedef int (*OwlLlmTokenSink)(const char* piece, size_t len,
                               int termine, void* user);

/* Handles bruts d'un modele charge (decision G). void* pour garder cet
 * en-tete libre de llama.h — le mapping vers les types llama vit dans
 * stdlib_llm.c uniquement. */
typedef struct {
    void* model;   /* struct llama_model*        */
    void* ctx;     /* struct llama_context*      */
    void* vocab;   /* const struct llama_vocab*  */
} OwlLlmHandles;

/* Un message de conversation (vue plain-C, empruntee : role/content
 * pointent dans des tampons possedes par l'appelant). */
typedef struct {
    const char* role;
    const char* content;
} OwlLlmChatMsg;

/* Reglages d'un appel de chat (decision C, plain-C). */
typedef struct {
    const OwlLlmChatMsg* messages;
    size_t               nb_messages;
    double               temperature;
    int                  max_tokens;
    /* GBNF optionnelle contraignant la sortie (, tool-calling). NULL =
     * comportement actuel inchange. Voyage par ce champ plutot que la signature
     * du seam pour que les faux generateurs de test restent inchanges. */
    const char*          grammar;
} OwlLlmChatParams;

/* Invite tokenisee (gabarit de dialogue applique). Sortie de
 * owl_llm_chat_prepare ; l'appelant libere via owl_llm_prepared_libere. */
typedef struct {
    int32_t* tokens;    /* tableau de llama_token (int32) alloue C-heap */
    int      nb_tokens;
} OwlLlmPrepared;

/* Issue d'une generation : raison d'arret + compteurs + drapeau d'erreur.
 * `stop_reason` prend OWL_STOP_NATUREL / OWL_STOP_MAX_TOKENS (sentinels.h). */
typedef struct {
    int stop_reason;
    int input_tokens;
    int output_tokens;
    int erreur;         /* 0 = ok ; != 0 = echec (prepare ou mi-flux) */
    /* Invite tokenisee plus longue que la fenetre de contexte / le batch du
     * modele : le moteur REJETTE avant tout decode (pas de GGML_ASSERT/abort).
     * `prompt_too_long` != 0 accompagne toujours `erreur` != 0 ; `context_limit`
     * = borne effective (n_batch == n_ctx) et `input_tokens` = jetons d'invite.
     * Les faux generateurs de test memset la struct -> defaut 0 (safe). */
    int prompt_too_long;
    int context_limit;
} OwlLlmChatIssue;

/* Borne haute de securite de la fenetre de contexte (override --ctx-size,
 * ). Le cache KV croit lineairement avec n_ctx ; au-dela de cette
 * borne l'allocation devient delirante (plusieurs Go). Un override superieur
 * est clampe a cette valeur. 131072 = 128k, plafond des plus grands modeles
 * a contexte etendu. */
#define OWL_LLM_N_CTX_MAX 131072u

/* Charge un GGUF en handles bruts (sans VM/GC, decision G). 0 = succes,
 * != 0 = echec (chemin absent, GGUF illisible, contexte impossible).
 * `ctx_override` : 0 = defaut model-aware plafonne (comportement
 * inchange) ; > 0 = fenetre de contexte forcee a cette valeur (clampee a
 * OWL_LLM_N_CTX_MAX). L'appelant assume le tradeoff RAM du cache KV. */
int  owl_llm_charge_handles(const char* path, uint32_t ctx_override,
                            OwlLlmHandles* out);

/* Libere les handles (idempotent, safe sur des handles jamais charges). */
void owl_llm_libere(OwlLlmHandles* h);

/* Applique le gabarit de dialogue du modele aux messages puis tokenise
 * l'invite. 0 = succes (out->tokens/nb_tokens poses), != 0 = echec. */
int  owl_llm_chat_prepare(const OwlLlmHandles* h,
                          const OwlLlmChatMsg* msgs, size_t nb_msgs,
                          OwlLlmPrepared* out);
void owl_llm_prepared_libere(OwlLlmPrepared* p);

/* Decode l'invite preparee et echantillonne token par token, driveant
 * `sink` a chaque token (decision B). Pose `issue` (raison/compteurs).
 * 0 = succes, != 0 = echec (issue->erreur pose). */
int  owl_llm_chat_stream(const OwlLlmHandles* h,
                         const OwlLlmPrepared* prompt,
                         const OwlLlmChatParams* params,
                         OwlLlmTokenSink sink, void* user,
                         OwlLlmChatIssue* issue);

/* ================================================================
 * Convertisseur schema JSON -> GBNF (tool-calling, ).
 *
 * Voie B (convertisseur maison borne, pas de vendoring) : traduit les schemas
 * de function OpenAI d'une requete `tools` en une GBNF qui FORCE le modele a
 * emettre un appel d'outil valide. Co-localise avec le prelude JSON GBNF dans
 * stdlib_llm.c. Surface plain-C : `struct ObjJson` reste forward-declaree pour
 * ne pas trainer object.h/vm.h dans ce seam.
 * ================================================================ */

/* Mode tool_choice, resolu par serve_llm.c AVANT l'appel au convertisseur :
 *   AUTO     -> grammaire = appel OU texte libre (branche `freetext`) ;
 *   REQUIRED -> grammaire force un appel parmi les outils ;
 *   NAMED    -> grammaire force l'outil `forced_name`.
 * `none`/absent ne passe pas ici (pas de grammaire du tout). */
enum {
    OWL_TC_AUTO = 0,
    OWL_TC_REQUIRED,
    OWL_TC_NAMED
};

/* Diagnostic du convertisseur. Un schema hors du sous-ensemble couvert
 * (`$ref`, `allOf`/`anyOf`/`oneOf`, `pattern`, bornes, `type` absent...) n'est
 * JAMAIS traduit en GBNF partielle : le convertisseur rend NULL, pose `ok=0`,
 * `reason` (motif lisible) et `where` (noeud fautif), et serve_llm.c mappe en
 * 400 explicite. `ok=1` accompagne une GBNF valide. */
typedef struct OwlToolGbnfError {
    int  ok;
    char reason[192];
    char where[160];
} OwlToolGbnfError;

struct ObjJson;   /* forward : convertisseur plain-C, pas d'object.h ici */

/* Convertit le tableau `tools` (schemas de function OpenAI, deja parse en
 * ObjJson JSON_T_ARRAY) en GBNF. `tool_choice_mode` = OWL_TC_* ; `forced_name`
 * est le nom d'outil impose en OWL_TC_NAMED (ignore sinon, peut etre NULL).
 * Rend une chaine C-heap NUL-terminee (l'appelant la `free`) racinee sur `root`,
 * ou NULL si un schema n'est pas couvert (`err` pose : ok=0, reason, where). */
char* owl_tool_schema_to_gbnf(struct ObjJson* tools, int tool_choice_mode,
                              const char* forced_name, OwlToolGbnfError* err);

/* Stop-string sur la sortie ACCUMULEE. Scanne la FIN de buf[0..len)
 * pour un marqueur de fin de tour connu emis en TEXTE (mono-token, multi-tokens,
 * ou coupe entre deux tokens) — que les gardes EOG/control ne voient pas quand
 * les metadonnees GGUF sont muettes.
 *   - buf se termine par un marqueur complet : *outMarkerStart = index de debut
 *     du marqueur, retourne 1 (tronquer a cet index + STOP) ;
 *   - sinon : retourne 0 et *outHoldback = nb d'octets de queue formant le plus
 *     long prefixe PARTIEL d'un marqueur (0 si aucun) — ces octets doivent etre
 *     RETENUS en streaming (ils pourraient completer un marqueur au prochain
 *     token). Ne scanne que la fenetre de queue (<= longueur max marqueur) : O(1)
 *     par token, pas O(n^2). Expose (non-static) pour test model-free. */
int llm_stop_marker_scan(const char* buf, size_t len,
                         size_t* outMarkerStart, size_t* outHoldback);

#endif /* owl_stdlib_llm_h */
