// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_presentation.h — Couche 2 : intention de presentation du designer.
 *
 * Couche decouplee du modele semantique (ui_catalog.c / OwlControlPropDef).
 * Elle ne decrit PAS ce qu'une propriete est (type, defaut, contrat runtime)
 * mais comment l'inspecteur du designer la presente : regroupement UX abstrait,
 * importance (courant/avance), type d'editeur abstrait, ordre, visibilite, et
 * sous-ensemble optionnel de valeurs d'enum a proposer.
 *
 * Contrat d'archi (cf docs/ADR architecture-presentation-designer) :
 *   - Couche 1 (ui_catalog.c) : semantique du langage. Intouchee, ne connait
 *     rien du front.
 *   - Couche 2 (ce module) : intention de presentation. Reference les props de
 *     la couche 1 par cle (kind + nom canon), ne les modifie jamais.
 *   - Couche 3 (front VSCode) : mapping groupe abstrait -> onglet concret.
 *
 * Le mapping groupe -> onglet Aurora vit cote front, PAS ici : la couche 2
 * ignore Aurora et ses onglets.
 *
 * Perimetre actuel : widget Saisie uniquement (pilote). Les autres widgets
 * sont opt-in via owl_presentation_kind_is_covered() — le self-test ne controle
 * que les kinds couverts.
 */

#ifndef OWL_UI_PRESENTATION_H
#define OWL_UI_PRESENTATION_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Regroupement semantique abstrait. Mappe vers un onglet par la couche 3. */
typedef enum {
    OWL_PRES_GROUP_IDENTITE,     /* nom / libelle d'identification */
    OWL_PRES_GROUP_CONTENU,      /* contenu, type, format, masque */
    OWL_PRES_GROUP_UI,           /* position, taille, etat, visibilite */
    OWL_PRES_GROUP_DISPOSITION,  /* layout enfant : marges, span, etirement */
    OWL_PRES_GROUP_APPARENCE,    /* police, couleurs, style */
    OWL_PRES_GROUP_COMPORTEMENT, /* lecture seule, obligatoire, bornes */
    OWL_PRES_GROUP_AIDE,         /* bulle, message d'aide */
    OWL_PRES_GROUP_AVANCE,       /* divers avances (groupe, ...) */
} OwlPresGroup;

/* Mise en avant dans l'inspecteur. `avance` = replie/masque par defaut. */
typedef enum {
    OWL_PRES_IMPORTANCE_COURANT,
    OWL_PRES_IMPORTANCE_AVANCE,
} OwlPresImportance;

/* Type d'editeur abstrait. Le front choisit le widget concret.
 * `variant` n'est pas dans la taxonomie initiale : ajoute pour la prop Groupe
 * (typee VARIANT cote semantique, exposee en avance). */
typedef enum {
    OWL_PRES_EDITOR_TEXT,
    OWL_PRES_EDITOR_INT,
    OWL_PRES_EDITOR_BOOL,
    OWL_PRES_EDITOR_ENUM,
    OWL_PRES_EDITOR_RADIO,   /* choix exclusif court (ex: Etat) — le front rend
                              * un groupe radio plutot qu'une liste deroulante */
    OWL_PRES_EDITOR_BOOL_COMBO, /* liste deroulante 2 choix adossee a une prop BOOL.
                              * Les deux libelles (faux/vrai) sont portes par
                              * bool_radio_labels ; le front rend un combo mappe au
                              * booleen (option 1 -> faux, option 2 -> vrai). */
    OWL_PRES_EDITOR_COLOR,
    OWL_PRES_EDITOR_FONT,
    OWL_PRES_EDITOR_IMAGE_FILE, /* image riche : catalogue d'icones + fichier.
                              * Le front rend un selecteur de catalogue + parcours
                              * fichier. Pose sur les props source-image
                              * (Bouton.Image, OptionMenu.Image, Champ Image,
                              * feuille de style styleImageBouton.Image). */
    OWL_PRES_EDITOR_SHORTCUT, /* raccourci clavier : capture de frappe. Le front
                              * rend un champ qui enregistre la combinaison de
                              * touches tapee plutot qu'un texte libre. Pose sur
                              * les props ToucheRaccourci / Raccourci. */
    OWL_PRES_EDITOR_MULTILANGUE,
    OWL_PRES_EDITOR_COMPOSITE,
    OWL_PRES_EDITOR_POSITION,
    OWL_PRES_EDITOR_VARIANT,
} OwlPresEditor;

/* Presence dans l'inspecteur. `cache` = jamais affiche (runtime-only,
 * raccourci plat couvert par un composite, ou redondant avec une autre prop). */
typedef enum {
    OWL_PRES_VIS_DESIGNER,
    OWL_PRES_VIS_CACHE,
} OwlPresVisibilite;

/*  — libelles d'un radio 2-etats porte par une prop BOOL. Capacite
 * generique : une prop booleenne (ex Multisélection) se presente comme un choix
 * exclusif a deux options plutot qu'une case a cocher. Le titre du groupe radio
 * est porte par labelFR/labelEN de l'entree ; ce descripteur porte les libelles
 * des deux options (faux -> option 1, vrai -> option 2), bilingue. Pose sur une
 * entree editor=RADIO d'une prop BOOL ; NULL pour tout autre cas. */
typedef struct {
    const char* falseFR;   /* libelle de l'option "faux" (FR) */
    const char* trueFR;    /* libelle de l'option "vrai" (FR) */
    const char* falseEN;   /* libelle de l'option "faux" (EN, invariant 3) */
    const char* trueEN;    /* libelle de l'option "vrai" (EN) */
} OwlPresBoolRadioLabels;

typedef struct {
    const char*       kind;        /* "Saisie", ... — cle vers la couche 1 */
    const char*       prop;        /* nom de la prop (forme canon, resolu via
                                    * owl_dsl_prop_find, accent/casse-insensible) */
    const char*       labelFR;     /* libelle court d'affichage dans l'inspecteur
                                    * (FR). Distinct du nom canon technique. NULL
                                    * autorise pour les entrees cache (invisibles
                                    * dans le designer). Obligatoire non-vide sur
                                    * toute entree visibilite=designer (self-test). */
    const char*       labelEN;     /* idem, EN (invariant 3 : bilingue). */
    OwlPresGroup      groupe;
    OwlPresImportance importance;
    OwlPresEditor     editor;
    int               ordre;       /* ordre d'affichage dans le groupe */
    OwlPresVisibilite visibilite;
    const char* const* enum_subset; /* NULL = toutes les valeurs semantiques ;
                                     * sinon liste NULL-terminee du sous-ensemble
                                     * propose dans l'editeur (ex: Etat sans
                                     * Invisible). */
    const char* const* enum_labels_fr; /* libelles d'affichage FR paralleles a
                                        * enum_subset (meme ordre, meme longueur).
                                        * NULL = le front affiche la valeur brute
                                        * (cas des enums dont les valeurs sont deja
                                        * lisibles, ex Etat=Actif/AffichageSeulement/
                                        * Grisé). Pose quand les valeurs sont des
                                        * constantes prefixees (ex typeBouton=btn*)
                                        * et doivent s'afficher en clair. */
    const char* const* enum_labels_en; /* idem, EN (invariant 3 : bilingue). */
    const OwlPresBoolRadioLabels* bool_radio_labels; /*  — pose sur une
                                     * entree editor=RADIO d'une prop BOOL : les
                                     * libelles des deux options (faux/vrai),
                                     * bilingue. NULL sinon (radio d'enum, ou
                                     * prop bool en case a cocher classique). */
    /* Visibilite conditionnelle inter-propriete : l'entree n'est proposee dans
     * l'inspecteur QUE si la prop de meme kind nommee visible_if_prop vaut
     * visible_if_value. Mecanisme declaratif consomme par le front (le contrat
     * reste source de verite). NULL/NULL = aucune condition (toujours visible).
     * Placer ces deux champs en FIN de struct : les initialiseurs positionnels
     * existants (qui s'arretent avant) les laissent a NULL automatiquement. La
     * prop controlante doit etre une enum : visible_if_value est une valeur
     * canon de cet enum (verrouille par le self-test presentation). */
    const char* visible_if_prop;
    const char* visible_if_value;
} OwlPresEntry;

/* Acces a la table de presentation (toutes entrees, tous kinds couverts). */
int                 owl_presentation_count(void);
const OwlPresEntry* owl_presentation_at(int index);

/* Lookup par kind + nom de prop (resolution accent/casse-insensible cote
 * couche 1). Retourne NULL si pas d'entree de presentation. */
const OwlPresEntry* owl_presentation_find(const char* kind, const char* prop);

/* Kinds opt-in couverts par la couche 2 (et donc par le self-test). Tant
 * qu'un widget n'est pas industrialise, il n'est pas couvert : le self-test
 * ne hurle pas sur ses props. */
bool owl_presentation_kind_is_covered(const char* kind);

/* Libelles canon (tokens JSON cote front) des enums de presentation. */
const char* owl_pres_group_name(OwlPresGroup g);
const char* owl_pres_importance_name(OwlPresImportance imp);
const char* owl_pres_editor_name(OwlPresEditor ed);
const char* owl_pres_visibilite_name(OwlPresVisibilite v);

/* Emet le tableau JSON des entrees de presentation (pour --vscode-data).
 * Format : [ {kind, name, group, importance, editor, order, visibility,
 *             enum_subset?}, ... ]. Pas de virgule de tete/queue. */
void owl_presentation_emit_json(FILE* out);

/* === Etage 2 : feuilles de composite ================================= */

/* Une feuille d'un type composite (ex: Police.Couleur). Declaree une fois
 * PAR TYPE COMPOSITE (pas par widget) et reutilisee partout ou le composite
 * est expose. C'est ici qu'est fige l'editeur d'une feuille quand il n'est
 * pas devinable depuis le type scalaire : Police.Couleur se stocke en entier
 * RVB cote runtime mais s'edite via un color picker (editor color). Le front
 * lit cette table pour rendre l'accordeon du composite. */
typedef struct {
    const char*   composite;  /* nom du type composite cible ("Police") */
    const char*   leaf;       /* nom de la feuille (champ reel du type) */
    OwlPresEditor editor;     /* editeur abstrait de la feuille */
    const char*   labelFR;    /* libelle court d'affichage de la feuille (FR) */
    const char*   labelEN;    /* idem, EN (invariant 3 : bilingue) */
    /* Feuilles enum (editor=enum) : sous-ensemble de valeurs + libelles
     * d'affichage FR/EN paralleles. Meme mecanisme que OwlPresEntry, mais porte
     * sur une feuille de composite (ex styleImageBouton.Position). NULL = le
     * front affiche la valeur brute (constante prefixee). Les 3 tableaux sont
     * NULL-terminees, meme ordre et meme longueur. */
    const char* const* enum_subset;    /* valeurs (constantes canon) */
    const char* const* enum_labels_fr; /* libelles FR paralleles */
    const char* const* enum_labels_en; /* libelles EN paralleles (invariant 3) */
} OwlPresLeafEntry;

int                     owl_presentation_leaf_count(void);
const OwlPresLeafEntry* owl_presentation_leaf_at(int index);

/* Emet l'objet JSON des feuilles de composite (pour --vscode-data).
 * Format : { "Police": [ {leaf, editor}, ... ], ... }. Cle = type composite.
 * Pas de virgule de tete/queue, entrees groupees par type composite. */
void owl_presentation_leaves_emit_json(FILE* out);

/* Emet l'objet JSON des etats par sous-element de style (pour --vscode-data).
 * Format : { "<cle>": [ {"fr","en"}, ... ], ... }. Vide si le sous-element ne
 * porte pas d'etats. byType=true -> cle = TYPE composite du sous-element
 * (ex "styleZoneBouton", non-collisionnant) ; byType=false -> cle = nom
 * d'affichage (ex "Libellé", legacy, collision possible). Pas de virgule de
 * tete/queue. Necessite un registry initialise (vm_init + stdlib_ui). */
void owl_presentation_element_states_emit_json(FILE* out, bool byType);

/*  — mapping racine de menu -> type de style. Les racines de menu
 * (MenuPrincipal / MenuContextuel) ne sont PAS des widgets materialises : pas de
 * prop Style dans le catalogue pour porter leur type de style. Cette table donne
 * au front le type de style associe a chaque racine (l'equivalent de la prop
 * Style d'un widget), pour qu'il sache quelle structure de style_elements
 * afficher sur quelle racine. La structure elle-meme vient du TYPE (registre),
 * pas d'ici. Formes FR et EN de la racine + type de style canon. */
int  owl_presentation_menu_style_root_count(void);
bool owl_presentation_menu_style_root_at(int idx, const char** outRootFR,
                                         const char** outRootEN,
                                         const char** outStyleType);

/* Self-test anti-derive. Pour chaque kind couvert :
 *   - toute prop du catalogue semantique a une entree de presentation ;
 *   - toute entree de presentation pointe une prop reelle existante.
 * Necessite un registry initialise (vm_init prealable). Retourne le nombre
 * de violations (0 = OK). */
int owl_presentation_self_test_run(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* OWL_UI_PRESENTATION_H */
