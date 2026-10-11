// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_errors_native.c — Helpers native-aware du registry erreurs.
 *
 * Lot 4  : pont entre `owl_errors` (registry pur, linke aussi
 * cote LSP) et `stdlib_erreur.c` (mecanisme soft-error global + variable
 * ErreurDetectee). Comme `owl_errors.c` est linke par le LSP qui ne
 * possede pas `stdlib_erreur.c`, on isole ici la fonction qui a besoin
 * du soft-error context pour eviter des symboles non resolus cote LSP.
 *
 * Couvre :
 *   - owl_emit_native_error : OwlErrorCode + native_name + args -> rendu FR
 *     via owl_error_render_fr_va puis forward vers owl_set_error_native()
 *     (nouveau transport interne soft-error qui stocke le code canonique
 *     + le nom de la native + le message + contexte VM).
 *
 * Strategie : zero duplication du format legacy. Le transport
 * owl_set_error_native() de stdlib_erreur.c gere deja la capture du
 * contexte VM (stack frame, line, file, procedure), la sentinelle
 * g_err_fresh, la sync de la variable globale ErreurDetectee.
 * Le helper rend juste le template FR via le format engine du registry,
 * puis passe la chaine finale + le code canonique + le nom de la native
 * au transport. */

#include "owl_errors.h"
#include "owl_stdlib.h"

#include <stdarg.h>

/* Forward-declare du nouveau transport soft-error (impl dans stdlib_erreur.c,
 * ajoute en jalon A mais stub). Jalon B activera l'impl complete. */
extern void owl_set_error_native(const char* code_canonical,
                                  const char* native_name,
                                  const char* message);

void owl_emit_native_error(OwlErrorCode code, const char* native_name, ...) {
    /* Rend le template FR avec les args. */
    va_list args;
    va_start(args, native_name);
    owl_error_render_bind_native(native_name);
    const char* msg = owl_error_render_active_va(code, args);
    owl_error_render_bind_native(NULL);
    va_end(args);

    /* Recupere le nom canonique du code pour stocker en g_err_code. */
    const OwlErrorDef* def = owl_registry_error_by_code(code);
    const char* code_canonical = def ? def->name : "E_UNKNOWN";

    /* Forward vers le nouveau transport soft-error. */
    owl_set_error_native(code_canonical, native_name, msg);
}
