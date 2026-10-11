// SPDX-License-Identifier: AGPL-3.0-only
#include "lsp_server.h"
#include "../src/owl_event_catalog.h"
#include "../src/formatter_canon.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Defini dans event_catalog_widgets.c — peuple le catalogue events
 * pour les widgets DSL (Bouton.surClic, Image.surMolette, etc.).
 * Le LSP en a besoin pour l'autocomplete + hover signature. */
extern void event_catalog_widgets_init(void);

/* ================================================================
 * OWL LANGUAGE SERVER -- entry point
 *
 * Usage: owl-lsp
 *   (communicates via JSON-RPC on stdin/stdout)
 *
 * The server provides:
 *   - Diagnostics (lexer errors on open/change/save)
 *   - Completion (keywords, types, native functions)
 *   - Hover (info on keywords and native functions)
 *   - Go-to-definition (find PROCEDURE/FONCTION declarations)
 * ================================================================ */

int main(void) {
#ifdef _WIN32
    /* LSP spec requires UTF-8. Without forcing the console codepage here
     * Windows will emit CP-1252 on stdout and clients see mojibake in
     * completion labels / hover text (ExceptionD�clenche etc.). */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    /* Init catalogue events — alimente owl_event_catalog_find/at consultes
     * par lsp_handlers.c pour l'autocomplete des PROCEDURE event. */
    event_catalog_widgets_init();
    /* Pose le provider de canon du formatter sur le backend CATALOGUE (tables
     * statiques generees). Le LSP ne passe jamais par vm_init, donc c'est ici
     * qu'on branche — symetrique de vm_init cote owl.exe (backend registry). */
    owl_format_set_canon(owl_fmt_canon_catalog());
    lsp_run();
    return 0;
}
