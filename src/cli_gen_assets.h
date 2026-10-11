// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_gen_assets.h — .
 *
 * Generation des artefacts du plugin VSCode (designer panel browse
 * icons) depuis owl.exe : un dossier embed pret a packager dans le
 * VSIX.
 *
 * Output structure ( : format packe, plus de svg/ loose) :
 *   <out-dir>/icons.blob    (bodies SVG concatenes decompresses)
 *   <out-dir>/catalog.json  (index : n/c/k/w/h + o=offset, l=length)
 *   <out-dir>/fr_en.json    (mapping bilingue)
 *   <out-dir>/.stamp        (cache)
 *
 * Pattern coherent avec prop_registry_gen.js / lsp_catalog_gen.c :
 * single-source au build, plugin VSCode statique au runtime.
 */

#ifndef OWL_CLI_GEN_ASSETS_H
#define OWL_CLI_GEN_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Point d'entree CLI. Retourne 0 succes, !=0 erreur (chemin invalide,
 * IO KO, blob decompression failed, etc.). */
int cli_gen_icon_assets(const char* out_dir);

#ifdef __cplusplus
}
#endif

#endif /* OWL_CLI_GEN_ASSETS_H */
