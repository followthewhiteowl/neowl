// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_lsp_server_h
#define owl_lsp_server_h

#include "lsp_json.h"
#include <stdbool.h>
#include <stddef.h>

/* ================================================================
 * LSP SERVER -- core protocol handler
 *
 * The LSP server reads JSON-RPC messages from stdin and writes
 * responses to stdout. Logging goes to stderr.
 *
 * Architecture:
 *   lsp_main.c     -- entry point, starts the server loop
 *   lsp_server.c   -- JSON-RPC transport, message dispatch
 *   lsp_handlers.c -- handler for each LSP method
 *   lsp_json.c     -- minimal JSON parser/serializer
 *
 * Shared with owl.exe (via #include):
 *   ../src/scanner.h/c   -- lexer (for tokenization, diagnostics)
 *   ../src/prescan.h/c   -- symbol discovery (for go-to-def, completion)
 *   ../src/project.h/c   -- multi-file project discovery
 * ================================================================ */

/* Server state */
typedef struct {
    bool initialized;
    bool shutdown_requested;
    char rootUri[1024];
    char rootPath[1024];

    /* Open documents (simple buffer per URI) */
    struct {
        char uri[512];
        char* content;
        int contentLen;
        int version;
    } documents[256];
    int documentCount;

    /* Path to owl executable (for --check), resolved at init */
    char owlExePath[512];

    /* URIs we have previously published diagnostics to (for clearing stale entries) */
    char diagnosedUris[64][1024];
    int diagnosedUriCount;

    /* Langue preferee pour les propositions de completion (cf cahier des
     * charges §4 : LSP unilingue paramétrable). "fr" par defaut, "en" si
     * le client envoie initializationOptions.language ou si la config
     * VSCode 'owl.language' = 'en'. Le serveur comprend toujours les deux
     * langues — seules les propositions sont filtrees. */
    char language[8];
} LspServer;

extern LspServer lspServer;

/* Main server loop -- reads stdin, dispatches, writes stdout */
void lsp_run(void);

/* Send a JSON-RPC response */
void lsp_send_response(int64_t id, JsonValue* result);
void lsp_send_error(int64_t id, int code, const char* message);
void lsp_send_notification(const char* method, JsonValue* params);

/* Logging (goes to stderr, never stdout) */
void lsp_log(const char* fmt, ...);

/* ================================================================
 * LSP HANDLERS -- called by the dispatch loop
 * ================================================================ */

/* Lifecycle */
JsonValue* handle_initialize(JsonValue* params);
void handle_initialized(JsonValue* params);
void handle_shutdown(int64_t id);

/* Document sync */
void handle_did_open(JsonValue* params);
void handle_did_change(JsonValue* params);
void handle_did_close(JsonValue* params);
void handle_did_save(JsonValue* params);

/* Language features */
JsonValue* handle_completion(JsonValue* params);
JsonValue* handle_hover(JsonValue* params);
JsonValue* handle_definition(JsonValue* params);
JsonValue* handle_references(JsonValue* params);  /*  Lot 1 */
JsonValue* handle_rename(JsonValue* params);          /*  Lot 2a — stub */
JsonValue* handle_prepare_rename(JsonValue* params);  /*  Lot 2a — stub */
JsonValue* handle_signature_help(JsonValue* params);
JsonValue* handle_semantic_tokens(JsonValue* params);
JsonValue* handle_formatting(JsonValue* params);

/*  -- documentSymbol + foldingRange (stubs dans lsp_document_structure.c) */
JsonValue* handle_document_symbol(JsonValue* params);
JsonValue* handle_folding_range(JsonValue* params);

/* Diagnostics (push from server) */
void publish_diagnostics(const char* uri);

/*  Lot 1 — le check compilateur (`owl --check`) part en arrière-plan :
 * le LSP est mono-thread et un spawn bloquant fige la boucle d'événements
 * (~115 ms à chaud, ~5 s à froid). Les diagnostics sont publiés à l'arrivée du
 * verdict, par lsp_check_pump.
 *
 * Un seul check en vol : une demande arrivée pendant un check est coalescée
 * (mémorisée puis relancée), et le verdict en cours est jeté s'il décrit un
 * contenu déjà périmé. */
void publish_compiler_diagnostics_async(const char* uri);

/* Récolte non bloquante du check en vol + publication. No-op sans check en
 * cours. À appeler depuis la boucle d'événements, y compris au repos. */
void lsp_check_pump(void);

/* 1 tant qu'un check est en vol (la boucle doit alors ticker sur
 * lsp_check_pump au lieu de s'endormir indéfiniment sur stdin). */
int lsp_check_is_pending(void);

/* Variante SYNCHRONE : lance le check et attend sa publication. Réservée aux
 * appelants dépourvus de boucle d'événements (harness de test in-process). */
void publish_compiler_diagnostics(const char* uri);

/*  — cache per-URI des diagnostics --check (couche donnees).
 * publish_compiler_diagnostics peuple le cache (par URI, fichiers cleared
 * inclus a count 0) ; publish_diagnostics fusionne le cache avec les erreurs
 * lexicales fraiches au lieu de l'ecraser. Empeche la navigation (didOpen)
 * d'effacer les erreurs semantiques du dernier save.
 *
 * Les 3 fonctions ci-dessous sont exposees pour le harness de test
 * (tests/lsp/lsp_test_main.c) qui ne peut pas intercepter les notifications
 * stdout : il verifie l'etat du cache, pas l'emission. */
#define LSP_DIAG_CACHE_API_IMPLEMENTED 1
int  lsp_get_compiler_diag_count(const char* uri);    /* count, -1 si URI inconnu */
void lsp_populate_compiler_diag_cache(const char* uri, int count); /* test only */
void lsp_clear_compiler_diag_cache(const char* uri);  /* count -> 0, garde l'entree */

/*  — etendue de la plage d'un diagnostic. lsp_diag_word_end_char expose
 * la logique pure (buffer + line/col -> end.character UTF-16, -1 si repli) ;
 * lsp_get_compiler_diag_end_char lit le end.character stocke au cache. */
#define LSP_DIAG_WORD_END_IMPLEMENTED 1
int  lsp_diag_word_end_char(const char* content, int line1based, int col1based);
int  lsp_get_compiler_diag_end_char(const char* uri, int index);

/*  — garde scheme : publish_compiler_diagnostics court-circuite sur
 * tout URI dont le scheme n'est pas file:// (ex owl-bloc://, doc virtuel sans
 * fichier disque). Le spy ci-dessous expose l'etat de la garde au harness de
 * test (1 = la garde a fire -> early-return, pas de spawn ni publication). */
#define LSP_DIAG_SCHEME_GUARD_IMPLEMENTED 1
int  lsp_diag_scheme_was_rejected(void);  /* test only : 1 si garde a fire, 0 sinon */
void lsp_diag_scheme_reset(void);          /* test only : remet la garde a 0 */

/*  — garde-fou no-clobber du timeout de check : sur LSP_RUN_TIMEOUT,
 * apply_check_result ne republie RIEN (pas de clearing des diagnostics actifs
 * ni voisins), juste un signal Info throttle. Hooks test-only ci-dessous pour
 * verrouiller ce comportement sans attendre le vrai plancher de 15 s :
 *   - lsp_test_set_timeout_override_ms(ms) force le timeout adaptatif du
 *     prochain check spawn (0 = desactive, comportement production inchange).
 *   - lsp_check_timeout_branch_count()/_reset() spy le nombre de passages
 *     dans le chemin LSP_RUN_TIMEOUT d'apply_check_result (independant du
 *     throttle de notification), pour prouver qu'un test a bien exerce CE
 *     chemin. */
void lsp_test_set_timeout_override_ms(int ms);   /* test only */
int  lsp_check_timeout_branch_count(void);        /* test only */
void lsp_check_timeout_branch_reset(void);        /* test only */

/*  — coeur PUR du timeout adaptatif du check projet (zero I/O, donc
 * testable directement) : convertit la taille totale de source (octets) en
 * budget de temps. timeout_ms = borne(15000, 60000, 15000 + round(25 * Ko)),
 * 1 Ko = 1024 octets. Le wrapper lsp_adaptive_check_timeout somme les octets
 * du scope puis delegue ici. */
int lsp_adaptive_timeout_for_bytes(size_t bytes);

#endif
