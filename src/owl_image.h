// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_image_h
#define owl_image_h

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * OWL_IMAGE — runtime opaque image buffer.
 *
 * Stockage interne : RGBA8 (4 channels x 8 bits = 32 bits par pixel),
 * row-major, top-down (y=0 = ligne du haut). Format unifié quel que
 * soit le format de fichier source (PNG/JPEG/BMP/GIF/SVG/HEIC/...).
 *
 * Backend par défaut : stb_image (header-only, vendorisé). Côté GUI,
 * une variante Qt6 peut être substituée pour intégration native dans
 * les Champ Image — cf src/owl_image_qt.cpp (V2).
 *
 * Couleur encoding : entier 32 bits 0xAABBGGRR (alpha en byte 4),
 * compatible avec le type canon Couleur (3029012, BGR sur 24 bits + alpha).
 * Helpers RVB(R,V,B) / TSL etc. dans stdlib_color.c restent valides.
 * ================================================================ */

#ifndef OWL_IMAGE_TYPEDEF_DECLARED
#define OWL_IMAGE_TYPEDEF_DECLARED 1
typedef struct OwlImage OwlImage;
#endif

/* Couleur encodée 0xAABBGGRR (alpha byte le plus haut). */
typedef uint32_t OwlImageColor;

/* Création / destruction. La couleur de remplissage est utilisée à
 * l'allocation initiale (image vierge) et lors de tout resize qui
 * agrandit l'image. */
OwlImage* owl_image_new_empty(void);
OwlImage* owl_image_new(int width, int height, OwlImageColor fillColor);

/*  — Adopte un buffer RGBA8 top-down (R,G,B,A par pixel, w*h*4
 * bytes) comme contenu d'une nouvelle Image. Le buffer EST COPIE en
 * interne ; l'appelant reste responsable de liberer son buffer source.
 * Retourne NULL si rgba == NULL ou si width/height <= 0. Utilise par la
 * capture Qt (QWidget::grab -> QImage RGBA -> Image OWL). */
OwlImage* owl_image_new_from_rgba(int width, int height, const uint8_t* rgba);

/*  — Libere entierement une OwlImage allouee HORS du chemin GC
 * (buffer pixels + police copiee + struct). A n'utiliser QUE sur une
 * OwlImage* dont on est proprietaire direct et qui n'a PAS ete adoptee par
 * une Image OWL (image_make_instance) : dans ce dernier cas le pointeur est
 * possede cote runtime et son cycle de vie ne t'appartient plus.
 * Usage : chemins C purs generant une image transitoire (regen du Champ
 * CodeBarres au resize). free(NULL) sur les buffers absents est sans effet. */
void owl_image_free(OwlImage* img);

/* Getters (lecture seule). */
int  owl_image_width(const OwlImage* img);
int  owl_image_height(const OwlImage* img);
bool owl_image_valid(const OwlImage* img);
bool owl_image_has_alpha(const OwlImage* img);
int  owl_image_bits_per_pixel(const OwlImage* img);

/* Couleur de remplissage : utilisée lors d'un resize qui ajoute des
 * pixels (Lot I.1 et au-delà). */
OwlImageColor owl_image_fill_color(const OwlImage* img);
void          owl_image_set_fill_color(OwlImage* img, OwlImageColor color);

/* Échelle de dessin : multiplicateur appliqué aux coordonnées des
 * fonctions d* (Lot I.3). En I.0 on stocke juste la valeur. */
double owl_image_draw_scale(const OwlImage* img);
void   owl_image_set_draw_scale(OwlImage* img, double scale);

/* Resize : alloue/réalloue le buffer pixels.
 * - Si l'image était invalide (Largeur=0 ou Hauteur=0), elle devient
 *   valide dès que les deux dimensions sont > 0 ; les pixels sont
 *   remplis avec fillColor.
 * - Si l'image était valide et grandit, les pixels existants sont
 *   préservés (top-left), les nouveaux pixels remplis avec fillColor.
 * - Si elle rétrécit, les pixels en dehors de la nouvelle zone sont
 *   tronqués.
 * - Width=0 ou Height=0 → l'image redevient invalide (free du buffer). */
void owl_image_resize(OwlImage* img, int newWidth, int newHeight);

/* Pixel access (Lot I.2, prototypes posés en I.0 pour cohérence). */
OwlImageColor owl_image_get_pixel(const OwlImage* img, int x, int y);
void          owl_image_set_pixel(OwlImage* img, int x, int y, OwlImageColor color);

/*  : ecrit un pixel avec alpha-blend source-over (alpha sortie
 * = alpha source). Equivalent a plot_blend interne — expose pour dPoint
 * qui doit melanger les composantes RGB tout en preservant l'opacite
 * source dans le canal alpha. */
void          owl_image_blend_pixel(OwlImage* img, int x, int y, OwlImageColor color);

/* Accès direct au buffer RGBA8 (utilisé par le backend GUI Qt pour
 * convertir vers QImage sans copie pixel-à-pixel). NULL si invalide. */
const uint8_t* owl_image_pixels(const OwlImage* img);

/* ----- Load (Lot I.1) ----- */
OwlImage* owl_image_load_from_file(const char* path);

/* SVG rasterisation (Lot I.6). Si target_w/target_h ≤ 0, utilise les
 * dimensions du viewBox SVG. Sinon, rasterise à la taille demandée
 * en préservant les proportions (centre + remplissage transparent
 * pour la zone non couverte). */
OwlImage* owl_image_load_from_svg_file(const char* path,
                                       int target_w, int target_h);

/* ----- Save (Lot I.1) ----- */
bool owl_image_save_png(const OwlImage* img, const char* path);
bool owl_image_save_jpeg(const OwlImage* img, const char* path, int quality);
bool owl_image_save_bmp(const OwlImage* img, const char* path);

/* PNG avec couleur de transparence : tous les pixels dont la composante
 * (R, G, B) matche transparentBGR (24 bits, alpha ignoré) auront leur
 * alpha forcé à 0 dans le fichier produit. L'image source n'est pas
 * mutée — un buffer temporaire est alloué pour la passe. */
bool owl_image_save_png_with_transparent(const OwlImage* img, const char* path,
                                         OwlImageColor transparentBGR);

/* ================================================================
 * Drawing state (Lot I.3a)
 *
 * État stylo + fond + image courante (pattern canon state-machine).
 * dDébutDessin pose l'image courante ; dFinDessin la libère ; les
 * fonctions dStylo/dFond/dEfface/dLigne/... (I.3b+) consomment ce
 * contexte si l'argument <Image> est omis.
 *
 * Par défaut : stylo Noir 1px plein opaque, fond Blanc plein opaque.
 * ================================================================ */

typedef enum {
    OWL_PEN_STYLE_PLEIN              = 0,  /* trait plein (défaut) */
    OWL_PEN_STYLE_TIRET              = 1,  /* --- */
    OWL_PEN_STYLE_POINTILLE          = 2,  /* ... */
    OWL_PEN_STYLE_POINTILLE_TIRET    = 3,  /* .-.- */
    OWL_PEN_STYLE_TIRET_2_POINTILLES = 4,  /* -..-..- */
    OWL_PEN_STYLE_INVISIBLE          = 5
} OwlPenStyle;

/* Stylo (trait) attaché à une image. Modifié par dStylo. */
void          owl_image_set_pen(OwlImage* img, OwlImageColor color,
                                int style, int width, int opacity);
OwlImageColor owl_image_pen_color(const OwlImage* img);
int           owl_image_pen_width(const OwlImage* img);
int           owl_image_pen_style(const OwlImage* img);
int           owl_image_pen_opacity(const OwlImage* img);
/* Vrai si dStylo a été appelé sur cette image (canon : le contour
 * des formes fermées n'est tracé que si stylo explicitement défini
 * ou si l'argument <Trait> est passé). */
bool          owl_image_pen_is_set(const OwlImage* img);

/* Fond (remplissage) attaché à une image. Modifié par dFond.
 * style : 0 plein, 1 transparent, 2 hachuré (cf sentinels.h OWL_FOND_*). */
void          owl_image_set_fond(OwlImage* img, OwlImageColor color,
                                 int style, int hatch, int opacity);
OwlImageColor owl_image_fond_color(const OwlImage* img);
int           owl_image_fond_style(const OwlImage* img);
int           owl_image_fond_opacity(const OwlImage* img);

/* Image courante (state-machine globale, posée par dDébutDessin et
 * libérée par dFinDessin). NULL si aucun dessin en cours. */
OwlImage* owl_image_current(void);
void      owl_image_set_current(OwlImage* img);

/* Efface l'image avec la couleur fournie. Si img invalide, no-op. */
void owl_image_clear(OwlImage* img, OwlImageColor color);

/* ================================================================
 * Drawing primitives (Lot I.3b)
 *
 * Toutes les primitives :
 *   - clipent silencieusement les coordonnées hors limites,
 *   - utilisent l'alpha-blending standard quand la couleur source
 *     n'est pas opaque (alpha ∈ [0..255]),
 *   - traitent (0, 0) comme le coin haut-gauche.
 * ================================================================ */

/* Trace une ligne entre (x1, y1) et (x2, y2) avec la couleur fournie
 * et la largeur indiquée (≥ 1). Algorithme : Bresenham + carré
 * approximatif pour les largeurs > 1. */
void owl_image_draw_line(OwlImage* img, int x1, int y1, int x2, int y2,
                         OwlImageColor color, int width);

/* Remplit le rectangle (x1, y1)..(x2, y2) inclus avec la couleur
 * fournie. Les coordonnées sont clamped aux bornes de l'image. */
void owl_image_fill_rect(OwlImage* img, int x1, int y1, int x2, int y2,
                         OwlImageColor color);

/* Dessine le contour seul du rectangle (x1, y1)..(x2, y2) avec la
 * couleur et la largeur de trait indiquées. */
void owl_image_draw_rect_outline(OwlImage* img, int x1, int y1,
                                 int x2, int y2,
                                 OwlImageColor color, int width);

/* Remplit le rectangle avec un dégradé linéaire entre c1 et c2.
 * angle : 0 = gauche→droite, 90 = haut→bas, 180 = droite→gauche,
 * 270 = bas→haut. Les autres valeurs sont normalisées au multiple de
 * 90° le plus proche. */
void owl_image_fill_rect_gradient(OwlImage* img, int x1, int y1,
                                  int x2, int y2,
                                  OwlImageColor c1, OwlImageColor c2,
                                  int angle);

/* ----- Cercles, ellipses, arcs (Lot I.3c) -----
 *
 * Toutes les primitives prennent le rectangle englobant (x1, y1)..(x2, y2)
 * et en déduisent le centre + rayons (rx, ry). Si rx == ry, c'est un
 * cercle parfait ; sinon une ellipse alignée sur les axes.
 *
 * Pour les arcs : (xs, ys) et (xe, ye) sont des points indicatifs ;
 * seul l'angle vu depuis le centre compte. Si le point n'est pas sur
 * le contour, il est implicitement projeté. L'arc va dans le sens
 * trigonométrique (anti-horaire) de l'angle de start vers celui de end. */
void owl_image_fill_ellipse(OwlImage* img, int x1, int y1, int x2, int y2,
                            OwlImageColor color);

void owl_image_draw_ellipse_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                    OwlImageColor color, int width);

/* Arc : juste le contour entre deux angles. */
void owl_image_draw_arc(OwlImage* img, int x1, int y1, int x2, int y2,
                        int xs, int ys, int xe, int ye,
                        OwlImageColor color, int width);

/* Corde : zone bornée par l'arc et le segment droit reliant les deux
 * extrémités. Le contour est tracé après le remplissage. */
void owl_image_fill_chord(OwlImage* img, int x1, int y1, int x2, int y2,
                          int xs, int ys, int xe, int ye,
                          OwlImageColor color);
void owl_image_draw_chord_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                  int xs, int ys, int xe, int ye,
                                  OwlImageColor color, int width);

/* Portion : zone bornée par l'arc et les deux rayons centre→point. */
void owl_image_fill_pie(OwlImage* img, int x1, int y1, int x2, int y2,
                        int xs, int ys, int xe, int ye,
                        OwlImageColor color);
void owl_image_draw_pie_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                int xs, int ys, int xe, int ye,
                                OwlImageColor color, int width);

/* ----- Polygones / lignes brisées / flood fill (Lot I.3d) -----
 *
 * coords est un tableau plat d'entiers : [x1, y1, x2, y2, ..., xn, yn].
 * count est le nombre de POINTS (donc le tableau a 2*count entiers).
 * Pour fill : algorithme scanline classique (intersections d'arêtes
 * triées par x, remplissage entre paires).
 * Pour outline : N-1 segments dLigne ; closed=true ferme du dernier
 * au premier. */
void owl_image_fill_polygon(OwlImage* img, const int* coords, int count,
                            OwlImageColor color);
void owl_image_draw_polyline(OwlImage* img, const int* coords, int count,
                             OwlImageColor color, int width, bool closed);

/* Flood fill 4-connecté à partir de (x, y).
 *   - Si has_border = false : remplit tous les pixels connexes ayant
 *     la même couleur initiale que (x, y).
 *   - Si has_border = true : remplit jusqu'à rencontrer un pixel de
 *     couleur border_color (boundary fill).
 * La comparaison se fait sur les 24 bits BGR (alpha ignoré). */
void owl_image_flood_fill(OwlImage* img, int x, int y,
                          OwlImageColor fill_color,
                          bool has_border, OwlImageColor border_color);

/* ----- Texte (Lot I.3e) -----
 *
 * Le rendu utilise stb_truetype : on charge un fichier TTF/OTF (ou
 * son contenu en buffer) et on rasterise les glyphes en alpha-blend.
 *
 * Le buffer de police est copié en interne (l'appelant peut libérer
 * son buffer juste après owl_image_set_font_*). Une seule police est
 * active à la fois par image. */

/* Attributs stockés (rendu effectif limité en V1 : on ne synthétise
 * pas le gras / italique sans variant dédié de la police). */
typedef enum {
    OWL_FONT_ATTR_NORMAL    = 0,
    OWL_FONT_ATTR_GRAS      = 1 << 0,
    OWL_FONT_ATTR_ITALIQUE  = 1 << 1,
    OWL_FONT_ATTR_SOULIGNE  = 1 << 2,
    OWL_FONT_ATTR_BARRE     = 1 << 3
} OwlFontAttr;

/* Charge une police TTF/OTF depuis un fichier. Retourne false si le
 * fichier est introuvable, illisible, ou si la police est invalide. */
bool owl_image_set_font_file(OwlImage* img, const char* path,
                             int point_size, int attrs);

/* Idem mais depuis un buffer en mémoire. La police est copiée en
 * interne : l'appelant peut libérer son buffer après l'appel. */
bool owl_image_set_font_memory(OwlImage* img, const uint8_t* data, int size,
                               int point_size, int attrs);

/* Renvoie true si une police valide est actuellement chargée. */
bool owl_image_has_font(const OwlImage* img);

/* Rend la chaîne UTF-8 à la position (x, y) qui désigne le coin
 * haut-gauche du texte (la baseline est calculée depuis l'ascent de
 * la police). La couleur encodée 0xAABBGGRR est utilisée comme teinte
 * source ; l'alpha de chaque pixel est modulé par l'intensité du
 * glyphe rasterisé (0..255). */
void owl_image_draw_text(OwlImage* img, int x, int y,
                         const char* utf8_text,
                         OwlImageColor color);

/* Mesure la largeur en pixels de la chaîne UTF-8 dans la police courante
 * de l'image (kerning inclus). Retourne 0 si l'image est invalide, si
 * aucune police n'est chargée, ou si la chaîne est vide. */
int owl_image_text_width_px(const OwlImage* img, const char* utf8_text);

/* Mesure la hauteur en pixels de la police courante (ascent - descent
 * mis à l'échelle, indépendant du contenu de la chaîne). Retourne 0 si
 * l'image est invalide ou si aucune police n'est chargée. */
int owl_image_text_height_px(const OwlImage* img);

/* ----- Transformations (Lot I.4) -----
 *
 * Ces opérations modifient l'image en place (réallocation du buffer
 * si la taille change). Renvoient false si l'image source est
 * invalide ou si l'opération échoue (allocation, paramètres absurdes).
 *
 * La couleur de remplissage de l'image est utilisée pour les pixels
 * qui apparaissent suite à l'opération (zones vides après rotation
 * ou redimensionnement homothétique). */

/* Modes de redimensionnement (bitmask combinable). Les valeurs sont
 * alignées sur les sentinelles publiques drEtiré=1, drHomothétique=2,
 * drHauteQualité=4 (cf sentinels.h OWL_RESIZE_OPT_*). */
typedef enum {
    OWL_RESIZE_ETIRE         = 0,         /* défaut : pixels étirés */
    OWL_RESIZE_HOMOTHETIQUE  = 2,         /* préserve les proportions, fill */
    OWL_RESIZE_HAUTE_QUALITE = 4          /* interpolation bilinéaire */
} OwlResizeOption;

bool owl_image_resize_to(OwlImage* img, int newW, int newH, int options);

/* Modes de rotation. */
typedef enum {
    OWL_ROTATE_DEFAUT             = 0,    /* agrandit pour contenir */
    OWL_ROTATE_ADAPTE             = 1,    /* tourne puis réduit à la taille initiale */
    OWL_ROTATE_SANS_AGRANDISSEMENT = 2    /* clip à la taille initiale */
} OwlRotateOption;

bool owl_image_rotate(OwlImage* img, double angleDegrees, int options);

/* Découpe une sous-zone (x, y) de taille (w, h). L'image résultante a
 * exactement la taille demandée ; les zones hors limites de la source
 * sont remplies avec CouleurRemplissage. */
bool owl_image_crop(OwlImage* img, int x, int y, int w, int h);

/* Copie la sous-zone (sx, sy, sw, sh) de la source dans la sous-zone
 * (dx, dy, dw, dh) de la destination, avec rééchantillonnage bilinéaire
 * si les tailles diffèrent. Si sw/sh = -1, prend toute la source ; idem
 * pour dw/dh. */
bool owl_image_copy_rect(OwlImage* src, OwlImage* dst,
                         int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh);

/* Fusionne src dans dst (alpha-blend source-over) avec une opacité
 * globale 0..255 modulant l'alpha de chaque pixel source. La taille
 * du bloc fusionné est min(src.size, dst.size) au top-left. */
bool owl_image_merge(OwlImage* dst, const OwlImage* src, int opacity);

/* ----- Effets et modifications de couleurs (Lot I.5) ----- */

/* Inversion R/G/B sur tous les pixels valides (alpha préservé). */
void owl_image_invert_colors(OwlImage* img);

/* Incruste une couleur uniforme avec opacité 0..255. opacity=255
 * remplace tous les pixels par la couleur ; opacity=0 ne change rien. */
void owl_image_colorize(OwlImage* img, OwlImageColor color, int opacity);

/* Modification du contraste : variation entre -100 (gris uniforme)
 * et +100 (saturation maximale). Formule : (v - 128) * (1 + var/100)
 * + 128, clampé 0..255. */
void owl_image_modify_contrast(OwlImage* img, int variation);

/* Multiplie l'alpha de chaque pixel par le facteur 0.0..1.0 (ou plus). */
void owl_image_modify_opacity(OwlImage* img, double factor);

/* Modifications HSL :
 *   delta_hue       : décalage de teinte en degrés (-360..+360, wrap)
 *   delta_saturation: variation -100..+100 (-100 = grayscale)
 *   delta_lightness : variation -100..+100 (+100 = blanc, -100 = noir)
 * Les variations sont additives sur la valeur HSL puis clampées. */
void owl_image_modify_tsl(OwlImage* img, int delta_hue,
                          int delta_saturation, int delta_lightness);

/* Flou box-blur uniforme. Rayon entier (1 = kernel 3x3, 2 = 5x5, ...). */
void owl_image_box_blur(OwlImage* img, int radius);

/* Couleur dominante : moyenne pondérée par alpha des pixels valides.
 * Retourne 0 (noir) si l'image est invalide. */
OwlImageColor owl_image_dominant_color(const OwlImage* img);

#endif
