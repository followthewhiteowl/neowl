// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_mcp.h — serveur MCP (Model Context Protocol) sur stdio.
 *
 * Expose la doc + le registre OWL à un host LLM compatible MCP
 * (Claude Desktop, Cursor, MCP Inspector, etc.) via JSON-RPC 2.0
 * newline-delimited sur stdin/stdout. stderr reste libre pour les logs
 * — le host les forward dans son UI.
 *
 * V1 : surface minimale (initialize / ping / tools/list /
 * resources/list / resources/templates/list / prompts/list). Listes
 * vides pour cette étape ; les tools et resources viennent aux étapes
 * suivantes.
 *
 * Lancé via : owl mcp serve
 */

#ifndef OWL_CLI_MCP_H
#define OWL_CLI_MCP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Lance la boucle MCP sur stdin/stdout. Retourne 0 sur EOF propre,
 * != 0 sur erreur d'I/O fatale. */
int cli_mcp_serve(int argc, char** argv);

/* Traite une seule ligne JSON-RPC entrante et retourne la réponse JSON
 * en malloc'd string (le caller free). Renvoie NULL si la requête était
 * une notification (pas de réponse attendue).
 *
 * Exposé pour les self-tests in-process : permet de driver le serveur
 * sans pipe/stdio. */
char* cli_mcp_handle_request(const char* line, int line_len);

#ifdef __cplusplus
}
#endif

#endif /* OWL_CLI_MCP_H */
