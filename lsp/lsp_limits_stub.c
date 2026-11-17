// SPDX-License-Identifier: AGPL-3.0-only
/* lsp_limits_stub.c — définition no-op de error_capacity_exceeded pour owl-lsp.
 *
 * prescan.c est un TU partagé entre owl.exe et owl-lsp. Depuis que le prescan
 * refuse bruyamment une saturation de classMethods[] via le chemin canon
 * error_capacity_exceeded(), ce symbole est référencé par prescan_file().
 *
 * Sa définition canon (parser-based) vit dans compiler.c : elle rend
 * E_SYNTAX_CAPACITY_EXCEEDED au point courant du parser via owl_emit_error_at.
 * Toute cette machinerie (static Parser parser, owl_errors_compile.c) est
 * EXCLUE d'owl-lsp — le LSP ne lie pas owl_core et n'a aucune autorité de
 * compilation. owl.exe reste le seul à émettre l'erreur de capacité.
 *
 * Côté LSP, atteindre le plafond prescan est donc une troncature silencieuse
 * assumée : le serveur fournit complétion/hover au mieux, et le vrai compile
 * (owl.exe --check) remonte l'erreur à l'utilisateur. Ce no-op résout le
 * symbole pour le link d'owl-lsp sans dupliquer de logique d'erreur.
 *
 * Chaque binaire résout SA définition (compiler.c pour owl.exe, ce stub pour
 * owl-lsp) ; ils ne sont jamais liés ensemble → pas de double définition. */

#include "owl_limits.h"

void error_capacity_exceeded(const char* limit_name, int max) {
    (void)limit_name;
    (void)max;
}
