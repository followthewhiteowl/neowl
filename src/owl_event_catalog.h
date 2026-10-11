// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_event_catalog.h — catalogue typé des events DSL widget.
 *
 * Source de vérité unique pour :
 *  - Le compilateur : `compiler.c::check_event_signature` valide la décl
 *    `PROCEDURE surX(...)` dans un widget body contre cette table.
 *  - Le runtime : `stdlib_ui.c::register_widget_events` itère le catalogue
 *    pour chaque widget créé et branche les events sur l'OwlEventFilter Qt.
 *  - Le LSP : autocomplete + hover signature + `--dump-lsp-catalog`.
 *  - Le self-test : règle 8 (params typés, descFR/EN non vides, pas de
 *    doublon) + règle 9 (tout binding runtime ⊆ catalogue).
 *
 * Cf. note interne sur l'evolution du widget Image et CLAUDE.md section
 * "Ajouter un nouveau widget UI".
 */

#ifndef OWL_EVENT_CATALOG_H
#define OWL_EVENT_CATALOG_H

#include "ui_decl.h"   /* OwlControlKind */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Un parametre canon d'un event. Le nom est exposé en hover LSP, donc
 * stable une fois publié. Le type doit être un nom de type OWL connu
 * ("entier", "chaîne", "booléen", "réel", ...). */
typedef struct {
    const char* name;        /* "x" */
    const char* typeName;    /* "entier" — accepte la forme accentuée OU ASCII */
    const char* descFR;      /* "Position horizontale du curseur..." */
    const char* descEN;
} OwlEventParam;

/* Un event canon disponible sur un widget kind. */
typedef struct {
    const char*           canonical;    /* "surMolette" */
    const char*           nameEN;       /* "onWheel" — peut etre NULL */
    const char*           descFR;       /* obligatoire — non NULL/vide */
    const char*           descEN;       /* obligatoire */
    int                   paramCount;   /* 0..N */
    const OwlEventParam*  params;       /* NULL si paramCount == 0 */
} OwlEventDef;

/* ============================================================
 * API d'enregistrement (appelée par stdlib_ui_init / autres modules)
 * ============================================================ */

/* Enregistre une table d'events pour un widget kind. La table doit etre
 * statique (pointeurs conservés). Le self-test valide chaque entrée
 * a posteriori (cf registry_self_test.c regle 8). */
void owl_event_catalog_register(OwlControlKind kind,
                                const OwlEventDef* defs, int count);

/* ============================================================
 * Lookup
 * ============================================================ */

/* Cherche un event par nom canonique pour un kind donne.
 * Match accent-insensitive et case-insensitive (cohérent avec le scanner).
 * Retourne NULL si non trouvé. */
const OwlEventDef* owl_event_catalog_find(OwlControlKind kind,
                                           const char* eventName);

/* Iteration : pour LSP autocomplete et dump catalogue.
 * Retourne le nombre d'events pour ce kind. */
int                 owl_event_catalog_count(OwlControlKind kind);
const OwlEventDef*  owl_event_catalog_at(OwlControlKind kind, int index);

/* Vidange (appelée par vm_init avant ré-enregistrement). */
void                owl_event_catalog_reset(void);

/* ============================================================
 * Catalogue parallèle pour les events d'État (rapports d'impression).
 *
 * Les états ne sont pas dans l'enum OwlControlKind (ce sont des
 * structures runtime distinctes, pas des widgets Qt). On expose donc
 * une API parallèle qui réutilise EXACTEMENT les mêmes types
 * `OwlEventDef` / `OwlEventParam` — même sémantique, juste un autre
 * conteneur de stockage.
 *
 * Couvre les events lifecycle de niveau Etat (Filtre / Lecture /
 * Fermeture). L'event implicite "Ouverture" (top-level body) n'est
 * pas dans le catalogue — c'est une convention syntaxique, pas un
 * event nommé.
 * ============================================================ */

void                owl_etat_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_etat_event_catalog_find(const char* eventName);
int                 owl_etat_event_catalog_count(void);
const OwlEventDef*  owl_etat_event_catalog_at(int index);

/* ============================================================
 * Catalogue des events de cycle de vie des sous-contrôles d'un
 * bloc État (Libellé, Image dans Corps/EnTeteDocument/...).
 *
 * Distinct du catalogue Etat ci-dessus : celui-ci couvre les events
 * niveau-bloc (Filtre/Lecture/Fermeture), alors qu'ici on est sur
 * les events niveau-élément à l'intérieur d'un bloc d'Etat.
 *
 * Aujourd'hui : Initialisation, AvantImpression (canon).
 * Le compilateur consomme ce catalogue pour valider les PROCEDURE
 * déclarées dans un sous-contrôle d'État — aucune liste codée en dur.
 * ============================================================ */

void                owl_etat_subcontrol_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_etat_subcontrol_event_catalog_find(const char* eventName);
int                 owl_etat_subcontrol_event_catalog_count(void);
const OwlEventDef*  owl_etat_subcontrol_event_catalog_at(int index);

/* ============================================================
 * Catalogue des events d'une OptionMenu (menu classique, ).
 *
 * Les noeuds de menu ne sont pas dans OwlControlKind (ce ne sont pas des
 * widgets posables) : catalogue parallele, memes types OwlEventDef.
 * Aujourd'hui : surSelection (clic ou validation clavier). Consomme par
 * le compilateur pour valider les PROCEDURE declarees dans une OptionMenu.
 * ============================================================ */

void                owl_menu_option_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_menu_option_event_catalog_find(const char* eventName);
int                 owl_menu_option_event_catalog_count(void);
const OwlEventDef*  owl_menu_option_event_catalog_at(int index);

/* ============================================================
 * Catalogue des events lifecycle de niveau Fenetre.
 *
 * Distinct du catalogue widget (`owl_event_catalog_*` indexe par
 * OwlControlKind) : les lifecycle Fenetre n'ont pas de payload type
 * et ne sont pas consommes via le dispatch `register_widget_events`
 * — c'est le compilateur (`is_predefined_window_block`) et le LSP
 * (autocomplete + semantic tokens) qui les consomment directement.
 *
 * Aujourd'hui : FinInitialisation, Fermeture, AutorisationFermeture,
 * DemandeMiseAJour (canon). Les alias EN sont portes par le champ
 * `nameEN` de chaque OwlEventDef et reconnus en lookup.
 *
 * Source unique : le compilateur, le LSP et le dump --vscode-data
 * consomment ce catalogue ; pas de table hardcodee parallele.
 * ============================================================ */

void                owl_window_lifecycle_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_window_lifecycle_catalog_find(const char* eventName);
int                 owl_window_lifecycle_catalog_count(void);
const OwlEventDef*  owl_window_lifecycle_catalog_at(int index);

/* ============================================================
 * Catalogue des events de cycle de vie d'un bloc
 * CollectionDeProcedures.
 *
 * Une collection n'est ni un widget (OwlControlKind) ni un rapport :
 * catalogue parallele, memes types OwlEventDef. Aujourd'hui : Terminaison
 * (liberation des ressources a la fermeture du projet). L'init implicite
 * vit au top-level du bloc (pas d'event nomme). Consomme par le LSP
 * (completion + hover + coloration) ; le hook runtime vit dans project_run.c.
 * ============================================================ */

void                owl_collection_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_collection_event_catalog_find(const char* eventName);
int                 owl_collection_event_catalog_count(void);
const OwlEventDef*  owl_collection_event_catalog_at(int index);

/* ============================================================
 * Catalogue des events de niveau ServiceREST (DSL de service HTTP).
 *
 * Un service n'est ni un widget (OwlControlKind) ni un rapport :
 * catalogue parallele, memes types OwlEventDef. Couvre les hooks
 * declares directement dans le corps d'un `IDENT est un ServiceREST` :
 * surAuthentification, surAutorisation, surErreur, FinInitialisation,
 * Fermeture. Source unique consommee par le compilateur (validation
 * des PROCEDURE de niveau service, en remplacement de l'ancienne liste
 * en dur) et le LSP (coloration event + completion filtree). Les alias
 * EN sont portes par le champ nameEN et reconnus en lookup.
 * ============================================================ */

void                owl_service_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_service_event_catalog_find(const char* eventName);
int                 owl_service_event_catalog_count(void);
const OwlEventDef*  owl_service_event_catalog_at(int index);

/* ============================================================
 * Catalogue des events de niveau OpérationHTTP.
 *
 * Une operation est le sous-bloc `IDENT est une OpérationHTTP` d'un
 * ServiceREST : catalogue parallele distinct du niveau service.
 * Aujourd'hui : surRequête (traitement d'une requete, retour chaine).
 * Meme consommation que le catalogue service (compilo + LSP).
 * ============================================================ */

void                owl_operation_event_catalog_register(const OwlEventDef* defs, int count);
const OwlEventDef*  owl_operation_event_catalog_find(const char* eventName);
int                 owl_operation_event_catalog_count(void);
const OwlEventDef*  owl_operation_event_catalog_at(int index);

/* ============================================================
 * Lookup transverse : cherche un event dans TOUS les catalogues
 * (widgets indexes par kind + Etat + sous-controle d'Etat +
 * lifecycle Fenetre + OptionMenu + CollectionDeProcedures +
 * ServiceREST + OpérationHTTP).
 *
 * Meme semantique de match que les find() specialises : case +
 * accent insensible, forme FR canonique OU alias EN (`nameEN`).
 *
 * Destine aux consommateurs qui n'ont pas de conteneur cible en
 * tete et veulent juste savoir « ce nom designe-t-il un event
 * publie ? » (self-test, outils doc). Retourne NULL sinon.
 * ============================================================ */
const OwlEventDef*  owl_event_catalog_find_any(const char* eventName);

#ifdef __cplusplus
}
#endif

#endif /* OWL_EVENT_CATALOG_H */
