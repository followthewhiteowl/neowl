// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_pdf.h - Natives canon Etats / PDF (i*).
 *
 * Compile seulement dans owl.exe (sous OWL_HAS_GUI), pas dans owl-cli.
 * Le backend est Qt6 via owl_pdf.h ↔ pdf_qt.cpp. */

#ifndef OWL_STDLIB_PDF_H
#define OWL_STDLIB_PDF_H

/* Injecte les implementations C (fn_i_*) dans l'array DEFS Qt-free de
 * stdlib_pdf_defs.c. A appeler AVANT stdlib_pdf_defs_register(). Build GUI
 * uniquement (ce header n'est inclus que sous OWL_HAS_GUI). */
void stdlib_pdf_impls_wire(void);

#endif
