// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_debug_stdio_h
#define owl_debug_stdio_h

#include <stdbool.h>

/* ================================================================
 * Debug stdio (Lot D.2.3 — alternative au TCP).
 *
 * Mode --debug-stdio : owl.exe lit du JSON line-delimited sur stdin
 * (commandes du debugger parent) et ecrit ses reponses + events sur
 * stdout. Aucun socket, aucun port, aucune surface IPC visible : c'est
 * le pattern parent-child standard (gdb, lldb, debugpy, node debug).
 *
 * Le binaire owl-dap.exe (D.2.4) sera le parent : il spawnera owl.exe
 * en sub-process avec stdin/stdout pipés et fera le forwarding DAP <->
 * stdio.
 *
 * stderr reste libre pour le user code (Trace, Erreur, ...). owl-dap
 * capturera stderr et le wrappera en OutputEvent DAP cote VSCode.
 *
 * Protocole identique a celui qu'avait debug_listener (TCP) : JSON
 * line-delimited, sans Content-Length. Documente dans dispatch_cmd.
 *
 * Architecture interne : un thread lit stdin et dispatche, la VM tourne
 * sur le main thread. Les API debug_api etant thread-safe par design
 * (volatile + busy-wait), pas de mutex requis.
 * ================================================================ */

/* Demarre le thread de lecture stdin + execute owl_debug_attach().
 * Retourne true sur succes. Aucun setup reseau ; faillible uniquement
 * si la creation du thread echoue (extremement rare). */
bool debug_stdio_start(void);

/* Bloque jusqu'a ce que le debugger envoie la commande `run` (apres avoir
 * flushe ses breakpoints initiaux), ou jusqu'a EOF stdin. A appeler par le
 * main thread juste apres debug_stdio_start() et AVANT d'executer le script,
 * pour eliminer la course de demarrage (script qui depasse un breakpoint avant
 * son enregistrement). No-op si le mode stdio n'a pas demarre. */
void debug_stdio_wait_for_run(void);

#endif
