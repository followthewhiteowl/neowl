// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_limits.c — Implémentation du registry des limites compile-time.
 *
 * Le tableau OWL_LIMITS[] est construit en passe 2 du double-include
 * de owl_limits.def. Toute la logique d'introspection lit ce tableau.
 * Cf. owl_limits.h pour la documentation de l'interface.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "owl_limits.h"

/* Passe 2 — construit OWL_LIMITS[] depuis le .def. */
#define OWL_LIMIT(name, value, cat, descFR, descEN) \
    { #name, (value), (cat), (descFR), (descEN) },
static const OwlLimitDef OWL_LIMITS[] = {
#include "owl_limits.def"
};
#undef OWL_LIMIT

static const int OWL_LIMITS_N =
    (int)(sizeof(OWL_LIMITS) / sizeof(OWL_LIMITS[0]));

void owl_limits_init(void) {
    /* Rien à faire pour l'instant : OWL_LIMITS[] est static const,
     * directement utilisable. La fonction existe pour homogénéité
     * avec les autres registries et pour servir de point d'extension. */
}

int owl_registry_limit_count(void) {
    return OWL_LIMITS_N;
}

const OwlLimitDef* owl_registry_limit_at(int index) {
    if (index < 0 || index >= OWL_LIMITS_N) return NULL;
    return &OWL_LIMITS[index];
}

/* Comparaison case-insensitive ASCII pure (les noms de limites sont
 * tous ASCII — MAX_FOO_BAR — donc inutile de passer par scanner_strip_accents). */
static int eq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

const OwlLimitDef* owl_registry_limit_find(const char* name) {
    if (!name || !*name) return NULL;
    /* Le nom utilisateur peut être passé tel quel ("MAX_USER_TYPES")
     * ou en lowercase. On normalise via eq_ci. */
    for (int i = 0; i < OWL_LIMITS_N; i++) {
        if (eq_ci(OWL_LIMITS[i].name, name)) return &OWL_LIMITS[i];
    }
    return NULL;
}

/* error_capacity_exceeded : implémentée dans compiler.c (où vit
 * l'helper `error()`). Déclarée ici car partagée par tous les sites
 * d'usage. */
