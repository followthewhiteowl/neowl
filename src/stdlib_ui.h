// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ui.h - Natives GUI canon pour OWL (compile avec OWL_HAS_GUI).
 *
 * Compilé seulement dans owl.exe (pas owl-cli, cf CMakeLists.txt). Le module
 * enregistre les 9 natives canon Tier 1 :
 *   Ouvre / OuvreFille / Ferme
 *   Info / Erreur / Avertissement / Confirmer / OuiNon
 *   FinProgramme
 */

#ifndef OWL_STDLIB_UI_H
#define OWL_STDLIB_UI_H

#include "etat_decl.h"   /*  : OwlEtatDecl pour owl_etat_prepare_props */

#ifdef __cplusplus
extern "C" {
#endif

void stdlib_ui_init(void);

/* Injecte les implementations Qt (fn_*) dans l'array DEFS Qt-free de
 * stdlib_ui_defs.c. A appeler AVANT stdlib_ui_defs_register(). Build GUI
 * uniquement (ce header n'est inclus que sous OWL_HAS_GUI). */
void stdlib_ui_impls_wire(void);

/* Prepare le pipeline rendu d'un Etat avant lookup props.
 *
 * Symetrique de ce que build_window() fait pour la Fenetre : eval les
 * ObjFunction 0-arg via resolve_dynamic_props et store en const.
 *
 * Couvre 3 niveaux : props de l'Etat, props de chaque bloc, props de chaque
 * controle de chaque bloc. Appele 1x par iImprimeEtat, avant tout acces aux
 * props (apply_etat_properties, render_etat_*, ...). Idempotent.
 *
 * Les cascades homonymes sont regroupees au compile-time depuis 
 * (cf. CascadeBuffer/cascade_buffer_flush dans compiler.c). */
void owl_etat_prepare_props(OwlEtatDecl* etat);
/*  — resout les props des sous-champs APRES le top-level de l'Etat. */
void owl_etat_resolve_control_props(OwlEtatDecl* etat);
/*  — re-evalue UNE prop de controle a evaluateur avec la valeur
 * courante des variables (degel expression PROPRIETES par rendu de bloc). */
void owl_etat_prop_reeval(OwlPropertyInit* p);

/*  — resout la Value widget associee a un nom (case+accent insensible)
 * via vm.globals[<strippe(name)>]. Retourne typiquement un ObjUiControlRef
 * (pour un champ) ou un ObjWindowRef (pour une fenetre), poses par le compilo
 * lors de la declaration. NIL_VAL si pas trouve.
 *
 *  — expose pour reuse depuis stdlib_pdf.c (push event_sender autour
 * des hooks d'Etat). Cf. stdlib_ui.c, definition unique. */
Value owl_moimeme_widget_value_for_name(const char* name);

/* Lot V — Live preview : compile le source, affiche la 1ere fenetre,
 * installe un watcher sur le file. A chaque modification, recompile +
 * rebuild la fenetre.
 *
 * Bloquant : entre dans la boucle event Qt jusqu'a fermeture user.
 * Retourne 0 succes, non-zero erreur. */
int stdlib_ui_preview_run(const char* filepath);

/*  — plancher irreductible d'une famille de champ, publie a l'editeur
 * par `owl --vscode-data`. `kind` est un OwlControlKind.
 *
 * Rend 1 si la famille est positionnable et a pu etre mesuree, 0 sinon (les
 * blocs structurels — Colonne, Volet, sous-blocs d'Etat, menus — n'ont pas de
 * plancher propre). `out_option_spacing` recoit l'espacement du layout interne
 * quand la famille en a un, -1 sinon.
 *
 * Mesure, pas table : un exemplaire neuf est cree par la meme fonction que le
 * montage reel, hors ecran, puis interroge. */
/*  — `out_content_agnostic` (1/0) : le plancher de la famille tient-il
 * constant quoi qu'on y mette ? Etabli en re-mesurant a police gonflee, jamais
 * par une liste ecrite a la main. Vrai pour Table / Liste / ZoneRepetee. */
int owl_ui_min_floor_for_kind(int kind, int* out_w, int* out_h,
                              int* out_option_spacing,
                              int* out_content_agnostic);

/*  — police de reference (points) a laquelle les planchers sont mesures,
 * pour que l'editeur derive un plancher qui suit sa police. Meme valeur pour
 * toutes les familles (le defaut du modele). A appeler tot apres vm_init : le
 * template Police n'est pas racine du GC. Rend -1 si indisponible. */
int owl_ui_default_police_taille(void);

/*  — self-test du contrat bindings sous Ouvre imbrique (mark/restore
 * de g_binding_count). C pur, pas de Qt. Cable via --self-test-ui-bindings.
 * Retourne le nombre d'echecs (0 = OK). */
int ui_bindings_self_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* OWL_STDLIB_UI_H */
