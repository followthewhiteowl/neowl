// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* encoding_self_test.h - Garde d'hygiene encodage des fichiers source OWL.
 *
 * Scanne recursivement tests/ + src/ + lsp/ (extensions .owl/.c/.h/.cpp/.def)
 * et echoue en blocage dur (exit != 0, casse `make`) si un fichier viole une
 * des 3 regles d'encodage :
 *   1. UTF-8 invalide (RFC 3629 strict : overlong, surrogates, continuations
 *      orphelines rejetes).
 *   2. BOM UTF-8 (EF BB BF en tete) — sauf whitelist par chemin.
 *   3. Signature mojibake C3 83 ('A tilde') : trace du double-encodage UTF-8
 *      (cf. , un .c reencode mojibake avait pourri 4 tests LSP sans
 *      etre detecte).
 *
 * Usage : `owl --self-test-encoding` (cf src/main.c).
 *         Exit 0 si OK, 1 si violations. Lance d'abord un jeu de tests
 *         in-memory (non-regression du detecteur lui-meme), puis le scan
 *         filesystem.
 */

#ifndef OWL_ENCODING_SELF_TEST_H
#define OWL_ENCODING_SELF_TEST_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lance les tests in-memory + le scan des sources, ecrit le rapport sur
 * `out`. Retourne le nombre de violations (0 si tout est propre). */
int encoding_self_test_run(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* OWL_ENCODING_SELF_TEST_H */
