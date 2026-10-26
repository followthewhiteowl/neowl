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
 * a debugger, en redirigeant stdin/stdout/stderr via des pipes anonymes
 * herites. Cote owl-dap (parent), on garde les ends "write to child stdin",
 * "read from child stdout" et "read from child stderr".
 *
 * Deux threads reader lisent en continu stdout et stderr du child et
 * appellent un callback pour chaque ligne, en indiquant le canal. Le callback
 * (typiquement dans dap_server.c) traduit le protocole stdout (response/event
 * interne -> DAP) et forward le stderr brut en OutputEvent category=stderr.
 *
 * Architecture pipe :
 *
 *   owl-dap.exe (parent)              owl.exe --debug-stdio (child)
 *      stdin_write_to_child  ──pipe──►  stdin (commands)
 *      stdout_read_from_child ◄─pipe──  stdout (responses + events)
 *      stderr_read_from_child ◄─pipe──  stderr (erreurs runtime brutes)
 *      thread_reader(stdout) + thread_reader(stderr)
 *
 * Mono-instance par process : un seul child peut etre actif a la fois
 * (un debugger debug une session a la fois). Concurrent spawn() retourne
 * false.
 * ================================================================ */

/* Canal d'ou provient une ligne lue du child. stdout porte le protocole
 * interne (responses + events JSON) ; stderr porte les sorties runtime brutes
 * (messages d'erreur d'execution owl, exit != 0). */
#define DAP_CHILD_STDOUT 0
#define DAP_CHILD_STDERR 1

/* Signature du callback appele pour CHAQUE ligne lue du child.
 * `line` est null-terminated, sans le \n final. La chaine est detenue par
 * le buffer interne du reader, valide uniquement pendant l'appel — copier
 * si besoin de la conserver. `channel` vaut DAP_CHILD_STDOUT ou
 * DAP_CHILD_STDERR. Deux threads reader (un par canal) peuvent appeler ce
 * handler concurremment : l'implementation doit etre thread-safe. */
typedef void (*DapChildLineHandler)(const char* line, int channel);

/* Lance owl.exe (resolu par PATH ou path explicite via OWL_DEBUGGEE_EXE
 * env var) avec --debug-stdio + scriptPath en argument. handler est
 * appele depuis les threads reader pour chaque ligne stdout/stderr recue
 * (canal en 2e arg).
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
