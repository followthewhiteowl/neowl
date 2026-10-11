// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_catalog.h - Catalogue compile-time des proprietes UI (Phase D).
 *
 * Apres Phase C (mots-cles contextuels), Phase D apporte la validation
 * compile-time des property inits :
 *
 *   Bouton btnOK
 *       Largeur = "abc"        // erreur : type incompatible
 *       PropFantome = 42        // erreur : propriete inconnue
 *       TypeBouton = TBValider  // OK : enum value valide
 *   FIN
 *
 * Chaque kind (Fenetre, Libelle, Bouton, Saisie) a une table statique de
 * OwlControlPropDef. Le compileur lookup au moment du parse et emet
 * l'erreur si necessaire.
 */

#ifndef OWL_UI_CATALOG_H
#define OWL_UI_CATALOG_H

#include "ui_decl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type attendu d'une propriete. Contraint le compile-time check de la
 * valeur litterale fournie en init. */
typedef enum {
    OWL_PROP_TYPE_INT,         /* entier */
    OWL_PROP_TYPE_STRING,      /* chaine */
    OWL_PROP_TYPE_BOOL,        /* booleen */
    OWL_PROP_TYPE_VARIANT,     /* tout type accepte (ValeurRenvoyee) */
    OWL_PROP_TYPE_POSITION,    /* tuple (entier, entier) — V1.5 */
    OWL_PROP_TYPE_COLOR,       /* entier RGB encode */
    OWL_PROP_TYPE_ENUM,        /* valeur d'enum nommee (TypeBouton, ...) */
    OWL_PROP_TYPE_MULTILINGUE, /* chaineMultilingue : accepte chaine litterale
                                * (coerce auto -> remplit langue source) OU
                                * litteral inline dict {langueXxx: "..."} qui
                                * compile vers __MultilingueNew + ARRAY_SET. */
    OWL_PROP_TYPE_OBJECT,      /* descripteur compose typé (Cadre, ...). Le nom
                                * du type cible est porte par object_type (et
                                * NON enum_name, reserve aux enumerations). La
                                * prop n'accepte qu'une instance de ce type ;
                                * la cascade `..SousChamp` est validee compile-
                                * time contre les champs du type. */
} OwlPropType;

/* Representation du defaut canon d'une propriete DSL ( Veine Y).
 *
 * UNSET (zero-init, =0) est la sentinelle GENERIQUE : la prop n'a pas
 * encore de defaut peuple explicitement. Elle compte comme "defaut
 * declare" pour le self-test (cf. resolution de la tension du verrou :
 * l'invariant "chaque prop a un defaut" est vrai des l'ajout du champ,
 * AVANT toute population reelle). NULL et AUTO sont des sentinelles
 * EXPLICITES, peuplees a la main pour les cas ou il n'y a pas de valeur
 * scalaire fixe : NULL = absence (couleur systeme -1, chaine vide,
 * instance composee vide), AUTO = derive du moteur (geometrie
 * auto-layout). BOOL/INT/ENUM/STRING portent une valeur concrete sourcee
 * du comportement runtime actuel (audit des litteraux aux sites d'usage,
 * pas d'invention). */
typedef enum {
    OWL_PROP_DEFAULT_UNSET = 0, /* sentinelle generique : non encore peuple */
    OWL_PROP_DEFAULT_NULL,      /* sentinelle explicite : absence de valeur */
    OWL_PROP_DEFAULT_AUTO,      /* sentinelle explicite : derive du moteur */
    OWL_PROP_DEFAULT_BOOL,      /* .i = 0 (Faux) / 1 (Vrai) */
    OWL_PROP_DEFAULT_INT,       /* .i = valeur entiere */
    OWL_PROP_DEFAULT_ENUM,      /* .s = nom canon de la valeur d'enum */
    OWL_PROP_DEFAULT_STRING,    /* .s = chaine litterale */
} OwlPropDefaultKind;

typedef struct {
    OwlPropDefaultKind kind;
    int         i;             /* INT / BOOL (0|1) */
    const char* s;             /* ENUM (nom valeur canon) / STRING */
} OwlPropDefault;

typedef struct {
    const char* nameFR;        /* "Largeur", "Libelle", "TypeBouton", ... */
    const char* nameAscii;     /* version sans accent, NULL si nameFR sans accent */
    const char* nameEN;        /* alias EN, NULL si pas defini */
    OwlPropType type;
    const char* enum_name;     /* nom de l'enum si type == ENUM, NULL sinon.
                                * Strictement reserve aux enumerations : un type
                                * compose cible passe par object_type. */
    const char* descFR;
    const char* descEN;
    const char* object_type;   /* nom du type compose cible si type == OBJECT
                                * (ex: "Cadre"), NULL sinon. En fin de struct
                                * pour que les inits positionnelles existantes
                                * le laissent NULL par defaut. */
    OwlPropDefault dft;        /* defaut canon. Zero-init = UNSET =
                                * sentinelle generique. En fin de struct pour
                                * que les inits positionnelles existantes le
                                * laissent UNSET sans modification. */
    bool runtime_only;         /*  — etat runtime (lecture/ecriture a
                                * l'execution), PAS une option de design. Defaut
                                * false. Le designer exclut ces props de son
                                * inspector (via le flag expose dans
                                * --vscode-data) ; elles restent valides en code
                                * (completion, coloration, runtime inchanges). */
    bool readonly_runtime;     /*  — prop design-init : ecrivable dans le
                                * bloc PROPRIETES (design) mais lecture seule en
                                * code imperatif. Le compilateur rejette
                                * `<widget>..<prop> = ...` hors PROPRIETES. Defaut
                                * false. Inverse conceptuel de runtime_only. */
} OwlControlPropDef;

/* Libelle court du kind de defaut ("unset"/"null"/"auto"/"bool"/"int"/
 * "enum"/"string"). Utilise par le self-test (messages) et les exports. */
const char* owl_prop_default_kind_name(OwlPropDefaultKind k);

/* Lookup d'une propriete au niveau Fenetre (top-level dans une declaration
 * Fenetre FEN_X ... FIN). Retourne NULL si nom inconnu. */
const OwlControlPropDef* owl_window_catalog_find(const char* prop_name);

/* Lookup d'une propriete sur un sous-champ. Retourne NULL si nom inconnu
 * pour ce kind precis. La meme prop peut exister sur plusieurs kinds avec
 * des types differents (ex: Largeur = entier sur tous, Libelle = chaine
 * sur Bouton/Libelle, Texte = chaine sur Saisie). */
const OwlControlPropDef* owl_control_catalog_find(OwlControlKind kind, const char* prop_name);

/*  — proprietes de mise en forme d'une cellule ou d'une ligne de Table
 * (`TABLE_X[ligne][colonne]..Couleur`, `TABLE_X[ligne]..CouleurFond`). Source de
 * verite unique consultee par le compileur (abstention du controle de nom de
 * colonne) et par la VM (stockage / relecture). Retourne NULL si le nom n'est
 * pas une propriete de mise en forme. Match casse-insensible FR ou EN. */
const OwlControlPropDef* owl_table_cell_catalog_find(const char* prop_name);

/*  — proprietes d'un noeud de menu classique (SousMenu, OptionMenu,
 * MenuSeparateur). Retourne NULL si le nom n'est pas accepte pour ce noeud. */
const OwlControlPropDef* owl_menu_catalog_find(OwlMenuNodeKind kind,
                                                const char* prop_name);
/* Liste lisible des proprietes acceptees, pour le message d'erreur. */
const char* owl_menu_catalog_prop_list(OwlMenuNodeKind kind);

/* DSL Etat V2 — proprietes du conteneur Etat (Format, Orientation, Marges). */
const OwlControlPropDef* owl_etat_catalog_find(const char* prop_name);

/* DSL Etat V2 — proprietes des sous-blocs (Hauteur). */
const OwlControlPropDef* owl_etat_block_catalog_find(const char* prop_name);

/* Nom lisible du type pour les messages d'erreur. */
const char* owl_prop_type_name(OwlPropType t);

/* Props widget legitimes appliquees generiquement par le runtime, hors du
 * catalogue per-kind (owl_control_catalog_find -> NULL). Source de verite
 * unique consultee a la fois par le compileur (rejet compile-time
 * E_SEM_DSL_PROP_UNKNOWN_FOR_KIND) et la VM (E_RT_UI_PROP_UNKNOWN_FOR_KIND)
 * pour ne pas rejeter ces props legitimes sur n'importe quel kind.
 *   - "Style" : agrege un descripteur compose, applique par
 *     apply_style_to_control (stdlib_ui.c). Pas une prop scalaire per-kind.
 * Liste volontairement minimale : toute prop reelle vit dans le catalogue. */
bool owl_ui_prop_is_universal(const char* prop_name);

/* Sous-ensemble des props universelles qui sont LECTURE SEULE : leur valeur
 * est l'identite declaree de l'element, non reassignable en code.
 *   - "Nom"/"Name" : identifiant declare du controle ou de la Fenetre.
 * Le compileur rejette leur affectation (E_SEM_DSL_PROP_IDENTITY_READONLY) et
 * la VM rejette le set-path a l'execution. Incluse dans is_universal pour que
 * la LECTURE compile proprement sur tout kind. */
bool owl_ui_prop_is_universal_readonly(const char* prop_name);

/*  — vrai si la prop existe sur au moins un kind de champ (union,
 * seuil >= 1) ou est universelle. Sert a valider un acces `..prop` via un
 * recepteur dynamique (MoiMême/MonChamp) dont le kind exact n'est connu
 * qu'au runtime : zero faux positif, rejet uniquement si aucune correspondance. */
bool owl_ui_prop_exists_on_any_control_kind(const char* prop_name);

/* Initialise les enums et constantes du DSL fenetre dans le registry.
 * Appele une fois par stdlib_init_all() au demarrage VM. Enregistre :
 *   - Enum TypeBouton (btnNormal/btnValider/btnInterruption/btnAbandon/btnAide)
 *   - Enum TypeLibelle (TLNormal/TLMultiLigne)
 *   - Enum TypeSaisie (typSaisieTexte/typSaisieMotDePasse/typSaisieMultiligne)
 *   - Constantes globales : btnNormal=0, btnValider=1, ..., typSaisieTexte=0, ...
 *   - Index plat des proprietes DSL par kind (Phase A registry alignment)
 */
void stdlib_ui_catalog_init(void);

/* ================================================================
 * Index plat des proprietes DSL par kind (Phase A — alignment registry)
 *
 * Chaque entree expose le kind ("Fenetre", "Libelle", "Bouton", "Saisie",
 * "Table", "Etat", "blocEtat") et un pointeur vers la def. Permet aux
 * consumers (--vscode-data, --self-test-registry, --doc) d'iterer toutes
 * les proprietes DSL declarees, sans dupliquer la table dans plusieurs
 * fichiers (cf audit chemins paralleles).
 *
 * Aujourd'hui peuple par stdlib_ui_catalog_init(). Sera consomme par :
 *   - registry_self_test.c (rules 7/8/9 : descFR/EN, enum_name valide,
 *     nameEN obligatoire)
 *   - main.c --vscode-data (Phase B : section "dsl_properties" dans le JSON)
 *   - LSP/formatter en Phase C
 * ================================================================ */

typedef struct {
    const char* kind;                    /* "Fenetre", "Libelle", etc. */
    const OwlControlPropDef* def;
} OwlDSLPropEntry;

int  owl_dsl_props_count(void);
const OwlDSLPropEntry* owl_dsl_prop_at(int index);

/* Lookup case-insensitive, accent-insensitive, multilingue (FR/ASCII/EN).
 * Retourne NULL si introuvable pour ce kind. */
const OwlControlPropDef* owl_dsl_prop_find(const char* kind, const char* prop_name);

#ifdef __cplusplus
}
#endif

#endif /* OWL_UI_CATALOG_H */
