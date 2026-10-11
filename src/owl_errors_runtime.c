// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_errors_runtime.c — Helpers runtime-aware du registry erreurs.
 *
 * Lot 3  : pont entre `owl_errors` (registry pur, linke aussi
 * cote LSP) et `vm.c` (machinerie d'emission stderr texte / JSON +
 * stack trace + longjmp exception handler). Comme `owl_errors.c` est
 * linke par le LSP qui ne possede pas `vm.c`, on isole ici la fonction
 * qui a besoin de la VM pour eviter des symboles non resolus cote LSP.
 *
 * Couvre :
 *   - owl_emit_runtime_error : OwlErrorCode + args -> rendu FR via
 *     owl_error_render_fr_va puis forward vers vm_emit_runtime_error_rendered
 *     (transport interne qui appelle le static runtime_error() de vm.c).
 *
 * Strategie : zero duplication du format legacy. Le transport runtime_error()
 * de vm.c gere deja stack trace, capture exception handler (longjmp implicite),
 * mode JSON g_json_error_mode et snippet source. Le helper rend juste le
 * template FR via le format engine du registry, et passe la chaine finale
 * au transport via "%s". */

#include "owl_errors.h"
#include "vm.h"

#include <stdarg.h>

void owl_emit_runtime_error(OwlErrorCode code, ...) {
    va_list args;
    va_start(args, code);
    const char* msg = owl_error_render_active_va(code, args);
    va_end(args);
    vm_emit_runtime_error_rendered(msg);
}
