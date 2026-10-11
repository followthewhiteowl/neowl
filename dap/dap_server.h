// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_dap_server_h
#define owl_dap_server_h

#include <stdbool.h>
#include <stdint.h>

/* Forward decl pour eviter d'include lsp_json.h ici. Les signatures
 * utilisent `struct JsonValue*` ; le typedef alias JsonValue est defini
 * dans lsp_json.h cote consommateurs qui ont besoin du contenu. */
struct JsonValue;

/* ================================================================
 * Boucle principale du serveur DAP.
 *
 * Lit des messages DAP framing Content-Length sur stdin, dispatche vers
 * les handlers selon `command`, ecrit les reponses sur stdout. Sort
 * proprement quand `disconnect` ou `terminate` est recu, ou sur EOF.
 * ================================================================ */

void dap_run(void);

/* ================================================================
 * Helpers I/O (utilises par les handlers individuels).
 * ================================================================ */

/* Log libre sur stderr (stdout reserve au protocole). Format printf-like. */
void dap_log(const char* fmt, ...);

/* Envoie une reponse a une request. body peut etre NULL.
 * success=true : body devient le payload `body` du response DAP.
 * success=false : body devient le `message` (raison de l'echec) ; si NULL,
 *                 utilise "command not implemented" par defaut. */
void dap_send_response(int64_t requestSeq, const char* command,
                       bool success, struct JsonValue* body);

/* Envoie un event DAP non sollicite (stopped, terminated, output, ...). */
void dap_send_event(const char* eventName, struct JsonValue* body);

#endif
