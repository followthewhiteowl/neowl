// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* event_catalog_widgets.h — API du catalogue d'events widget + vue
 * dedupliquee consommee par la famille evt* (DeclencheEvenement / TriggerEvent,
 * ).
 *
 * event_catalog_widgets_init() enregistre les tables d'events par kind dans
 * owl_event_catalog (source de verite unique). Juste apres, une VUE DEDUPLIQUEE
 * des events widget est construite : un meme event partage entre plusieurs
 * widgets (surClic, surAChaqueModification, ...) n'apparait qu'une fois, avec
 * un INDEX STABLE. Cet index sert de cle de mapping evt<->event :
 *   - ui_catalog.c genere une constante evt* de valeur OWL_EVT_BASE + index ;
 *   - le runtime (stdlib_ui.c) decode value -> index -> OwlEventDef ;
 *   - le compilateur resout la constante -> index -> signature (params).
 *
 * Ajouter un event au catalogue widget expose donc sa constante evt*
 * automatiquement, sans edition manuelle (1:1, zero filtre).
 */

#ifndef OWL_EVENT_CATALOG_WIDGETS_H
#define OWL_EVENT_CATALOG_WIDGETS_H

#include "owl_event_catalog.h"   /* OwlEventDef */

#ifdef __cplusplus
extern "C" {
#endif

/* Enregistre les tables d'events par widget kind + (re)construit la vue
 * dedupliquee. Appelee depuis stdlib_ui_catalog_init() et le LSP. */
void event_catalog_widgets_init(void);

/* Nombre d'events widget uniques (apres deduplication par nom canonique). */
int owl_widget_event_dedup_count(void);

/* Event widget unique a l'index stable donne, ou NULL si hors bornes. */
const OwlEventDef* owl_widget_event_dedup_at(int index);

#ifdef __cplusplus
}
#endif

#endif /* OWL_EVENT_CATALOG_WIDGETS_H */
