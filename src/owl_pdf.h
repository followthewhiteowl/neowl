// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_pdf.h - API C pour generer des Etats / PDF.
 *
 * Header agnostique du backend : le runtime OWL (C99) ne voit jamais Qt
 * ni aucun engine PDF. L'implementation V1 vit dans pdf_qt.cpp (Qt6 :
 * QPdfWriter + QPainter). Un futur pdf_skia.cpp ou pdf_cairo.cpp peut
 * remplacer sans toucher au code OWL utilisateur — l'API reste stable.
 *
 * stdlib_pdf.c (seul includer de ce header) est compile dans owl.exe avec
 * OWL_HAS_GUI defini. La variante owl-cli (sans Qt/qpdf) ne l'embarque pas
 * et degrade proprement via les #ifdef.
 *
 * Convention d'unites : tout est en MILLIMETRES (mm) en V1. canon utilise
 * historiquement l'unite "mm/100" mais les utilisateurs raisonnent en
 * mm direct. Le backend convertit en pixels DPI via QPainter.
 *
 * Origin : haut-gauche du papier (apres marges si iMarge appelee).
 */

#ifndef OWL_PDF_H
#define OWL_PDF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Types opaques : le client C ne voit jamais le vrai type backend. */
typedef struct OwlPdfDoc  OwlPdfDoc;

/* Type opaque pour les images OWL (cf owl_image.h). En mode iImageMemoire,
 * chaque page finalisee est passee au callback sous forme d'OwlImage*.
 * Guard pour eviter la redefinition si owl_image.h est aussi inclus. */
#ifndef OWL_IMAGE_TYPEDEF_DECLARED
#define OWL_IMAGE_TYPEDEF_DECLARED 1
typedef struct OwlImage OwlImage;
#endif

/* ------- Cycle de vie d'un Etat ------- */

/* Cree un nouveau document PDF, vise un fichier de sortie. La page 1
 * est creee implicitement. Le format est A4 portrait par defaut.
 * Retourne NULL si echec d'ouverture du fichier.
 *
 * Apres owl_pdf_open, le document est *courant* — toutes les fonctions
 * iImprime/iLigne/etc. operent dessus. owl_pdf_close finalise et libere. */
OwlPdfDoc* owl_pdf_open(const char* output_path_utf8);

/*  — Variante "image memoire" : au lieu de produire un PDF sur
 * disque, chaque page est rasterisee dans une QImage interne et flushee
 * vers le callback on_page sous forme d'OwlImage* (RGBA8). Le callback
 * recoit l'image apres chaque iSautePage et apres owl_pdf_close.
 * Le callback prend possession de l'OwlImage* (a charge a lui de la
 * stocker ou la liberer). userdata est repasse tel quel.
 *
 * dpi : resolution de rasterisation (300 par defaut pour parite avec PDF).
 * La taille de page est posee a A4 portrait par defaut ; le caller appelle
 * owl_pdf_set_page_size_mm / owl_pdf_set_margins ensuite. */
typedef void (*OwlPdfPageCallback)(OwlImage* page, void* userdata);
OwlPdfDoc* owl_pdf_open_image_memory(int dpi,
                                      OwlPdfPageCallback on_page,
                                      void* userdata);

/* Termine et ferme le document courant. Tout doit avoir ete ecrit avant.
 * Retourne 1 succes / 0 echec. */
int  owl_pdf_close(OwlPdfDoc* doc);

/* Document "courant" — toutes les fonctions iXxx operent sur le doc actif. */
OwlPdfDoc* owl_pdf_get_current(void);

/* ------- Mise en page ------- */

/* Marges en mm. S'appliquent au document complet. Ne peut etre change
 * qu'avant la 1ere primitive d'impression. */
void owl_pdf_set_margins(OwlPdfDoc* doc, double top, double bottom,
                          double left, double right);

/* Taille de page en mm (largeur, hauteur). Defaut A4 = 210x297. Pour
 * paysage : echanger les deux. */
void owl_pdf_set_page_size_mm(OwlPdfDoc* doc, double width_mm, double height_mm);

/* iParametrePDF — options Qt6 supportees (canon).
 * Doivent etre appelees IMMEDIATEMENT apres owl_pdf_open, avant la 1ere
 * primitive d'impression. */
void owl_pdf_set_grayscale(OwlPdfDoc* doc, int active);     /* iPDFNiveauGris */
void owl_pdf_set_pdf_a1b(OwlPdfDoc* doc, int active);       /* iPDFA / iPDFA1b */

/* Cree une nouvelle page apres celle en cours. Le curseur revient en
 * haut-gauche. */
void owl_pdf_new_page(OwlPdfDoc* doc);

/* Numero de la page courante (1-based) et nombre total imprime. */
int  owl_pdf_page_number(OwlPdfDoc* doc);

/* Initialise le compteur logique de page (canon iNumPage(<n>)).
 * N'affecte pas le PDF physique — seulement la valeur retournee par
 * owl_pdf_page_number et le point de depart pour les newPage suivants. */
void owl_pdf_set_page_number(OwlPdfDoc* doc, int n);

/* Dimensions imprimables en mm (apres marges). */
double owl_pdf_page_width_mm(OwlPdfDoc* doc);
double owl_pdf_page_height_mm(OwlPdfDoc* doc);
double owl_pdf_remaining_height_mm(OwlPdfDoc* doc);

/* ------- Texte ------- */

/* Imprime sans saut de ligne (continue sur la meme baseline). */
void owl_pdf_print_word(OwlPdfDoc* doc, const char* text_utf8);

/* Imprime dans un rectangle (X1,Y1)-(X2,Y2) en mm. alignement :
 *   0 = gauche (defaut), 1 = centre, 2 = droite. Wrap auto. */
void owl_pdf_print_zone(OwlPdfDoc* doc, const char* text_utf8,
                         double x1, double y1, double x2, double y2,
                         int alignement);

/* Largeur en mm que prendrait le texte avec la police courante. */
double owl_pdf_text_width_mm(OwlPdfDoc* doc, const char* text_utf8);

/* Hauteur en mm de la police courante (line height = ascent + descent + leading). */
double owl_pdf_text_height_mm(OwlPdfDoc* doc);

/* Charge une image et retourne ses dimensions en mm a la resolution du
 * document. *out_w_mm et *out_h_mm sont mis a 0 si l'image ne charge pas.
 * Aucun facteur d'agrandissement applique (taille native). */
void owl_pdf_image_size_mm(OwlPdfDoc* doc, const char* image_path_utf8,
                            double* out_w_mm, double* out_h_mm);

/* Saute une ligne avec la police courante (avance Y). */
void owl_pdf_newline(OwlPdfDoc* doc);

/* Pose la position X (en mm relatif a la marge gauche) du prochain print.
 * Utilise par les chaines de controle iPosX(...) parsees par stdlib_pdf. */
void owl_pdf_set_cursor_x(OwlPdfDoc* doc, double x_mm);

/* Pose la position Y (en mm relatif a la marge haute) du prochain print. */
void owl_pdf_set_cursor_y(OwlPdfDoc* doc, double y_mm);

/* Lecture de la position courante du curseur (mm). */
double owl_pdf_get_cursor_x(OwlPdfDoc* doc);
double owl_pdf_get_cursor_y(OwlPdfDoc* doc);

/* ------- Geometrie -------
 *
 * Style trait : 0=continu, 1=tiret, 2=pointille, 3=mixte
 *               (mapping vers Qt::PenStyle dans le backend)
 * Brosse remplissage : 0=pleine, 1=horiz, 2=vert, 3=croisee,
 *                      4=diag desc, 5=diag mont, 6=croisee diag
 *               (mapping vers Qt::BrushStyle)
 * Bord (iCadre uniquement) : bitmask 0x01=haut, 0x02=bas, 0x04=gauche,
 *               0x08=droit. 0xFF (ou 0) = tous (defaut). */

/* Trait de (x1,y1) a (x2,y2). Epaisseur en mm. RGB packe BGR (canon :
 * R + V*256 + B*65536). border_color_bgr < 0 = noir par defaut.
 * cap_style : 0 = round (defaut iLigne), 1 = flat/carre (iTrait*). */
void owl_pdf_line(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                   double thickness_mm, int color_bgr, int trait_style, int cap_style);

/* Rectangle. fill_color_bgr < 0 = pas de remplissage. trait_style/brush_style
 * cf doc en tete. border_mask : voir Bord. */
void owl_pdf_rect(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                   double thickness_mm, int border_color_bgr, int fill_color_bgr,
                   int trait_style, int brush_style, int border_mask);

/* Rectangle aux coins arrondis. radius en mm. Pas de border_mask (canon). */
void owl_pdf_rounded_rect(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                           double radius_h_mm, double radius_v_mm,
                           double thickness_mm, int border_color_bgr, int fill_color_bgr,
                           int trait_style, int brush_style);

/* Ellipse inscrite dans le rectangle. */
void owl_pdf_ellipse(OwlPdfDoc* doc, double x1, double y1, double x2, double y2,
                      double thickness_mm, int border_color_bgr, int fill_color_bgr,
                      int trait_style, int brush_style);

/* ------- Images ------- */

/* Mode d'affichage : 0=normal etire, 1=homothetique (proportionnel ancrage
 * haut-gauche), 2=homocentre (proportionnel centre dans la zone). */

/* Charge une image (JPEG/PNG/BMP) et la dessine dans le rectangle cible.
 * mode = 0/1/2 cf ci-dessus. Si target_x2/y2 <= target_x1/y1 (zone vide),
 * l'image est dessinee a sa taille native a partir de target_x1/y1
 * scaled by zoom_pct (canon syntaxe 2). */
void owl_pdf_image(OwlPdfDoc* doc, const char* image_path_utf8,
                    double x1, double y1, double x2, double y2,
                    int mode, double zoom_pct);

/* Variante "partie d'image" (canon syntaxe 3) : dessine la sous-zone
 * (sx1,sy1)-(sx2,sy2) de l'image dans le rect cible (x1,y1)-(x2,y2). */
void owl_pdf_image_part(OwlPdfDoc* doc, const char* image_path_utf8,
                         double x1, double y1, double x2, double y2,
                         double sx1, double sy1, double sx2, double sy2,
                         int mode);

/* Active/désactive le traitement transparent du magenta pur (#FF00FF)
 * dans toutes les impressions d'image suivantes (canon iMagentaTransparent).
 * Etat global au process. */
void owl_pdf_set_magenta_transparent(int active);
int  owl_pdf_get_magenta_transparent(void);

/* Ouvre un dialog de selection de police (QFontDialog cross-platform).
 * Si l'utilisateur valide, crée une police identifiée par font_id avec
 * la sélection. Retourne 1 si validé, 0 si annulé. */
int owl_pdf_select_font_dialog(OwlPdfDoc* doc, int font_id);

/* ------- Polices ------- */

/* Cree (ou remplace) une police identifiee par font_id. style est une
 * combinaison de OWL_IPOL_GRAS|ITALIQUE|SOULIGNE|BARRE (cf sentinels.h).
 * size_pt en points typographiques (1pt = 1/72 inch). */
void owl_pdf_font_create(OwlPdfDoc* doc, int font_id,
                          double size_pt, int style_flags,
                          const char* family_name);

/* Active la police pour les prochaines primitives texte. */
void owl_pdf_font_use(OwlPdfDoc* doc, int font_id);

/* ------- Configuration imprimante (canon iConfigure) -------
 *
 * Indépendant des OwlPdfDoc — agit sur l'état imprimante globale du
 * processus (équivalent du QPrinter courant dans Qt).
 *
 * Cross-platform via Qt6::PrintSupport : sur Windows ça lance le PrintDlg
 * natif, sur Linux le dialog GTK/CUPS, sur Mac NSPrintPanel. */

/* Ouvre le dialog standard de configuration d'impression (modal).
 * Retourne 1 si l'utilisateur valide, 0 si annule.
 * Initialise QApplication si pas encore présent (via owl_ui_init). */
int owl_printer_configure_dialog(void);

/* Sélectionne une imprimante par nom (tel qu'apparu dans le panneau
 * de config OS / QPrinterInfo::availablePrinters).
 *  - persistent = 0 : changement temporaire pour le process en cours
 *  - persistent = 1 : changement persistant de l'imprimante par défaut OS
 *                     (Windows uniquement en V1, retourne 0 ailleurs).
 * Retourne 1 si l'imprimante a été trouvée et activée, 0 sinon. */
int owl_printer_set_by_name(const char* printer_name_utf8, int persistent);

/* Liste des imprimantes systeme separees par RC ('\n'). Buffer interne. */
const char* owl_printer_list_names(void);

/* Info d'imprimante : nom, driver, ou port. info_type :
 *   0 = nom (defaut), 1 = driver (makeAndModel), 2 = port (location).
 * use_current : 1 = imprimante courante (set via iConfigure), 0 = imprimante
 * par défaut OS. Retourne "" si aucune trouvée. Buffer interne. */
const char* owl_printer_info(int use_current, int info_type);

#ifdef __cplusplus
}
#endif

#endif /* OWL_PDF_H */
