// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_image_stb.c — Implémentation du runtime image basée sur stb_image.
 *
 * En I.0, on pose juste la struct OwlImage et les helpers d'allocation.
 * Le chargement/save (stb_image_load, stbi_write_png) arrive en I.1.
 * Pixel access (lot I.2), dessin (I.3), transformations (I.4), effets
 * (I.5), SVG (I.6), HEIC (I.7), intégration GUI (I.8) suivent.
 */

#include "owl_image.h"

/* stb_image / stb_image_write : header-only, public domain, vendorisés
 * dans vendor/stb/. On force l'implémentation inline ici. */
#define STB_IMAGE_IMPLEMENTATION
/* NB: STBI_NO_STDIO est defini = stbi_load disabled. Pour avoir
 * stbi_load(path, ...) on NE definit PAS la macro. */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Note : on ne définit PAS STBTT_STATIC. Avec static, stb_truetype.h
 * déclare toutes ses fonctions en static, ce qui déclenche
 * `unused-function` sous `-Werror` pour les fonctions de la lib
 * qu'on n'utilise pas (stbtt_GetCodepointShape, etc.). En linkage
 * externe, seules les fonctions effectivement référencées sont
 * conservées par le linker. */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* nanosvg + rasterizer (Lot I.6). Header-only, public domain. On
 * neutralise quelques warnings de lib externes (signed/unsigned,
 * fonctions non utilisées, faux positifs sur initialisation). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct OwlImage {
    int            width;
    int            height;
    uint8_t*       pixels;     /* RGBA8 row-major top-down, NULL si invalide */
    OwlImageColor  fillColor;
    double         drawScale;
    /* Drawing state (Lot I.3a) — stylo + fond, attachés à chaque image. */
    OwlImageColor  pen_color;     /* couleur du trait */
    int            pen_style;     /* OwlPenStyle (0=plein) */
    int            pen_width;     /* largeur en pixels (>=1) */
    int            pen_opacity;   /* 0..255 */
    bool           pen_set;       /* dStylo appelé au moins une fois (canon : trait opt-in) */
    OwlImageColor  fond_color;    /* couleur du fond */
    int            fond_style;    /* OWL_FOND_PLEIN/TRANSPARENT/HACHURE */
    int            fond_hatch;    /* 0..5 (cf doc dFond) */
    int            fond_opacity;  /* 0..255 */
    /* Police courante (Lot I.3e). NULL si aucune police chargée. */
    uint8_t*       font_data;     /* buffer TTF/OTF copié, possédé */
    int            font_data_size;
    stbtt_fontinfo font_info;
    bool           font_inited;
    int            font_point_size;  /* taille en points */
    int            font_attrs;       /* combinaison OwlFontAttr */
};

/* ================================================================
 * INTERNAL HELPERS
 * ================================================================ */

static inline void color_to_rgba8(OwlImageColor c, uint8_t out[4]) {
    /* Encoding 0xAABBGGRR : compat canon Couleur (BGR 24-bit + alpha en haut).
     * On extrait vers RGBA8 ordonné R,G,B,A pour le buffer interne. */
    out[0] = (uint8_t)(c & 0xFF);          /* R = byte bas */
    out[1] = (uint8_t)((c >> 8) & 0xFF);   /* G */
    out[2] = (uint8_t)((c >> 16) & 0xFF);  /* B */
    out[3] = (uint8_t)((c >> 24) & 0xFF);  /* A */
}

static inline OwlImageColor rgba8_to_color(const uint8_t in[4]) {
    return (OwlImageColor)in[0]
         | ((OwlImageColor)in[1] << 8)
         | ((OwlImageColor)in[2] << 16)
         | ((OwlImageColor)in[3] << 24);
}

static void fill_pixels(uint8_t* buf, int count, OwlImageColor color) {
    uint8_t rgba[4];
    color_to_rgba8(color, rgba);
    for (int i = 0; i < count; i++) {
        buf[i * 4 + 0] = rgba[0];
        buf[i * 4 + 1] = rgba[1];
        buf[i * 4 + 2] = rgba[2];
        buf[i * 4 + 3] = rgba[3];
    }
}

/* ================================================================
 * ALLOCATION / RESIZE
 * ================================================================ */

OwlImage* owl_image_new_empty(void) {
    OwlImage* img = (OwlImage*)calloc(1, sizeof(OwlImage));
    if (!img) return NULL;
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;
    /* Couleur de remplissage par défaut : Blanc opaque (cohérent canon
     * où `Blanc` = 0xFFFFFF en BGR — alpha 0xFF ajouté pour opaque). */
    img->fillColor = 0xFFFFFFFFu;
    img->drawScale = 1.0;
    /* Drawing state defaults (Lot I.3a) :
     *   stylo : Noir 1px plein opaque
     *   fond  : Blanc plein opaque                                       */
    img->pen_color    = 0xFF000000u;  /* Noir opaque */
    img->pen_style    = 0;            /* OWL_PEN_STYLE_PLEIN */
    img->pen_width    = 1;
    img->pen_opacity  = 255;
    img->pen_set      = false;        /* canon : trait opt-in tant que dStylo non appelé */
    img->fond_color   = 0xFFFFFFFFu;  /* Blanc opaque */
    img->fond_style   = 0;            /* OWL_FOND_PLEIN */
    img->fond_hatch   = 0;
    img->fond_opacity = 255;
    return img;
}

OwlImage* owl_image_new(int width, int height, OwlImageColor fillColor) {
    OwlImage* img = owl_image_new_empty();
    if (!img) return NULL;
    img->fillColor = fillColor;
    if (width > 0 && height > 0) {
        owl_image_resize(img, width, height);
    }
    return img;
}

/*  — Adopte un buffer RGBA8 externe (typiquement venu d'un grab
 * Qt converti en Format_RGBA8888, qui produit exactement le meme byte
 * layout que notre stockage interne : R, G, B, A par pixel). On copie
 * dans un buffer alloue par malloc pour rester compatible free(). */
OwlImage* owl_image_new_from_rgba(int width, int height, const uint8_t* rgba) {
    if (!rgba || width <= 0 || height <= 0) return NULL;
    OwlImage* img = owl_image_new_empty();
    if (!img) return NULL;
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint8_t* pixels = (uint8_t*)malloc(bytes);
    if (!pixels) { free(img); return NULL; }
    memcpy(pixels, rgba, bytes);
    img->width  = width;
    img->height = height;
    img->pixels = pixels;
    return img;
}

/*  — libere buffer pixels + police + struct. Calque exactement les
 * buffers possedes par OwlImage (pixels via malloc, font_data via malloc dans
 * set_font_*) ; la struct est calloc'ee dans owl_image_new_empty. Le GC ne
 * possede jamais l'OwlImage* directement (il gere l'ObjInstance wrapper qui
 * stocke le pointeur en _ptr), donc appeler ceci sur une image transitoire
 * non-wrappee ne cree pas de double-free. */
void owl_image_free(OwlImage* img) {
    if (!img) return;
    free(img->pixels);
    free(img->font_data);
    free(img);
}

void owl_image_resize(OwlImage* img, int newWidth, int newHeight) {
    if (!img) return;
    if (newWidth < 0)  newWidth  = 0;
    if (newHeight < 0) newHeight = 0;

    /* Cas dégradés : passe à invalide. */
    if (newWidth == 0 || newHeight == 0) {
        free(img->pixels);
        img->pixels = NULL;
        img->width = newWidth;
        img->height = newHeight;
        return;
    }

    size_t newBytes = (size_t)newWidth * (size_t)newHeight * 4;
    uint8_t* newPixels = (uint8_t*)malloc(newBytes);
    if (!newPixels) return;  /* OOM — silent, image inchangée */

    /* Pré-remplissage avec couleur de remplissage. */
    fill_pixels(newPixels, newWidth * newHeight, img->fillColor);

    /* Si une image existait, copier la zone commune (top-left). */
    if (img->pixels && img->width > 0 && img->height > 0) {
        int copyW = (img->width  < newWidth)  ? img->width  : newWidth;
        int copyH = (img->height < newHeight) ? img->height : newHeight;
        for (int y = 0; y < copyH; y++) {
            const uint8_t* src = img->pixels + (size_t)y * img->width * 4;
            uint8_t* dst = newPixels + (size_t)y * newWidth * 4;
            memcpy(dst, src, (size_t)copyW * 4);
        }
    }

    free(img->pixels);
    img->pixels = newPixels;
    img->width = newWidth;
    img->height = newHeight;
}

/* ================================================================
 * GETTERS
 * ================================================================ */

int  owl_image_width(const OwlImage* img)  { return img ? img->width : 0; }
int  owl_image_height(const OwlImage* img) { return img ? img->height : 0; }

bool owl_image_valid(const OwlImage* img) {
    return img && img->pixels != NULL && img->width > 0 && img->height > 0;
}

bool owl_image_has_alpha(const OwlImage* img) {
    /* Stockage interne RGBA8 -> on a toujours un canal alpha disponible
     * dès qu'une image est valide (8 bits dédiés au transparent). */
    return owl_image_valid(img);
}

int owl_image_bits_per_pixel(const OwlImage* img) {
    return owl_image_valid(img) ? 32 : 0;
}

OwlImageColor owl_image_fill_color(const OwlImage* img) {
    return img ? img->fillColor : 0xFFFFFFFFu;
}

void owl_image_set_fill_color(OwlImage* img, OwlImageColor color) {
    if (img) img->fillColor = color;
}

double owl_image_draw_scale(const OwlImage* img) {
    return img ? img->drawScale : 1.0;
}

void owl_image_set_draw_scale(OwlImage* img, double scale) {
    if (img) img->drawScale = scale;
}

/* ================================================================
 * PIXEL ACCESS (utilisé en I.2, posé ici pour cohérence)
 * ================================================================ */

OwlImageColor owl_image_get_pixel(const OwlImage* img, int x, int y) {
    if (!owl_image_valid(img)) return 0;
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) return 0;
    const uint8_t* p = img->pixels + ((size_t)y * img->width + x) * 4;
    return rgba8_to_color(p);
}

void owl_image_set_pixel(OwlImage* img, int x, int y, OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) return;
    uint8_t* p = img->pixels + ((size_t)y * img->width + x) * 4;
    color_to_rgba8(color, p);
}

/*  : owl_image_blend_pixel definie plus bas, apres plot_blend
 * (static, file-local) auquel elle delegue. */

const uint8_t* owl_image_pixels(const OwlImage* img) {
    return img ? img->pixels : NULL;
}

/* ================================================================
 * LOAD (Lot I.1)
 *
 * stb_image décode PNG, JPEG, BMP, GIF (1er frame), TGA, PSD, PIC, PNM.
 * On force le décodage en RGBA8 pour cohérence avec le storage interne.
 * Retourne NULL en cas d'échec (fichier absent, format non reconnu,
 * fichier corrompu) — le caller décide quoi en faire.
 * ================================================================ */

OwlImage* owl_image_load_from_file(const char* path) {
    if (!path) return NULL;
    int w = 0, h = 0, channels = 0;
    /* Force 4 channels (RGBA8) — stb convertit si nécessaire. */
    uint8_t* data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) return NULL;
    if (w <= 0 || h <= 0) { stbi_image_free(data); return NULL; }

    OwlImage* img = owl_image_new_empty();
    if (!img) { stbi_image_free(data); return NULL; }
    /* Buffer stb alloué via malloc compatible avec free, donc on peut
     * l'adopter directement dans img->pixels. */
    img->width = w;
    img->height = h;
    img->pixels = data;
    return img;
}

/* SVG rasterisation (Lot I.6). nanosvg parse le DOM SVG, nanosvgrast
 * rasterise en RGBA8. Le buffer pixels produit est compatible avec
 * notre stockage interne, on l'adopte directement. */
OwlImage* owl_image_load_from_svg_file(const char* path,
                                       int target_w, int target_h) {
    if (!path) return NULL;
    /* Parse à 96 DPI standard. */
    NSVGimage* svg = nsvgParseFromFile(path, "px", 96.0f);
    if (!svg) return NULL;
    if (svg->width <= 0 || svg->height <= 0) {
        nsvgDelete(svg);
        return NULL;
    }

    int w = target_w > 0 ? target_w : (int)(svg->width  + 0.5f);
    int h = target_h > 0 ? target_h : (int)(svg->height + 0.5f);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    /* Échelle préservant les proportions si target_w/h sont fournis
     * différents du ratio SVG. On utilise min(w/W, h/H) et on centre. */
    float scale = 1.0f;
    int draw_w = w, draw_h = h;
    int offset_x = 0, offset_y = 0;
    if (target_w > 0 || target_h > 0) {
        float sx = (float)w / svg->width;
        float sy = (float)h / svg->height;
        scale = sx < sy ? sx : sy;
        draw_w = (int)(svg->width  * scale + 0.5f);
        draw_h = (int)(svg->height * scale + 0.5f);
        offset_x = (w - draw_w) / 2;
        offset_y = (h - draw_h) / 2;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return NULL; }

    /* Le buffer destination doit faire w * h * 4 bytes. nanosvgrast
     * rasterise en RGBA8 row-major, qui est notre format interne. */
    size_t bytes = (size_t)w * (size_t)h * 4;
    uint8_t* pixels = (uint8_t*)calloc(1, bytes);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);
        return NULL;
    }
    nsvgRasterize(rast, svg, (float)offset_x, (float)offset_y, scale,
                  pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    OwlImage* img = owl_image_new_empty();
    if (!img) { free(pixels); return NULL; }
    img->width = w;
    img->height = h;
    img->pixels = pixels;
    return img;
}

/* ================================================================
 * SAVE (Lot I.1)
 *
 * stb_image_write supporte PNG (lossless RGBA), JPEG (lossy RGB-only),
 * BMP (RGB ou RGBA selon version). Pour JPEG, alpha ignoré.
 * Retourne true si OK, false sinon (path invalide, OOM, etc.).
 * ================================================================ */

bool owl_image_save_png(const OwlImage* img, const char* path) {
    if (!owl_image_valid(img) || !path) return false;
    /* stride en bytes par ligne = width * 4 (RGBA8). */
    int rc = stbi_write_png(path, img->width, img->height, 4,
                            img->pixels, img->width * 4);
    return rc != 0;
}

bool owl_image_save_jpeg(const OwlImage* img, const char* path, int quality) {
    if (!owl_image_valid(img) || !path) return false;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    /* stb_image_write accepte 4 channels mais ignore l'alpha pour JPEG. */
    int rc = stbi_write_jpg(path, img->width, img->height, 4,
                            img->pixels, quality);
    return rc != 0;
}

bool owl_image_save_bmp(const OwlImage* img, const char* path) {
    if (!owl_image_valid(img) || !path) return false;
    int rc = stbi_write_bmp(path, img->width, img->height, 4, img->pixels);
    return rc != 0;
}

bool owl_image_save_png_with_transparent(const OwlImage* img, const char* path,
                                         OwlImageColor transparentBGR) {
    if (!owl_image_valid(img) || !path) return false;
    /* Copie temporaire pour ne pas muter l'image source. Comparaison en
     * (R, G, B) seulement (alpha ignoré côté transparentBGR). */
    size_t bytes = (size_t)img->width * (size_t)img->height * 4;
    uint8_t* copy = (uint8_t*)malloc(bytes);
    if (!copy) return false;
    memcpy(copy, img->pixels, bytes);

    uint8_t targetR, targetG, targetB;
    {
        uint8_t rgba[4];
        color_to_rgba8((OwlImageColor)transparentBGR & 0x00FFFFFFu, rgba);
        targetR = rgba[0];
        targetG = rgba[1];
        targetB = rgba[2];
    }
    int total = img->width * img->height;
    for (int i = 0; i < total; i++) {
        if (copy[i * 4 + 0] == targetR &&
            copy[i * 4 + 1] == targetG &&
            copy[i * 4 + 2] == targetB) {
            copy[i * 4 + 3] = 0;  /* alpha = transparent */
        }
    }
    int rc = stbi_write_png(path, img->width, img->height, 4,
                            copy, img->width * 4);
    free(copy);
    return rc != 0;
}

/* ================================================================
 * DRAWING STATE (Lot I.3a)
 * ================================================================ */

void owl_image_set_pen(OwlImage* img, OwlImageColor color,
                       int style, int width, int opacity) {
    if (!img) return;
    img->pen_color   = color;
    img->pen_style   = style;
    if (width < 1) width = 1;
    img->pen_width   = width;
    if (opacity < 0)   opacity = 0;
    if (opacity > 255) opacity = 255;
    img->pen_opacity = opacity;
    img->pen_set     = true;
}

OwlImageColor owl_image_pen_color(const OwlImage* img) {
    return img ? img->pen_color : 0xFF000000u;
}
int owl_image_pen_width(const OwlImage* img)   { return img ? img->pen_width   : 1; }
int owl_image_pen_style(const OwlImage* img)   { return img ? img->pen_style   : 0; }
int owl_image_pen_opacity(const OwlImage* img) { return img ? img->pen_opacity : 255; }
bool owl_image_pen_is_set(const OwlImage* img) { return img ? img->pen_set     : false; }

void owl_image_set_fond(OwlImage* img, OwlImageColor color,
                        int style, int hatch, int opacity) {
    if (!img) return;
    img->fond_color  = color;
    img->fond_style  = style;
    img->fond_hatch  = hatch;
    if (opacity < 0)   opacity = 0;
    if (opacity > 255) opacity = 255;
    img->fond_opacity = opacity;
}

OwlImageColor owl_image_fond_color(const OwlImage* img) {
    return img ? img->fond_color : 0xFFFFFFFFu;
}
int owl_image_fond_style(const OwlImage* img)   { return img ? img->fond_style   : 0; }
int owl_image_fond_opacity(const OwlImage* img) { return img ? img->fond_opacity : 255; }

/* État global "image courante" (cible des fonctions d* sans <Image>
 * explicite). Posé par dDébutDessin, libéré par dFinDessin. NULL si
 * aucune session de dessin en cours.
 *
 * Note : non thread-safe (pas grave en V1, OWL est mono-thread).
 * À revisiter si on attaque le multithread (cf ROADMAP). */
static OwlImage* g_current_image = NULL;

OwlImage* owl_image_current(void)              { return g_current_image; }
void      owl_image_set_current(OwlImage* img) { g_current_image = img; }

void owl_image_clear(OwlImage* img, OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    fill_pixels(img->pixels, img->width * img->height, color);
}

/* ================================================================
 * DRAWING PRIMITIVES (Lot I.3b)
 *
 * Toutes les primitives clip silencieusement et utilisent un alpha-
 * blending source-over standard. Si l'alpha source vaut 255, on
 * remplace le pixel ; sinon on mixe avec celui présent.
 * ================================================================ */

/* Pose un pixel avec alpha-blending source-over. Si l'image est
 * invalide ou (x, y) hors bornes, no-op silencieux. */
static inline void plot_blend(OwlImage* img, int x, int y, OwlImageColor src) {
    if (!img->pixels) return;
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) return;
    uint8_t* p = img->pixels + ((size_t)y * img->width + x) * 4;
    uint8_t srgba[4];
    color_to_rgba8(src, srgba);
    int sa = srgba[3];
    if (sa == 0) return;       /* totalement transparent : rien à faire */
    if (sa == 255) {
        p[0] = srgba[0]; p[1] = srgba[1]; p[2] = srgba[2]; p[3] = 255;
        return;
    }
    /*  : source-over canonique pour les composantes RGB, mais
     * l'alpha SORTIE prend la valeur de l'opacite source (canon
     * dStylo opacite N : le pixel rendu porte cette opacite, et n'est
     * pas sature a 255 par le canvas opaque sous-jacent). */
    p[0] = (uint8_t)((srgba[0] * sa + p[0] * (255 - sa) + 127) / 255);
    p[1] = (uint8_t)((srgba[1] * sa + p[1] * (255 - sa) + 127) / 255);
    p[2] = (uint8_t)((srgba[2] * sa + p[2] * (255 - sa) + 127) / 255);
    p[3] = (uint8_t)sa;
}

/*  : API publique vers plot_blend, utilisee par fn_dPoint pour
 * le cas opacite < 255 (alpha-blend RGB, alpha sortie = alpha source). */
void owl_image_blend_pixel(OwlImage* img, int x, int y, OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    plot_blend(img, x, y, color);
}

/* Pose un point épais (width × width pixels centré sur x, y). */
static void plot_thick(OwlImage* img, int cx, int cy,
                       OwlImageColor color, int width) {
    if (width <= 1) {
        plot_blend(img, cx, cy, color);
        return;
    }
    int half = width / 2;
    for (int dy = -half; dy < width - half; dy++) {
        for (int dx = -half; dx < width - half; dx++) {
            plot_blend(img, cx + dx, cy + dy, color);
        }
    }
}

void owl_image_draw_line(OwlImage* img, int x1, int y1, int x2, int y2,
                         OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    if (width < 1) width = 1;
    /* Bresenham standard. */
    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;
    for (;;) {
        plot_thick(img, x, y, color, width);
        if (x == x2 && y == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

void owl_image_fill_rect(OwlImage* img, int x1, int y1, int x2, int y2,
                         OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    /* Normaliser : (x1,y1) = top-left, (x2,y2) = bottom-right inclus. */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    /* Clamp aux bornes de l'image. */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= img->width)  x2 = img->width  - 1;
    if (y2 >= img->height) y2 = img->height - 1;
    if (x1 > x2 || y1 > y2) return;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            plot_blend(img, x, y, color);
        }
    }
}

void owl_image_draw_rect_outline(OwlImage* img, int x1, int y1,
                                 int x2, int y2,
                                 OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    if (width < 1) width = 1;
    /* Normaliser. */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    /* 4 segments. Pour width=1, c'est juste 4 lignes ; pour width>1,
     * plot_thick élargit chaque pixel. */
    owl_image_draw_line(img, x1, y1, x2, y1, color, width);   /* top */
    owl_image_draw_line(img, x1, y2, x2, y2, color, width);   /* bottom */
    owl_image_draw_line(img, x1, y1, x1, y2, color, width);   /* left */
    owl_image_draw_line(img, x2, y1, x2, y2, color, width);   /* right */
}

/* ================================================================
 * ELLIPSES, ARCS, CORDES, PORTIONS (Lot I.3c)
 *
 * Implémentation : test point-in-shape par scan de la bbox. Simple,
 * correct, pas le plus rapide mais OK pour V1 sur les tailles
 * courantes. Pour les gros cercles (>500px de rayon) on aura intérêt
 * à passer en midpoint ellipse + scan-line en V2.
 *
 * Repère écran : y croît vers le bas. Pour les angles canon
 * (sens trigonométrique standard, 0 = est, 90 = nord, etc.), on
 * inverse le signe de dy au calcul des angles.
 * ================================================================ */

/* Calcul l'angle (en radians, plage [-π, π]) d'un point par rapport
 * au centre de l'ellipse. Tient compte de l'inversion d'axe Y et
 * normalise via les rayons pour fonctionner aussi sur ellipses. */
static double angle_at_point(int cx, int cy, int rx, int ry,
                             int px, int py) {
    if (rx == 0) rx = 1;
    if (ry == 0) ry = 1;
    /* Normaliser dans l'espace cercle unitaire pour que l'angle
     * paramétrique soit cohérent sur les ellipses. */
    double dx = (double)(px - cx) / (double)rx;
    double dy = (double)(cy - py) / (double)ry;  /* y inversé */
    return atan2(dy, dx);
}

/* Renvoie true si le point (px, py) est dans la zone de l'arc défini
 * par les angles (start, end) en sens trigonométrique. Gère le wrap. */
static bool angle_in_arc(double a, double start, double end) {
    /* Normalise a, start, end à [0, 2π). */
    const double TAU = 2.0 * 3.14159265358979323846;
    a     = fmod(a     + TAU, TAU);
    start = fmod(start + TAU, TAU);
    end   = fmod(end   + TAU, TAU);
    if (start <= end) return (a >= start && a <= end);
    /* Wrap : ex. start = 350°, end = 10° → arc traverse 0. */
    return (a >= start || a <= end);
}

/* Test point dans ellipse alignée. */
static bool point_in_ellipse(int cx, int cy, int rx, int ry, int px, int py) {
    if (rx <= 0 || ry <= 0) return false;
    double nx = (double)(px - cx) / (double)rx;
    double ny = (double)(py - cy) / (double)ry;
    return (nx * nx + ny * ny) <= 1.0;
}

void owl_image_fill_ellipse(OwlImage* img, int x1, int y1, int x2, int y2,
                            OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int cx2 = x1 + x2;  /* 2*cx, on évite la div par 2 pour précision */
    int cy2 = y1 + y2;
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return;
    /* Centre = (cx2/2, cy2/2). Pour la précision on travaille en
     * coordonnées doubles. */
    double cx = cx2 / 2.0;
    double cy = cy2 / 2.0;
    /* Clamp aux bornes de l'image. */
    int sx = x1 < 0 ? 0 : x1;
    int sy = y1 < 0 ? 0 : y1;
    int ex = x2 >= img->width  ? img->width  - 1 : x2;
    int ey = y2 >= img->height ? img->height - 1 : y2;
    for (int py = sy; py <= ey; py++) {
        for (int px = sx; px <= ex; px++) {
            double nx = (px - cx) / (double)rx;
            double ny = (py - cy) / (double)ry;
            if (nx * nx + ny * ny <= 1.0) {
                plot_blend(img, px, py, color);
            }
        }
    }
}

void owl_image_draw_ellipse_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                    OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return;
    /* Échantillonnage paramétrique : N points = max(rx, ry) * 4
     * (assez pour avoir < 1 pixel entre échantillons). */
    int rmax = rx > ry ? rx : ry;
    int n = rmax * 4;
    if (n < 16) n = 16;
    double cx = (x1 + x2) / 2.0;
    double cy = (y1 + y2) / 2.0;
    if (width < 1) width = 1;
    for (int i = 0; i < n; i++) {
        double t = (i * 2.0 * 3.14159265358979323846) / n;
        int px = (int)(cx + rx * cos(t) + 0.5);
        int py = (int)(cy + ry * sin(t) + 0.5);
        plot_thick(img, px, py, color, width);
    }
}

void owl_image_draw_arc(OwlImage* img, int x1, int y1, int x2, int y2,
                        int xs, int ys, int xe, int ye,
                        OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return;
    int cx_i = (x1 + x2) / 2;
    int cy_i = (y1 + y2) / 2;
    double cx = (x1 + x2) / 2.0;
    double cy = (y1 + y2) / 2.0;
    /* Angles paramétriques start/end (sens trigonométrique). */
    double a_start = angle_at_point(cx_i, cy_i, rx, ry, xs, ys);
    double a_end   = angle_at_point(cx_i, cy_i, rx, ry, xe, ye);
    /* Échantillonnage. Sens trigonométrique = anti-horaire dans le
     * repère mathématique = horaire à l'écran (y inversé). On va de
     * a_start vers a_end en augmentant l'angle. */
    const double TAU = 2.0 * 3.14159265358979323846;
    double span = a_end - a_start;
    while (span <  0)   span += TAU;
    while (span > TAU)  span -= TAU;
    int rmax = rx > ry ? rx : ry;
    int n = (int)(rmax * span * 4.0 / TAU);
    if (n < 8) n = 8;
    if (width < 1) width = 1;
    for (int i = 0; i <= n; i++) {
        double t = a_start + span * i / n;
        /* Repère écran : sin inversé pour faire face en bas du repère
         * mathématique. cos garde son signe. */
        int px = (int)(cx + rx * cos(t) + 0.5);
        int py = (int)(cy - ry * sin(t) + 0.5);
        plot_thick(img, px, py, color, width);
    }
}

void owl_image_fill_chord(OwlImage* img, int x1, int y1, int x2, int y2,
                          int xs, int ys, int xe, int ye,
                          OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return;
    int cx_i = (x1 + x2) / 2;
    int cy_i = (y1 + y2) / 2;
    /* Côté de la corde où se trouve l'arc : on regarde le signe du
     * produit en croix d'un point sur l'arc à mi-chemin entre xs/ys
     * et xe/ye (ce milieu d'arc est forcément du bon côté). */
    double a_start = angle_at_point(cx_i, cy_i, rx, ry, xs, ys);
    double a_end   = angle_at_point(cx_i, cy_i, rx, ry, xe, ye);
    const double TAU = 2.0 * 3.14159265358979323846;
    double span = a_end - a_start;
    while (span <  0)   span += TAU;
    while (span > TAU)  span -= TAU;
    double a_mid = a_start + span / 2.0;
    double mx = cx_i + rx * cos(a_mid);
    double my = cy_i - ry * sin(a_mid);

    /* Direction de la corde et signe attendu côté arc. */
    int dx_chord = xe - xs;
    int dy_chord = ye - ys;
    /* Cross product (mid - xs) x (chord). > 0 ou < 0 selon le côté. */
    double sign_arc = (mx - xs) * dy_chord - (my - ys) * dx_chord;

    /* Clamp aux bornes. */
    int sx = x1 < 0 ? 0 : x1;
    int sy = y1 < 0 ? 0 : y1;
    int ex = x2 >= img->width  ? img->width  - 1 : x2;
    int ey = y2 >= img->height ? img->height - 1 : y2;
    for (int py = sy; py <= ey; py++) {
        for (int px = sx; px <= ex; px++) {
            if (!point_in_ellipse(cx_i, cy_i, rx, ry, px, py)) continue;
            double s = (double)(px - xs) * dy_chord - (double)(py - ys) * dx_chord;
            /* Même signe que mid → côté arc. Le 0 (sur la corde) est
             * inclus pour ne pas laisser de trou visuel. */
            bool same_side = (sign_arc >= 0 && s >= 0) ||
                             (sign_arc <  0 && s <= 0);
            if (same_side) {
                plot_blend(img, px, py, color);
            }
        }
    }
}

void owl_image_draw_chord_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                  int xs, int ys, int xe, int ye,
                                  OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    /* Arc + corde droite. */
    owl_image_draw_arc(img, x1, y1, x2, y2, xs, ys, xe, ye, color, width);
    owl_image_draw_line(img, xs, ys, xe, ye, color, width);
}

void owl_image_fill_pie(OwlImage* img, int x1, int y1, int x2, int y2,
                        int xs, int ys, int xe, int ye,
                        OwlImageColor color) {
    if (!owl_image_valid(img)) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx <= 0 || ry <= 0) return;
    int cx = (x1 + x2) / 2;
    int cy = (y1 + y2) / 2;
    double a_start = angle_at_point(cx, cy, rx, ry, xs, ys);
    double a_end   = angle_at_point(cx, cy, rx, ry, xe, ye);

    int sx = x1 < 0 ? 0 : x1;
    int sy = y1 < 0 ? 0 : y1;
    int ex = x2 >= img->width  ? img->width  - 1 : x2;
    int ey = y2 >= img->height ? img->height - 1 : y2;
    for (int py = sy; py <= ey; py++) {
        for (int px = sx; px <= ex; px++) {
            if (!point_in_ellipse(cx, cy, rx, ry, px, py)) continue;
            double a = angle_at_point(cx, cy, rx, ry, px, py);
            if (angle_in_arc(a, a_start, a_end)) {
                plot_blend(img, px, py, color);
            }
        }
    }
}

void owl_image_draw_pie_outline(OwlImage* img, int x1, int y1, int x2, int y2,
                                int xs, int ys, int xe, int ye,
                                OwlImageColor color, int width) {
    if (!owl_image_valid(img)) return;
    int cx = (x1 + x2) / 2;
    int cy = (y1 + y2) / 2;
    /* Arc + 2 rayons centre→point. */
    owl_image_draw_arc(img, x1, y1, x2, y2, xs, ys, xe, ye, color, width);
    owl_image_draw_line(img, cx, cy, xs, ys, color, width);
    owl_image_draw_line(img, cx, cy, xe, ye, color, width);
}

/* ================================================================
 * POLYGONES, POLYLIGNES, FLOOD FILL (Lot I.3d)
 * ================================================================ */

void owl_image_fill_polygon(OwlImage* img, const int* coords, int count,
                            OwlImageColor color) {
    if (!owl_image_valid(img) || !coords || count < 3) return;

    /* Bornes verticales du polygone, clampées à l'image. */
    int ymin = coords[1];
    int ymax = coords[1];
    for (int i = 1; i < count; i++) {
        int y = coords[2 * i + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= img->height) ymax = img->height - 1;
    if (ymin > ymax) return;

    /* Pour chaque ligne y, on calcule les x d'intersection avec les
     * arêtes du polygone, on trie, et on remplit entre paires. La
     * limite max d'intersections par ligne est `count`. */
    int* xs = (int*)malloc(sizeof(int) * count);
    if (!xs) return;

    for (int y = ymin; y <= ymax; y++) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            int j = (i + 1) % count;
            int xi = coords[2 * i];
            int yi = coords[2 * i + 1];
            int xj = coords[2 * j];
            int yj = coords[2 * j + 1];
            /* L'arête traverse-t-elle la ligne scanline y ? On utilise
             * la convention "min inclus, max exclus" pour éviter les
             * doubles comptages aux sommets. */
            if ((yi <= y && yj >  y) || (yj <= y && yi >  y)) {
                /* Intersection x = xi + (y - yi) * (xj - xi) / (yj - yi). */
                double xinter = xi + (double)(y - yi) * (xj - xi) / (double)(yj - yi);
                xs[n++] = (int)(xinter + 0.5);
            }
        }
        /* Tri par insertion (n petit en pratique). */
        for (int i = 1; i < n; i++) {
            int v = xs[i], k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        /* Remplir entre paires. */
        for (int i = 0; i + 1 < n; i += 2) {
            int xa = xs[i], xb = xs[i + 1];
            if (xa < 0) xa = 0;
            if (xb >= img->width) xb = img->width - 1;
            for (int x = xa; x <= xb; x++) {
                plot_blend(img, x, y, color);
            }
        }
    }
    free(xs);
}

void owl_image_draw_polyline(OwlImage* img, const int* coords, int count,
                             OwlImageColor color, int width, bool closed) {
    if (!owl_image_valid(img) || !coords || count < 2) return;
    if (width < 1) width = 1;
    for (int i = 0; i + 1 < count; i++) {
        owl_image_draw_line(img,
            coords[2 * i],     coords[2 * i + 1],
            coords[2 * i + 2], coords[2 * i + 3],
            color, width);
    }
    if (closed && count >= 3) {
        owl_image_draw_line(img,
            coords[2 * (count - 1)],     coords[2 * (count - 1) + 1],
            coords[0],                   coords[1],
            color, width);
    }
}

/* ================================================================
 * TEXTE (Lot I.3e) — rasterizer stb_truetype
 * ================================================================ */

bool owl_image_set_font_memory(OwlImage* img, const uint8_t* data, int size,
                               int point_size, int attrs) {
    if (!img || !data || size <= 0) return false;
    /* Copie interne pour que stbtt_fontinfo reste valide même si le
     * buffer fourni par l'appelant est libéré. */
    uint8_t* copy = (uint8_t*)malloc((size_t)size);
    if (!copy) return false;
    memcpy(copy, data, (size_t)size);
    /* Initialise la police. stbtt_InitFont prend un offset = 0 pour
     * les TTF simples ; pour les TTC (collections) il faudrait
     * énumérer via stbtt_GetFontOffsetForIndex. Pour V1 on prend
     * l'index 0 systématiquement. */
    int offset = stbtt_GetFontOffsetForIndex(copy, 0);
    if (offset < 0) offset = 0;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, copy, offset)) {
        free(copy);
        return false;
    }
    /* Remplacer la police existante. */
    free(img->font_data);
    img->font_data      = copy;
    img->font_data_size = size;
    img->font_info      = info;
    img->font_inited    = true;
    img->font_point_size = point_size > 0 ? point_size : 12;
    img->font_attrs     = attrs;
    return true;
}

bool owl_image_set_font_file(OwlImage* img, const char* path,
                             int point_size, int attrs) {
    if (!img || !path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (1 << 28)) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) { free(buf); return false; }
    bool ok = owl_image_set_font_memory(img, buf, (int)sz, point_size, attrs);
    free(buf);
    return ok;
}

bool owl_image_has_font(const OwlImage* img) {
    return img && img->font_inited;
}

/* Décode un codepoint UTF-8 en partant de *p. Avance *p au caractère
 * suivant. Retourne -1 sur séquence invalide (et avance d'un byte). */
static int decode_utf8(const char** p, const char* end) {
    if (!p || !*p || *p >= end) return -1;
    const unsigned char* s = (const unsigned char*)*p;
    int c = *s;
    if (c < 0x80) {
        (*p)++;
        return c;
    }
    int extra, mask;
    if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; }
    else { (*p)++; return -1; }
    int cp = c & mask;
    for (int i = 0; i < extra; i++) {
        if ((const char*)(s + 1 + i) >= end) { (*p)++; return -1; }
        unsigned char nb = s[1 + i];
        if ((nb & 0xC0) != 0x80) { (*p)++; return -1; }
        cp = (cp << 6) | (nb & 0x3F);
    }
    *p += 1 + extra;
    return cp;
}

void owl_image_draw_text(OwlImage* img, int x, int y,
                         const char* utf8_text, OwlImageColor color) {
    if (!owl_image_valid(img) || !owl_image_has_font(img) || !utf8_text) return;

    float scale = stbtt_ScaleForPixelHeight(&img->font_info,
                                            (float)img->font_point_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&img->font_info, &ascent, &descent, &lineGap);
    int baseline = y + (int)(ascent * scale);

    /* Couleur source : on extrait R/G/B et l'alpha global. Pour
     * chaque pixel du glyphe on multiplie son intensité (0..255) par
     * cet alpha pour obtenir le composant alpha effectif. */
    uint8_t srcR = (uint8_t)(color & 0xFFu);
    uint8_t srcG = (uint8_t)((color >>  8) & 0xFFu);
    uint8_t srcB = (uint8_t)((color >> 16) & 0xFFu);
    uint8_t srcA = (uint8_t)((color >> 24) & 0xFFu);
    if (srcA == 0) srcA = 255;  /* couleur sans alpha explicite -> opaque */

    int cursor_x = x;
    const char* p   = utf8_text;
    const char* end = utf8_text + strlen(utf8_text);
    int prev_cp = 0;
    while (p < end) {
        int cp = decode_utf8(&p, end);
        if (cp < 0 || cp == 0) continue;

        /* Kerning entre le glyphe précédent et le courant. */
        if (prev_cp != 0) {
            int kern = stbtt_GetCodepointKernAdvance(&img->font_info, prev_cp, cp);
            cursor_x += (int)(kern * scale);
        }

        int adv, lsb;
        stbtt_GetCodepointHMetrics(&img->font_info, cp, &adv, &lsb);

        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(
            &img->font_info, 0.0f, scale, cp, &gw, &gh, &xoff, &yoff);

        if (bitmap && gw > 0 && gh > 0) {
            int gx0 = cursor_x + (int)(lsb * scale) + xoff;
            int gy0 = baseline + yoff;
            for (int gy = 0; gy < gh; gy++) {
                int py = gy0 + gy;
                if (py < 0 || py >= img->height) continue;
                for (int gx = 0; gx < gw; gx++) {
                    int px = gx0 + gx;
                    if (px < 0 || px >= img->width) continue;
                    uint8_t intensity = bitmap[gy * gw + gx];
                    if (intensity == 0) continue;
                    /* Alpha composite : intensité * srcA / 255. */
                    int a = (intensity * srcA + 127) / 255;
                    /* Construire la couleur 0xAABBGGRR. */
                    OwlImageColor c =
                          ((OwlImageColor)a    << 24)
                        | ((OwlImageColor)srcB << 16)
                        | ((OwlImageColor)srcG <<  8)
                        | ((OwlImageColor)srcR);
                    plot_blend(img, px, py, c);
                }
            }
        }
        if (bitmap) stbtt_FreeBitmap(bitmap, NULL);

        cursor_x += (int)(adv * scale);
        prev_cp = cp;
    }

    /* Soulignage / barré : trait horizontal sous / sur le baseline.
     * Largeur estimée depuis la police (distance baseline au descent). */
    int text_pixel_width = cursor_x - x;
    if (text_pixel_width > 0 && img->font_attrs &
        (OWL_FONT_ATTR_SOULIGNE | OWL_FONT_ATTR_BARRE)) {
        int line_thickness = (int)((float)img->font_point_size / 12.0f);
        if (line_thickness < 1) line_thickness = 1;
        OwlImageColor line_color = color;
        if (((line_color >> 24) & 0xFFu) == 0) line_color |= 0xFF000000u;
        if (img->font_attrs & OWL_FONT_ATTR_SOULIGNE) {
            int ly = baseline + (int)(-descent * scale * 0.5f);
            for (int t = 0; t < line_thickness; t++) {
                owl_image_draw_line(img, x, ly + t, x + text_pixel_width, ly + t,
                                    line_color, 1);
            }
        }
        if (img->font_attrs & OWL_FONT_ATTR_BARRE) {
            int ly = baseline - (int)(ascent * scale * 0.30f);
            for (int t = 0; t < line_thickness; t++) {
                owl_image_draw_line(img, x, ly + t, x + text_pixel_width, ly + t,
                                    line_color, 1);
            }
        }
    }
}

int owl_image_text_width_px(const OwlImage* img, const char* utf8_text) {
    if (!owl_image_valid(img) || !owl_image_has_font(img) || !utf8_text) return 0;

    /* On cast const-away pour accéder à font_info (stbtt n'a pas d'API
     * const-aware ; les appels de mesure ne mutent pas la struct). */
    OwlImage* mimg = (OwlImage*)img;
    float scale = stbtt_ScaleForPixelHeight(&mimg->font_info,
                                            (float)mimg->font_point_size);

    float cursor_x = 0.0f;
    const char* p   = utf8_text;
    const char* end = utf8_text + strlen(utf8_text);
    int prev_cp = 0;
    while (p < end) {
        int cp = decode_utf8(&p, end);
        if (cp < 0 || cp == 0) continue;

        if (prev_cp != 0) {
            int kern = stbtt_GetCodepointKernAdvance(&mimg->font_info, prev_cp, cp);
            cursor_x += kern * scale;
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&mimg->font_info, cp, &adv, &lsb);
        cursor_x += adv * scale;
        prev_cp = cp;
    }
    return (int)(cursor_x + 0.5f);
}

int owl_image_text_height_px(const OwlImage* img) {
    if (!owl_image_valid(img) || !owl_image_has_font(img)) return 0;
    OwlImage* mimg = (OwlImage*)img;
    float scale = stbtt_ScaleForPixelHeight(&mimg->font_info,
                                            (float)mimg->font_point_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&mimg->font_info, &ascent, &descent, &lineGap);
    (void)lineGap;
    /* Hauteur de glyphe = ascent - descent (descent est négatif chez
     * stbtt). On arrondit au plus proche pour exposer un entier
     * pixel-exact. */
    float h = (ascent - descent) * scale;
    if (h < 0) h = -h;
    return (int)(h + 0.5f);
}

/* ================================================================
 * TRANSFORMATIONS (Lot I.4)
 * ================================================================ */

/* Helper : échantillonne la couleur d'un pixel source en bilinéaire.
 * (sx, sy) en coordonnées flottantes. Retourne 0 (transparent) si
 * hors bornes. */
static OwlImageColor sample_bilinear(const uint8_t* pixels, int w, int h,
                                     double sx, double sy) {
    if (sx < 0 || sy < 0 || sx > w - 1 || sy > h - 1) return 0;
    int ix0 = (int)sx;
    int iy0 = (int)sy;
    int ix1 = ix0 + 1 >= w ? ix0 : ix0 + 1;
    int iy1 = iy0 + 1 >= h ? iy0 : iy0 + 1;
    double fx = sx - ix0;
    double fy = sy - iy0;
    const uint8_t* p00 = pixels + (iy0 * w + ix0) * 4;
    const uint8_t* p10 = pixels + (iy0 * w + ix1) * 4;
    const uint8_t* p01 = pixels + (iy1 * w + ix0) * 4;
    const uint8_t* p11 = pixels + (iy1 * w + ix1) * 4;
    uint8_t out[4];
    for (int c = 0; c < 4; c++) {
        double v0 = p00[c] * (1.0 - fx) + p10[c] * fx;
        double v1 = p01[c] * (1.0 - fx) + p11[c] * fx;
        out[c] = (uint8_t)(v0 * (1.0 - fy) + v1 * fy + 0.5);
    }
    return rgba8_to_color(out);
}

bool owl_image_resize_to(OwlImage* img, int newW, int newH, int options) {
    if (!owl_image_valid(img) || newW <= 0 || newH <= 0) return false;
    int oldW = img->width;
    int oldH = img->height;
    if (oldW == newW && oldH == newH) return true;

    bool homothetique = (options & OWL_RESIZE_HOMOTHETIQUE) != 0;
    bool haute_qualite = (options & OWL_RESIZE_HAUTE_QUALITE) != 0;

    /* En mode homothétique, on calcule le scale qui permet à la source
     * de tenir entièrement dans (newW, newH) en préservant le ratio.
     * Le reste est rempli avec la couleur de remplissage. */
    double scaleX, scaleY;
    int destX0 = 0, destY0 = 0;
    int destW = newW, destH = newH;
    if (homothetique) {
        double sx = (double)newW / (double)oldW;
        double sy = (double)newH / (double)oldH;
        double s = sx < sy ? sx : sy;
        destW = (int)(oldW * s + 0.5);
        destH = (int)(oldH * s + 0.5);
        if (destW < 1) destW = 1;
        if (destH < 1) destH = 1;
        destX0 = (newW - destW) / 2;
        destY0 = (newH - destH) / 2;
        scaleX = (double)oldW / (double)destW;
        scaleY = (double)oldH / (double)destH;
    } else {
        scaleX = (double)oldW / (double)newW;
        scaleY = (double)oldH / (double)newH;
    }

    size_t newBytes = (size_t)newW * (size_t)newH * 4;
    uint8_t* newPixels = (uint8_t*)malloc(newBytes);
    if (!newPixels) return false;
    /* Pré-remplissage avec la couleur de remplissage. */
    fill_pixels(newPixels, newW * newH, img->fillColor);

    /* Pour chaque pixel destination dans la zone (destX0..+destW). */
    for (int dy = 0; dy < destH; dy++) {
        int py = destY0 + dy;
        if (py < 0 || py >= newH) continue;
        double sy = (dy + 0.5) * scaleY - 0.5;
        for (int dx = 0; dx < destW; dx++) {
            int px = destX0 + dx;
            if (px < 0 || px >= newW) continue;
            double sx = (dx + 0.5) * scaleX - 0.5;
            uint8_t rgba[4];
            if (haute_qualite) {
                OwlImageColor c = sample_bilinear(img->pixels, oldW, oldH, sx, sy);
                color_to_rgba8(c, rgba);
            } else {
                int isx = (int)(sx + 0.5);
                int isy = (int)(sy + 0.5);
                if (isx < 0) isx = 0;
                if (isy < 0) isy = 0;
                if (isx >= oldW) isx = oldW - 1;
                if (isy >= oldH) isy = oldH - 1;
                const uint8_t* src = img->pixels + (isy * oldW + isx) * 4;
                rgba[0] = src[0]; rgba[1] = src[1];
                rgba[2] = src[2]; rgba[3] = src[3];
            }
            uint8_t* dst = newPixels + (py * newW + px) * 4;
            dst[0] = rgba[0]; dst[1] = rgba[1];
            dst[2] = rgba[2]; dst[3] = rgba[3];
        }
    }

    free(img->pixels);
    img->pixels = newPixels;
    img->width = newW;
    img->height = newH;
    return true;
}

bool owl_image_rotate(OwlImage* img, double angleDegrees, int options) {
    if (!owl_image_valid(img)) return false;
    int oldW = img->width;
    int oldH = img->height;
    /* Normaliser l'angle dans [0, 360). */
    double angle = fmod(angleDegrees, 360.0);
    if (angle < 0) angle += 360.0;
    if (angle == 0.0) return true;

    /* Rotation horaire (sens canon : positif = horaire). En coords
     * écran y-down, ça revient à -theta dans la convention math.
     * On utilise l'angle horaire directement dans les formules.
     *
     * Snap aux multiples exacts de 90° : sin(π) en double vaut
     * environ 1.22e-16 plutôt que 0 exact, ce qui peut faire basculer
     * les pixels en bord d'image hors limites par un epsilon. */
    double cos_a, sin_a;
    if      (angle == 0.0)              { cos_a =  1.0; sin_a =  0.0; }
    else if (angle == 90.0)             { cos_a =  0.0; sin_a =  1.0; }
    else if (angle == 180.0)            { cos_a = -1.0; sin_a =  0.0; }
    else if (angle == 270.0)            { cos_a =  0.0; sin_a = -1.0; }
    else {
        double rad = angle * 3.14159265358979323846 / 180.0;
        cos_a = cos(rad);
        sin_a = sin(rad);
    }

    /* Calculer la bounding box résultante après rotation. */
    int newW, newH;
    if (options == OWL_ROTATE_SANS_AGRANDISSEMENT ||
        options == OWL_ROTATE_ADAPTE) {
        newW = oldW;
        newH = oldH;
    } else {
        /* drDéfaut : agrandir pour contenir l'image tournée. */
        double absC = fabs(cos_a);
        double absS = fabs(sin_a);
        newW = (int)(oldW * absC + oldH * absS + 0.5);
        newH = (int)(oldW * absS + oldH * absC + 0.5);
        if (newW < 1) newW = 1;
        if (newH < 1) newH = 1;
    }

    size_t newBytes = (size_t)newW * (size_t)newH * 4;
    uint8_t* newPixels = (uint8_t*)malloc(newBytes);
    if (!newPixels) return false;
    fill_pixels(newPixels, newW * newH, img->fillColor);

    /* Centre source et centre destination. On utilise (W-1)/2 plutôt
     * que W/2 pour que la rotation soit symétrique autour du pixel
     * central : sur une image WxH, les indices vont de 0 à W-1, donc
     * le centre géométrique est à (W-1)/2.0. */
    double srcCx = (oldW - 1) / 2.0;
    double srcCy = (oldH - 1) / 2.0;
    double dstCx = (newW - 1) / 2.0;
    double dstCy = (newH - 1) / 2.0;

    /* Inverse mapping : pour chaque pixel destination, trouver le pixel
     * source correspondant. Comme la rotation horaire envoie un point
     * src vers (cos_a*x + sin_a*y, -sin_a*x + cos_a*y) en coords
     * mathématiques, l'inverse (rotation anti-horaire) envoie dst vers
     * src. En coords écran y-down on a un signe inversé donc on
     * inverse encore : on applique cos_a/sin_a directement. */
    for (int dy = 0; dy < newH; dy++) {
        for (int dx = 0; dx < newW; dx++) {
            double tx = dx - dstCx;
            double ty = dy - dstCy;
            /* Rotation inverse pour récupérer la position source. */
            double sx =  cos_a * tx + sin_a * ty + srcCx;
            double sy = -sin_a * tx + cos_a * ty + srcCy;
            if (sx < 0 || sy < 0 || sx > oldW - 1 || sy > oldH - 1) continue;
            OwlImageColor c = sample_bilinear(img->pixels, oldW, oldH, sx, sy);
            uint8_t rgba[4];
            color_to_rgba8(c, rgba);
            uint8_t* d = newPixels + (dy * newW + dx) * 4;
            d[0] = rgba[0]; d[1] = rgba[1];
            d[2] = rgba[2]; d[3] = rgba[3];
        }
    }

    free(img->pixels);
    img->pixels = newPixels;
    img->width = newW;
    img->height = newH;

    /* drAdapte : après rotation, on réduit pour revenir à la taille
     * originale. */
    if (options == OWL_ROTATE_ADAPTE && (newW != oldW || newH != oldH)) {
        owl_image_resize_to(img, oldW, oldH, OWL_RESIZE_HAUTE_QUALITE);
    }
    return true;
}

bool owl_image_crop(OwlImage* img, int x, int y, int w, int h) {
    if (!owl_image_valid(img) || w <= 0 || h <= 0) return false;
    int oldW = img->width;
    int oldH = img->height;
    size_t newBytes = (size_t)w * (size_t)h * 4;
    uint8_t* newPixels = (uint8_t*)malloc(newBytes);
    if (!newPixels) return false;
    fill_pixels(newPixels, w * h, img->fillColor);

    for (int dy = 0; dy < h; dy++) {
        int sy = y + dy;
        if (sy < 0 || sy >= oldH) continue;
        for (int dx = 0; dx < w; dx++) {
            int sx = x + dx;
            if (sx < 0 || sx >= oldW) continue;
            const uint8_t* s = img->pixels + (sy * oldW + sx) * 4;
            uint8_t* d = newPixels + (dy * w + dx) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    free(img->pixels);
    img->pixels = newPixels;
    img->width = w;
    img->height = h;
    return true;
}

bool owl_image_copy_rect(OwlImage* src, OwlImage* dst,
                         int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh) {
    if (!owl_image_valid(src) || !owl_image_valid(dst)) return false;
    /* Valeurs par défaut : -1 = toute la source / toute la destination. */
    if (sw < 0) sw = src->width  - sx;
    if (sh < 0) sh = src->height - sy;
    if (dw < 0) dw = sw;
    if (dh < 0) dh = sh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;

    /* Pour chaque pixel destination, on map vers une position dans la
     * sous-zone source (avec rééchantillonnage bilinéaire si tailles
     * différentes). */
    double scaleX = (double)sw / (double)dw;
    double scaleY = (double)sh / (double)dh;
    for (int yy = 0; yy < dh; yy++) {
        int py = dy + yy;
        if (py < 0 || py >= dst->height) continue;
        double srcY = sy + (yy + 0.5) * scaleY - 0.5;
        for (int xx = 0; xx < dw; xx++) {
            int px = dx + xx;
            if (px < 0 || px >= dst->width) continue;
            double srcX = sx + (xx + 0.5) * scaleX - 0.5;
            OwlImageColor c = sample_bilinear(src->pixels, src->width, src->height,
                                              srcX, srcY);
            /* Écriture directe (pas d'alpha-blend pour dCopie ; pour le
             * mélange, l'utilisateur passe par dFusionne). */
            uint8_t rgba[4];
            color_to_rgba8(c, rgba);
            uint8_t* d = dst->pixels + (py * dst->width + px) * 4;
            d[0] = rgba[0]; d[1] = rgba[1];
            d[2] = rgba[2]; d[3] = rgba[3];
        }
    }
    return true;
}

bool owl_image_merge(OwlImage* dst, const OwlImage* src, int opacity) {
    if (!owl_image_valid(dst) || !owl_image_valid(src)) return false;
    if (opacity < 0)   opacity = 0;
    if (opacity > 255) opacity = 255;
    if (opacity == 0) return true;
    int w = src->width  < dst->width  ? src->width  : dst->width;
    int h = src->height < dst->height ? src->height : dst->height;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* s = src->pixels + (y * src->width + x) * 4;
            /* Module l'alpha source par l'opacité globale. */
            int srcA = (s[3] * opacity + 127) / 255;
            if (srcA == 0) continue;
            uint8_t* d = dst->pixels + (y * dst->width + x) * 4;
            /* Source-over compositing standard. */
            int dstA = d[3];
            int outA = srcA + ((dstA * (255 - srcA) + 127) / 255);
            if (outA > 255) outA = 255;
            d[0] = (uint8_t)((s[0] * srcA + d[0] * (255 - srcA) + 127) / 255);
            d[1] = (uint8_t)((s[1] * srcA + d[1] * (255 - srcA) + 127) / 255);
            d[2] = (uint8_t)((s[2] * srcA + d[2] * (255 - srcA) + 127) / 255);
            d[3] = (uint8_t)outA;
        }
    }
    return true;
}

/* ================================================================
 * EFFETS ET MODIFICATIONS DE COULEURS (Lot I.5)
 * ================================================================ */

void owl_image_invert_colors(OwlImage* img) {
    if (!owl_image_valid(img)) return;
    int n = img->width * img->height;
    for (int i = 0; i < n; i++) {
        uint8_t* p = img->pixels + i * 4;
        p[0] = 255 - p[0];
        p[1] = 255 - p[1];
        p[2] = 255 - p[2];
        /* alpha préservé */
    }
}

void owl_image_colorize(OwlImage* img, OwlImageColor color, int opacity) {
    if (!owl_image_valid(img)) return;
    if (opacity < 0)   opacity = 0;
    if (opacity > 255) opacity = 255;
    if (opacity == 0)  return;
    uint8_t srgba[4];
    color_to_rgba8(color, srgba);
    int sa = opacity;  /* l'opacité passée pilote la teinte */
    int n = img->width * img->height;
    for (int i = 0; i < n; i++) {
        uint8_t* p = img->pixels + i * 4;
        if (sa == 255) {
            p[0] = srgba[0]; p[1] = srgba[1]; p[2] = srgba[2];
        } else {
            p[0] = (uint8_t)((srgba[0] * sa + p[0] * (255 - sa) + 127) / 255);
            p[1] = (uint8_t)((srgba[1] * sa + p[1] * (255 - sa) + 127) / 255);
            p[2] = (uint8_t)((srgba[2] * sa + p[2] * (255 - sa) + 127) / 255);
        }
    }
}

void owl_image_modify_contrast(OwlImage* img, int variation) {
    if (!owl_image_valid(img)) return;
    if (variation < -100) variation = -100;
    if (variation >  100) variation =  100;
    /* factor : 1 + variation/100 ∈ [0, 2]. */
    double factor = 1.0 + (double)variation / 100.0;
    int n = img->width * img->height;
    for (int i = 0; i < n; i++) {
        uint8_t* p = img->pixels + i * 4;
        for (int c = 0; c < 3; c++) {
            double v = (p[c] - 128.0) * factor + 128.0;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            p[c] = (uint8_t)(v + 0.5);
        }
    }
}

void owl_image_modify_opacity(OwlImage* img, double factor) {
    if (!owl_image_valid(img)) return;
    if (factor < 0) factor = 0;
    int n = img->width * img->height;
    for (int i = 0; i < n; i++) {
        uint8_t* p = img->pixels + i * 4;
        double v = p[3] * factor;
        if (v > 255) v = 255;
        p[3] = (uint8_t)(v + 0.5);
    }
}

/* Conversion RGB <-> HSL. H ∈ [0, 360), S ∈ [0, 1], L ∈ [0, 1]. */
static void rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b,
                       double* h, double* s, double* l) {
    double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
    double maxv = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    double minv = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    *l = (maxv + minv) / 2.0;
    if (maxv == minv) { *h = 0; *s = 0; return; }
    double d = maxv - minv;
    *s = (*l > 0.5) ? (d / (2.0 - maxv - minv)) : (d / (maxv + minv));
    if      (maxv == rf) *h = (gf - bf) / d + (gf < bf ? 6.0 : 0.0);
    else if (maxv == gf) *h = (bf - rf) / d + 2.0;
    else                 *h = (rf - gf) / d + 4.0;
    *h *= 60.0;
}

static double hue_to_rgb(double p, double q, double t) {
    if (t < 0)   t += 1.0;
    if (t > 1)   t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5)     return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static void hsl_to_rgb(double h, double s, double l,
                       uint8_t* r, uint8_t* g, uint8_t* b) {
    double rf, gf, bf;
    if (s == 0) {
        rf = gf = bf = l;
    } else {
        double q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);
        double p = 2.0 * l - q;
        double hh = h / 360.0;
        rf = hue_to_rgb(p, q, hh + 1.0/3.0);
        gf = hue_to_rgb(p, q, hh);
        bf = hue_to_rgb(p, q, hh - 1.0/3.0);
    }
    int ri = (int)(rf * 255.0 + 0.5);
    int gi = (int)(gf * 255.0 + 0.5);
    int bi = (int)(bf * 255.0 + 0.5);
    if (ri < 0)   ri = 0;
    if (ri > 255) ri = 255;
    if (gi < 0)   gi = 0;
    if (gi > 255) gi = 255;
    if (bi < 0)   bi = 0;
    if (bi > 255) bi = 255;
    *r = (uint8_t)ri; *g = (uint8_t)gi; *b = (uint8_t)bi;
}

void owl_image_modify_tsl(OwlImage* img, int delta_hue,
                          int delta_saturation, int delta_lightness) {
    if (!owl_image_valid(img)) return;
    /* Variations clampées. */
    if (delta_saturation < -100) delta_saturation = -100;
    if (delta_saturation >  100) delta_saturation =  100;
    if (delta_lightness  < -100) delta_lightness  = -100;
    if (delta_lightness  >  100) delta_lightness  =  100;
    double dh = (double)delta_hue;          /* en degrés */
    double ds = delta_saturation / 100.0;   /* -1..+1 */
    double dl = delta_lightness  / 100.0;   /* -1..+1 */

    int n = img->width * img->height;
    for (int i = 0; i < n; i++) {
        uint8_t* p = img->pixels + i * 4;
        double h, s, l;
        rgb_to_hsl(p[0], p[1], p[2], &h, &s, &l);
        h = fmod(h + dh, 360.0);
        if (h < 0) h += 360.0;
        s += ds;
        if (s < 0) s = 0;
        if (s > 1) s = 1;
        l += dl;
        if (l < 0) l = 0;
        if (l > 1) l = 1;
        hsl_to_rgb(h, s, l, &p[0], &p[1], &p[2]);
    }
}

void owl_image_box_blur(OwlImage* img, int radius) {
    if (!owl_image_valid(img) || radius < 1) return;
    int w = img->width;
    int h = img->height;
    /* Buffer temporaire pour la passe horizontale → verticale. */
    size_t bytes = (size_t)w * (size_t)h * 4;
    uint8_t* tmp = (uint8_t*)malloc(bytes);
    if (!tmp) return;

    /* Passe horizontale : moyenne sur (2*radius+1) pixels. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sumR = 0, sumG = 0, sumB = 0, sumA = 0, count = 0;
            for (int dx = -radius; dx <= radius; dx++) {
                int sx = x + dx;
                if (sx < 0 || sx >= w) continue;
                const uint8_t* p = img->pixels + (y * w + sx) * 4;
                sumR += p[0]; sumG += p[1]; sumB += p[2]; sumA += p[3];
                count++;
            }
            uint8_t* d = tmp + (y * w + x) * 4;
            d[0] = (uint8_t)(sumR / count);
            d[1] = (uint8_t)(sumG / count);
            d[2] = (uint8_t)(sumB / count);
            d[3] = (uint8_t)(sumA / count);
        }
    }
    /* Passe verticale : tmp → img->pixels. */
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            int sumR = 0, sumG = 0, sumB = 0, sumA = 0, count = 0;
            for (int dy = -radius; dy <= radius; dy++) {
                int sy = y + dy;
                if (sy < 0 || sy >= h) continue;
                const uint8_t* p = tmp + (sy * w + x) * 4;
                sumR += p[0]; sumG += p[1]; sumB += p[2]; sumA += p[3];
                count++;
            }
            uint8_t* d = img->pixels + (y * w + x) * 4;
            d[0] = (uint8_t)(sumR / count);
            d[1] = (uint8_t)(sumG / count);
            d[2] = (uint8_t)(sumB / count);
            d[3] = (uint8_t)(sumA / count);
        }
    }
    free(tmp);
}

OwlImageColor owl_image_dominant_color(const OwlImage* img) {
    if (!owl_image_valid(img)) return 0;
    /* Couleur dominante = couleur RGB exacte la plus fréquente parmi
     * les pixels opaques (alpha != 0). Une moyenne pondérée donnerait
     * une teinte "représentative" mais pas la dominante au sens canon
     * (cas 9900 bleu + 100 vert → la moyenne tirerait vers le bleu
     * mais sans garantie, et 99% jaune + 1% magenta+cyan retournerait
     * une nuance terne).
     *
     * Comptage exact via hashmap open-addressing sur clé 24 bits (RGB).
     * Pour 10000 px typiques le coût est trivial ; pour des images plus
     * grosses la load factor reste maitrisée (capacité = next_pow2(n*2)
     * cappé à 1<<20 buckets, ~16 MB de scratch au pire). */
    int n = img->width * img->height;
    if (n <= 0) return 0;

    size_t cap = 64;
    while (cap < (size_t)n * 2 && cap < (1u << 20)) cap <<= 1;
    /* Bucket key sentinelle : 0xFFFFFFFFu = libre (RGB tient sur 24 bits
     * donc cette valeur ne collisionne jamais avec une couleur réelle). */
    typedef struct { uint32_t key; uint32_t count; } Bucket;
    Bucket* table = (Bucket*)malloc(cap * sizeof(Bucket));
    if (!table) return 0;
    for (size_t i = 0; i < cap; i++) { table[i].key = 0xFFFFFFFFu; table[i].count = 0; }
    size_t mask = cap - 1;

    uint32_t best_key   = 0;
    uint32_t best_count = 0;

    for (int i = 0; i < n; i++) {
        const uint8_t* p = img->pixels + i * 4;
        if (p[3] == 0) continue;
        uint32_t key = ((uint32_t)p[0])
                     | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16);
        /* Hash multiplicatif Knuth — bonne distribution pour 24 bits. */
        size_t h = ((size_t)key * 2654435761u) & mask;
        for (;;) {
            if (table[h].key == 0xFFFFFFFFu) {
                table[h].key = key;
                table[h].count = 1;
                if (1u > best_count) { best_count = 1; best_key = key; }
                break;
            }
            if (table[h].key == key) {
                table[h].count++;
                if (table[h].count > best_count) {
                    best_count = table[h].count;
                    best_key   = key;
                }
                break;
            }
            h = (h + 1) & mask;
        }
    }

    free(table);
    if (best_count == 0) return 0;
    uint8_t r = (uint8_t)(best_key & 0xFF);
    uint8_t g = (uint8_t)((best_key >> 8) & 0xFF);
    uint8_t b = (uint8_t)((best_key >> 16) & 0xFF);
    /* Encoding 0xAABBGGRR (alpha forcé opaque pour la couleur de retour). */
    return ((OwlImageColor)0xFF << 24)
         | ((OwlImageColor)b   << 16)
         | ((OwlImageColor)g   <<  8)
         | ((OwlImageColor)r);
}

void owl_image_flood_fill(OwlImage* img, int x, int y,
                          OwlImageColor fill_color,
                          bool has_border, OwlImageColor border_color) {
    if (!owl_image_valid(img)) return;
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) return;

    /* Couleur cible (BGR 24 bits, alpha ignoré pour la comparaison). */
    OwlImageColor target = (OwlImageColor)owl_image_get_pixel(img, x, y) & 0x00FFFFFFu;
    OwlImageColor border = border_color & 0x00FFFFFFu;
    OwlImageColor fill24 = fill_color & 0x00FFFFFFu;

    /* Si la couleur de remplissage est déjà la cible : rien à faire
     * (et évite la boucle infinie en flood fill standard). */
    if (!has_border && target == fill24) return;
    /* Si has_border et le pixel de départ est déjà sur la bordure :
     * rien à faire. */
    if (has_border && target == border) return;

    /* Pile explicite de positions à traiter. Capacité initiale
     * raisonnable, double à la demande. */
    int cap = 1024;
    int sp = 0;
    int* stack = (int*)malloc(sizeof(int) * 2 * cap);
    if (!stack) return;
    stack[2 * sp]     = x;
    stack[2 * sp + 1] = y;
    sp++;

    while (sp > 0) {
        sp--;
        int cx = stack[2 * sp];
        int cy = stack[2 * sp + 1];
        if (cx < 0 || cy < 0 || cx >= img->width || cy >= img->height) continue;
        OwlImageColor c = (OwlImageColor)owl_image_get_pixel(img, cx, cy) & 0x00FFFFFFu;
        if (has_border) {
            /* Boundary fill : on s'arrête au pixel qui a la couleur
             * border ; tous les autres sont coloriés. */
            if (c == border)   continue;
            if (c == fill24)   continue;  /* déjà rempli */
        } else {
            /* Flood fill standard : on remplit uniquement les pixels
             * de couleur target. */
            if (c != target) continue;
        }
        /* Coloriser ce pixel (alpha hérité de fill_color). */
        owl_image_set_pixel(img, cx, cy, fill_color);

        /* Pousser les 4 voisins. Re-allocation si la pile sature. */
        if (sp + 4 > cap) {
            cap *= 2;
            int* new_stack = (int*)realloc(stack, sizeof(int) * 2 * cap);
            if (!new_stack) { free(stack); return; }
            stack = new_stack;
        }
        stack[2 * sp]     = cx + 1; stack[2 * sp + 1] = cy;     sp++;
        stack[2 * sp]     = cx - 1; stack[2 * sp + 1] = cy;     sp++;
        stack[2 * sp]     = cx;     stack[2 * sp + 1] = cy + 1; sp++;
        stack[2 * sp]     = cx;     stack[2 * sp + 1] = cy - 1; sp++;
    }
    free(stack);
}

void owl_image_fill_rect_gradient(OwlImage* img, int x1, int y1,
                                  int x2, int y2,
                                  OwlImageColor c1, OwlImageColor c2,
                                  int angle) {
    if (!owl_image_valid(img)) return;
    /* Normaliser. */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    /* Normaliser angle au multiple de 90° le plus proche. */
    int a = ((angle % 360) + 360) % 360;
    int dir;  /* 0=L→R, 1=T→B, 2=R→L, 3=B→T */
    if      (a < 45 || a >= 315) dir = 0;
    else if (a < 135)            dir = 1;
    else if (a < 225)            dir = 2;
    else                         dir = 3;

    uint8_t rgba1[4], rgba2[4];
    color_to_rgba8(c1, rgba1);
    color_to_rgba8(c2, rgba2);

    /* Clamp aux bornes pour la passe pixel, mais le calcul du ratio
     * doit utiliser les bornes ORIGINALES (sinon le dégradé est
     * "comprimé" au-delà des bornes de l'image). */
    int orig_x1 = x1, orig_y1 = y1, orig_x2 = x2, orig_y2 = y2;
    int cx1 = x1 < 0 ? 0 : x1;
    int cy1 = y1 < 0 ? 0 : y1;
    int cx2 = x2 >= img->width  ? img->width  - 1 : x2;
    int cy2 = y2 >= img->height ? img->height - 1 : y2;
    if (cx1 > cx2 || cy1 > cy2) return;

    int span;
    if (dir == 0 || dir == 2) span = orig_x2 - orig_x1;
    else                      span = orig_y2 - orig_y1;
    if (span <= 0) {
        /* Rectangle d'épaisseur 0 ou 1 : remplir avec c1. */
        owl_image_fill_rect(img, x1, y1, x2, y2, c1);
        return;
    }

    for (int y = cy1; y <= cy2; y++) {
        for (int x = cx1; x <= cx2; x++) {
            int pos;  /* position le long de l'axe du dégradé */
            switch (dir) {
                case 0: pos = x - orig_x1;            break;  /* L→R */
                case 1: pos = y - orig_y1;            break;  /* T→B */
                case 2: pos = orig_x2 - x;            break;  /* R→L */
                default:pos = orig_y2 - y;            break;  /* B→T */
            }
            /* Ratio en fixed-point [0..256] pour préserver la précision
             * sur 8 bits par canal. */
            int t = (pos * 256 + span / 2) / span;
            if (t < 0)   t = 0;
            if (t > 256) t = 256;
            uint8_t rgba[4];
            rgba[0] = (uint8_t)((rgba1[0] * (256 - t) + rgba2[0] * t) >> 8);
            rgba[1] = (uint8_t)((rgba1[1] * (256 - t) + rgba2[1] * t) >> 8);
            rgba[2] = (uint8_t)((rgba1[2] * (256 - t) + rgba2[2] * t) >> 8);
            rgba[3] = (uint8_t)((rgba1[3] * (256 - t) + rgba2[3] * t) >> 8);
            plot_blend(img, x, y, rgba8_to_color(rgba));
        }
    }
}
