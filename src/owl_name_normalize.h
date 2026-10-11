// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_name_normalize_h
#define owl_name_normalize_h

#include <stdbool.h>
#include <stddef.h>

/* ================================================================
 * NORMALISATION DE NOM — lookup insensible casse + accent
 * ================================================================
 * Opere sur les octets UTF-8, plage Latin-1 (C0-FF) qui couvre tous les
 * accents des identifiants. Source UNIQUE partagee par :
 *   - le registre vivant (owl_registry, owl.exe)
 *   - le backend catalogue du formatter (owl-lsp.exe)
 * pour garantir une normalisation byte-identique entre les deux backends
 * (invariant de parite du formatter : meme entree -> meme canon, peu importe
 * le binaire hote).
 * ================================================================ */

/* Normalise `src` (accent-fold + lowercase) dans `out` (NUL-termine). */
void owl_normalize_name(const char* src, char* out, int outCap);

/* Vrai si `candidate` normalise est egal a `normalizedQuery` (deja normalise). */
bool owl_name_matches(const char* candidate, const char* normalizedQuery);

/* Canonicalise la casse d'un tag BCP 47 (case-insensible RFC 5646).
 * Langue -> minuscule, region (2e sous-tag de 2 lettres) -> MAJUSCULE, reste
 * minuscule ("en-gb"/"EN-GB" -> "en-GB", "fr-fr" -> "fr-FR"). Tag NULL/vide ->
 * out vide. Jamais d'overflow. Dep-free : source UNIQUE partagee par le runtime
 * multilingue (object.c), le champ Langues du projet (compiler.c) et le
 * formatter (formatter.c), garantit une canonisation byte-identique entre
 * backends registre (owl.exe) et catalogue (owl-lsp). */
void owl_bcp47_canon(const char* tag, char* out, size_t cap);

#endif
