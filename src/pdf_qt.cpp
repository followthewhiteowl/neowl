// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* pdf_qt.cpp — backend PDF Qt6 pour owl_pdf.h.
 *
 * Strategie : 1 OwlPdfDoc = 1 QPdfWriter (target file) + 1 QPainter ouvert
 * dessus. Toutes les primitives operent en mm via QPainter.setWindow + un
 * facteur d'echelle calcule a partir du DPI du QPdfWriter.
 *
 * Pieges connus QPdfWriter :
 *  - DPI logique fixe a 1200 par defaut. On force a 300 pour la perf
 *    visuelle (suffisant pour la plupart des usages).
 *  - Embedding fontes : toujours subset, pas d'option toggle.
 *  - Pas de modification de PDF existant — V3 via PoDoFo.
 */

#include "owl_pdf.h"
extern "C" {
#include "owl_image.h"
}

#include <QPdfWriter>
#include <QPainter>
#include <QPageSize>
#include <QPageLayout>
#include <QMarginsF>
#include <QFont>
#include <QImage>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <QFileInfo>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPrintDialog>
#include <QFontDialog>
#include <QFontDatabase>
#include <QFile>
#include <QPixmap>
#include <QBitmap>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QPainter sur QPdfWriter necessite une QGuiApplication initialisee
 * (pour le subsystem font / image). owl_ui_init() la cree si absente. */
extern "C" int owl_ui_init(void);

/* --- Etat interne d'un OwlPdfDoc ---
 *
 * Deux modes :
 *  - PDF (writer != nullptr, memImage == nullptr) : painter attache au
 *    QPdfWriter, sortie sur fichier disque.
 *  - Image memoire ( : writer == nullptr, memImage != nullptr) :
 *    painter attache a une QImage RGBA reallouee a chaque page. A chaque
 *    iSautePage / owl_pdf_close, l'image est convertie en OwlImage* et
 *    passee au callback memOnPage.
 *
 * Le champ device est toujours valide (writer en mode PDF, memImage en mode
 * memoire). Tout le code de rendu lit device + dpi via les accesseurs. */
struct OwlPdfDoc {
    QPdfWriter* writer;        /* != nullptr ssi mode PDF disque */
    QImage*     memImage;      /* != nullptr ssi mode image memoire */
    QPaintDevice* device;      /* writer ou memImage (jamais nullptr) */
    QPainter*   painter;
    int         dpi;           /* resolution logique (300 par defaut) */
    /* Callback image-memoire. NULL si mode PDF. */
    OwlPdfPageCallback memOnPage;
    void*       memUserdata;
    /* Marges en mm. */
    double      marginTop, marginBottom, marginLeft, marginRight;
    /* Page courante en mm (apres update via setPageSize). */
    double      pageWidthMm, pageHeightMm;
    /* Curseur pour iImprime / iSauteLigne. En mm relatifs au coin haut-gauche
     * du *printable area* (apres marges). */
    double      cursorX, cursorY;
    /* Police courante. fonts[font_id] -> QFont. */
    QHash<int, QFont> fonts;
    int         currentFontId;
    QFont       currentFont;
    /* Page courante (1-based). */
    int         pageNum;
};

static OwlPdfDoc* g_current = nullptr;

/* Conversion mm -> point logique du QPainter (1 point = 1/dpi inch).
 * Lit le dpi stocke dans le doc (300 par defaut, ou valeur posee a
 * l'ouverture image-memoire). */
static double mm_to_units(OwlPdfDoc* doc, double mm) {
    return (mm / 25.4) * (double)doc->dpi;
}

/* Convertit un BGR pack canon (R + V*256 + B*65536) en QColor. -1 = noir. */
static QColor color_from_bgr(int bgr) {
    if (bgr < 0) return Qt::black;
    int r = bgr & 0xFF;
    int g = (bgr >> 8) & 0xFF;
    int b = (bgr >> 16) & 0xFF;
    return QColor(r, g, b);
}

/* --- Init fontes embarquees ---
 *
 * On charge Liberation Sans/Serif/Mono + DejaVu Sans depuis les Qt resources
 * (assets/fonts.qrc) et on les enregistre dans QFontDatabase une seule fois
 * par process. Garantit le rendu identique sur toutes les plateformes :
 * meme glyphes, memes metriques, meme hinting.
 *
 * Mapping legacy : si l'utilisateur passe "Arial"/"Helvetica" -> Liberation
 * Sans (metricalement compatibles). "Times"/"Times New Roman" -> Liberation
 * Serif. "Courier"/"Courier New" -> Liberation Mono. Pour toute autre famille,
 * on utilise Qt::QFontDatabase qui chargera la fonte systeme si dispo, sinon
 * fallback Liberation Sans. */

static const char* const OWL_EMBED_FONTS[] = {
    ":/fonts/LiberationSans-Regular.ttf",
    ":/fonts/LiberationSans-Bold.ttf",
    ":/fonts/LiberationSans-Italic.ttf",
    ":/fonts/LiberationSans-BoldItalic.ttf",
    ":/fonts/LiberationSerif-Regular.ttf",
    ":/fonts/LiberationSerif-Bold.ttf",
    ":/fonts/LiberationSerif-Italic.ttf",
    ":/fonts/LiberationSerif-BoldItalic.ttf",
    ":/fonts/LiberationMono-Regular.ttf",
    ":/fonts/LiberationMono-Bold.ttf",
    ":/fonts/LiberationMono-Italic.ttf",
    ":/fonts/LiberationMono-BoldItalic.ttf",
    ":/fonts/DejaVuSans-Regular.ttf",
    ":/fonts/DejaVuSans-Bold.ttf",
    nullptr
};

static bool g_fontsRegistered = false;

static void register_embedded_fonts(void) {
    if (g_fontsRegistered) return;
    /* Force le link + l'execution de l'initialiseur du .qrc (assets/fonts.qrc).
     * Quand la ressource est compilee dans une lib statique (owl_runtime_gui),
     * son constructeur global n'est reference par aucun symbole -> dead-strippe
     * au link de l'exe final -> ":/fonts/" jamais enregistre. Referencer
     * qInitResources_fonts via la macro force sa survie et son execution.
     * Idempotent : sur owl.exe (deja initialise) c'est un no-op. */
    Q_INIT_RESOURCE(fonts);
    for (int i = 0; OWL_EMBED_FONTS[i] != nullptr; i++) {
        QFile f(QString::fromUtf8(OWL_EMBED_FONTS[i]));
        if (!f.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "[owl_pdf] fonte embedded introuvable : %s\n", OWL_EMBED_FONTS[i]);
            continue;
        }
        QByteArray data = f.readAll();
        int id = QFontDatabase::addApplicationFontFromData(data);
        if (id < 0) {
            fprintf(stderr, "[owl_pdf] echec d'enregistrement de %s\n", OWL_EMBED_FONTS[i]);
        }
    }
    g_fontsRegistered = true;
}

/* Mappe les familles classiques (notamment polices Microsoft non-libres) vers
 * leur equivalent Liberation/DejaVu embarque. Permet a l'utilisateur de
 * specifier "Arial", "Calibri", "Verdana" etc. dans son code canon et obtenir
 * un rendu deterministe sans avoir a embarquer les fontes Microsoft (probleme
 * de licence). Retourne le nom embarque ou la famille originale si pas de
 * mapping (Qt cherchera alors systeme puis fallback). */
static QString map_legacy_family(const QString& family) {
    QString lower = family.toLower();
    if (lower.isEmpty()) return QStringLiteral("Liberation Sans");
    /* Sans-serif courants -> Liberation Sans. */
    if (lower == "arial" || lower == "helvetica" || lower == "sans" ||
        lower == "sans-serif" || lower == "sans serif" ||
        lower == "verdana" || lower == "tahoma" || lower == "calibri" ||
        lower == "comic sans ms" || lower == "comic sans" ||
        lower == "trebuchet ms" || lower == "trebuchet" ||
        lower == "segoe ui" || lower == "lucida sans" ||
        lower == "lucida sans unicode" || lower == "geneva")
        return QStringLiteral("Liberation Sans");
    /* Serifs courants -> Liberation Serif. */
    if (lower == "times" || lower == "times new roman" || lower == "serif" ||
        lower == "roman" || lower == "georgia" || lower == "garamond" ||
        lower == "palatino" || lower == "palatino linotype" ||
        lower == "book antiqua" || lower == "cambria")
        return QStringLiteral("Liberation Serif");
    /* Monospace courants -> Liberation Mono. */
    if (lower == "courier" || lower == "courier new" || lower == "monospace" ||
        lower == "mono" || lower == "consolas" || lower == "lucida console" ||
        lower == "lucida sans typewriter")
        return QStringLiteral("Liberation Mono");
    return family;  /* nom non-mappe : Qt cherchera systeme puis fallback */
}

/* Configure les hints de rendu pour produire un PDF deterministe entre OS.
 * Sans hinting font-engine (qui depend du driver OS), antialiasing actif,
 * design metrics (sub-pixel) pour la mesure des glyphes. */
static void apply_deterministic_render_hints(QPainter* painter, QFont* font) {
    if (painter) {
        painter->setRenderHint(QPainter::Antialiasing,         true);
        painter->setRenderHint(QPainter::TextAntialiasing,     true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    }
    if (font) {
        font->setHintingPreference(QFont::PreferNoHinting);
        font->setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::PreferQuality | QFont::ForceOutline));
    }
}

/* --- Lifecycle --- */

/* Helpers communs aux 2 modes : initialise les champs partages (curseur,
 * marges, police, page num). N'alloue ni device ni painter. */
static void init_common_doc_state(OwlPdfDoc* doc) {
    doc->marginTop = doc->marginBottom = doc->marginLeft = doc->marginRight = 0;
    doc->cursorX = 0;
    doc->cursorY = 0;
    doc->currentFontId = -1;
    /* Defaut : Liberation Sans 10pt. Police embarquee donc identique partout. */
    doc->currentFont = QFont(QStringLiteral("Liberation Sans"));
    doc->currentFont.setPointSizeF(10.0);
    doc->pageNum = 1;
}

extern "C" OwlPdfDoc* owl_pdf_open(const char* output_path_utf8) {
    if (!output_path_utf8 || !*output_path_utf8) return nullptr;
    /* Garantit QApplication active : QPainter::begin sur QPdfWriter charge
     * le subsystem fonts qui crash sans QApp. */
    owl_ui_init();
    register_embedded_fonts();
    OwlPdfDoc* doc = new OwlPdfDoc;
    doc->memImage = nullptr;
    doc->memOnPage = nullptr;
    doc->memUserdata = nullptr;
    doc->dpi = 300;
    doc->writer = new QPdfWriter(QString::fromUtf8(output_path_utf8));
    doc->writer->setResolution(doc->dpi);
    /* Defaut A4 portrait, pas de marges (le caller appelle iMarge). */
    doc->writer->setPageSize(QPageSize(QPageSize::A4));
    doc->writer->setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
    doc->device = doc->writer;
    doc->painter = new QPainter();
    if (!doc->painter->begin(doc->writer)) {
        delete doc->painter;
        delete doc->writer;
        delete doc;
        return nullptr;
    }
    init_common_doc_state(doc);
    QSizeF pageMm = doc->writer->pageLayout().fullRect(QPageLayout::Millimeter).size();
    doc->pageWidthMm  = pageMm.width();
    doc->pageHeightMm = pageMm.height();
    apply_deterministic_render_hints(doc->painter, &doc->currentFont);
    doc->painter->setFont(doc->currentFont);
    g_current = doc;
    return doc;
}

/* Dimensions pixels d'une page (mode image memoire) selon mm et dpi. */
static QSize img_page_pixels(double w_mm, double h_mm, int dpi) {
    int w_px = (int)((w_mm / 25.4) * dpi + 0.5);
    int h_px = (int)((h_mm / 25.4) * dpi + 0.5);
    if (w_px < 1) w_px = 1;
    if (h_px < 1) h_px = 1;
    return QSize(w_px, h_px);
}

/* (Re)cree la QImage interne pour une page vierge en mode image memoire.
 * Detruit l'ancien painter/QImage (caller a deja flushe si besoin). */
static bool mem_begin_page(OwlPdfDoc* doc) {
    QSize px = img_page_pixels(doc->pageWidthMm, doc->pageHeightMm, doc->dpi);
    doc->memImage = new QImage(px, QImage::Format_RGBA8888);
    doc->memImage->fill(Qt::white);
    doc->device = doc->memImage;
    doc->painter = new QPainter();
    if (!doc->painter->begin(doc->memImage)) {
        delete doc->painter; doc->painter = nullptr;
        delete doc->memImage; doc->memImage = nullptr;
        return false;
    }
    apply_deterministic_render_hints(doc->painter, &doc->currentFont);
    doc->painter->setFont(doc->currentFont);
    return true;
}

/* Finalise la QImage courante : end painter, convertit en OwlImage* RGBA,
 * passe au callback, libere ressources Qt. Caller doit recreer la page
 * ensuite via mem_begin_page (ou laisser doc se fermer si fin de doc). */
static void mem_flush_page(OwlPdfDoc* doc) {
    if (!doc->memImage || !doc->painter) return;
    doc->painter->end();
    delete doc->painter; doc->painter = nullptr;
    /* Conversion QImage RGBA8888 -> OwlImage : meme byte layout, copie
     * ligne par ligne pour gerer le row padding eventuel. */
    int w = doc->memImage->width();
    int h = doc->memImage->height();
    OwlImage* page = nullptr;
    if (w > 0 && h > 0) {
        size_t bytes = (size_t)w * (size_t)h * 4;
        unsigned char* buf = (unsigned char*)malloc(bytes);
        if (buf) {
            for (int y = 0; y < h; y++) {
                memcpy(buf + (size_t)y * w * 4,
                       doc->memImage->constScanLine(y),
                       (size_t)w * 4);
            }
            page = owl_image_new_from_rgba(w, h, buf);
            free(buf);
        }
    }
    delete doc->memImage; doc->memImage = nullptr;
    if (doc->memOnPage) doc->memOnPage(page, doc->memUserdata);
}

extern "C" OwlPdfDoc* owl_pdf_open_image_memory(int dpi,
                                                  OwlPdfPageCallback on_page,
                                                  void* userdata) {
    if (dpi <= 0) dpi = 300;
    owl_ui_init();
    register_embedded_fonts();
    OwlPdfDoc* doc = new OwlPdfDoc;
    doc->writer = nullptr;
    doc->memImage = nullptr;
    doc->memOnPage = on_page;
    doc->memUserdata = userdata;
    doc->dpi = dpi;
    /* Defaut A4 portrait — le caller (apply_etat_properties) appelle
     * owl_pdf_set_page_size_mm ensuite si l'Etat declare un autre format. */
    init_common_doc_state(doc);
    doc->pageWidthMm  = 210.0;
    doc->pageHeightMm = 297.0;
    if (!mem_begin_page(doc)) {
        delete doc;
        return nullptr;
    }
    g_current = doc;
    return doc;
}

extern "C" int owl_pdf_close(OwlPdfDoc* doc) {
    if (!doc) return 0;
    /* Mode image memoire : flush la derniere page avant de jeter. */
    if (doc->memImage) {
        mem_flush_page(doc);
    } else if (doc->painter) {
        doc->painter->end();
        delete doc->painter;
    }
    if (doc->writer) delete doc->writer;
    if (g_current == doc) g_current = nullptr;
    delete doc;
    return 1;
}

extern "C" OwlPdfDoc* owl_pdf_get_current(void) { return g_current; }

/* --- Mise en page --- */

extern "C" void owl_pdf_set_margins(OwlPdfDoc* doc, double top, double bottom,
                                     double left, double right) {
    if (!doc) return;
    doc->marginTop = top;
    doc->marginBottom = bottom;
    doc->marginLeft = left;
    doc->marginRight = right;
    /* Reset le curseur en haut-gauche du printable area. */
    doc->cursorX = 0;
    doc->cursorY = 0;
}

extern "C" void owl_pdf_set_page_size_mm(OwlPdfDoc* doc, double w, double h) {
    if (!doc || w <= 0 || h <= 0) return;
    doc->pageWidthMm = w;
    doc->pageHeightMm = h;
    if (doc->writer) {
        QPageSize ps(QSizeF(w, h), QPageSize::Millimeter);
        doc->writer->setPageSize(ps);
        return;
    }
    /* Mode image memoire : recree la QImage aux nouvelles dim. La page en
     * cours est rejetee (pas encore flushee) car owl_pdf_set_page_size_mm
     * est appele depuis apply_etat_properties AVANT toute primitive. */
    if (doc->memImage && doc->painter) {
        doc->painter->end();
        delete doc->painter; doc->painter = nullptr;
        delete doc->memImage; doc->memImage = nullptr;
        mem_begin_page(doc);
    }
}

/* iPDFNiveauGris : setColorModel(Grayscale). Doit etre appele avant la
 * 1ere primitive (Qt n'autorise pas le changement en cours d'impression).
 * No-op en mode image memoire (option PDF-specifique). */
extern "C" void owl_pdf_set_grayscale(OwlPdfDoc* doc, int active) {
    if (!doc || !doc->writer) return;
    doc->writer->setColorModel(active ? QPdfWriter::ColorModel::Grayscale
                                       : QPdfWriter::ColorModel::RGB);
}

/* iPDFA / iPDFA1b : setPdfVersion(PdfVersion_A1b). Idem, avant la 1ere
 * primitive. Note Qt6 : la conformance PDF/A complete (polices embed,
 * pas de transparence) reste a la charge du caller — Qt valide juste
 * la version dans le header PDF. No-op en mode image memoire. */
extern "C" void owl_pdf_set_pdf_a1b(OwlPdfDoc* doc, int active) {
    if (!doc || !doc->writer) return;
    doc->writer->setPdfVersion(active ? QPagedPaintDevice::PdfVersion_A1b
                                       : QPagedPaintDevice::PdfVersion_1_4);
}

extern "C" void owl_pdf_new_page(OwlPdfDoc* doc) {
    if (!doc) return;
    if (doc->writer) {
        doc->writer->newPage();
    } else if (doc->memImage) {
        /* Mode image memoire : flush la page courante + ouvre une page
         * vierge aux memes dim. Le callback recoit l'OwlImage finalisee. */
        mem_flush_page(doc);
        mem_begin_page(doc);
    }
    doc->pageNum++;
    doc->cursorX = 0;
    doc->cursorY = 0;
}

extern "C" int owl_pdf_page_number(OwlPdfDoc* doc) {
    return doc ? doc->pageNum : 0;
}

extern "C" void owl_pdf_set_page_number(OwlPdfDoc* doc, int n) {
    if (!doc) return;
    doc->pageNum = n;
}

extern "C" double owl_pdf_page_width_mm(OwlPdfDoc* doc) {
    if (!doc) return 0;
    return doc->pageWidthMm - doc->marginLeft - doc->marginRight;
}
extern "C" double owl_pdf_page_height_mm(OwlPdfDoc* doc) {
    if (!doc) return 0;
    return doc->pageHeightMm - doc->marginTop - doc->marginBottom;
}
extern "C" double owl_pdf_remaining_height_mm(OwlPdfDoc* doc) {
    if (!doc) return 0;
    return owl_pdf_page_height_mm(doc) - doc->cursorY;
}

/* --- Texte --- */

/* Hauteur de ligne approximative en mm pour la police courante. */
static double line_height_mm(OwlPdfDoc* doc) {
    /* QFontMetrics donne en pixels du painter. On retourne en mm. */
    QFontMetricsF fm(doc->currentFont, doc->device);
    double pixels = fm.height();
    /* pixels du painter = unites a notre dpi. mm = pixels * 25.4 / dpi. */
    return pixels * 25.4 / (double)doc->dpi;
}

extern "C" void owl_pdf_print_word(OwlPdfDoc* doc, const char* text_utf8) {
    if (!doc || !doc->painter) return;
    QString s = QString::fromUtf8(text_utf8 ? text_utf8 : "");
    QFontMetricsF fm(doc->currentFont, doc->device);
    double ascent_mm = fm.ascent() * 25.4 / (double)doc->dpi;
    double xMm = doc->marginLeft + doc->cursorX;
    double yMm = doc->marginTop  + doc->cursorY + ascent_mm;
    QPen savedPen = doc->painter->pen();
    if (savedPen.style() == Qt::NoPen) doc->painter->setPen(QPen(Qt::black));
    doc->painter->drawText(QPointF(mm_to_units(doc, xMm),
                                    mm_to_units(doc, yMm)), s);
    doc->painter->setPen(savedPen);
    /* Avance le curseur X de la largeur du texte. */
    doc->cursorX += fm.horizontalAdvance(s) * 25.4 / (double)doc->dpi;
}

/* alignement = bitmask combinable : 0x01=iGauche, 0x02=iDroite, 0x04=iCentreH,
 * 0x10=iHaut, 0x20=iBas, 0x40=iCentreV. Defaut canon = iHaut + iGauche. */
extern "C" void owl_pdf_print_zone(OwlPdfDoc* doc, const char* text_utf8,
                                    double x1, double y1, double x2, double y2,
                                    int alignement) {
    if (!doc || !doc->painter) return;
    QString s = QString::fromUtf8(text_utf8 ? text_utf8 : "");
    Qt::Alignment a;
    /* Horizontal */
    if      (alignement & 0x04) a |= Qt::AlignHCenter;
    else if (alignement & 0x02) a |= Qt::AlignRight;
    else                         a |= Qt::AlignLeft;
    /* Vertical */
    if      (alignement & 0x40) a |= Qt::AlignVCenter;
    else if (alignement & 0x20) a |= Qt::AlignBottom;
    else                         a |= Qt::AlignTop;
    QRectF r(mm_to_units(doc, x1 + doc->marginLeft),
             mm_to_units(doc, y1 + doc->marginTop),
             mm_to_units(doc, x2 - x1),
             mm_to_units(doc, y2 - y1));
    /* Force une pen visible pour le texte. Sinon, si la pen courante est
     * NoPen (cas typique apres un iCadre/owl_pdf_rect avec thickness=0),
     * drawText ne dessine rien — le texte devient invisible. Sauvegarde
     * la pen courante pour la restaurer apres. */
    QPen savedPen = doc->painter->pen();
    if (savedPen.style() == Qt::NoPen) {
        doc->painter->setPen(QPen(Qt::black));
    }
    doc->painter->drawText(r, (int)a | (int)Qt::TextWordWrap, s);
    doc->painter->setPen(savedPen);
}

extern "C" double owl_pdf_text_width_mm(OwlPdfDoc* doc, const char* text_utf8) {
    if (!doc) return 0;
    QFontMetricsF fm(doc->currentFont, doc->device);
    double w = fm.horizontalAdvance(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    return w * 25.4 / (double)doc->dpi;
}

extern "C" double owl_pdf_text_height_mm(OwlPdfDoc* doc) {
    if (!doc) return 0;
    QFontMetricsF fm(doc->currentFont, doc->device);
    /* fm.height() = ascent + descent + leading (en pixels du painter). */
    return fm.height() * 25.4 / (double)doc->dpi;
}

extern "C" void owl_pdf_image_size_mm(OwlPdfDoc* doc, const char* image_path_utf8,
                                       double* out_w_mm, double* out_h_mm) {
    if (out_w_mm) *out_w_mm = 0;
    if (out_h_mm) *out_h_mm = 0;
    if (!doc || !image_path_utf8) return;
    QImage img(QString::fromUtf8(image_path_utf8));
    if (img.isNull()) return;
    /* Conversion px -> mm via dpi du writer (canon : "calcul par rapport a
     * la resolution de l'imprimante selectionnee"). */
    double dpi = (double)doc->dpi;
    if (out_w_mm) *out_w_mm = img.width()  * 25.4 / dpi;
    if (out_h_mm) *out_h_mm = img.height() * 25.4 / dpi;
}

extern "C" void owl_pdf_newline(OwlPdfDoc* doc) {
    if (!doc) return;
    doc->cursorY += line_height_mm(doc);
    doc->cursorX = 0;
}

extern "C" void owl_pdf_set_cursor_x(OwlPdfDoc* doc, double x_mm) {
    if (!doc) return;
    doc->cursorX = x_mm;
}

extern "C" void owl_pdf_set_cursor_y(OwlPdfDoc* doc, double y_mm) {
    if (!doc) return;
    doc->cursorY = y_mm;
}

extern "C" double owl_pdf_get_cursor_x(OwlPdfDoc* doc) {
    return doc ? doc->cursorX : 0;
}

extern "C" double owl_pdf_get_cursor_y(OwlPdfDoc* doc) {
    return doc ? doc->cursorY : 0;
}

/* --- Geometrie --- */

/* Mapping iTrait -> Qt::PenStyle. */
static Qt::PenStyle pen_style_from_int(int trait_style) {
    switch (trait_style) {
        case 1: return Qt::DashLine;     /* iTiret */
        case 2: return Qt::DotLine;      /* iPointille */
        case 3: return Qt::DashDotLine;  /* iMixte */
        default: return Qt::SolidLine;   /* iContinu */
    }
}

/* Mapping iBrosse -> Qt::BrushStyle. */
static Qt::BrushStyle brush_style_from_int(int brush_style) {
    switch (brush_style) {
        case 1: return Qt::HorPattern;       /* iHorizontale */
        case 2: return Qt::VerPattern;       /* iVerticale */
        case 3: return Qt::CrossPattern;     /* iCroisee */
        case 4: return Qt::FDiagPattern;     /* iDiagonaleDescendante */
        case 5: return Qt::BDiagPattern;     /* iDiagonaleMontante */
        case 6: return Qt::DiagCrossPattern; /* iCroiseeDiagonale */
        default: return Qt::SolidPattern;    /* iPleine */
    }
}

extern "C" void owl_pdf_line(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                              double thickness_mm, int color_bgr, int trait_style, int cap_style) {
    if (!doc || !doc->painter) return;
    QPen pen(color_from_bgr(color_bgr));
    pen.setWidthF(mm_to_units(doc, thickness_mm > 0 ? thickness_mm : 0.353));
    pen.setStyle(pen_style_from_int(trait_style));
    /* cap_style : 0 = RoundCap (defaut iLigne), 1 = FlatCap (iTrait*). */
    pen.setCapStyle(cap_style == 1 ? Qt::FlatCap : Qt::RoundCap);
    doc->painter->setPen(pen);
    doc->painter->drawLine(
        QPointF(mm_to_units(doc, x1 + doc->marginLeft),
                mm_to_units(doc, y1 + doc->marginTop)),
        QPointF(mm_to_units(doc, x2 + doc->marginLeft),
                mm_to_units(doc, y2 + doc->marginTop)));
}

/* Setup pen + brush communs aux primitives rect/ellipse/rounded.
 * thickness_mm <= 0 = pas de bord. fill_bgr < 0 = pas de remplissage. */
static void setup_pen_brush(OwlPdfDoc* doc, double thickness_mm, int border_bgr,
                             int fill_bgr, int trait_style, int brush_style) {
    QPen pen = thickness_mm > 0 ? QPen(color_from_bgr(border_bgr)) : QPen(Qt::NoPen);
    if (thickness_mm > 0) {
        pen.setWidthF(mm_to_units(doc, thickness_mm));
        pen.setStyle(pen_style_from_int(trait_style));
    }
    QBrush brush;
    if (fill_bgr >= 0) {
        brush = QBrush(color_from_bgr(fill_bgr), brush_style_from_int(brush_style));
    } else {
        brush = Qt::NoBrush;
    }
    doc->painter->setPen(pen);
    doc->painter->setBrush(brush);
}

extern "C" void owl_pdf_rect(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                              double thickness_mm, int border_bgr, int fill_bgr,
                              int trait_style, int brush_style, int border_mask) {
    if (!doc || !doc->painter) return;
    setup_pen_brush(doc, thickness_mm, border_bgr, fill_bgr, trait_style, brush_style);
    QRectF r(mm_to_units(doc, x1 + doc->marginLeft),
             mm_to_units(doc, y1 + doc->marginTop),
             mm_to_units(doc, x2 - x1),
             mm_to_units(doc, y2 - y1));
    /* Si tous les bords ou aucun mask : drawRect direct (avec ou sans pen). */
    bool all_borders = (border_mask == 0) || ((border_mask & 0x0F) == 0x0F);
    if (all_borders) {
        doc->painter->drawRect(r);
        return;
    }
    /* Sous-ensemble de bords : draw fill seul (sans pen) puis chaque cote. */
    QPen savedPen = doc->painter->pen();
    doc->painter->setPen(Qt::NoPen);
    if (fill_bgr >= 0) doc->painter->drawRect(r);
    doc->painter->setPen(savedPen);
    if (border_mask & 0x01)  /* haut */
        doc->painter->drawLine(r.topLeft(), r.topRight());
    if (border_mask & 0x02)  /* bas */
        doc->painter->drawLine(r.bottomLeft(), r.bottomRight());
    if (border_mask & 0x04)  /* gauche */
        doc->painter->drawLine(r.topLeft(), r.bottomLeft());
    if (border_mask & 0x08)  /* droit */
        doc->painter->drawLine(r.topRight(), r.bottomRight());
}

extern "C" void owl_pdf_rounded_rect(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                                      double radius_h_mm, double radius_v_mm,
                                      double thickness_mm, int border_bgr, int fill_bgr,
                                      int trait_style, int brush_style) {
    if (!doc || !doc->painter) return;
    setup_pen_brush(doc, thickness_mm, border_bgr, fill_bgr, trait_style, brush_style);
    QRectF r(mm_to_units(doc, x1 + doc->marginLeft),
             mm_to_units(doc, y1 + doc->marginTop),
             mm_to_units(doc, x2 - x1),
             mm_to_units(doc, y2 - y1));
    doc->painter->drawRoundedRect(r,
        mm_to_units(doc, radius_h_mm),
        mm_to_units(doc, radius_v_mm));
}

extern "C" void owl_pdf_ellipse(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                                 double thickness_mm, int border_bgr, int fill_bgr,
                                 int trait_style, int brush_style) {
    if (!doc || !doc->painter) return;
    QPen pen = thickness_mm > 0 ? QPen(color_from_bgr(border_bgr)) : QPen(Qt::NoPen);
    if (thickness_mm > 0) {
        pen.setWidthF(mm_to_units(doc, thickness_mm));
        pen.setStyle(pen_style_from_int(trait_style));
    }
    QBrush brush = fill_bgr >= 0
        ? QBrush(color_from_bgr(fill_bgr), brush_style_from_int(brush_style))
        : Qt::NoBrush;
    doc->painter->setPen(pen);
    doc->painter->setBrush(brush);
    QRectF r(mm_to_units(doc, x1 + doc->marginLeft),
             mm_to_units(doc, y1 + doc->marginTop),
             mm_to_units(doc, x2 - x1),
             mm_to_units(doc, y2 - y1));
    doc->painter->drawEllipse(r);
}

/* --- Images --- */

/* iMagentaTransparent : flag global. Décl. ici car utilisée par
 * apply_magenta_transparency ci-dessous (les setter/getter sont plus bas). */
static bool g_magentaTransparent = false;

/* Si g_magentaTransparent est actif, applique un alpha 0 sur tous les pixels
 * magenta pur (#FF00FF). Sinon retourne l'image inchangee. */
static QImage apply_magenta_transparency(const QImage& src) {
    if (!g_magentaTransparent || src.isNull()) return src;
    QImage out = src.convertToFormat(QImage::Format_ARGB32);
    /* createMaskFromColor : produit un masque 1-bit ou les pixels matchant
     * sont noirs (a transparenter). On l'applique comme alpha mask. */
    out.setAlphaChannel(out.createMaskFromColor(qRgb(255,0,255), Qt::MaskOutColor));
    return out;
}

/* Calcule le rect ajuste si mode = homothetique/homocentre (preserve ratio).
 * Retourne le rect cible original si mode = normal. */
static QRectF fit_to_mode(const QRectF& target, const QImage& img, int mode) {
    if (mode == 0 || target.width() <= 0 || target.height() <= 0) return target;
    if (img.isNull()) return target;
    double iw = img.width(), ih = img.height();
    if (iw <= 0 || ih <= 0) return target;
    double aspect = iw / ih;
    double rw = target.width(), rh = target.height();
    double r_aspect = rw / rh;
    QRectF fitted = target;
    if (aspect > r_aspect) {
        /* Image plus large que la zone : limite par largeur. */
        double new_h = rw / aspect;
        if (mode == 2) {
            /* HomoCentre : centre verticalement. */
            fitted = QRectF(target.left(), target.top() + (rh - new_h) / 2, rw, new_h);
        } else {
            /* Homothetique : ancrage haut-gauche. */
            fitted = QRectF(target.left(), target.top(), rw, new_h);
        }
    } else {
        double new_w = rh * aspect;
        if (mode == 2) {
            fitted = QRectF(target.left() + (rw - new_w) / 2, target.top(), new_w, rh);
        } else {
            fitted = QRectF(target.left(), target.top(), new_w, rh);
        }
    }
    return fitted;
}

extern "C" void owl_pdf_image(OwlPdfDoc* doc, const char* image_path_utf8,
                               double x1, double y1, double x2, double y2,
                               int mode, double zoom_pct) {
    if (!doc || !doc->painter || !image_path_utf8) return;
    QImage rawImg(QString::fromUtf8(image_path_utf8));
    if (rawImg.isNull()) {
        fprintf(stderr, "[owl_pdf_image] impossible de charger '%s'\n", image_path_utf8);
        return;
    }
    QImage img = apply_magenta_transparency(rawImg);
    QRectF target;
    if (x2 <= x1 || y2 <= y1) {
        /* Syntaxe 2 canon : position seule + zoom. Calcul taille native
         * en mm a partir des dpi du writer. zoom_pct 0 = 100%. */
        if (zoom_pct <= 0) zoom_pct = 100.0;
        double dpi = (double)doc->dpi;
        double w_mm = (img.width()  * 25.4 / dpi) * (zoom_pct / 100.0);
        double h_mm = (img.height() * 25.4 / dpi) * (zoom_pct / 100.0);
        target = QRectF(mm_to_units(doc, x1 + doc->marginLeft),
                        mm_to_units(doc, y1 + doc->marginTop),
                        mm_to_units(doc, w_mm),
                        mm_to_units(doc, h_mm));
        doc->painter->drawImage(target, img);
        return;
    }
    target = QRectF(mm_to_units(doc, x1 + doc->marginLeft),
                    mm_to_units(doc, y1 + doc->marginTop),
                    mm_to_units(doc, x2 - x1),
                    mm_to_units(doc, y2 - y1));
    QRectF fitted = fit_to_mode(target, img, mode);
    doc->painter->drawImage(fitted, img);
}

extern "C" void owl_pdf_image_part(OwlPdfDoc* doc, const char* image_path_utf8,
                                    double x1, double y1, double x2, double y2,
                                    double sx1, double sy1, double sx2, double sy2,
                                    int mode) {
    if (!doc || !doc->painter || !image_path_utf8) return;
    QImage rawImg(QString::fromUtf8(image_path_utf8));
    if (rawImg.isNull()) {
        fprintf(stderr, "[owl_pdf_image_part] impossible de charger '%s'\n", image_path_utf8);
        return;
    }
    QImage img = apply_magenta_transparency(rawImg);
    /* Source rect en mm — conversion vers pixels image via dpi du writer.
     * Note : la doc specifie le source rect en mm (idiosyncrasie canon),
     * pas en pixels. Cohérent avec le reste des coords canon. */
    double dpi = (double)doc->dpi;
    double mm_to_px = dpi / 25.4;
    QRectF source((sx1) * mm_to_px, (sy1) * mm_to_px,
                  (sx2 - sx1) * mm_to_px, (sy2 - sy1) * mm_to_px);
    QRectF target(mm_to_units(doc, x1 + doc->marginLeft),
                  mm_to_units(doc, y1 + doc->marginTop),
                  mm_to_units(doc, x2 - x1),
                  mm_to_units(doc, y2 - y1));
    /* Crop a la sub-image avant fit_to_mode. */
    QImage cropped = img.copy(source.toRect());
    QRectF fitted = fit_to_mode(target, cropped, mode);
    doc->painter->drawImage(fitted, cropped);
}

/* --- Polices --- */

extern "C" void owl_pdf_font_create(OwlPdfDoc* doc, int font_id,
                                     double size_pt, int style_flags,
                                     const char* family_name) {
    if (!doc) return;
    QString requested = QString::fromUtf8(family_name && *family_name ? family_name : "");
    QString mapped = map_legacy_family(requested);
    QFont f(mapped);
    /* setPointSizeF preserve les fractions (vs constructor int qui arrondit). */
    f.setPointSizeF(size_pt);
    /* style_flags = bits OWL_IPOL_GRAS|ITALIQUE|SOULIGNE|BARRE
     * (cf sentinels.h). On les decode comme bitmask sur les 4 derniers bits. */
    if (style_flags & 1) f.setBold(true);
    if (style_flags & 2) f.setItalic(true);
    if (style_flags & 4) f.setUnderline(true);
    if (style_flags & 8) f.setStrikeOut(true);
    /* Hints deterministes appliques aussi sur les fontes user. */
    apply_deterministic_render_hints(nullptr, &f);
    doc->fonts.insert(font_id, f);
}

extern "C" void owl_pdf_font_use(OwlPdfDoc* doc, int font_id) {
    if (!doc || !doc->painter) return;
    auto it = doc->fonts.find(font_id);
    if (it == doc->fonts.end()) return;
    doc->currentFont = it.value();
    doc->currentFontId = font_id;
    doc->painter->setFont(doc->currentFont);
}

/* ============================================================
 * Configuration imprimante (iConfigure) — cross-platform via Qt
 * ============================================================ */

/* Imprimante "courante" du process. NULL tant que iConfigure n'a pas
 * sélectionné une imprimante. */
static QPrinter* g_currentPrinter = nullptr;

extern "C" int owl_printer_configure_dialog(void) {
    /* QPrintDialog nécessite une QApplication (event loop pas obligatoire). */
    owl_ui_init();
    QPrinter* printer = g_currentPrinter ? g_currentPrinter : new QPrinter();
    QPrintDialog dialog(printer);
    int rc = dialog.exec();   /* QDialog::Accepted = 1, Rejected = 0 */
    if (rc == QDialog::Accepted) {
        if (!g_currentPrinter) g_currentPrinter = printer;
        return 1;
    }
    if (!g_currentPrinter) delete printer;  /* dialog jeté, on garde rien */
    return 0;
}

extern "C" int owl_printer_set_by_name(const char* printer_name_utf8, int persistent) {
    /* persistent réservé pour V2 (winspool::SetDefaultPrinter Windows-only).
     * V1 : on ne fait que du process-local — le caller stdlib_pdf.c filtre
     * déjà avant d'arriver ici, donc on ignore le paramètre. */
    (void)persistent;
    if (!printer_name_utf8 || !*printer_name_utf8) return 0;
    owl_ui_init();
    QString wanted = QString::fromUtf8(printer_name_utf8);
    /* Recherche exacte d'abord, puis fallback case-insensitive. */
    QPrinterInfo info = QPrinterInfo::printerInfo(wanted);
    if (info.isNull()) {
        for (const QPrinterInfo& candidate : QPrinterInfo::availablePrinters()) {
            if (candidate.printerName().compare(wanted, Qt::CaseInsensitive) == 0) {
                info = candidate;
                break;
            }
        }
    }
    if (info.isNull()) return 0;
    if (!g_currentPrinter) g_currentPrinter = new QPrinter();
    g_currentPrinter->setPrinterName(info.printerName());
    return 1;
}

/* Buffer statique pour owl_printer_list_names / owl_printer_info — durée de
 * vie jusqu'au prochain appel. */
static QByteArray g_printerListBuf;
static QByteArray g_printerInfoBuf;

extern "C" const char* owl_printer_list_names(void) {
    owl_ui_init();
    QStringList names;
    for (const QPrinterInfo& p : QPrinterInfo::availablePrinters()) {
        names << p.printerName();
    }
    g_printerListBuf = names.join(QStringLiteral("\n")).toUtf8();
    return g_printerListBuf.constData();
}

extern "C" const char* owl_printer_info(int use_current, int info_type) {
    owl_ui_init();
    QPrinterInfo info;
    if (use_current && g_currentPrinter && !g_currentPrinter->printerName().isEmpty()) {
        info = QPrinterInfo::printerInfo(g_currentPrinter->printerName());
    }
    if (info.isNull()) {
        /* Fallback ou demande explicite : imprimante par defaut OS. */
        info = QPrinterInfo::defaultPrinter();
    }
    if (info.isNull()) {
        g_printerInfoBuf.clear();
        return "";
    }
    QString out;
    switch (info_type) {
        case 1: /* iNomDriver : Qt expose makeAndModel */
            out = info.makeAndModel();
            break;
        case 2: /* iNomPort : Qt expose location (peut etre vide selon l'OS) */
            out = info.location();
            break;
        case 0:
        default:
            out = info.printerName();
            break;
    }
    g_printerInfoBuf = out.toUtf8();
    return g_printerInfoBuf.constData();
}

/* --- iMagentaTransparent : flag global declare plus haut. --- */

extern "C" void owl_pdf_set_magenta_transparent(int active) {
    g_magentaTransparent = (active != 0);
}
extern "C" int owl_pdf_get_magenta_transparent(void) {
    return g_magentaTransparent ? 1 : 0;
}

/* --- iSelectPolice : QFontDialog --- */
extern "C" int owl_pdf_select_font_dialog(OwlPdfDoc* doc, int font_id) {
    if (!doc) return 0;
    owl_ui_init();
    bool ok = false;
    QFont selected = QFontDialog::getFont(&ok, doc->currentFont, nullptr,
                                           QString::fromUtf8("Sélection de police"));
    if (!ok) return 0;
    doc->fonts.insert(font_id, selected);
    return 1;
}
