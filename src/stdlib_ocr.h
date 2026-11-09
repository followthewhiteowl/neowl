// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr.h - Cablage des implementations C des natives OCR.
 *
 * Injecte les impls (fn_ocr_*) dans l'array DEFS Tesseract-free de
 * stdlib_ocr_defs.c. A appeler AVANT stdlib_ocr_defs_register(). Build OCR
 * uniquement (ce header n'est inclus que sous OWL_HAS_OCR). */

#ifndef OWL_STDLIB_OCR_H
#define OWL_STDLIB_OCR_H

void stdlib_ocr_impls_wire(void);

#endif
