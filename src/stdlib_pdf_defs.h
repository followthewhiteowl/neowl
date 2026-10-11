// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_pdf_defs.h - Donnees de registre des natives Etats / PDF (i*).
 *
 * Partie Qt-FREE du module PDF : l'array des OwlNativeDef (noms, signatures,
 * descriptions bilingues, references d'enum) sans aucune implementation C.
 * Compile dans owl.exe (GUI) ET dans les binaires console (owl-cli, owl-build)
 * pour que le COMPILATEUR resolve les natives PDF (arite, appartenance d'enum,
 * out-params) a l'identique dans les deux contextes — meme si le backend Qt6
 * qui porte les impls (stdlib_pdf.c) n'est pas linke cote console.
 *
 * Cf. .queue .../ + ADR : le registre console annonce les signatures,
 * l'implementation Qt reste detachee (impl=NULL cote console). */

#ifndef OWL_STDLIB_PDF_DEFS_H
#define OWL_STDLIB_PDF_DEFS_H

#include "object.h"  /* NativeFn */

/* Enregistre l'array DEFS des natives PDF dans le registre. INCONDITIONNEL
 * (console comme GUI). Cote GUI, stdlib_pdf_impls_wire() doit avoir rempli les
 * .impl AVANT cet appel pour que register_module prenne le chemin normal. */
void stdlib_pdf_defs_register(void);

/* Pose l'implementation C d'une native PDF dans l'array DEFS, par nom canonique
 * (ou ascii). No-op si le nom est inconnu. Appele par stdlib_pdf_impls_wire()
 * (GUI uniquement) avant stdlib_pdf_defs_register(). */
void stdlib_pdf_defs_set_impl(const char* canonical, NativeFn fn);

#endif
