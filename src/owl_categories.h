// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Categories de fonctions natives — resolution et affichage bilingues.
 *
 * Table alimentee par owl_categories.def (single source). La forme FR est la
 * cle portee par le champ `category` du registre ; la forme EN est acceptee
 * au filtre `--cat` et rendue en `--lang en`.
 *
 * Header-only : la table fait 39 lignes de pointeurs, la dupliquer dans les
 * deux ou trois TU qui l'interrogent coute moins que de disperser le point
 * unique. Ajouter une categorie = une ligne dans le .def, rien d'autre.
 */
#ifndef OWL_CATEGORIES_H
#define OWL_CATEGORIES_H

#include "scanner.h"   /* scanner_strip_accents — invariant 2 */

#include <string.h>

typedef struct {
    const char* fr;
    const char* en;
} OwlCategoryPair;

static const OwlCategoryPair OWL_CATEGORIES[] = {
#define OWL_CATEGORY(fr, en) { fr, en },
#include "owl_categories.def"
#undef OWL_CATEGORY
};
#define OWL_CATEGORIES_COUNT \
    ((int)(sizeof(OWL_CATEGORIES) / sizeof(OWL_CATEGORIES[0])))

/* Comparaison casse- ET accent-insensible, alignee sur l'invariant runtime :
 * `--cat Systeme` vaut `--cat Système`, comme `httpRequete` vaut
 * `httpRequête`. */
static inline int owl_category_eq_ci(const char* a, const char* b) {
    char ab[128], bb[128];
    int n = scanner_strip_accents(a, (int)strlen(a), ab, (int)sizeof ab);
    int m = scanner_strip_accents(b, (int)strlen(b), bb, (int)sizeof bb);
    if (n != m) return 0;
    for (int i = 0; i < n; i++) {
        char ca = ab[i], cb = bb[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Resout `input` (forme FR ou EN) vers la paire correspondante. NULL si
 * aucune categorie ne porte ce nom. */
static inline const OwlCategoryPair* owl_category_resolve(const char* input) {
    if (!input || !*input) return NULL;
    for (int i = 0; i < OWL_CATEGORIES_COUNT; i++) {
        if (owl_category_eq_ci(OWL_CATEGORIES[i].fr, input) ||
            owl_category_eq_ci(OWL_CATEGORIES[i].en, input)) {
            return &OWL_CATEGORIES[i];
        }
    }
    return NULL;
}

/* Forme a afficher pour une cle FR du registre. Rend `fr` tel quel en FR, ou
 * si le nom n'est pas une categorie de natives (`--cat` designe aussi des
 * kinds de widget, des buckets d'explain, des categories de limites). */
static inline const char* owl_category_display(const char* fr,
                                               int lang_is_en) {
    if (!fr || !lang_is_en) return fr;
    for (int i = 0; i < OWL_CATEGORIES_COUNT; i++) {
        if (strcmp(OWL_CATEGORIES[i].fr, fr) == 0) return OWL_CATEGORIES[i].en;
    }
    return fr;
}

#endif /* OWL_CATEGORIES_H */
