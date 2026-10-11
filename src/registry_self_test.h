// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* registry_self_test.h - Self-tests des invariants du registry OWL.
 *
 * Voir docs/internal_documentation/cahier_des_charges_langage.md pour la liste des
 * invariants protégés.
 *
 * Usage : `owl --self-test-registry` (cf src/main.c).
 *         Exit 0 si OK, 1 si violations. Sortie JSON sur stdout.
 */

#ifndef OWL_REGISTRY_SELF_TEST_H
#define OWL_REGISTRY_SELF_TEST_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lance les self-tests et écrit le rapport JSON sur `out`.
 * Retourne le nombre de violations détectées (0 si tout est OK). */
int registry_self_test_run(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* OWL_REGISTRY_SELF_TEST_H */
