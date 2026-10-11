// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du centrage + clamp de position de fenetre.
 *
 * Exercice la math pure de owl_ui_center_clamp (cf. src/ui_position.c),
 * sans ecran ni Qt : la fonction ne prend que des entiers (geometrie
 * ecran + taille fenetre) et clampe la coordonnee centree au coin
 * haut-gauche de la zone disponible, pour qu'une fenetre plus grande
 * que l'ecran ne pousse pas sa barre de titre hors champ.
 *
 * Cable via --self-test-ui-position (cf. main.c). Retourne le nombre
 * d'echecs (0 = tout vert). */

#include "ui_position.h"

#include <stdio.h>

static int g_pass = 0;
static int g_fail = 0;

#define UIP_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Cas 1 : fenetre geante centree -> clampe au coin (0,0). */
static void test_giant_window_clamps_to_corner(void) {
    int x = -1, y = -1;
    owl_ui_center_clamp(0, 0, 1920, 1080, 3000, 2000, &x, &y);
    UIP_CHECK(x == 0, "geante: x clampe a 0 (pas negatif)");
    UIP_CHECK(y == 0, "geante: y clampe a 0 (pas negatif)");
}

/* Cas 2 : fenetre normale centree -> centrage inchange (non-regression). */
static void test_normal_window_centers(void) {
    int x = -1, y = -1;
    owl_ui_center_clamp(0, 0, 1920, 1080, 640, 480, &x, &y);
    UIP_CHECK(x == 640, "normale: x centre (1920-640)/2");
    UIP_CHECK(y == 300, "normale: y centre (1080-480)/2");
}

/* Cas 3 : origine ecran non nulle -> clampe a l'origine geo, pas 0. */
static void test_giant_window_clamps_to_geo_origin(void) {
    int x = -1, y = -1;
    owl_ui_center_clamp(100, 50, 1920, 1080, 3000, 2000, &x, &y);
    UIP_CHECK(x == 100, "origine: x clampe a geoX=100");
    UIP_CHECK(y == 50, "origine: y clampe a geoY=50");
}

/* Cas 4 : mono-axe. x explicite (>=0) inchange ; seul y centre est clampe. */
static void test_mono_axis_leaves_explicit_untouched(void) {
    int x = 200, y = -1;
    owl_ui_center_clamp(0, 0, 1920, 1080, 3000, 2000, &x, &y);
    UIP_CHECK(x == 200, "mono-axe: x explicite intact (pas de clamp)");
    UIP_CHECK(y == 0, "mono-axe: y centre puis clampe a 0");
}

/* Cas 5 : mono-axe inverse. y explicite intact ; x centre normal (non-clampe). */
static void test_mono_axis_x_centered_y_explicit(void) {
    int x = -1, y = 777;
    owl_ui_center_clamp(0, 0, 1920, 1080, 640, 480, &x, &y);
    UIP_CHECK(x == 640, "mono-axe: x centre (1920-640)/2");
    UIP_CHECK(y == 777, "mono-axe: y explicite intact");
}

int ui_position_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_giant_window_clamps_to_corner();
    test_normal_window_centers();
    test_giant_window_clamps_to_geo_origin();
    test_mono_axis_leaves_explicit_untouched();
    test_mono_axis_x_centered_y_explicit();

    if (g_pass + g_fail == 0) {
        fprintf(stdout, "ui-position: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    if (g_fail > 0) fprintf(stdout, "ui-position: %d FAIL, %d PASS\n", g_fail, g_pass);
    else            fprintf(stdout, "ui-position: %d PASS\n", g_pass);
    return g_fail;
}
