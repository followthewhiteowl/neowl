// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icons_canon.h — .
 *
 * Naming canon FR -> EN pour catalogue d'icones embarque. Permet a
 * `owl icons search <mot_fr>` de matcher les noms canon Iconify EN
 * equivalents (ex: "maison" -> [home, house]).
 *
 * Table source : tools/icons_canon_fr_en/mapping.def (curation manuelle
 * ~3100 termes FR distincts). Genere automatiquement par
 * tools/icons_canon_gen/ au build via add_custom_command CMake.
 *
 * Pas de mecanisme EN -> FR (un user qui tape EN matche deja le path
 * direct du catalogue). Pas d'integration au resolver `icones.X` runtime
 * (ticket separe si demande).
 */

#ifndef OWL_ICONS_CANON_H
#define OWL_ICONS_CANON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char*        fr;        /* lowercase ASCII, normalise pre-curation */
    const char* const* en;        /* NULL-terminated array de tokens EN */
    int                en_count;  /* nombre d'entrees dans en (exclut NULL) */
} OwlIconCanonEntry;

/* Table generee, triee par fr ASC pour bsearch. */
extern const OwlIconCanonEntry OWL_ICONS_FR_EN_MAPPING[];
extern const int               OWL_ICONS_FR_EN_COUNT;

/* Lookup : si `fr_lowercase` matche une entree (apres normalisation
 * case+accent insensible cote caller — voir
 * owl_icones_canon_normalize_fr ci-dessous), remplit *out_en_list +
 * *out_count et retourne true. Sinon retourne false (out_* non
 * modifies).
 *
 * `fr_lowercase` est suppose deja normalise lowercase ASCII.
 */
bool owl_icones_fr_to_en(const char* fr_lowercase,
                         const char* const** out_en_list,
                         int* out_count);

/* Normalisation FR : lowercase + strip accents (regle 6 invariants 1+2
 * OWL). Ecrit dans `out_buf` (taille `out_size`). Tronque silencieusement
 * si overflow. Retourne true si non-tronque.
 *
 * Exemples : "Maison" -> "maison", "Cafe" -> "cafe", "élève" -> "eleve".
 */
bool owl_icones_canon_normalize_fr(const char* input,
                                   char* out_buf, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* OWL_ICONS_CANON_H */
