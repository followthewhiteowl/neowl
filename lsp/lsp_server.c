// SPDX-License-Identifier: AGPL-3.0-only
#include "lsp_server.h"
#include "lsp_proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

LspServer lspServer;

/* ================================================================
 * LOGGING (stderr only -- stdout is for JSON-RPC)
 * ================================================================ */

void lsp_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[owl-lsp] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

/* ================================================================
 * JSON-RPC TRANSPORT
 * ================================================================ */

/* Read a single LSP message from stdin.
 * Format: "Content-Length: NNN\r\n\r\n{...json...}"
 *
 *  Lot 1 : la lecture passe par lsp_stdin_* (buffer explicite) et non
 * plus par <stdio.h>. C'est ce qui rend `lsp_stdin_wait` fiable — un octet
 * resté dans un buffer de la libc serait invisible au peek OS, et un message
 * client dormirait pendant tout le tick d'inactivité. */
static char* read_message(int* outLen) {
    /* Read headers */
    int contentLength = -1;
    char headerLine[1024];

    for (;;) {
        if (lsp_stdin_read_line(headerLine, sizeof(headerLine)) == 0) return NULL;

        /* Empty line (just \r\n) signals end of headers */
        if (strcmp(headerLine, "\r\n") == 0 || strcmp(headerLine, "\n") == 0)
            break;

        if (strncmp(headerLine, "Content-Length:", 15) == 0) {
            contentLength = atoi(headerLine + 15);
        }
        /* Ignore other headers (Content-Type, etc.) */
    }

    if (contentLength <= 0) return NULL;

    /* Read body */
    char* body = malloc(contentLength + 1);
    int bytesRead = lsp_stdin_read_exact(body, contentLength);
    if (bytesRead < contentLength) {
        free(body);
        return NULL;
    }
    body[contentLength] = '\0';
    *outLen = contentLength;
    return body;
}

/* Send a raw JSON-RPC message to stdout */
static void send_message(const char* json, int len) {
    fprintf(stdout, "Content-Length: %d\r\n\r\n", len);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

void lsp_send_response(int64_t id, JsonValue* result) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "jsonrpc", json_new_string("2.0"));
    json_object_set(msg, "id", json_new_int(id));
    json_object_set(msg, "result", result ? result : json_new_null());

    int len;
    char* json = json_serialize(msg, &len);
    send_message(json, len);
    free(json);
    json_free(msg);
}

void lsp_send_error(int64_t id, int code, const char* message) {
    JsonValue* err = json_new_object();
    json_object_set(err, "code", json_new_int(code));
    json_object_set(err, "message", json_new_string(message));

    JsonValue* msg = json_new_object();
    json_object_set(msg, "jsonrpc", json_new_string("2.0"));
    json_object_set(msg, "id", json_new_int(id));
    json_object_set(msg, "error", err);

    int len;
    char* json = json_serialize(msg, &len);
    send_message(json, len);
    free(json);
    json_free(msg);
}

void lsp_send_notification(const char* method, JsonValue* params) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "jsonrpc", json_new_string("2.0"));
    json_object_set(msg, "method", json_new_string(method));
    json_object_set(msg, "params", params ? params : json_new_object());

    int len;
    char* json = json_serialize(msg, &len);
    send_message(json, len);
    free(json);
    json_free(msg);
}

/* ================================================================
 * DOCUMENT STORE
 * ================================================================ */

static int find_document(const char* uri) {
    for (int i = 0; i < lspServer.documentCount; i++) {
        if (strcmp(lspServer.documents[i].uri, uri) == 0) return i;
    }
    return -1;
}

void handle_did_open(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    if (!td) return;
    const char* uri = json_get_string(td, "uri");
    const char* text = json_get_string(td, "text");
    if (!uri || !text) return;

    int idx = lspServer.documentCount;
    if (idx >= 256) return; /* full */
    lspServer.documentCount++;

    strncpy(lspServer.documents[idx].uri, uri, 511);
    lspServer.documents[idx].content = strdup(text);
    lspServer.documents[idx].contentLen = (int)strlen(text);
    lspServer.documents[idx].version = (int)json_get_int(td, "version", 0);

    lsp_log("Opened: %s (%d bytes)", uri, lspServer.documents[idx].contentLen);
    publish_diagnostics(uri);
}

void handle_did_change(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    if (!td) return;
    const char* uri = json_get_string(td, "uri");
    if (!uri) return;

    int idx = find_document(uri);
    if (idx < 0) return;

    /* We use full document sync, so take the last content change */
    JsonValue* changes = json_get(params, "contentChanges");
    if (!changes || changes->type != JSON_ARRAY || changes->as.arrayVal.count == 0) return;

    JsonValue* lastChange = changes->as.arrayVal.items[changes->as.arrayVal.count - 1];
    const char* text = json_get_string(lastChange, "text");
    if (!text) return;

    free(lspServer.documents[idx].content);
    lspServer.documents[idx].content = strdup(text);
    lspServer.documents[idx].contentLen = (int)strlen(text);
    lspServer.documents[idx].version = (int)json_get_int(td, "version", 0);

    publish_diagnostics(uri);
}

void handle_did_close(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    if (!td) return;
    const char* uri = json_get_string(td, "uri");
    if (!uri) return;

    int idx = find_document(uri);
    if (idx < 0) return;

    free(lspServer.documents[idx].content);
    /* Shift remaining documents */
    for (int i = idx; i < lspServer.documentCount - 1; i++) {
        lspServer.documents[i] = lspServer.documents[i + 1];
    }
    lspServer.documentCount--;
    lsp_log("Closed: %s", uri);
}

void handle_did_save(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    if (!td) return;
    const char* uri = json_get_string(td, "uri");
    /* Check complet au save — en arrière-plan ( Lot 1) : à froid le
     * spawn coûte des secondes, que la boucle d'événements ne doit pas payer. */
    if (uri) publish_compiler_diagnostics_async(uri);
}

/* ================================================================
 * MESSAGE DISPATCH
 * ================================================================ */

static void dispatch(JsonValue* msg) {
    const char* method = json_get_string(msg, "method");
    JsonValue* params = json_get(msg, "params");
    JsonValue* idVal = json_get(msg, "id");
    int64_t id = idVal && idVal->type == JSON_INT ? idVal->as.intVal : -1;
    bool isRequest = (idVal != NULL); /* requests have id, notifications don't */

    if (!method) {
        if (isRequest) lsp_send_error(id, -32600, "Missing method");
        return;
    }

    lsp_log(">> %s (id=%lld)", method, (long long)id);

    /* Lifecycle */
    if (strcmp(method, "initialize") == 0) {
        JsonValue* result = handle_initialize(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "initialized") == 0) {
        handle_initialized(params);
        return;
    }
    if (strcmp(method, "shutdown") == 0) {
        handle_shutdown(id);
        return;
    }
    if (strcmp(method, "exit") == 0) {
        exit(lspServer.shutdown_requested ? 0 : 1);
    }

    /* Document sync */
    if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(params); return;
    }
    if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(params); return;
    }
    if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(params); return;
    }
    if (strcmp(method, "textDocument/didSave") == 0) {
        handle_did_save(params); return;
    }

    /* Language features */
    if (strcmp(method, "textDocument/completion") == 0) {
        JsonValue* items = handle_completion(params);
        /* Wrap in CompletionList with isIncomplete=true so VSCode re-queries
         * the LSP on every keystroke instead of caching the first result and
         * filtering client-side. Critical for context-sensitive completion
         * (typeDecl after `est`, dot context, etc.) where the offered set
         * depends on surrounding code. */
        JsonValue* list = json_new_object();
        json_object_set(list, "isIncomplete", json_new_bool(true));
        json_object_set(list, "items", items);
        lsp_send_response(id, list);
        return;
    }
    if (strcmp(method, "textDocument/hover") == 0) {
        JsonValue* result = handle_hover(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/definition") == 0) {
        JsonValue* result = handle_definition(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/references") == 0) {
        JsonValue* result = handle_references(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/rename") == 0) {
        JsonValue* result = handle_rename(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/prepareRename") == 0) {
        JsonValue* result = handle_prepare_rename(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/signatureHelp") == 0) {
        JsonValue* result = handle_signature_help(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        JsonValue* result = handle_semantic_tokens(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/formatting") == 0) {
        JsonValue* result = handle_formatting(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/documentSymbol") == 0) {
        JsonValue* result = handle_document_symbol(params);
        lsp_send_response(id, result);
        return;
    }
    if (strcmp(method, "textDocument/foldingRange") == 0) {
        JsonValue* result = handle_folding_range(params);
        lsp_send_response(id, result);
        return;
    }

    /* Unknown method */
    if (isRequest) {
        lsp_send_error(id, -32601, "Method not found");
    }
}

/* ================================================================
 * MAIN LOOP
 * ================================================================ */

/*  Lot 1 — période du tick d'inactivité. Ne tourne QUE tant qu'un
 * check compilateur est en vol : sans check en cours, la boucle se rendort
 * dans une lecture bloquante (zéro CPU au repos). */
#define LSP_IDLE_TICK_MS 20

void lsp_run(void) {
#ifdef _WIN32
    /* Binary mode on stdout to avoid \r\n mangling. stdin n'est plus lu via
     * <stdio.h> ( Lot 1) : les primitives lsp_stdin_* lisent le
     * handle OS brut, donc sans traduction de fin de ligne par construction. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    memset(&lspServer, 0, sizeof(lspServer));
    lsp_log("OWL Language Server starting...");

    for (;;) {
        /* Tick d'inactivité : tant qu'aucun message client n'arrive, on fait
         * avancer le check compilateur lancé en arrière-plan et on publie ses
         * diagnostics dès qu'ils sont là. Sans ce tick, un résultat arrivé
         * après la dernière frappe attendrait le prochain message du client —
         * or l'utilisateur qui s'arrête de taper pour lire ses erreurs n'en
         * envoie plus aucun (déplacer le curseur ne produit rien côté LSP). */
        while (lsp_check_is_pending()) {
            if (lsp_stdin_wait(LSP_IDLE_TICK_MS) != 0) break;  /* message dispo ou EOF */
            lsp_check_pump();
        }

        int len;
        char* body = read_message(&len);
        if (!body) {
            lsp_log("EOF on stdin, exiting.");
            break;
        }

        JsonValue* msg = json_parse(body, len);
        if (msg) {
            dispatch(msg);
            json_free(msg);
        } else {
            lsp_log("Failed to parse JSON message.");
        }
        free(body);

        /* Sous flot continu de messages (frappe rapide), le tick ci-dessus ne
         * s'exécute jamais : on récolte aussi ici pour ne pas faire attendre
         * un résultat déjà prêt. */
        lsp_check_pump();
    }
}
