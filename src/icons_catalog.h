// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icons_catalog.h — Catalogue d'icones SVG Iconify embarque dans owl.exe.
 *
 * Lot 2a-i : ce header decrit la surface publique du
 * catalogue genere automatiquement au build par le tool
 * `tools/icons_catalog_gen/icons_catalog_gen.c`. Le tool parse les 12
 * JSON sets vendores dans `vendor/iconify-sets/`, applique les filtres
 * (fluent -24-, line-md svg-static only), calcule un xxhash3-64 par
 * body SVG, concatene les bodies en un blob unique compresse zstd
 * niveau 19, et emet le tout sous forme de tableaux statiques C dans
 * `${CMAKE_BINARY_DIR}/icons_catalog_gen.c`.
 *
 * Lot 2a-ii (a venir) cablera le resolver runtime (stdlib_icones.c)
 * sur ce catalogue ; pour l'instant le code generere est link mais
 * dormant : aucun consommateur dans owl_core ne reference encore les
 * symboles ci-dessous. L'objectif Lot 2a-i est de poser l'infra :
 * tool + blob + table.
 *
 * Contrat memoire :
 *   - Toutes les chaines (`name`) pointent dans une zone de strings
 *     concatenees, terminee NUL, durée de vie binaire (static const).
 *   - `OWL_ICONS_BLOB_ZSTD` est le blob compresse, decompresse a la
 *     demande en RAM par le resolver (cache lazy par set, Lot 2a-ii).
 *   - `offset` / `length` reperent le body SVG dans le blob RAW (apres
 *     decompression du blob zstd complet). Le body est un fragment
 *     `<path .../>...` sans enveloppe `<svg ...>` — le resolver wrap
 *     a la volee avec `viewBox` deduit du set.
 *   - `svg_hash` = XXH64 (seed 0) du body wrappe avec son enveloppe
 *     `<svg viewBox='0 0 W H'>...body...</svg>`. Permet la verification
 *     post-decompression et le content-addressing pour le bytecode
 *     utilisateur (cf. ticket DSL Icone V8 §"Couche basse"). XXH64 vs
 *     xxhash3 : le header xxhash vendor zstd patche `XXH_NO_XXH3` qui
 *     masque l'API XXH3 ; XXH64 reste accessible et suffit pour le
 *     content-addressing (pas un usage crypto).
 */
#ifndef OWL_ICONS_CATALOG_H
#define OWL_ICONS_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Une entree = une icone Iconify post-filtre. Le tableau est trie
 * lexicographiquement par (set_id, name) pour permettre une recherche
 * binaire au resolver (Lot 2a-ii). */
typedef struct {
    const char* name;               /* nom Iconify (kebab-case, ex "save"). */
    uint8_t     set_id;             /* indice 0..11 dans OWL_ICONS_SET_NAMES. */
    uint16_t    width;              /* b : 0 = utilise OWL_ICONS_SET_VIEWBOX[set_id]. */
    uint16_t    height;             /* idem. Sinon = viewBox W / H natif de l'icone. */
    uint64_t    svg_hash;           /* xxhash3-64 du body wrappe (cf. doc en tete). */
    uint32_t    offset;             /* offset dans le blob RAW (post-decompression). */
    uint32_t    length;             /* taille du body en octets dans le blob RAW. */
    bool        couleur_modifiable; /* heuristique : `currentColor` present dans body. */
} OwlIconCatalogEntry;

/* Tableau principal, terminé sentinel (`name = NULL`). */
extern const OwlIconCatalogEntry OWL_ICONS_CATALOG[];
extern const size_t              OWL_ICONS_CATALOG_COUNT;

/* Blob compresse zstd niveau 19 contenant la concatenation de tous les
 * body SVG (sans separateur — offsets/length localisent chaque entree). */
extern const unsigned char OWL_ICONS_BLOB_ZSTD[];
extern const size_t        OWL_ICONS_BLOB_ZSTD_SIZE;
extern const size_t        OWL_ICONS_BLOB_RAW_SIZE;

/* Noms des 12 sets, indexes par `set_id`. Ordre = ordre de priorite
 * fige par le ticket (material-symbols en 0, flat-color-icons en 11).
 * Termine par NULL. */
extern const char* const OWL_ICONS_SET_NAMES[];
extern const size_t      OWL_ICONS_SET_COUNT;

/* b — Viewbox natif (W=H) par set_id. Le resolver utilise
 * cette valeur pour wrapper le body SVG avec le bon viewBox au lieu
 * d'un hardcode 24x24 qui faisait tronquer les icones 32x32 / 48x48 /
 * 512x512 (fluent-emoji-flat, flat-color-icons, game-icons, etc.).
 * Indexe par set_id, termine par 0 sentinel. */
extern const int OWL_ICONS_SET_VIEWBOX[];

/* Lookup catalogue brut, sans materialisation Value ni emission d'erreur
 * native (cf. owl_stdlib.h::owl_icones_namespace_lookup pour la variante
 * qui materialise + emet une erreur). Utile pour les consommateurs CLI
 * (cf. cli_doc.c verb `owl icons preview`) qui veulent decider eux-memes
 * du flow d'erreur. Retourne NULL si miss. */
const OwlIconCatalogEntry* owl_icones_catalog_find(const char* name);

/*  — owl_icones_materialize_entry est declaree dans owl_stdlib.h
 * (qui inclut value.h) pour eviter cycle d'include ici. */

/*  — accesseur public au blob raw decompresse (lazy decompress
 * cache global). Permet aux consommateurs CLI (cli_gen_assets) d'iterer
 * OWL_ICONS_CATALOG et lire `body = blob_raw + entry->offset` sans
 * dependre du detail interne de stdlib_icones.c. NULL si decompression
 * a echoue. */
const unsigned char* owl_icones_blob_raw_ensure(void);

#endif /* OWL_ICONS_CATALOG_H */
