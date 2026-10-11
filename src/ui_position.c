// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Math pure du centrage + clamp de position de fenetre.
 *
 * Extrait de owl_ui_window_set_position (ui_qt.cpp) pour etre testable sans
 * ecran ni Qt. Vit dans owl_core : lie a toutes les cibles, y compris les
 * builds sans GUI. Voir ui_position.h pour le contrat. */

#include "ui_position.h"

void owl_ui_center_clamp(int geoX, int geoY, int geoW, int geoH,
                         int fw, int fh, int* x, int* y) {
    if (x && *x < 0) {
        int cx = geoX + (geoW - fw) / 2;
        if (cx < geoX) cx = geoX;  /* fenetre plus large que l'ecran : barre de titre visible */
        *x = cx;
    }
    if (y && *y < 0) {
        int cy = geoY + (geoH - fh) / 2;
        if (cy < geoY) cy = geoY;  /* idem verticalement */
        *y = cy;
    }
}
