// SPDX-License-Identifier: AGPL-3.0-only
// qtsvg_smoketest.cpp — smoke test du vendoring qtsvg v6.10.0.
//
// But : valider que QSvgRenderer charge un SVG inline, sait le rasteriser
// vers un QImage, et que les pixels rendus ne sont pas vides. C'est le
// pre-requis pour le runtime catalogue icones qui va
// decompresser des blobs SVG zstd et les passer a QSvgRenderer pour
// les exposer comme BTN..Image = icones.X.
//
// Verifs :
//   1. QSvgRenderer accepte un buffer SVG inline (sans fichier disque)
//   2. isValid() == true sur le SVG charge
//   3. Render vers un QImage 64x64 -> au moins UN pixel non transparent
//   4. defaultSize() retourne le viewBox declare (16x16 ici)
//
// Le SVG test ci-dessous : un cercle rouge plein 16x16, conforme SVG
// Tiny 1.2 (sous-ensemble que QSvgRenderer supporte). Volontairement
// minimaliste pour eliminer tout doute sur la compat du POC.

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QByteArray>
#include <QSvgRenderer>
#include <cstdio>
#include <cstdlib>

static const char k_svg[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" "
    "width=\"16\" height=\"16\">"
    "<circle cx=\"8\" cy=\"8\" r=\"6\" fill=\"#ff0000\"/>"
    "</svg>";

int main(int argc, char** argv) {
    // QSvgRenderer requiert un QCoreApplication minimum pour les ressources
    // Qt internes (font db lazy, image plugins, etc.). QGuiApplication car
    // QImage + QPainter touchent au backend graphique. On utilise le plugin
    // offscreen pour eviter une dependance display reel.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // 1) Charge le SVG inline via QByteArray.
    QByteArray data(k_svg, static_cast<int>(sizeof(k_svg) - 1));
    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "qtsvg_smoketest: QSvgRenderer::isValid() = false\n");
        return 1;
    }

    // 2) defaultSize() doit refleter viewBox 16x16.
    QSize ds = renderer.defaultSize();
    if (ds.width() != 16 || ds.height() != 16) {
        std::fprintf(stderr,
            "qtsvg_smoketest: defaultSize=%dx%d, attendu 16x16\n",
            ds.width(), ds.height());
        return 2;
    }

    // 3) Render vers QImage 64x64 (test upscale + transparence).
    QImage img(64, 64, QImage::Format_ARGB32);
    img.fill(0x00000000); // transparent
    QPainter p(&img);
    renderer.render(&p);
    p.end();

    // 4) Verifier qu'au moins un pixel non transparent existe (cercle rouge
    //    rendu quelque part). Sinon : SVG silently muet = bug renderer.
    int red_pixels = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            QRgb px = img.pixel(x, y);
            if (qAlpha(px) > 0) {
                ++red_pixels;
            }
        }
    }
    if (red_pixels < 10) {
        std::fprintf(stderr,
            "qtsvg_smoketest: %d pixels non-transparents (attendu beaucoup plus)\n",
            red_pixels);
        return 3;
    }

    std::printf("qtsvg_smoketest OK (viewBox %dx%d, %d pixels rendus)\n",
                ds.width(), ds.height(), red_pixels);
    return 0;
}
