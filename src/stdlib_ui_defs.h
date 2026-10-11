// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ui_defs.h - Donnees de registre des natives UI (Info, Erreur, ...).
 *
 * Partie Qt-FREE du module UI : l'array des OwlNativeDef (noms, signatures,
 * descriptions bilingues, references d'enum, method bindings) + les enums et
 * constantes UI (touche, fsel, modeTaille, recherche*, themeSnapshot), SANS
 * aucune implementation C. Compile dans owl.exe (GUI) ET dans les binaires
 * console (owl-cli, owl-build) pour que le COMPILATEUR resolve les natives UI
 * (arite, appartenance d'enum, out-params) a l'identique dans les deux
 * contextes — meme si le backend Qt6 qui porte les impls (stdlib_ui.c) n'est
 * pas linke cote console.
 *
 * Delta vs stdlib_pdf_defs.c : les enums/consts UI etaient enregistres par
 * stdlib_ui_init (gate OWL_HAS_GUI). Ils migrent ici et sont enregistres
 * INCONDITIONNELLEMENT par stdlib_ui_defs_register() : la validation
 * d'appartenance d'enum des parametres (fsel, touche, modeTaille...) a besoin
 * des valeurs cote console.
 *
 * Cf. .queue .../ +  : le registre console annonce les
 * signatures, l'implementation Qt reste detachee (impl=NULL cote console). */

#ifndef OWL_STDLIB_UI_DEFS_H
#define OWL_STDLIB_UI_DEFS_H

#include "object.h"  /* NativeFn */

/* Enregistre l'array DEFS des natives UI + les enums/consts UI dans le
 * registre. INCONDITIONNEL (console comme GUI). Cote GUI, stdlib_ui_impls_wire()
 * doit avoir rempli les .impl AVANT cet appel pour que register_module prenne
 * le chemin normal (registre byte-identique a l'historique). */
void stdlib_ui_defs_register(void);

/* Pose l'implementation C d'une native UI dans l'array DEFS, par nom canonique
 * (ou ascii). No-op si le nom est inconnu. Appele par stdlib_ui_impls_wire()
 * (GUI uniquement) avant stdlib_ui_defs_register(). */
void stdlib_ui_defs_set_impl(const char* canonical, NativeFn fn);

/* Vrai si `canonical` (ou son alias ascii) est une native du module UI. Permet
 * au self-test registre d'isoler exactement les natives UI (categories melees :
 * UI / Fichiers / Test / Image / Clavier) sans faux positif sur les natives
 * core partageant une categorie. */
bool stdlib_ui_defs_contains(const char* canonical);

#endif
