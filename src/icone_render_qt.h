// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icone_render_qt.h — API C pour rasterizing SVG -> RGBA via Qt6Svg.
 *
 * Voir icone_render_qt.cpp pour les details d'impl.
 */

#ifndef OWL_ICONE_RENDER_QT_H
#define OWL_ICONE_RENDER_QT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rasterise un SVG (markup brut) vers un buffer RGBA target_size x target_size.
 *
 *   svg_raw, svg_len : markup SVG (UTF-8, pas force null-terminated).
 *   color_bgr        : couleur (format OWL 0x00BBGGRR) appliquee si want_color.
 *   want_color       : si vrai, substitue "currentColor" -> #RRGGBB avant render.
 *                      Permet a une icone mono-color d'etre teintee.
 *   target_size      : cote du carre cible en pixels (ex 64).
 *   rotation_deg     : rotation en degres autour du centre (0/90/180/270 usuels).
 *
 *   out_rgba, out_w, out_h : sortie. *out_rgba est alloue via malloc ; caller free.
 *
 * Retour : true si succes (renderer valide + >= 1 pixel non transparent),
 *          false sinon. Sur false, *out_rgba reste NULL.
 */
bool owl_icone_rasterize_rgba(const char* svg_raw,
                              int svg_len,
                              unsigned int color_bgr,
                              bool want_color,
                              int target_size,
                              int rotation_deg,
                              int forme,
                              unsigned int bg_color_bgr,
                              bool want_bg,
                              unsigned char** out_rgba,
                              int* out_w,
                              int* out_h);

#ifdef __cplusplus
}
#endif

#endif /* OWL_ICONE_RENDER_QT_H */
