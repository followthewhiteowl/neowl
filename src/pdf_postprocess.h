// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* pdf_postprocess.h - Post-traitement PDF via qpdf pour iParametrePDF
 * Option B (canon).
 *
 * Le pipeline est :
 *   1. Qt6 QPdfWriter genere le PDF "brut" (page layout, fontes, images)
 *   2. iFinImprime() detecte si g_pdf_params requiert qpdf (chiffrement,
 *      signets, PDF/A-3b, attachments)
 *   3. Si oui, on appelle owl_pdf_postprocess pour ajouter ces options
 *      en post-traitement, ce qui produit le PDF final.
 *
 * Si la lib qpdf n'est pas dispo au build (OWL_HAS_QPDF non defini), la
 * fonction warne sur stderr et retourne 0 sans modifier le fichier — le
 * PDF brut Qt6 est laisse tel quel.
 */

#ifndef OWL_PDF_POSTPROCESS_H
#define OWL_PDF_POSTPROCESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Options pour le post-traitement. Mappe directement sur les champs de
 * PdfParams (stdlib_pdf.c) qui peuvent necessiter qpdf. */
typedef struct {
    /* Chiffrement / mots de passe (canon iParametrePDF syntaxes 2/3). */
    const char* passwordOpen;       /* mdp d'ouverture, NULL si aucun */
    const char* passwordOwner;      /* mdp owner (modification protection) */
    int         protectionFlags;    /* bitmask OWL_PDF_PROT_IMPRESSION/...  */
    /* Signets / outline. */
    bool        signetPage;         /* iSignetPage : 1 bookmark par page */
    /* Conformite archivage. */
    bool        pdfA3b;             /* iPDFA3b (Qt6 ne fait que A-1b) */
} OwlPdfPostOptions;

/* Post-traite path_in (PDF Qt6 brut) et ecrit le resultat dans path_out.
 * path_out peut etre identique a path_in (atomic replace via fichier
 * temporaire). Retourne 0 si OK, code d'erreur >0 sinon (warning sur
 * stderr quoi qu'il arrive). */
int owl_pdf_postprocess(const char* path_in, const char* path_out,
                         const OwlPdfPostOptions* opt);

/* Indique si qpdf est dispo au build (OWL_HAS_QPDF defini). Le runtime
 * peut ainsi adapter ses warnings : "non supporte" si pas qpdf, "applique"
 * si qpdf present. */
bool owl_pdf_has_qpdf(void);

#ifdef __cplusplus
}
#endif

#endif /* OWL_PDF_POSTPROCESS_H */
