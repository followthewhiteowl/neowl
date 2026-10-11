// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_errors_compile.c — Helpers compile-aware du registry erreurs.
 *
 * Lot 2  : pont entre `owl_errors` (registry pur, linke aussi
 * cote LSP) et `compiler.c` (machinerie d'emission stderr / JSON /
 * diagBuffer). Comme `owl_errors.c` est linke par le LSP qui ne possede
 * pas `compiler.c`, on isole ici les fonctions qui ont besoin du compiler
 * pour eviter des symboles non resolus cote LSP.
 *
 * Couvre :
 *   - owl_emit_error_at   : Token + code -> diagnostic via compile_emit_diagnostic_at
 *   - owl_emit_warning_at : idem mais warning (pas de hadError)
 *   - owl_emit_error_current : recupere parser.current via compile_current_token
 *
 * Tous les helpers passent le rendu FR du template au pont compiler.c qui
 * gere le format final (texte stderr [file:line] OU JSON g_json_compile_errors)
 * conformement au legacy error_at / warning_at. */

#include "owl_errors.h"
#include "compiler.h"

#include <stdarg.h>

void owl_emit_error_at(const Token* tok, OwlErrorCode code, ...) {
    if (!tok) return;
    va_list args;
    va_start(args, code);
    const char* msg = owl_error_render_active_va(code, args);
    va_end(args);
    compile_emit_diagnostic_at(tok, msg, /*isWarning=*/false);
}

void owl_emit_warning_at(const Token* tok, OwlErrorCode code, ...) {
    if (!tok) return;
    va_list args;
    va_start(args, code);
    const char* msg = owl_error_render_active_va(code, args);
    va_end(args);
    compile_emit_diagnostic_at(tok, msg, /*isWarning=*/true);
}

void owl_emit_error_current(OwlErrorCode code, ...) {
    const Token* tok = compile_current_token();
    if (!tok) return;
    va_list args;
    va_start(args, code);
    const char* msg = owl_error_render_active_va(code, args);
    va_end(args);
    compile_emit_diagnostic_at(tok, msg, /*isWarning=*/false);
}
