// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icone_render_qt.cpp — Rasterizing SVG -> RGBA via Qt6Svg.
 *
 * Helper bas-niveau exporte en C pour deux consommateurs :
 *   1. stdlib_icones.c (IconeVersImage) : convertit un ObjIcone en
 *      ObjImage rasterisee, utile pour sauvegarde PNG/JPEG.
 *   2. stdlib_ui.c (polymorphisme BTN..Image = ICN) : rasterise une
 *      icone vers un buffer RGBA passe a owl_ui_button_set_icon_rgba.
 *
 * API (cf. icone_render_qt.h) :
 *   - owl_icone_rasterize_rgba(svg, len, color_bgr, want_color, target_size,
 *                              rotation_deg, out_rgba, out_w, out_h)
 *     Alloue un buffer RGBA (target_size x target_size) via malloc.
 *     Le caller doit free.
 *     Retourne true en cas de succes (renderer valide + au moins 1 pixel),
 *     false sinon (svg malformed, out-of-memory, ou QSvgRenderer invalid).
 *
 * Init Qt lazy : si une QGuiApplication tourne deja (mode UI ouvert via
 * owl_ui_init), on l'utilise ; sinon on cree une QCoreApplication minimale
 * "offscreen" suffisante pour QSvgRenderer (les fontDb et plugins SVG ne
 * necessitent pas le backend graphique complet). */

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QByteArray>
#include <QSvgRenderer>
#include <QTransform>
#include <QColor>
#include <cstring>
#include <cstdlib>

/* QCoreApplication minimal lazy : suffisant pour les ressources Qt
 * internes que QSvgRenderer consulte (image plugins, fontDb stub).
 * Pas de backend graphique. Cohabite avec une QApplication preexistante
 * (mode UI) puisque QCoreApplication::instance() retourne le singleton
 * actif quel qu'il soit. */
static QCoreApplication* g_icone_app = nullptr;
static int  g_icone_argc = 1;
static char g_icone_argv0[] = "owl";
static char* g_icone_argv[] = { g_icone_argv0, nullptr };

static void ensure_qt_for_svg() {
    if (QCoreApplication::instance() == nullptr) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        g_icone_app = new QCoreApplication(g_icone_argc, g_icone_argv);
    }
}

extern "C" bool owl_icone_rasterize_rgba(const char* svg_raw,
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
                                         int* out_h) {
    if (!svg_raw || svg_len <= 0 || target_size <= 0 || !out_rgba
        || !out_w || !out_h) {
        return false;
    }
    *out_rgba = nullptr;
    *out_w    = 0;
    *out_h    = 0;

    ensure_qt_for_svg();

    /* Substitution currentColor : QSvgRenderer ne resout pas currentColor
     * de maniere portable. Si l'icone est mono-color (want_color = true),
     * on remplace les occurrences de "currentColor" par la couleur cible
     * formattee en #RRGGBB (Qt accepte cette syntaxe SVG). On evite ainsi
     * une passe Qt-side de re-painting + masking. */
    QByteArray svgData;
    if (want_color) {
        /* color_bgr est en format OWL : 0x00BBGGRR (alpha implicite).
         * Convertit en #RRGGBB pour SVG. */
        unsigned int r = color_bgr & 0xFF;
        unsigned int g = (color_bgr >> 8) & 0xFF;
        unsigned int b = (color_bgr >> 16) & 0xFF;
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                 (unsigned)r, (unsigned)g, (unsigned)b);
        QByteArray src(svg_raw, svg_len);
        svgData = src.replace("currentColor", hex);
    } else {
        svgData = QByteArray(svg_raw, svg_len);
    }

    QSvgRenderer renderer(svgData);
    if (!renderer.isValid()) {
        return false;
    }

    /* c — Preserve l'aspect ratio du viewBox SVG natif.
     * Avant : QImage(target_size, target_size, ...) -> les icones non
     * carrees (youtube/docker logos avec viewBox 24x16 ou 100x40)
     * etaient ecrasees en carre.
     *
     * Maintenant : target_size = hauteur cible. Largeur = hauteur *
     * (vbw / vbh). Fallback carre si viewBox indisponible. */
    QSize vb = renderer.defaultSize();
    int render_h = target_size;
    int render_w = target_size;
    if (vb.isValid() && vb.height() > 0 && vb.width() > 0) {
        double ratio = (double)vb.width() / (double)vb.height();
        render_w = (int)(target_size * ratio + 0.5);
        if (render_w <= 0) render_w = 1;
        /* Cap a 4x la hauteur pour eviter des cas extremes (viewBox
         * pathologique 1000x10 -> 2400 chars de large dans le CLI). */
        if (render_w > target_size * 4) render_w = target_size * 4;
    }

    /* QImage::Format_ARGB32 : 32 bits ARGB (0xAARRGGBB) — convertit
     * en RGBA cote consommateur. */
    QImage img(render_w, render_h, QImage::Format_ARGB32);
    img.fill(0x00000000); /* transparent */

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    /*  : applique fond colore + clipping forme avant le render SVG.
     * forme : 0=aucune, 1=formeCarree, 2=formeCercle. */
    if (forme != 0) {
        /* Couleur de fond (transparent par defaut si want_bg=false). */
        if (want_bg) {
            unsigned int r = bg_color_bgr & 0xFF;
            unsigned int g = (bg_color_bgr >> 8) & 0xFF;
            unsigned int b = (bg_color_bgr >> 16) & 0xFF;
            QColor bg((int)r, (int)g, (int)b);
            p.setBrush(bg);
            p.setPen(Qt::NoPen);
            if (forme == 2) {
                /* formeCercle : ellipse inscrit dans la zone render. */
                p.drawEllipse(QRectF(0, 0, render_w, render_h));
            } else {
                /* formeCarree : rectangle plein. */
                p.drawRect(QRectF(0, 0, render_w, render_h));
            }
        }
        /* Clipping forme : le SVG est rasterise SEULEMENT dans la
         * silhouette de la forme. */
        QPainterPath clip;
        if (forme == 2) {
            clip.addEllipse(QRectF(0, 0, render_w, render_h));
        } else {
            clip.addRect(QRectF(0, 0, render_w, render_h));
        }
        p.setClipPath(clip);
    }

    /* Rotation autour du centre. Doc : sens trigonometrique
     * (positif = anti-horaire) mais en pratique les utilisateurs
     * attendent une rotation sens horaire (style "tourne de 90 a droite").
     * Qt QPainter rotate() est sens horaire — on garde ce convention.
     * Lot 2a final reverifiera vs canon. */
    if (rotation_deg != 0) {
        p.translate(render_w / 2.0, render_h / 2.0);
        p.rotate(rotation_deg);
        p.translate(-render_w / 2.0, -render_h / 2.0);
    }
    renderer.render(&p);
    p.end();

    /* Verifie qu'au moins un pixel non-transparent a ete produit. Si
     * l'SVG est silently muet (probleme renderer), on signale invalide. */
    int non_transparent = 0;
    for (int y = 0; y < img.height() && non_transparent == 0; ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                non_transparent = 1;
                break;
            }
        }
    }
    if (non_transparent == 0) {
        return false;
    }

    /* Convertit ARGB -> RGBA pour l'API OWL. */
    const int n = render_w * render_h * 4;
    unsigned char* buf = (unsigned char*)malloc((size_t)n);
    if (!buf) return false;

    for (int y = 0; y < render_h; ++y) {
        for (int x = 0; x < render_w; ++x) {
            QRgb px = img.pixel(x, y);
            int idx = (y * render_w + x) * 4;
            buf[idx + 0] = (unsigned char)qRed(px);
            buf[idx + 1] = (unsigned char)qGreen(px);
            buf[idx + 2] = (unsigned char)qBlue(px);
            buf[idx + 3] = (unsigned char)qAlpha(px);
        }
    }

    *out_rgba = buf;
    *out_w    = render_w;
    *out_h    = render_h;
    return true;
}
