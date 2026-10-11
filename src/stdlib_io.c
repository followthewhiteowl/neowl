// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"

#include <time.h>

/* ================================================================
 * OUTPUT
 * ================================================================ */

/* Sur sortie non-TTY, stdout est plein-tampon sur ce toolchain (msvcrt) :
 * setvbuf(_IOLBF) y est alias sur le plein-tampon (mesure, cf. main.c), donc
 * un worker qui ecrit une ligne puis pompe en boucle parait muet jusqu'a sa
 * sortie. Quand ce flag est actif, les natives ligne-orientees vident stdout
 * apres le \n pour que le flux soit observable en temps reel. Pose au demarrage
 * par main() selon isatty(stdout). */
static bool g_io_line_flush = false;

void owl_io_set_line_flush(bool enabled) {
    g_io_line_flush = enabled;
}

static Value fn_trace(int argCount, Value* args) {
    for (int i = 0; i < argCount; i++) {
        if (i > 0) printf(" ");
        value_print(args[i]);
    }
    printf("\n");
    if (g_io_line_flush) fflush(stdout);
    return NIL_VAL;
}

/* ================================================================
 * Registry declarations — I/O family.
 *
 * Console* functions live in stdlib_console.c.
 * TypeVar lives in stdlib_reflect.c (integer type code, canon-compat).
 *
 * Info/Erreur/Avertissement are MODAL dialogs (canon) and live solely
 * in stdlib_ui.c. The console-print fallbacks that used to shadow them
 * here have been removed (they collided in the flat registry).
 * ================================================================ */

static const OwlParam p_exprs[] = {
    {"expressions", "tout", "valeurs à afficher (séparées par virgule)"},
    {0}
};
static const OwlSignature sig_trace[]  = { {-1, p_exprs, "rien",   "écrit sur stdout avec retour à la ligne"}, {0} };

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "Trace",
        .nameEN     = "Trace",
        .category   = "Console",
        .descFR     = "Écrit une ou plusieurs valeurs sur stdout avec retour à la ligne.",
        .descEN     = "Print one or more values to stdout with newline.",
        .signatures = sig_trace,
        .impl       = fn_trace,
    },
    {
        .canonical  = "dbgSortieStandard",
        .nameEN     = "dbgStandardOutput",
        .category   = "Debug",
        .descFR     = "Affiche un message dans la sortie standard du débogueur.",
        .descEN     = "Print a message in the debugger standard output.",
        .signatures = sig_trace,
        .impl       = fn_trace,
    },
    /* TypeVar is owned by stdlib_reflect.c (canon: returns
     * an integer type code, not a string). The obsolete string-returning
     * fn_typevar in this file is unused and will be removed in a later
     * cleanup pass. */
};

void stdlib_io_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}
