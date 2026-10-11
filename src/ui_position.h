// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_UI_POSITION_H
#define OWL_UI_POSITION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Centre une position de fenetre puis clampe le coin haut-gauche dans la
 * zone d'ecran disponible.
 *
 * Convention d'entree : une coordonnee < 0 demande le centrage sur cet axe.
 * Une coordonnee >= 0 est explicite et laissee intacte (pas de clamp).
 *
 * Pour un axe centre, la coordonnee est calculee comme
 *   geo_origin + (geo_taille - taille_fenetre) / 2
 * puis clampee a geo_origin si le resultat passe dessous (cas d'une fenetre
 * plus grande que l'ecran : sans clamp, la barre de titre sortirait du champ).
 *
 * Math pure, sans dependance Qt/ecran : testable isolement.
 *
 * geoX/geoY  : origine de la zone d'ecran disponible.
 * geoW/geoH  : dimensions de la zone d'ecran disponible.
 * fw/fh      : largeur/hauteur de la fenetre.
 * x/y        : in/out. Modifie uniquement l'axe dont la valeur d'entree < 0. */
void owl_ui_center_clamp(int geoX, int geoY, int geoW, int geoH,
                         int fw, int fh, int* x, int* y);

#ifdef __cplusplus
}
#endif

#endif /* OWL_UI_POSITION_H */
