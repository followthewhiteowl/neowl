// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_new.h — API du générateur de squelettes `owl new <type>`.
 *
 * Exposé pour le self-test (rule 37) qui compile chaque squelette et
 * casse le build s'il devient invalide. Implémentation dans main.c. */

#ifndef OWL_NEW_H
#define OWL_NEW_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Génère le squelette pour (kind, sub) dans `out`. Si kind=="champ",
 * `sub` désigne le sous-kind (bouton, libelle, table, ...). Pour
 * fenetre/classe, `sub` doit être NULL.
 * Retourne 0 si la combinaison est reconnue, -1 sinon. */
int owl_new_generate(const char* kind, const char* sub, const char* name,
                     FILE* out);

/* Table des combinaisons supportées. Terminée par {NULL, NULL}.
 * Consommée par registry_self_test::check_rule37_new_skeleton_compiles. */
typedef struct {
    const char* kind;  /* "fenetre", "classe", "champ" */
    const char* sub;   /* NULL pour fenetre/classe ; sous-kind pour champ */
} OwlNewVariant;

extern const OwlNewVariant OWL_NEW_VARIANTS[];

#ifdef __cplusplus
}
#endif

#endif /* OWL_NEW_H */
