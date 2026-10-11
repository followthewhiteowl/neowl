// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_DOC_PHANTOM_LINT_H
#define OWL_DOC_PHANTOM_LINT_H

/* Detecteur partage du lint anti-natives-fantomes (, volet B).
 *
 * Une seule fonction, deux surfaces :
 *   - production : Regle 63 de registry_self_test.c, sur le corpus reel
 *     des fiches .md de docs/public_documentation ;
 *   - calibration : harnais --self-test-doc-phantom-lint sur les fixtures
 *     .md de tests/doc-lint-fixtures (catch et ignore).
 *
 * Le lint FLAG tout symbole cite COMME native/fonction dans la PROSE qui
 * est absent du registre (natives/types/consts/enums, formes FR et EN,
 * accent- et casse-insensible). Trois heuristiques, calibrees sur les
 * fixtures (cf. tests/doc-lint-fixtures/CALIBRATION.md) :
 *   1. appel `Nom(...)` avec parentheses ;
 *   2. token backtick TitleCase dans une phrase contenant
 *      "native"/"fonction" ;
 *   3. enumeration parenthesee `(`A`, `B`, `C`)` dont au moins un membre
 *      est un native reel (les autres membres inconnus sont flagges).
 * Exclusions transverses : blocs de code fences ```...```, acces
 * propriete `..x` (invariant 6). */

#define OWL_DOC_PHANTOM_MAX_HITS 256
#define OWL_DOC_PHANTOM_NAME_MAX 128

typedef struct {
    char name[OWL_DOC_PHANTOM_NAME_MAX];
    int  line;   /* 1-based, ligne de la 1re occurrence du symbole flagge */
} OwlDocPhantomHit;

/* Remplit hits[] (borne a maxHits), dedoublonne par nom. Retourne le
 * nombre de hits. Necessite un registre initialise (vm_init prealable). */
int owl_doc_phantom_detect(const char* buf, OwlDocPhantomHit* hits, int maxHits);

#endif /* OWL_DOC_PHANTOM_LINT_H */
