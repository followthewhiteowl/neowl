// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_limits_h
#define owl_limits_h

/* ================================================================
 * OWL LIMITS — Single-source compile-time limits du compilateur.
 *
 * Toutes les limites compile-time du compilateur OWL sont déclarées
 * dans owl_limits.def via la macro OWL_LIMIT(name, value, category,
 * descFR, descEN). Ce fichier expose deux vues construites par
 * double-include de owl_limits.def :
 *
 *   1. enum OwlLimit : la valeur entière, utilisable partout où un
 *      `#define MAX_*` était présent. Sémantiquement équivalent.
 *      Exemple : `if (count >= MAX_USER_TYPES) { ... }`.
 *
 *   2. struct OwlLimitDef + tableau OWL_LIMITS[] : introspection
 *      depuis `owl list limits`, `owl explain MAX_*`, MCP tool,
 *      --vscode-data et self-test.
 *
 * Ajouter une limite : éditer UNIQUEMENT owl_limits.def. Le drift
 * est mécaniquement impossible (enum et tableau partagent la même
 * source). Cf. .
 *
 * Quand un site dépasse la limite, appeler `error_capacity_exceeded`
 * — c'est l'unique chemin d'erreur uniforme pour ces saturations.
 * ================================================================ */

/* Passe 1 — enum compilable. Chaque OWL_LIMIT(N,V,...) → N = V, */
#define OWL_LIMIT(name, value, cat, descFR, descEN) name = (value),
enum OwlLimit {
#include "owl_limits.def"
    OWL_LIMIT_COUNT_SENTINEL_  /* jamais utilisé, ferme l'enum proprement */
};
#undef OWL_LIMIT

/* UINT8_COUNT historiquement défini dans common.h. La limite est
 * réexposée sous le nom MAX_LOCALS_PER_FUNCTION pour introspection
 * cohérente, mais aucun code n'a besoin de la redéfinir : on garde
 * le double nom (UINT8_COUNT compile-time, MAX_LOCALS_PER_FUNCTION
 * runtime/intro) pour ne rien casser. */

/* Passe 2 — tableau introspectable. */
typedef struct OwlLimitDef {
    const char* name;
    int         value;
    const char* category;
    const char* descFR;
    const char* descEN;
} OwlLimitDef;

/* API d'introspection. Implémentée dans owl_limits.c.
 * Tableau plat, ordre = ordre du .def. */
int                 owl_registry_limit_count(void);
const OwlLimitDef*  owl_registry_limit_at(int index);
/* Lookup case-insensitive sur `name` (ex: "MAX_USER_TYPES",
 * "max_user_types", "Max_User_Types"). Renvoie NULL si introuvable. */
const OwlLimitDef*  owl_registry_limit_find(const char* name);

/* Init facultative — appelée depuis stdlib_init() au boot. À ce jour
 * pas d'allocation runtime : le tableau OWL_LIMITS[] est `static const`,
 * directement utilisable. La fonction existe pour rester homogène avec
 * les autres registries (natives/types/consts/enums) et pour servir de
 * point d'extension futur (ex: hash precomputed pour le lookup). */
void owl_limits_init(void);

/* ----------------------------------------------------------------
 * error_capacity_exceeded — chemin unique pour les saturations
 * compile-time. Rend E_SYNTAX_CAPACITY_EXCEEDED du catalogue (donc
 * bilingue) au point courant du parser.
 *
 * `limit_name` : nom du symbole limite ("MAX_USER_TYPES", …) — sert de
 *                placeholder {limite} et renvoie l'utilisateur vers
 *                `owl explain <name>`. C'est un identifiant de registre,
 *                pas une prose : rien a traduire.
 * `max`  : valeur de la limite.
 * ---------------------------------------------------------------- */
void error_capacity_exceeded(const char* limit_name, int max);

#endif
