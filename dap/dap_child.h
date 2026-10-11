// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_dap_child_h
#define owl_dap_child_h

#include <stdbool.h>
#include <stdint.h>

/* Forward decl pour eviter d'include lsp_json.h ici. Les signatures
 * utilisent `struct JsonValue*` ; le typedef alias JsonValue est defini
 * dans lsp_json.h cote consommateurs qui ont besoin du contenu. */
struct JsonValue;

/* ================================================================
 * Sub-process owl.exe debuggee (Lot D.2.4).
 *
 * dap_child_spawn() lance owl.exe avec --debug-stdio + le path du script
 * a debugger, en redirigeant stdin/stdout via des pipes anonymes herites.
 * Cote owl-dap (parent), on garde les ends "write to child stdin" et
 * "read from child stdout".
 *
 * Un thread reader lit en continu stdout du child et appelle un callback
 * pour chaque ligne JSON. Le callback (typiquement dans dap_server.c)
 * traduit response/event interne -> DAP et le forward a VSCode.
 *
 * stderr du child est laisse a son sort pour V0 — on le wrappera en
 * OutputEvent DAP plus tard (D.2.5/D.2.7 polish). En attendant les
 * Trace() du user code apparaissent simplement sur stderr du
 * processus parent.
 *
 * Architecture pipe :
 *
 *   owl-dap.exe (parent)              owl.exe --debug-stdio (child)
 *      stdin_write_to_child  ──pipe──►  stdin (commands)
 *      stdout_read_from_child ◄─pipe──  stdout (responses + events)
 *      thread_reader [boucle sur stdout_read_from_child]
 *
 * Mono-instance par process : un seul child peut etre actif a la fois
 * (un debugger debug une session a la fois). Concurrent spawn() retourne
 * false.
 * ================================================================ */

/* Signature du callback appele pour CHAQUE ligne lue du stdout child.
 * `line` est null-terminated, sans le \n final. La chaine est detenue par
 * le buffer interne du reader, valide uniquement pendant l'appel — copier
 * si besoin de la conserver. */
typedef void (*DapChildLineHandler)(const char* line);

/* Lance owl.exe (resolu par PATH ou path explicite via OWL_DEBUGGEE_EXE
 * env var) avec --debug-stdio + scriptPath en argument. handler est
 * appele depuis le thread reader pour chaque ligne stdout recue.
 *
 * Retourne true sur succes. False si :
 *   - un child est deja actif (un seul a la fois)
 *   - CreatePipe / CreateProcess (ou pipe/fork) a echoue
 *   - le binaire owl.exe est introuvable
 *
 * Le child tourne jusqu'a ce que :
 *   - il termine de lui-meme (script complete, ou disconnect du parent)
 *   - dap_child_kill() est appele par le parent
 *   - le parent meurt (auto-cleanup OS via close des pipes -> EOF child)
 */
bool dap_child_spawn(const char* scriptPath, DapChildLineHandler handler);

/* Ecrit une ligne JSON sur stdin du child. Le \n est ajoute automatiquement.
 * Thread-safe (serialise par mutex interne) — peut etre appele depuis le
 * thread principal DAP qui dispatche les requests, sans collision avec
 * le thread reader.
 *
 * Retourne false si pas de child actif ou si l'ecriture echoue. */
bool dap_child_send(const char* jsonLine);

/* Comme dap_child_send mais prend un JsonValue, le serialise et le free
 * pour reduire le boilerplate cote handlers. */
bool dap_child_send_json(struct JsonValue* msg);

/* Envoie une commande au child et attend la reponse correspondante (meme
 * id). Bloque jusqu'au timeout en ms (5000 = recommande). Retourne le
 * JsonValue parse de la reponse — l'appelant doit le free. NULL en cas
 * de timeout / erreur transport.
 *
 * Implementation : un slot global "reponse en attente", une seule commande
 * synchrone a la fois (mutex). Suffit pour le pattern DAP courant
 * (setBreakpoints/continue/step/get_stack sont sequentiels, pas en
 * parallele). Si on devient async un jour, on passera a une mailbox. */
struct JsonValue* dap_child_request(struct JsonValue* msg, int timeoutMs);

/* Tue le child (graceful d'abord — close stdin -> EOF -> exit propre ;
 * apres timeout 500ms : SIGTERM/TerminateProcess). Idempotent. */
void dap_child_kill(void);

/* True si un child est actuellement en cours. */
bool dap_child_is_running(void);

#endif
