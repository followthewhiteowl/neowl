// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr_defs.h - Donnees de registre des natives OCR (OCRExtraitTexte, ...).
 *
 * Partie Tesseract-FREE du module OCR : l'array des OwlNativeDef (noms,
 * signatures, descriptions bilingues) SANS aucune dependance au moteur OCR.
 * Compile dans TOUS les binaires (owl.exe, owl-cli, owl-build, console comme
 * GUI) pour que le COMPILATEUR resolve les natives OCR (arite, types) a
 * l'identique quel que soit le build — meme si le backend Tesseract qui porte
 * les impls (stdlib_ocr_engine.cpp) n'est linke que sous OWL_HAS_OCR.
 *
 * Calque exact du pattern UI/PDF (stdlib_ui_defs / stdlib_pdf_defs) : le
 * registre annonce les signatures, l'impl reste detachee (impl=NULL quand OCR
 * n'est pas compile) -> appel runtime = erreur claire, jamais un plantage. */

#ifndef OWL_STDLIB_OCR_DEFS_H
#define OWL_STDLIB_OCR_DEFS_H

#include "object.h"  /* NativeFn */

/* Enregistre l'array DEFS des natives OCR dans le registre. INCONDITIONNEL
 * (tous les builds). Sous OWL_HAS_OCR, stdlib_ocr_impls_wire() doit avoir rempli
 * les .impl AVANT cet appel pour que register_module prenne le chemin normal. */
void stdlib_ocr_defs_register(void);

/* Pose l'implementation C d'une native OCR dans l'array DEFS, par nom canonique
 * (ou ascii). No-op si le nom est inconnu. Appele par stdlib_ocr_impls_wire()
 * (OCR uniquement) avant stdlib_ocr_defs_register(). */
void stdlib_ocr_defs_set_impl(const char* canonical, NativeFn fn);

#endif
