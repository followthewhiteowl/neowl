// SPDX-License-Identifier: AGPL-3.0-only
#include "dap_server.h"

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ================================================================
 * OWL DEBUG ADAPTER -- entry point
 *
 * Usage: owl-dap
 *   communique en JSON-RPC sur stdin/stdout au protocole DAP
 *   (Debug Adapter Protocol, Microsoft).
 *
 * Lance par VSCode quand l'utilisateur appuie F5 sur un .owl. Mediateur
 * entre l'IDE (qui parle DAP) et la VM OWL en cours d'execution (qui
 * sera pilotee par les API src/debug_api.h).
 *
 * Lot D.2 du roadmap debugger DAP. V0 (D.2.1) = squelette : initialize +
 * disconnect, repond aux autres requests par un "not yet implemented".
 * Le pilotage de la VM debuggee (D.2.4 IPC) viendra ensuite.
 * ================================================================ */

int main(void) {
#ifdef _WIN32
    /* DAP comme LSP : JSON UTF-8 sur stdout. Sans ca Windows mangerait
     * les accents en CP-1252 et VSCode afficherait du mojibake. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    dap_run();
    return 0;
}
