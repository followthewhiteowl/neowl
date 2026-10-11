// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* string_compare_opts.h -- helper unique pour comparaison de chaines avec
 * options cc et tcc (doc 3024011 + options de tri tableau).
 *
 * Le helper accepte un bitmask base sur les valeurs CC (low-byte) :
 *   1=SansCasse, 2=SansAccent, 4=SansEspace (trim), 8=SansEspaceInt,
 *   16=SansPonctuation, 32=OrdreLexicographique, 64=RespecteNumerique.
 *
 * Les valeurs TCC utilisees par les fonctions de tri tableau vivent dans
 * le high-byte (0x100, 0x200, etc.) avec en plus une inversion historique
 * entre lex et numerique : convertir via owl_tcc_to_cmp_flags() avant appel.
 */
#ifndef OWL_STRING_COMPARE_OPTS_H
#define OWL_STRING_COMPARE_OPTS_H

#define OWLCMP_SANS_CASSE             1
#define OWLCMP_SANS_ACCENT            2
#define OWLCMP_SANS_ESPACE            4
#define OWLCMP_SANS_ESPACE_INT        8
#define OWLCMP_SANS_PONCTUATION      16
#define OWLCMP_ORDRE_LEXICOGRAPHIQUE 32
#define OWLCMP_RESPECTE_NUMERIQUE    64

/* Compare a et b selon flags. Retourne -1 / 0 / 1. */
int owl_string_compare_with_opts(const char* a, const char* b, int flags);

/* Conversion bitmask TCC (high-byte, lex/num swappes) -> OWLCMP (low-byte). */
int owl_tcc_to_cmp_flags(int tcc_opts);

#endif /* OWL_STRING_COMPARE_OPTS_H */
