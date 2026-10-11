// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_presentation.c — Couche 2 : intention de presentation du designer.
 *
 * Voir ui_presentation.h pour le contrat d'archi 3 couches. Ce fichier porte
 * la table de presentation du widget Saisie (pilote). Chaque entree reference
 * une prop de la couche 1 (ui_catalog.c) par cle (kind + nom), sans jamais la
 * modifier : la semantique du langage ignore le front.
 *
 * La table couvre TOUTES les props du catalogue Saisie (designer + cache), de
 * sorte que le self-test (owl_presentation_self_test_run) garantit qu'aucune
 * prop n'est oubliee : ajouter une prop a PROPS_INPUT sans la classer ici casse
 * le build.
 */

#include "ui_presentation.h"
#include "ui_catalog.h"
#include "compiler.h"      /* modele de style (racines + palettes d'etats) */
#include "owl_registry.h"  /* owl_registry_type_find, OwlTypeDef / OwlField */
#include "vm.h"       /* vm.types : enumeration des feuilles d'un composite */
#include "object.h"   /* ObjInstance, IS_INSTANCE / AS_INSTANCE */
#include "table.h"

#include <string.h>
#include <stdlib.h>

/* Sous-ensemble de l'enum Etat propose dans le radio du designer : les 3 etats
 * "visibles". Invisible est exclu (le masquage passe par Visible) mais reste
 * pilotable au runtime — la semantique (ui_catalog.c) porte les 4+ valeurs. */
static const char* const ETAT_VISIBLE_SUBSET[] = {
    "Actif", "AffichageSeulement", "Gris\xc3\xa9", NULL
};

/* Sous-ensemble de l'enum cadrage propose dans la liste deroulante du Champ
 * Image : les 3 modes d'affichage (centre / etire / homothetique). Le defaut
 * effectif (cadHomothetique) est porte par la couche 1 (.dft du catalogue),
 * partage avec le runtime. */
static const char* const CADRAGE_SUBSET[] = {
    "cadCentre", "cadEtire", "cadHomothetique", NULL
};

/* Sous-ensemble Etat propre au Champ Image : Actif / Inactif / Grise. Distinct
 * du subset partage ETAT_VISIBLE_SUBSET (Actif / AffichageSeulement / Grise)
 * des autres widgets — un champ image n'a pas de mode "affichage seulement"
 * pertinent (pas de saisie), Inactif y est plus parlant. Invisible reste exclu
 * (masquage via Visible). */
static const char* const ETAT_IMAGE_SUBSET[] = {
    "Actif", "Inactif", "Gris\xc3\xa9", NULL
};

/* Radio "Type de bouton". Les valeurs sont les constantes canon
 * du panel (btn*) validees compile-time ; l'inspecteur les affiche en clair via
 * les libelles derives par le transformateur (table d'overrides typeBouton :
 * EN irregulier). Le front rend un libelle et pose la constante. */
static const char* const TYPEBOUTON_VALUES[] = {
    "btnNormal", "btnValider", "btnInterruption", "btnAbandon", "btnAide", NULL
};

/*  — radio "État initial" du Champ Table : les 4 etats du tableau.
 * Valeurs canon de l'enum etatsTableInitial (accent UTF-8) ; libelles
 * d'affichage FR/EN paralleles car les valeurs brutes (surtout
 * "NiSaisieNiSélection") ne sont pas lisibles en clair. */
static const char* const ETATINITIAL_TABLE_SUBSET[] = {
    "EnSaisie", "Gris\xc3\xa9", "S\xc3\xa9lection", "NiSaisieNiS\xc3\xa9lection", NULL
};
static const char* const ETATINITIAL_TABLE_FR[] = {
    "En saisie", "Gris\xc3\xa9", "S\xc3\xa9lection", "Ni saisie ni s\xc3\xa9lection", NULL
};
static const char* const ETATINITIAL_TABLE_EN[] = {
    "Editable", "Grayed", "Selection", "Read-only", NULL
};

/*  — liste deroulante "Orientation de la table" : une seule valeur
 * (Vertical). L'enum n'a pas de prefixe strippable (une valeur), donc le front
 * affiche la valeur brute — deja lisible. */
static const char* const ORIENTATION_TABLE_VALUES[] = { "Vertical", NULL };

/*  — radio "Mode de sélection" du Champ Liste. La prop bool
 * Multisélection est presentee comme un choix exclusif a deux options :
 * faux -> sélection simple (mono), vrai -> sélection multiple. Le titre du
 * groupe radio est porte par labelFR/labelEN de l'entree ("Mode de sélection").
 * Capacite generique bool_radio_labels (reutilisable par toute prop bool). */
static const OwlPresBoolRadioLabels LISTE_MODE_SELECTION_RADIO = {
    "Sélection simple", "Sélection multiple",   /* FR : faux / vrai */
    "Single selection", "Multiple selection"    /* EN : faux / vrai */
};

/*  — radio "Largeur des volets" du Champ Onglet (reglage Détail). Les
 * 2 valeurs de l'enum largeurVolets. Le libelle de largeurÉgale est derive par
 * le transformateur (strip du prefixe largeur* -> Égale) ; celui de
 * largeurProportionnelle passe par un override (cf. PRES_ENUM_LABEL_OVERRIDES),
 * la derivation ne disant pas proportionnelle A QUOI. */
static const char* const ONGLET_LARGEURVOLETS_SUBSET[] = {
    "largeurProportionnelle", "largeur\xc3\x89gale", NULL
};

/*  — radio "Alignement du libellé" du Champ Onglet (reglage Détail).
 * Sous-ensemble de l'enum alignement partage : les 3 alignements horizontaux
 * (debut/centre/fin). alignÉtire est exclu (sans sens pour un texte de
 * languette) mais reste accepte au runtime. Libelles derives par le
 * transformateur (strip align* -> Début / Centre / Fin), coherents avec les
 * autres consommateurs de l'enum alignement. */
static const char* const ONGLET_ALIGNLIBELLE_SUBSET[] = {
    "alignD\xc3\xa9""but", "alignCentre", "alignFin", NULL
};

/*  — combo "Orientation" du Champ Jauge, adosse a la prop bool Vertical
 * (canon : faux = horizontale, vrai = verticale). Meme descripteur bilingue que
 * bool_radio_labels mais rendu en liste deroulante (editor bool_combo) plutot
 * qu'en interrupteur. Le front rend un combo 2 choix mappe au booleen. */
static const OwlPresBoolRadioLabels JAUGE_ORIENTATION_COMBO = {
    "Horizontale", "Verticale",   /* FR : faux / vrai */
    "Horizontal",  "Vertical"     /* EN : faux / vrai */
};

/* Table de presentation. Props referencees par forme canon ASCII (resolues
 * accent/casse-insensible via owl_dsl_prop_find). Colonnes :
 *   kind, prop, labelFR, labelEN, groupe, importance, editor, ordre,
 *   visibilite, enum_subset.
 * labelFR/labelEN : NULL sur les entrees cache (invisibles dans l'inspecteur) ;
 * obligatoires non-vides sur toute entree designer (garde self-test). */
static const OwlPresEntry PRES[] = {
    /* --- Identite -------------------------------------------------------- */
    { "Saisie", "Libelle",          "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "LargeurLibelle",   "Largeur du libellé",  "Label width",          OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Contenu (data pure : valeur de depart du champ) ----------------- */
    { "Saisie", "ContenuInitial",   "Contenu Initial",     "Initial content",      OWL_PRES_GROUP_CONTENU,  OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT,        10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Identite (ex-Contenu : reglages du champ hors donnee) ----------- */
    { "Saisie", "Indication",       "Indication",          "Placeholder",          OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "TypeSaisie",       "Type de saisie",      "Input type",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM,        40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MaxLongueur",      "Longueur max",        "Max length",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MasqueSaisie",     "Masque de saisie",    "Input mask",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,        60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MasqueAffichage",  "Masque d'affichage",  "Display mask",         OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,        65, OWL_PRES_VIS_DESIGNER, NULL },
    /* FormatAffichage (Texte/RTF) n'a d'effet qu'en saisie multiligne : masque
     * dans l'inspecteur hors typSaisieMultiligne (proposee a tort sur Texte /
     * Mot de passe ou elle etait silencieusement ignoree). enum_labels + radio
     * absents (NULL, NULL, NULL) avant la condition visible_if. */
    { "Saisie", "FormatAffichage",  "Format d'affichage",  "Display format",       OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        70, OWL_PRES_VIS_DESIGNER, NULL, NULL, NULL, NULL, "TypeSaisie", "typSaisieMultiligne" },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    /* Bloc prioritaire (etat initial + visibilite + position/taille) en tete,
     * les reglages fins (opacite, cadrage, min/max) apres. */
    { "Saisie", "Etat",             "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       10, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Saisie", "Visible",          "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "X",                "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "Y",                "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "Largeur",          "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "Hauteur",          "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "Opacite",          "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "CurseurSouris",    "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "CadrageHorizontal","Cadrage horizontal",  "Horizontal alignment", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "CadrageVertical",  "Cadrage vertical",    "Vertical alignment",   OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,       100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "LargeurMin",       "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "LargeurMax",       "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "HauteurMin",       "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "HauteurMax",       "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    /* Marges (uniforme puis les 4 cotes) et fusions de cellules : reglages de
     * placement dans la grille, rattaches a l'onglet UI apres la geometrie. */
    { "Saisie", "Marge",            "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MargeHaut",        "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MargeBas",         "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MargeGauche",      "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "MargeDroite",      "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "ColSpan",          "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "RowSpan",          "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Comportement ---------------------------------------------------- */
    { "Saisie", "BorneMin",           "Borne min",          "Min value",           OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "BorneMax",           "Borne max",          "Max value",           OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    /* Marges/spans deplaces vers l'onglet UI ; ce groupe ne garde que
     * l'etirement et l'alignement dans la cellule parente. */
    { "Saisie", "Etire",            "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Saisie", "Alignement",       "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence ------------------------------------------------------- */
    /* Police et CouleurFond plats masques : couverts par l'editeur Style
     * ( ; unification runtime = ). Le composite Style est
     * le seul chemin d'apparence de l'inspecteur. */
    { "Saisie", "Police",           "Police",              "Font",                 OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 10, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "CouleurFond",      "Couleur de fond",     "Background color",     OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     20, OWL_PRES_VIS_CACHE, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Saisie", "Bulle",            "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    { "Saisie", "ToucheRaccourci",  "Raccourci clavier",   "Keyboard shortcut",   OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachee : Groupe gere par code en v1 (aucune prop exposee) ------- */
    { "Saisie", "Groupe",           NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Style type (objet compose) : editeur composite derive du type
     * styleSaisie. Les feuilles primitives + les sous-composites (Police,
     * Cadre) sont declarees dans PRES_LEAVES ; le front en derive un
     * sous-accordeon. Les sous-etats (Survol/Focus) restent non exposes. */
    { "Saisie", "Style",            "Style",               "Style",                OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : runtime-only ------------------------------------------ */
    { "Saisie", "Valeur",           NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "Curseur",          NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT,OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "FinCurseur",       NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT,OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "TexteSelectionne", NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "Modifie",          NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT,OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachee :  socle format projet. FormatNombre est un objet
     * composite (SéparateurDécimal/SéparateurMilliers/ChiffresAprèsVirgule)
     * configuré en DSL inline `{ }` ou hérité du projet. Pas d'éditeur designer
     * au Lot 0 (couvre le runtime/DSL d'abord) -> cache, comme Valeur. */
    { "Saisie", "FormatNombre",      NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    /*  — FormatMonétaire : meme statut que FormatNombre (objet composite
     * configuré en DSL inline `{ }` ou hérité du projet, pas d'éditeur designer). */
    { "Saisie", "FormatMonetaire",   NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Saisie", "Grise",            NULL, NULL, OWL_PRES_GROUP_UI,   OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : raccourcis plats du composite Police ------------------ */
    { "Saisie", "PoliceNom",        NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceTaille",     NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceGras",       NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceItalique",   NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceSoulignee",  NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceBarree",     NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Saisie", "PoliceCouleur",    NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Libellé — 2e widget industrialise. Jeu complet de */
    /* props communes, memes groupes/ordres que le Saisie, + la prop Style */
    /* (type styleLibellé : Libellé + CadreExtérieur). Etat en radio        */
    /* Actif/AffichageSeulement(alias Inactif)/Grisé.                       */
    /* ================================================================== */
    /* --- Identite -------------------------------------------------------- */
    { "Libelle", "Libelle",          "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Identite (ex-Contenu : le Libellé n'a pas de donnee propre) ----- */
    { "Libelle", "TypeLibelle",      "Type de libellé",     "Label type",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "FormatAffichage",  "Format d'affichage",  "Display format",       OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        40, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat / geometrie / min-max / marges / span) ----------------- */
    { "Libelle", "Etat",             "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       10, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Libelle", "Visible",          "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "X",                "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Y",                "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Largeur",          "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Hauteur",          "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Opacite",          "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "CurseurSouris",    "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "CadrageHorizontal","Cadrage horizontal",  "Horizontal alignment", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "CadrageVertical",  "Cadrage vertical",    "Vertical alignment",   OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,       100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "LargeurMin",       "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "LargeurMax",       "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "HauteurMin",       "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "HauteurMax",       "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Marge",            "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "MargeHaut",        "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "MargeBas",         "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "MargeGauche",      "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "MargeDroite",      "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "ColSpan",          "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "RowSpan",          "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition ----------------------------------------------------- */
    { "Libelle", "Etire",            "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Libelle", "Alignement",       "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleLibellé). Les flats
     * (Couleur/CouleurFond/marges/Police/Cadre) sont couverts par le Style,
     * donc caches ci-dessous. */
    { "Libelle", "Style",            "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Libelle", "Bulle",            "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvertes par Style (apparence a plat) ---------------- */
    { "Libelle", "Couleur",          NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "CouleurFond",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "Cadre",            NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "RetraitGauche",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,       0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "RetraitDroit",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,       0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "RetraitHaut",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,       0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "RetraitBas",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,       0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Libelle", "Grise",            NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : props avancees non exposees en V1 --------------------- */
    { "Libelle", "Cadrage",          NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "AffichageActif",   NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "Altitude",         NULL, NULL, OWL_PRES_GROUP_UI,          OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "Ellipse",          NULL, NULL, OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "Message",          NULL, NULL, OWL_PRES_GROUP_AIDE,        OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "SensEcriture",     NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceEtirement",  NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceTailleMin",  NULL, NULL, OWL_PRES_GROUP_APPARENCE,   OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : composite Police + raccourcis plats (couvert par Style) */
    { "Libelle", "Police",           NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceNom",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceTaille",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceGras",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceItalique",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceSoulignee",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceBarree",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle", "PoliceCouleur",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : avance ------------------------------------------------ */
    { "Libelle", "Groupe",           NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Bouton — 3e widget industrialise. Jeu complet de   */
    /* props communes, memes groupes/ordres que Saisie/Libellé, + la prop   */
    /* Style (type styleBouton : Bouton stateful + CadreExtérieur). Etat en */
    /* radio Actif/AffichageSeulement(alias Inactif)/Grisé.                 */
    /* ================================================================== */
    /* --- Identite -------------------------------------------------------- */
    { "Bouton", "Libelle",          "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Identite (ex-Contenu : le Bouton n'a pas de donnee propre) ------ */
    { "Bouton", "Image",            "Image",               "Image",                OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_IMAGE_FILE,  20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "ImageTaille",      "Taille de l'image",   "Image size",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Comportement ---------------------------------------------------- */
    { "Bouton", "ToucheRaccourci",  "Raccourci clavier",   "Keyboard shortcut",    OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_SHORTCUT, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    /* Libelles derives par le transformateur (override table typeBouton) — pas
     * de tableaux statiques ici (source de verite unique). enum_subset conserve
     * la liste canon pour l'editeur radio. */
    { "Bouton", "TypeBouton",       "Type de bouton",      "Button type",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_RADIO,        5, OWL_PRES_VIS_DESIGNER, TYPEBOUTON_VALUES, NULL, NULL },
    { "Bouton", "Etat",             "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       10, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Bouton", "Visible",          "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "X",                "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Y",                "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Largeur",          "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Hauteur",          "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Opacite",          "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "CurseurSouris",    "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "LargeurMin",       "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "LargeurMax",       "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "HauteurMin",       "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "HauteurMax",       "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Marge",            "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "MargeHaut",        "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "MargeBas",         "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "MargeGauche",      "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "MargeDroite",      "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "ColSpan",          "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "RowSpan",          "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Bouton", "Etire",            "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Bouton", "Alignement",       "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleBouton) ------------------ */
    { "Bouton", "Style",            "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Bouton", "Bulle",            "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Bouton", "Grise",            NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : Police plate (couverte par l'editeur Style) ----------- */
    { "Bouton", "Police",           NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceNom",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceTaille",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceGras",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceItalique",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceSoulignee",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceBarree",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "PoliceCouleur",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton", "CouleurFond",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : avance ------------------------------------------------ */
    { "Bouton", "Groupe",           NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Image — 4e widget industrialise. Calque sur Bouton  */
    /* pour l'Etat (radio Actif/AffichageSeulement/Grisé) et le Général ;   */
    /* Style typé (styleImage : CadreExtérieur). La source de l'image et le */
    /* mode de cadrage sont des props top-level de Contenu (Image, Cadrage).*/
    /* ================================================================== */
    /* --- Identite (ex-Contenu : source d'image + cadrage, pas de donnee) - */
    { "Image", "Image",            "Image",               "Image",                OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_IMAGE_FILE,  10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Cadrage",          "Cadrage",             "Framing",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM,        20, OWL_PRES_VIS_DESIGNER, CADRAGE_SUBSET },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "Image", "Visible",          "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Etat",             "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    { "Image", "X",                "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Y",                "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Largeur",          "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Hauteur",          "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Opacite",          "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "CurseurSouris",    "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "LargeurMin",       "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "LargeurMax",       "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "HauteurMin",       "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "HauteurMax",       "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Marge",            "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "MargeHaut",        "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "MargeBas",         "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "MargeGauche",      "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "MargeDroite",      "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "ColSpan",          "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "RowSpan",          "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Image", "Etire",            "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Image", "Alignement",       "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleImage) ------------------- */
    { "Image", "Style",            "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Image", "Bulle",            "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Image", "Grise",            NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : couvert par le composite Style (apparence a plat) ----- */
    { "Image", "CouleurFond",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Image", "Cadre",            NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : runtime-only / avance --------------------------------- */
    /* ImageTaille reste une prop runtime (override taille d'icone, )
     * mais confond dans l'UI Image a cote de Cadrage + la boite -> masquee. */
    { "Image", "ImageTaille",      NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Image", "Zoom",             NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Image", "Altitude",         NULL, NULL, OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Image", "Groupe",           NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Interrupteur — 5e widget industrialise. V1 SIMPLE : */
    /* cases a cocher + texte. Contenu = les Options (repetable) ; UI = Etat */
    /* radio Actif/Inactif/Grisé + Visible ; Style typé (styleInterrupteur). */
    /* Plates redondantes (Couleur/CouleurFond/Cadre + Police) masquees au    */
    /* profit du Style. Sous-lib / colonnes / modes exotiques hors V1.        */
    /* ================================================================== */
    /* --- Contenu (point central : les options repetables) ---------------- */
    { "Interrupteur", "Options",       "Options",             "Options",              OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_VARIANT,     10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "Interrupteur", "Visible",       "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Etat",          "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    { "Interrupteur", "X",             "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Y",             "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Largeur",       "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Hauteur",       "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Opacite",       "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "CurseurSouris", "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "LargeurMin",    "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "LargeurMax",    "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "HauteurMin",    "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "HauteurMax",    "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Marge",         "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "MargeHaut",     "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "MargeBas",      "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "MargeGauche",   "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "MargeDroite",   "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "ColSpan",       "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "RowSpan",       "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Interrupteur", "Etire",         "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Alignement",    "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "HauteurOption", "Hauteur des options", "Option height",        OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 40, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleInterrupteur) ------------ */
    { "Interrupteur", "Style",         "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Interrupteur", "Bulle",         "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Interrupteur", "Grise",         NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : apparence plate (couverte par l'editeur Style) -------- */
    { "Interrupteur", "Couleur",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "CouleurFond",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Cadre",         NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Police",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceNom",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceTaille",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceGras",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceItalique",NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceSoulignee",NULL,NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceBarree",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "PoliceCouleur", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : specifiques runtime / hors V1 designer ---------------- */
    { "Interrupteur", "ModeAffichage", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "NombreColonne", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "TroisEtats",    NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Valeur",        NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "ValeurInitiale",NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Occurrence",    NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Altitude",      NULL, NULL, OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Interrupteur", "Groupe",        NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Sélecteur — 6e widget industrialise. Pendant radio */
    /* exclusif de l'Interrupteur. Meme structure : Contenu = les Options   */
    /* (repetable) ; UI = Etat Actif/Inactif/Grisé + Visible ; Style typé   */
    /* (styleSélecteur). Pas de TroisEtats (radio = selection unique).      */
    /* Plates redondantes (Couleur/CouleurFond/Cadre + Police) masquees au  */
    /* profit du Style.                                                     */
    /* ================================================================== */
    /* --- Contenu (point central : les options repetables) ---------------- */
    { "Selecteur", "Options",       "Options",             "Options",              OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_VARIANT,     10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "Selecteur", "Visible",       "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Etat",          "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    { "Selecteur", "X",             "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Y",             "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Largeur",       "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Hauteur",       "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Opacite",       "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "CurseurSouris", "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "LargeurMin",    "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "LargeurMax",    "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "HauteurMin",    "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "HauteurMax",    "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Marge",         "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "MargeHaut",     "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "MargeBas",      "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "MargeGauche",   "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "MargeDroite",   "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "ColSpan",       "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "RowSpan",       "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Selecteur", "Etire",         "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Alignement",    "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "HauteurOption", "Hauteur des options", "Option height",        OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 40, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleSélecteur) --------------- */
    { "Selecteur", "Style",         "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Selecteur", "Bulle",         "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Selecteur", "Grise",         NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : apparence plate (couverte par l'editeur Style) -------- */
    { "Selecteur", "Couleur",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "CouleurFond",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "Cadre",         NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "Police",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceNom",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceTaille",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceGras",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceItalique",NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceSoulignee",NULL,NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceBarree",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "PoliceCouleur", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : specifiques runtime / hors V1 designer ---------------- */
    { "Selecteur", "ModeAffichage", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "NombreColonne", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "Valeur",        NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "ValeurInitiale",NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "Occurrence",    NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "Altitude",      NULL, NULL, OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Selecteur", "Groupe",        NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Liste — list box (QListWidget).                   */
    /* Contenu = ContenuInitial (champ multi-ligne, un item par ligne) ;   */
    /* UI = Visible / État + Mode de sélection (radio simple/multiple,      */
    /* mappe Multisélection bool) ; Style typé (styleListe). Plates         */
    /* redondantes (Couleur/CouleurFond/Cadre + Police) masquees au profit  */
    /* du Style. Valeur / ValeurMémorisée / ... = runtime, hors designer.  */
    /* ================================================================== */
    /* --- Contenu (data pure : items de depart, un par ligne) ------------- */
    { "Liste", "ContenuInitial", "Contenu initial",     "Initial content",      OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT,        10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + mode de selection + geometrie) ----------------------- */
    { "Liste", "Visible",        "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Etat",           "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    /* Mode de sélection : radio 2-etats sur la prop bool Multisélection
     * (faux = selection simple, vrai = selection multiple). Capacite generique
     * bool_radio_labels. Place juste apres Visible/État. */
    { "Liste", "Multiselection", "Mode de sélection",   "Selection mode",       OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       25, OWL_PRES_VIS_DESIGNER, NULL, NULL, NULL, &LISTE_MODE_SELECTION_RADIO },
    { "Liste", "X",              "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Y",              "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Largeur",        "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Hauteur",        "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Opacite",        "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "CurseurSouris",  "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Ellipse",        "Ellipse",             "Ellipsis",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "RetraitGauche",  "Retrait gauche",      "Left padding",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Zoom",           "Zoom",                "Zoom",                 OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "OptionTri",      "Option de tri",       "Sort option",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,       120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "LargeurMin",     "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "LargeurMax",     "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "HauteurMin",     "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        145, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "HauteurMax",     "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        148, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Marge",          "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "MargeHaut",      "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "MargeBas",       "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "MargeGauche",    "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "MargeDroite",    "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "ColSpan",        "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "RowSpan",        "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Liste", "Etire",          "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Alignement",     "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleListe) ------------------- */
    { "Liste", "Style",          "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Liste", "Bulle",          "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Liste", "Grise",          NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : apparence plate (couverte par l'editeur Style) -------- */
    { "Liste", "Couleur",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "CouleurFond",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Cadre",          NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Police",         NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceNom",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceTaille",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceGras",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceItalique", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceSoulignee",NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceBarree",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "PoliceCouleur",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : contenu runtime + selection + tri (hors V1 designer) -- */
    { "Liste", "Contenu",        NULL, NULL, OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Triee",          NULL, NULL, OWL_PRES_GROUP_UI,      OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Valeur",         NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "ValeurAffichee", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "ValeurMemorisee",NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "ValeurInitiale", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Occurrence",     NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Vide",           NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "Altitude",       NULL, NULL, OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Liste", "Groupe",         NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Combo — liste deroulante = structure Saisie       */
    /* (Libellé integre + LargeurLibellé, groupe identite/Général) + dropdown */
    /* (ContenuInitial multi-ligne). Détail : EnSaisie + MaxLignesAffichées. */
    /* Style typé (styleCombo, 5 sous-elements). Plates redondantes         */
    /* (Couleur/CouleurFond/Cadre + Police) masquees au profit du Style.    */
    /* Valeur / ValeurMémorisée / ... = runtime, hors designer.            */
    /* ================================================================== */
    /* --- Général (identite : libellé integre) ---------------------------- */
    { "Combo", "Libelle",        "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "LargeurLibelle", "Largeur du libellé",  "Label width",          OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Contenu (data pure : items de depart, un par ligne) ------------- */
    { "Combo", "ContenuInitial", "Contenu initial",     "Initial content",      OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT,        10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Détail (disposition : saisie libre + max lignes, cf HauteurOption) --- */
    { "Combo", "EnSaisie",       "Combo avec saisie",   "Editable combo",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "MaxLignesAffichees", "Lignes affichées max", "Max visible items", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "Combo", "Visible",        "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Etat",           "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Combo", "X",              "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Y",              "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Largeur",        "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Hauteur",        "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Opacite",        "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "CurseurSouris",  "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "OptionTri",      "Option de tri",       "Sort option",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,       120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "LargeurMin",     "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "LargeurMax",     "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "HauteurMin",     "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        145, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "HauteurMax",     "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        148, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Marge",          "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "MargeHaut",      "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "MargeBas",       "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "MargeGauche",    "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "MargeDroite",    "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "ColSpan",        "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "RowSpan",        "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout enfant) ------------------------------------- */
    { "Combo", "Etire",          "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Alignement",     "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleCombo) ------------------- */
    { "Combo", "Style",          "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Combo", "Bulle",          "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Combo", "Grise",          NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : apparence plate (couverte par l'editeur Style) -------- */
    { "Combo", "Couleur",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "CouleurFond",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Cadre",          NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Police",         NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceNom",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceTaille",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceGras",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceItalique", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceSoulignee",NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceBarree",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "PoliceCouleur",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : contenu runtime + selection + tri (hors V1 designer) -- */
    { "Combo", "Contenu",        NULL, NULL, OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Triee",          NULL, NULL, OWL_PRES_GROUP_UI,      OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Valeur",         NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "ValeurAffichee", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "ValeurMemorisee",NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "ValeurInitiale", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Occurrence",     NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Vide",           NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,    0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "Altitude",       NULL, NULL, OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Combo", "Groupe",         NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Table — tableau (QTableWidget). Général : Libellé  */
    /* + combo Orientation (1 valeur, Vertical). UI : Visible + radio État  */
    /* initial (4 etats du tableau) + geometrie. Détail : Hauteur d'une     */
    /* ligne (px). Style typé (styleTable, 8 sous-elements). Grisé + Etat +  */
    /* Police/FONT_PROPS masques (couverts par État initial / Style — cf.    */
    /*  pour l'ajout catalogue). Colonnes = sous-elements DSL        */
    /* propres (kind Colonne). Base props calquees sur le Champ Liste.       */
    /* ================================================================== */
    /* --- Général (identite : libelle + orientation du tableau) ----------- */
    { "Table", "Libelle",       "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Orientation",   "Orientation de la table", "Table orientation", OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 10, OWL_PRES_VIS_DESIGNER, ORIENTATION_TABLE_VALUES },
    { "Table", "ColonneExtensible", "Colonne extensible", "Stretch column", OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT, 20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "Table", "Visible",       "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "EtatInitial",   "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETATINITIAL_TABLE_SUBSET, ETATINITIAL_TABLE_FR, ETATINITIAL_TABLE_EN },
    { "Table", "X",             "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Y",             "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Largeur",       "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Hauteur",       "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "CurseurSouris", "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "LargeurMin",    "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "LargeurMax",    "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "HauteurMin",    "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        145, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "HauteurMax",    "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        148, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Marge",         "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "MargeHaut",     "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "MargeBas",      "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "MargeGauche",   "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "MargeDroite",   "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "ColSpan",       "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "RowSpan",       "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Détail (disposition : hauteur de ligne + layout enfant) --------- */
    { "Table", "HauteurLigne",  "Hauteur d'une ligne", "Row height",           OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,  5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "HauteurTitre",  "Hauteur de l'en-tête", "Header height",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,  7, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Etire",         "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Alignement",    "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleTable) ------------------- */
    { "Table", "Style",         "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Table", "Bulle",         "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par État initial (Grisé) ---------------------- */
    { "Table", "Grise",         NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachee : Etat unifié runtime-only. Le radio État initial du    ---
     * designer (EtatInitial, enum etatsTableInitial) porte deja le choix
     * d'etat au design ; Etat (enum etat : Actif/AffichageSeulement/Grisé/
     * Invisible/AffichageSansSélection) est l'accesseur runtime unifié
     * transverse aux widgets. Pas de 2e radio d'etat dans l'inspecteur ->
     * cache. AffichageSansSélection (propre au Table) reste donc pilotable
     * au runtime uniquement, hors radio designer. */
    { "Table", "Etat",          NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachee : ModeLoupe accesseur runtime-only (comme Etat). Enum
     * modeLoupe (rechercheCommencePar/filtreContient) pilotant le comportement
     * de la loupe de titre de colonne. Expo designer differee -> cache. */
    { "Table", "ModeLoupe",     NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachee : MenuLoupe reference d'objet menu (VARIANT), affectee par un
     * bloc MenuContextuel via `MenuLoupe = MNU_X` (attache du menu de la loupe,
     * ). Comme MenuPrincipal cote Fenetre : pas d'edition designer par
     * valeur -> cache. */
    { "Table", "MenuLoupe",     NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : composite Police + raccourcis plats (couvert par Style
     * styleTable, comme sur Saisie/Libellé). ------------------------------ */
    { "Table", "Police",           NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceNom",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceTaille",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceGras",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceItalique",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceSoulignee",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceBarree",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Table", "PoliceCouleur",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : avance ------------------------------------------------ */
    { "Table", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "Table", "Groupe",        NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Jauge — barre de progression (QProgressBar).       */
    /* Général : orientation (bool Vertical). UI : Visible + radio État     */
    /* initial (Actif/AffichageSeulement/Grisé) + geometrie. Détail :       */
    /* BorneMin/BorneMax/Valeur + Symetrique + TexteJauge. Style typé        */
    /* (styleJauge, 3 sous-elements). Flats couleur (Couleur/CouleurFond/    */
    /* CouleurJauge/Police) couverts par le Style -> caches.                */
    /* ================================================================== */
    /* --- Général (identite : orientation) -------------------------------- */
    /* Orientation : combo 2 choix adosse a la prop bool Vertical (faux =
     * horizontale, vrai = verticale). Rendu en liste deroulante (bool_combo),
     * pas en interrupteur. Libelles portes par JAUGE_ORIENTATION_COMBO. */
    { "Jauge", "Vertical",       "Orientation",         "Orientation",          OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL_COMBO,  10, OWL_PRES_VIS_DESIGNER, NULL, NULL, NULL, &JAUGE_ORIENTATION_COMBO },

    /* --- UI (etat / geometrie / min-max / marges / span) ----------------- */
    /* Visible avant État initial (ordre demande par le designer). */
    { "Jauge", "Visible",        "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Etat",           "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Jauge", "X",              "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Y",              "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Largeur",        "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Hauteur",        "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Opacite",        "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "CurseurSouris",  "Curseur souris",      "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "LargeurMin",     "Largeur min",         "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "LargeurMax",     "Largeur max",         "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "HauteurMin",     "Hauteur min",         "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "HauteurMax",     "Hauteur max",         "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Marge",          "Marge",               "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "MargeHaut",      "Marge haut",          "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "MargeBas",       "Marge bas",           "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "MargeGauche",    "Marge gauche",        "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "MargeDroite",    "Marge droite",        "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "ColSpan",        "Fusion colonnes",     "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "RowSpan",        "Fusion lignes",       "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Détail (données : bornes + valeur + format) --------------------- */
    { "Jauge", "BorneMin",       "Borne min",           "Min value",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,   10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "BorneMax",       "Borne max",           "Max value",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,   20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Valeur",         "Valeur",              "Value",                OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,   30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Symetrique",     "Symétrique",          "Symmetric",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,  40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "TexteJauge",     "Texte affiché",       "Displayed text",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT,  50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "CadrageHorizontal","Cadrage du texte",   "Text alignment",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,  55, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Etire",          "Étirer",              "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,  60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Jauge", "Alignement",     "Alignement",          "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM,  70, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleJauge) ------------------- */
    { "Jauge", "Style",          "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "Jauge", "Bulle",          "Bulle d'aide",        "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvertes par Style (apparence a plat) ---------------- */
    { "Jauge", "Couleur",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "CouleurFond",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "CouleurJauge",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "Cadre",          NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : couvert par Etat (Grise) ------------------------------ */
    { "Jauge", "Grise",          NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : props avancees non exposees en V1 --------------------- */
    { "Jauge", "Altitude",       NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : composite Police + raccourcis plats (couvert par Style) */
    { "Jauge", "Police",           NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceNom",        NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceTaille",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceGras",       NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceItalique",   NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceSoulignee",  NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceBarree",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_BOOL,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge", "PoliceCouleur",    NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : avance ------------------------------------------------ */
    { "Jauge", "Groupe",         NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Colonne — sous-element DSL de la Table (kind propre).    */
    /* Général : Libellé multilingue + Type de cellule. Détail : Largeur    */
    /* (px, draggable au designer). UI : Visible + radio État (3 etats,     */
    /* enum etat partage). Style typé (styleColonne, 6 sous-elements),      */
    /* surcharge locale du styleTable colonne par colonne.                  */
    /* ================================================================== */
    /* --- Général (identite : titre + type de cellule) -------------------- */
    { "Colonne", "Libelle",     "Libellé",             "Caption",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Colonne", "Type",        "Type de cellule",     "Cell type",            OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM,        20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Colonne", "MasqueSaisie", "Masque de saisie",   "Input mask",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,        30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat) ------------------------------------------------------- */
    { "Colonne", "Visible",     "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,  10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Colonne", "Etat",        "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO, 20, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },

    /* --- Détail (disposition : largeur de la colonne) ------------------- */
    { "Colonne", "Largeur",     "Largeur",             "Width",                OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleColonne) ---------------- */
    { "Colonne", "Style",       "Style",               "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },
    /*  — mise en forme runtime au niveau colonne (`COL_X..Couleur` /
     * `..CouleurFond`) : props du catalogue exposees pour explain + completion,
     * mais editees par code, pas dans l'inspecteur -> cachees en couche 2 (meme
     * traitement que Couleur/CouleurFond des autres champs). */
    { "Colonne", "Couleur",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Colonne", "CouleurFond", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Zone répétée — conteneur a repetition (Looper).   */
    /* Général : Sens de repetition (enum zrSens, Vertical seul).          */
    /* UI : Visible + radio État + radio Mode de sélection (Multisélection */
    /* bool) + geometrie. Détail : Hauteur d'une répétition (px) + layout  */
    /* enfant. Style typé (styleZoneRépétée, 7 sous-elements). Base props   */
    /* calquees sur le Champ Liste (conteneur). Ligne sélectionnée /        */
    /* Occurrence = runtime-only (VIS_CACHE) ; plates redondantes           */
    /* (CouleurFond/Cadre) masquees au profit du Style.                    */
    /* ================================================================== */
    /* --- Général (identite : sens de repetition) ------------------------- */
    { "ZoneRepetee", "Sens",     "Sens de répétition",  "Repetition direction", OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + mode de selection + geometrie) ----------------------- */
    { "ZoneRepetee", "Visible",  "Visible",             "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Etat",     "État initial",        "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       20, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    /* Mode de sélection : radio 2-etats sur la prop bool Multisélection
     * (faux = selection simple, vrai = selection multiple). Meme capacite
     * generique bool_radio_labels que le Champ Liste. */
    { "ZoneRepetee", "Multiselection", "Mode de sélection", "Selection mode",   OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       25, OWL_PRES_VIS_DESIGNER, NULL, NULL, NULL, &LISTE_MODE_SELECTION_RADIO },
    { "ZoneRepetee", "X",        "X",                   "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Y",        "Y",                   "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION,    40, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Largeur",  "Largeur",             "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         50, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Hauteur",  "Hauteur",             "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,         60, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Opacite",  "Opacité",             "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,         70, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "CurseurSouris", "Curseur souris", "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,        80, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "LargeurMin", "Largeur min",       "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        130, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "LargeurMax", "Largeur max",       "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        140, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "HauteurMin", "Hauteur min",       "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        145, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "HauteurMax", "Hauteur max",       "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        148, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Marge",     "Marge",              "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,        150, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "MargeHaut", "Marge haut",         "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        160, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "MargeBas",  "Marge bas",          "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        170, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "MargeGauche", "Marge gauche",     "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        180, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "MargeDroite", "Marge droite",     "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        190, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "ColSpan",   "Fusion colonnes",    "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        200, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "RowSpan",   "Fusion lignes",      "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,        210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Détail (disposition : hauteur d'une repetition + layout enfant) - */
    { "ZoneRepetee", "HauteurRepetition", "Hauteur d'une répétition", "Repetition height", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Etire",     "Étirer",             "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Alignement", "Alignement",        "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleZoneRépétée) ------------- */
    { "ZoneRepetee", "Style",     "Style",              "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "ZoneRepetee", "Bulle",     "Bulle d'aide",       "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : couvert par État (Grise) ------------------------------ */
    { "ZoneRepetee", "Grise",     NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : apparence plate (couverte par l'editeur Style) -------- */
    { "ZoneRepetee", "CouleurFond", NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "ZoneRepetee", "Cadre",     NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : selection + occurrence runtime (hors designer) -------- */
    { "ZoneRepetee", "LigneSelectionnee", NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "ZoneRepetee", "Occurrence", NULL, NULL, OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* --- Cachees : avance ------------------------------------------------ */
    { "ZoneRepetee", "Altitude",  NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "ZoneRepetee", "ToucheRaccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_SHORTCUT, 5, OWL_PRES_VIS_DESIGNER, NULL },
    { "ZoneRepetee", "Groupe",    NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Conteneur — 11e widget stylé. Groupeur passif :  */
    /* PAS d'État/EtatInitial (aucun sens saisie). UI : Visible +         */
    /* géométrie. Disposition : layout des enfants (Disposition + Padding */
    /* + Espacement + Align + Colonnes/Lignes) et layout de ce conteneur  */
    /* dans son parent (Étire/Marge/Alignement/spans). Style typé         */
    /* (styleConteneur, 1 sous-element CadreExtérieur).                   */
    /* ================================================================== */
    /* --- UI (visibilite + geometrie) ------------------------------------ */
    { "Conteneur", "Visible",     "Visible",     "Visible",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "X",           "X",           "X",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Y",           "Y",           "Y",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Largeur",     "Largeur",     "Width",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Hauteur",     "Hauteur",     "Height",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "CurseurSouris", "Curseur souris", "Mouse cursor", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM,   60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "LargeurMin",  "Largeur min", "Min width",       OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,      70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "LargeurMax",  "Largeur max", "Max width",       OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,      80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "HauteurMin",  "Hauteur min", "Min height",      OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,      90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "HauteurMax",  "Hauteur max", "Max height",      OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Marge",       "Marge",       "Margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,     110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "MargeHaut",   "Marge haut",  "Top margin",      OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "MargeBas",    "Marge bas",   "Bottom margin",   OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     130, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "MargeGauche", "Marge gauche","Left margin",     OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     140, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "MargeDroite", "Marge droite","Right margin",    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     150, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "ColSpan",     "Fusion colonnes", "Column span", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     160, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "RowSpan",     "Fusion lignes",   "Row span",    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     170, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout des enfants + placement dans le parent) ----- */
    { "Conteneur", "Disposition", "Disposition", "Layout",          OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Padding",     "Marge intérieure", "Inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "PaddingHaut", "Marge intérieure haut", "Top inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "PaddingBas",  "Marge intérieure bas", "Bottom inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "PaddingGauche", "Marge intérieure gauche", "Left inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "PaddingDroite", "Marge intérieure droite", "Right inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Espacement",  "Espacement",  "Spacing",         OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "AlignH",      "Alignement horizontal", "Horizontal alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "AlignV",      "Alignement vertical", "Vertical alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Colonnes",    "Colonnes",    "Columns",         OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Lignes",      "Lignes",      "Rows",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 110, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Etire",       "Étirer",      "Stretch",         OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 120, OWL_PRES_VIS_DESIGNER, NULL },
    { "Conteneur", "Alignement",  "Alignement",  "Alignment",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 130, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleConteneur) --------------- */
    { "Conteneur", "Style",       "Style",       "Style",           OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : avance (non editable au designer) --------------------- */
    { "Conteneur", "Altitude",    NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Conteneur", "Groupe",      NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Fenêtre — élément parent (pas un widget). Édition des   */
    /* props au designer, même éditeur 7 onglets. Identité : Titre. UI :   */
    /* géométrie + visibilité + état. Disposition : layout des enfants     */
    /* (Disposition + Padding + Espacement + Align + Colonnes/Lignes).     */
    /* Apparence : CouleurFond (fond plat, pas de styleFenêtre ce soir).   */
    /* ValeurRenvoyée = runtime-only (VIS_CACHE).                          */
    /* ================================================================== */
    /* --- Identité -------------------------------------------------------- */
    { "Fenetre", "Titre",       "Titre",       "Title",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (geometrie + visibilite + etat) ------------------------------ */
    { "Fenetre", "Visible",     "Visible",     "Visible",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Grise",       "Grisé",       "Grayed",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "X",           "X",           "X",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Y",           "Y",           "Y",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Largeur",     "Largeur",     "Width",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Hauteur",     "Hauteur",     "Height",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Redimensionnable", "Redimensionnable", "Resizable", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Icone",       "Icône",       "Icon",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_TEXT,     80, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition (layout des enfants) -------------------------------- */
    { "Fenetre", "Disposition", "Disposition", "Layout",          OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Padding",     "Marge intérieure", "Inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "PaddingHaut", "Marge intérieure haut", "Top inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "PaddingBas",  "Marge intérieure bas", "Bottom inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "PaddingGauche", "Marge intérieure gauche", "Left inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "PaddingDroite", "Marge intérieure droite", "Right inner padding", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Espacement",  "Espacement",  "Spacing",         OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 70, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "AlignH",      "Alignement horizontal", "Horizontal alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 80, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "AlignV",      "Alignement vertical", "Vertical alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_ENUM, 90, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Colonnes",    "Colonnes",    "Columns",         OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 100, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Lignes",      "Lignes",      "Rows",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 110, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : fond plat + Style type (composite styleConteneur) --- */
    { "Fenetre", "CouleurFond", "Couleur de fond", "Background color", OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Fenetre", "Style",       "Style",           "Style",            OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : valeur de retour runtime (hors designer) -------------- */
    { "Fenetre", "ValeurRenvoyee", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    /* MenuPrincipal : reference d'objet menu (VARIANT), affectee par bloc ou a
     * chaud. Pas un scalaire visuel editable au canvas -> cachee, comme
     * ValeurRenvoyee. La barre de menu s'edite par son propre bloc, pas via
     * l'inspecteur de props de la fenetre. */
    { "Fenetre", "MenuPrincipal", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ CodeBarres — calque du Champ Image (output-only). */
    /* Général = la donnee du code (symbologie + contenu + correction) ;   */
    /* UI = etat radio (Actif/Inactif/Grisé) + visibilite + geometrie ;    */
    /* Détail = cadrage du code dans le champ + layout enfant ; Style typé */
    /* (styleCodeBarre : CadreExtérieur). La prop bitfield Option (options  */
    /* d'affichage/orientation) reste masquee — son editeur cases-a-cocher */
    /* arrivera cote designer, elle est classee cache pour l'instant.       */
    /* ================================================================== */
    /* --- Identite (la donnee du code : type + contenu + correction) ------ */
    { "CodeBarres", "TypeCodeBarres",   "Type de code-barres",  "Barcode type",         OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Contenu",          "Contenu",              "Content",              OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_TEXT, 20, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "CorrectionErreur", "Niveau de correction", "Correction level",     OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI (etat + geometrie) ------------------------------------------- */
    { "CodeBarres", "Visible",          "Visible",              "Visible",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     10, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Etat",             "État initial",         "Initial state",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,    20, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    { "CodeBarres", "X",                "X",                    "X",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Y",                "Y",                    "Y",                    OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Largeur",          "Largeur",              "Width",                OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      50, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Hauteur",          "Hauteur",              "Height",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      60, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Opacite",          "Opacité",              "Opacity",              OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,      70, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "CurseurSouris",    "Curseur souris",       "Mouse cursor",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,     80, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "LargeurMin",       "Largeur min",          "Min width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     110, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "LargeurMax",       "Largeur max",          "Max width",            OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     120, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "HauteurMin",       "Hauteur min",          "Min height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     130, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "HauteurMax",       "Hauteur max",          "Max height",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     140, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Marge",            "Marge",                "Margin",               OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,     150, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "MargeHaut",        "Marge haut",           "Top margin",           OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     160, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "MargeBas",         "Marge bas",            "Bottom margin",        OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     170, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "MargeGauche",      "Marge gauche",         "Left margin",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     180, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "MargeDroite",      "Marge droite",         "Right margin",         OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     190, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "ColSpan",          "Fusion colonnes",      "Column span",          OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     200, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "RowSpan",          "Fusion lignes",        "Row span",             OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,     210, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Disposition : cadrage du code dans le champ + layout enfant ----- */
    { "CodeBarres", "Etire",            "Étirer",               "Stretch",              OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "CadrageHorizontal","Position horizontale", "Horizontal position",  OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 20, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "CadrageVertical",  "Position verticale",   "Vertical position",    OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "CodeBarres", "Alignement",       "Alignement",           "Alignment",            OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 40, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : Style type (composite styleCodeBarre) --------------- */
    { "CodeBarres", "Style",            "Style",                "Style",                OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Aide ------------------------------------------------------------ */
    { "CodeBarres", "Bulle",            "Bulle d'aide",         "Tooltip",              OWL_PRES_GROUP_AIDE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Cachees : descripteur complet + apparence a plat couverte par Style */
    { "CodeBarres", "CodeBarres",       NULL, NULL, OWL_PRES_GROUP_IDENTITE,  OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_VARIANT,   0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres", "Grise",            NULL, NULL, OWL_PRES_GROUP_UI,        OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,      0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres", "CouleurFond",      NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COLOR,     0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres", "Cadre",            NULL, NULL, OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_COMPOSITE, 0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres", "Altitude",         NULL, NULL, OWL_PRES_GROUP_UI,        OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,       0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres", "Groupe",           NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },
    /* Option : bitfield d'affichage/orientation, editeur cases-a-cocher a venir. */
    { "CodeBarres", "Option",           NULL, NULL, OWL_PRES_GROUP_IDENTITE,  OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_ENUM,      0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Champ Onglet — widget positionne qui empile des volets. */
    /* Couche 2 lot 1 : props STRUCTURELLES editables au designer seulement */
    /* — Position de la barre de languettes (enum, cf. Détail) + Valeur     */
    /* (volet actif au demarrage, 1-based, en Contenu). Geometrie,          */
    /* etat et layout enfant restent caches a ce stade (pilotes au canvas) ; */
    /* le style (fond, entetes de volets) est un lot ulterieur.             */
    /* ================================================================== */
    /* --- Contenu (props structurelles editables) ------------------------ */
    { "Onglet", "Valeur",     "Volet affiché au démarrage", "Initial displayed pane", OWL_PRES_GROUP_CONTENU, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT, 20, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- Apparence : style typé (styleOnglet) --------------------------- */
    { "Onglet", "Style",      "Style", "Style", OWL_PRES_GROUP_APPARENCE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_COMPOSITE, 30, OWL_PRES_VIS_DESIGNER, NULL },

    /* --- UI : etat initial (radio) + Visible ; geometrie/layout caches ---- */
    { "Onglet", "Etat",       "État initial", "Initial state", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,    10, OWL_PRES_VIS_DESIGNER, ETAT_VISIBLE_SUBSET },
    { "Onglet", "Visible",    "Visible",  "Visible", OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     20, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "X",          "X",        "X",       OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 30, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "Y",          "Y",        "Y",       OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_POSITION, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "Largeur",    "Largeur",  "Width",   OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      50, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "Hauteur",    "Hauteur",  "Height",  OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,      60, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "Grise",      NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,     0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "Etire",      "Étirer", "Stretch", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "Marge",      NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "MargeHaut",  NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "MargeBas",   NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "MargeGauche", NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "MargeDroite", NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "Alignement", "Alignement", "Alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 30, OWL_PRES_VIS_DESIGNER, NULL },
    /* Bloc "barre de languettes" : ou elle se pose, comment les languettes sont
     * dimensionnees, comment leur texte est cale. Ordre logique 40/50/60. */
    { "Onglet", "Position",   "Position des volets", "Tab position", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM, 40, OWL_PRES_VIS_DESIGNER, NULL },
    { "Onglet", "LargeurVolets", "Largeur des volets", "Tab width", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO, 50, OWL_PRES_VIS_DESIGNER, ONGLET_LARGEURVOLETS_SUBSET },
    { "Onglet", "AlignementLibelle", "Alignement du libellé", "Label alignment", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO, 60, OWL_PRES_VIS_DESIGNER, ONGLET_ALIGNLIBELLE_SUBSET },
    { "Onglet", "ColSpan",    NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "RowSpan",    NULL, NULL, OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_AVANCE,  OWL_PRES_EDITOR_INT,  0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "LargeurMax", NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "LargeurMin", NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "HauteurMax", NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "HauteurMin", NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_INT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet", "Groupe",     NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_VARIANT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Volet — page d'un Champ Onglet (sous-bloc de role).     */
    /* Couche 2 lot 1 : Libellé (texte de l'onglet, identite) + Disposition */
    /* (mode de layout des enfants, comme un Conteneur). Visible reste cache */
    /* (pilote au runtime / canvas).                                       */
    /* ================================================================== */
    { "Volet", "Libelle",     "Libellé",     "Caption", OWL_PRES_GROUP_IDENTITE,    OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Volet", "Disposition", "Disposition", "Layout",  OWL_PRES_GROUP_DISPOSITION, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_ENUM,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "Volet", "Visible",     NULL, NULL, OWL_PRES_GROUP_UI, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Note — emplacement de donnee libre par instance, prop  */
    /* de code lue/ecrite au runtime sur la Fenetre et tous les champs.   */
    /* Cachee de l'inspecteur designer (comme Groupe / ValeurRenvoyee) :  */
    /* c'est un slot de stockage applicatif, pas une option de design.    */
    /* ================================================================== */
    { "Saisie",       "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Libelle",      "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Bouton",       "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Image",        "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Interrupteur", "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Selecteur",    "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Liste",        "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Combo",        "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Table",        "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "ZoneRepetee",  "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Jauge",        "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Conteneur",    "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "CodeBarres",   "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Fenetre",      "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },
    { "Onglet",       "Note", NULL, NULL, OWL_PRES_GROUP_COMPORTEMENT, OWL_PRES_IMPORTANCE_AVANCE, OWL_PRES_EDITOR_TEXT, 0, OWL_PRES_VIS_CACHE, NULL },

    /* ================================================================== */
    /* Noeuds de menu enfants — inspecteur editable calque sur les widgets.*/
    /* Modele etroit (ui_catalog.c PROPS_MENU_*) : un noeud de menu n'a ni  */
    /* geometrie ni couleur propre. Deux onglets seulement portent du       */
    /* contenu : Général (identite) et UI (ui) ; les autres restent vides   */
    /* et sont masques en contexte menu. L'"Etat initial" radio (Etat) est  */
    /* modelise comme sur un widget, sous-ensemble Actif/Inactif/Grisé      */
    /* (ETAT_IMAGE_SUBSET). Visible reste exposé ; Grisé est couvert par le  */
    /* radio Etat, donc caché comme sur tous les widgets.         */
    /* Les racines MenuPrincipal/MenuContextuel n'ont pas de prop de noeud.  */
    /* ================================================================== */
    /* --- OptionMenu ------------------------------------------------------ */
    { "OptionMenu", "Libelle",   "Libellé",           "Caption",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "OptionMenu", "Image",     "Image",             "Image",             OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_IMAGE_FILE,  20, OWL_PRES_VIS_DESIGNER, NULL },
    { "OptionMenu", "Visible",   "Visible",           "Visible",           OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "OptionMenu", "Etat",      "État initial",      "Initial state",     OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       15, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    { "OptionMenu", "Raccourci", "Raccourci clavier", "Keyboard shortcut", OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_SHORTCUT,    30, OWL_PRES_VIS_DESIGNER, NULL },
    { "OptionMenu", "Coche",     "Coché",             "Checked",           OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        40, OWL_PRES_VIS_DESIGNER, NULL },
    /* --- Cachees : couvert par Etat (Grise),  --------------------- */
    { "OptionMenu", "Grise",     NULL,                NULL,                OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,         0, OWL_PRES_VIS_CACHE, NULL },

    /* --- SousMenu -------------------------------------------------------- */
    { "SousMenu",   "Libelle",   "Libellé",           "Caption",           OWL_PRES_GROUP_IDENTITE, OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_MULTILANGUE, 10, OWL_PRES_VIS_DESIGNER, NULL },
    { "SousMenu",   "Visible",   "Visible",           "Visible",           OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
    { "SousMenu",   "Etat",      "État initial",      "Initial state",     OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_RADIO,       15, OWL_PRES_VIS_DESIGNER, ETAT_IMAGE_SUBSET },
    /* --- Cachees : couvert par Etat (Grise),  --------------------- */
    { "SousMenu",   "Grise",     NULL,                NULL,                OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,         0, OWL_PRES_VIS_CACHE, NULL },

    /* --- MenuSéparateur -------------------------------------------------- */
    { "MenuSeparateur", "Visible", "Visible",         "Visible",           OWL_PRES_GROUP_UI,       OWL_PRES_IMPORTANCE_COURANT, OWL_PRES_EDITOR_BOOL,        10, OWL_PRES_VIS_DESIGNER, NULL },
};

#define PRES_COUNT ((int)(sizeof(PRES) / sizeof(PRES[0])))

/* Feuilles de composite (etage 2). Une table PAR TYPE COMPOSITE, reutilisee
 * par tout widget exposant ce composite. Pour Police, le type semantique de
 * Couleur est un entier RVB cote runtime (cf. stdlib_builtin_types.c) : seul
 * cet etage sait qu'il s'edite via un color picker. Les noms de feuilles sont
 * les formes ASCII canon des champs du type (storage : Souligne / Barre). */
/* Brique de feuilles commune aux sous-elements de style. Miroir de
 * STYLE_BRICK_FIELDS (stdlib_builtin_types.c) cote presentation : les 8 props de
 * base editables d'un sous-element, avec leur editeur abstrait resolu. Factorise
 * pour garantir un contrat identique entre ZoneSaisie/Libellé/CadreExtérieur. Le
 * membre Etats de la zone n'y figure pas (collection d'etats, non editable a plat). */
#define STYLE_BRICK_LEAVES(COMP)                                                            \
    { COMP, "Couleur",       OWL_PRES_EDITOR_COLOR,     "Couleur du texte", "Text color"       }, \
    { COMP, "CouleurFond",   OWL_PRES_EDITOR_COLOR,     "Couleur de fond",  "Background color" }, \
    { COMP, "RetraitGauche", OWL_PRES_EDITOR_INT,       "Retrait gauche",   "Left padding"     }, \
    { COMP, "RetraitDroit",  OWL_PRES_EDITOR_INT,       "Retrait droit",    "Right padding"    }, \
    { COMP, "RetraitHaut",   OWL_PRES_EDITOR_INT,       "Retrait haut",     "Top padding"      }, \
    { COMP, "RetraitBas",    OWL_PRES_EDITOR_INT,       "Retrait bas",      "Bottom padding"   }, \
    { COMP, "Police",        OWL_PRES_EDITOR_COMPOSITE, "Police",           "Font"             }, \
    { COMP, "Cadre",         OWL_PRES_EDITOR_COMPOSITE, "Cadre",            "Frame"            }

/* Feuille enum styleImageBouton.Position : position de l'icone par
 * rapport au texte. Valeurs = constantes canon du panel ; libelles derives par
 * le transformateur (table d'overrides positionImageBouton : prefixe mixte
 * posBtnImage / posBtnTexteSeul + EN idiomatique). */
static const char* const POSIMGBTN_VALUES[] = {
    "posBtnImageGauche", "posBtnImageDroite", "posBtnImageHaut",
    "posBtnImageBas", "posBtnImageSeule", "posBtnTexteSeul", NULL
};

/* Feuille enum Cadre.Trait : style du trait du cadre. Valeurs = constantes
 * canon du panel (ordre = E_TraitCadre_values) ; libelles derives par le
 * transformateur (table d'overrides traitCadre : FR court + EN CSS-idiomatique)
 * pour que le combo affiche "Continu/Tirets/..." et non les constantes brutes. */
static const char* const TRAITCADRE_VALUES[] = {
    "TraitContinu", "TraitAucun", "TraitTiret", "TraitPointill\xc3\xa9",
    "TraitDouble", NULL
};

/* Feuille enum styleDégradé.Type : type de remplissage dégradé.
 * Valeurs = constantes canon du panel (ordre = E_TypeDegrade_values) ; libelles
 * derives par le transformateur. */
static const char* const TYPEDEGRADE_VALUES[] = {
    "d\xc3\xa9grad\xc3\xa9""Aucun", "d\xc3\xa9grad\xc3\xa9""Lin\xc3\xa9""aire",
    "d\xc3\xa9grad\xc3\xa9Radial", NULL
};

static const OwlPresLeafEntry PRES_LEAVES[] = {
    { "Police", "Nom",      OWL_PRES_EDITOR_FONT,  "Nom",      "Name"          },
    { "Police", "Taille",   OWL_PRES_EDITOR_INT,   "Taille",   "Size"          },
    { "Police", "Gras",     OWL_PRES_EDITOR_BOOL,  "Gras",     "Bold"          },
    { "Police", "Italique", OWL_PRES_EDITOR_BOOL,  "Italique", "Italic"        },
    { "Police", "Souligne", OWL_PRES_EDITOR_BOOL,  "Souligné", "Underline"     },
    { "Police", "Barre",    OWL_PRES_EDITOR_BOOL,  "Barré",    "Strikethrough" },
    { "Police", "Couleur",  OWL_PRES_EDITOR_COLOR, "Couleur",  "Color"         },

    /* styleSaisie : struct pur de sous-elements structurels, chacun
     * rendu en sous-accordeon (sous-composite). Les props de base + les etats
     * vivent sous ZoneSaisie/Libellé/CadreExtérieur, plus a la racine. */
    { "styleSaisie", "ZoneSaisie",     OWL_PRES_EDITOR_COMPOSITE, "Zone de saisie",  "Input zone"  },
    { "styleSaisie", "Libelle",        OWL_PRES_EDITOR_COMPOSITE, "Libellé",         "Label"       },
    { "styleSaisie", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* styleLibellé : modele widget du Champ Libellé, struct de deux
     * sous-elements structurels rendus en sous-accordeon. Clé = forme canon
     * accentuee (= object_type de la prop Style, cf. check d'affectation typé). */
    { "styleLibell\xc3\xa9", "Libelle",        OWL_PRES_EDITOR_COMPOSITE, "Libellé",         "Label"       },
    { "styleLibell\xc3\xa9", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* styleImage : modele widget du Champ Image, struct d'un seul
     * sous-element structurel (CadreExtérieur, la boite). La source de l'image
     * et le mode d'echelle sont des props top-level, hors Style. */
    { "styleImage", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* styleCodeBarre : modele widget du Champ CodeBarres, struct d'un
     * seul sous-element structurel (CadreExtérieur, la boite). Symbologie et
     * contenu sont des props top-level, hors Style. */
    { "styleCodeBarre", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* styleConteneur : modele widget du Champ Conteneur, struct d'un
     * seul sous-element structurel (CadreExtérieur, la boite). Groupeur passif :
     * aucun texte, aucun etat. */
    { "styleConteneur", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* styleOnglet (, exposé au designer par ) : modele widget du
     * Champ Onglet. Trois sous-elements structurels — la boite sous les
     * languettes (CadreExtérieur, type restreint styleCadreExtérieurOnglet) et
     * les deux surfaces de languette (VoletActif / VoletInactif, meme type
     * styleVolet, stateful survol). */
    { "styleOnglet", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame"   },
    { "styleOnglet", "VoletActif",     OWL_PRES_EDITOR_COMPOSITE, "Volet actif",     "Active pane"   },
    { "styleOnglet", "VoletInactif",   OWL_PRES_EDITOR_COMPOSITE, "Volet inactif",   "Inactive pane" },

    /* styleInterrupteur : modele widget du Champ Interrupteur, struct
     * de quatre sous-elements structurels rendus en sous-accordeon. Le texte des
     * cases (TexteOptions) reutilise la brique texte (styleLibelleSaisie), le
     * conteneur et la boite de coche reutilisent styleCadreExterieur, la marque
     * (Coche) est une feuille dediee styleCoche. Cle = forme canon (= object_type). */
    { "styleInterrupteur", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",   "Outer frame" },
    { "styleInterrupteur", "TexteOptions",   OWL_PRES_EDITOR_COMPOSITE, "Texte des options", "Option text" },
    { "styleInterrupteur", "Coche",          OWL_PRES_EDITOR_COMPOSITE, "Coche",             "Check mark"  },
    { "styleInterrupteur", "CadreCoche",     OWL_PRES_EDITOR_COMPOSITE, "Cadre de la coche", "Check box"   },

    /* styleCoche : sous-element marque de coche. Feuille unique
     * Couleur (couleur de la marque cochée). */
    { "styleCoche", "Couleur", OWL_PRES_EDITOR_COLOR, "Couleur", "Color" },

    /* styleSélecteur : modele widget du Champ Sélecteur (radio),
     * struct de quatre sous-elements structurels rendus en sous-accordeon. Le
     * texte des options (TexteOptions) reutilise la brique texte
     * (styleLibelleSaisie), le conteneur et la boite du rond reutilisent
     * styleCadreExterieur, la puce (rond) est une feuille dediee stylePuce.
     * Cle = forme canon accentuee (= object_type de la prop Style). */
    { "styleS\xc3\xa9lecteur", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",   "Outer frame" },
    { "styleS\xc3\xa9lecteur", "TexteOptions",   OWL_PRES_EDITOR_COMPOSITE, "Texte des options", "Option text" },
    { "styleS\xc3\xa9lecteur", "Puce",           OWL_PRES_EDITOR_COMPOSITE, "Puce",              "Bullet"      },
    { "styleS\xc3\xa9lecteur", "CadrePuce",      OWL_PRES_EDITOR_COMPOSITE, "Cadre de la puce",  "Bullet box"  },

    /* stylePuce : sous-element rond du bouton radio. Feuille unique
     * Couleur (couleur du rond selectionne). */
    { "stylePuce", "Couleur", OWL_PRES_EDITOR_COLOR, "Couleur", "Color" },

    /* styleListe : modele widget du Champ Liste (list box), struct de
     * trois sous-elements structurels rendus en sous-accordeon. Le cadre + fond
     * (CadreExtérieur) reutilise styleCadreExterieur ; le texte des lignes
     * (LibelléLignes) et le texte de la ligne selectionnee (LigneSélectionnée)
     * reutilisent la brique texte (styleLibelleSaisie). Cle = forme canon
     * (= object_type de la prop Style). */
    { "styleListe", "CadreExterieur",    OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",    "Outer frame"  },
    { "styleListe", "LibelleLignes",     OWL_PRES_EDITOR_COMPOSITE, "Libellé des lignes", "Row label"    },
    { "styleListe", "LigneSelectionnee", OWL_PRES_EDITOR_COMPOSITE, "Ligne sélectionnée", "Selected row" },

    /* styleCombo : modele widget du Champ Combo (liste deroulante),
     * struct de cinq sous-elements structurels rendus en sous-accordeon. Libellé
     * (le libellé integre) + CadreExtérieur (le conteneur) + ZoneSaisie (le champ
     * combo) reprennent la structure du Saisie ; LibelléLignes (items du popup) +
     * LigneSélectionnée (item surligné) reprennent le dropdown de la Liste. Cle =
     * forme canon (= object_type de la prop Style). */
    { "styleCombo", "Libelle",           OWL_PRES_EDITOR_COMPOSITE, "Libellé",            "Label"        },
    { "styleCombo", "CadreExterieur",    OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",    "Outer frame"  },
    { "styleCombo", "ZoneSaisie",        OWL_PRES_EDITOR_COMPOSITE, "Zone de saisie",     "Input zone"   },
    { "styleCombo", "LibelleLignes",     OWL_PRES_EDITOR_COMPOSITE, "Libellé des lignes", "Row label"    },
    { "styleCombo", "LigneSelectionnee", OWL_PRES_EDITOR_COMPOSITE, "Ligne sélectionnée", "Selected row" },

    /* styleTable : modele widget du Champ Table, struct de huit
     * sous-elements structurels rendus en sous-accordeon. CadreExtérieur (le
     * cadre + fond) reutilise styleCadreExterieur ; TitreColonne / LibelléLignes
     * / LignesPaires / LignesImpaires / LigneSélectionnée / LigneSurvolée
     * reutilisent la brique texte (styleLibelleSaisie) ; Quadrillage (couleur des
     * separateurs) est une feuille dediee styleQuadrillage. Cle = forme canon
     * (= object_type de la prop Style). Note : LignesPaires / LignesImpaires
     * portent la brique texte complete mais seul CouleurFond y a du sens (fond
     * des lignes alternees) — le masquage des feuilles texte non pertinentes est
     * un raffinement designer (Lot 4), la brique complete est exposee ici pour
     * satisfaire la bijection. */
    { "styleTable", "CadreExterieur",    OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",     "Outer frame"   },
    { "styleTable", "TitreColonne",      OWL_PRES_EDITOR_COMPOSITE, "Titre de colonne",    "Column title"  },
    { "styleTable", "LibelleLignes",     OWL_PRES_EDITOR_COMPOSITE, "Libellé des lignes",  "Row label"     },
    { "styleTable", "LignesPaires",      OWL_PRES_EDITOR_COMPOSITE, "Lignes paires",       "Even rows"     },
    { "styleTable", "LignesImpaires",    OWL_PRES_EDITOR_COMPOSITE, "Lignes impaires",     "Odd rows"      },
    { "styleTable", "LigneSelectionnee", OWL_PRES_EDITOR_COMPOSITE, "Ligne sélectionnée",  "Selected row"  },
    { "styleTable", "LigneSurvolee",     OWL_PRES_EDITOR_COMPOSITE, "Ligne survolée",      "Hovered row"   },
    { "styleTable", "Quadrillage",       OWL_PRES_EDITOR_COMPOSITE, "Quadrillage",         "Gridlines"     },

    /* styleJauge : modele widget du Champ Jauge, struct de trois
     * sous-elements rendus en sous-accordeon. CadreExtérieur (piste + bordure +
     * fond) reutilise styleCadreExterieur ; Remplissage (barre : CouleurFond) et
     * Libellé (texte % : Couleur/Police) reutilisent la brique texte
     * (styleLibelleSaisie). Cle = forme canon (= object_type de la prop Style).
     * Reserve Remplissage : la brique texte complete est exposee pour satisfaire
     * la bijection, mais seul CouleurFond y a du sens (barre de progression). */
    { "styleJauge", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },
    { "styleJauge", "Remplissage",    OWL_PRES_EDITOR_COMPOSITE, "Remplissage",     "Fill"        },
    { "styleJauge", "Libelle",        OWL_PRES_EDITOR_COMPOSITE, "Libellé",         "Label"       },

    /* styleColonne : surcharge locale du styleTable, colonne par
     * colonne. Sous-ensemble de styleTable SANS CadreExtérieur ni Quadrillage
     * (propres au tableau entier). Memes types de sous-elements (styleLibelleSaisie).
     * Meme reserve LignesPaires / LignesImpaires (seul CouleurFond pertinent). */
    { "styleColonne", "TitreColonne",      OWL_PRES_EDITOR_COMPOSITE, "Titre de colonne",   "Column title" },
    { "styleColonne", "LibelleLignes",     OWL_PRES_EDITOR_COMPOSITE, "Libellé des lignes", "Row label"    },
    { "styleColonne", "LignesPaires",      OWL_PRES_EDITOR_COMPOSITE, "Lignes paires",      "Even rows"    },
    { "styleColonne", "LignesImpaires",    OWL_PRES_EDITOR_COMPOSITE, "Lignes impaires",    "Odd rows"     },
    { "styleColonne", "LigneSelectionnee", OWL_PRES_EDITOR_COMPOSITE, "Ligne sélectionnée", "Selected row" },
    { "styleColonne", "LigneSurvolee",     OWL_PRES_EDITOR_COMPOSITE, "Ligne survolée",     "Hovered row"  },

    /* styleZoneRépétée : modele widget du Champ Zone répétée, struct
     * de sept sous-elements structurels rendus en sous-accordeon. CadreExtérieur
     * (cadre + fond du widget entier) et CadreInterne (cadre de chaque
     * repetition) reutilisent styleCadreExterieur ; LignesImpaires / LignesPaires
     * / LigneSélectionnée / LigneSurvolée (fonds par-rep) reutilisent la brique
     * texte (styleLibelleSaisie) ; Séparateur (couleur du trait entre reps) est
     * une feuille dediee styleQuadrillage. Cle = forme canon ACCENTUEE
     * (= object_type de la prop Style). Defauts sobres : fonds transparents,
     * cadres invisibles — le masquage des feuilles texte non pertinentes des
     * fonds par-rep est un raffinement designer, la brique complete est exposee
     * ici pour satisfaire la bijection. */
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "CadreExterieur",    OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur",    "Outer frame"  },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "CadreInterne",      OWL_PRES_EDITOR_COMPOSITE, "Cadre interne",      "Inner frame"  },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "LignesImpaires",    OWL_PRES_EDITOR_COMPOSITE, "Lignes impaires",    "Odd rows"     },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "LignesPaires",      OWL_PRES_EDITOR_COMPOSITE, "Lignes paires",      "Even rows"    },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "LigneSelectionnee", OWL_PRES_EDITOR_COMPOSITE, "Ligne sélectionnée", "Selected row" },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "LigneSurvolee",     OWL_PRES_EDITOR_COMPOSITE, "Ligne survolée",     "Hovered row"  },
    { "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "Separateur",        OWL_PRES_EDITOR_COMPOSITE, "Séparateur",         "Separator"    },

    /* styleQuadrillage : sous-element separateurs du tableau.
     * Feuille unique Couleur (couleur des lignes de separation). */
    { "styleQuadrillage", "Couleur", OWL_PRES_EDITOR_COLOR, "Couleur", "Color" },

    /* styleBouton (, ) : modele widget du Champ Bouton, struct
     * de trois sous-elements structurels rendus en sous-accordeon. Le sous-element
     * Libellé (texte) est stateful (palette survol/appuyé/focus/grisé -> onglets
     * d'etat cote front) ; Image (icone : Fichier + Position) est statique. Le leaf
     * matche le nom de champ storage (ASCII) du type. Clé composite = forme canon
     * accentuee (= object_type). */
    { "styleBouton", "Libelle",        OWL_PRES_EDITOR_COMPOSITE, "Libellé",         "Caption"     },
    { "styleBouton", "Image",          OWL_PRES_EDITOR_COMPOSITE, "Image",           "Image"       },
    { "styleBouton", "CadreExterieur", OWL_PRES_EDITOR_COMPOSITE, "Cadre extérieur", "Outer frame" },

    /* Feuilles des sous-elements de style. Chaque sous-element de
     * styleSaisie (ZoneSaisie/Libellé/CadreExtérieur) est un sous-composite : le
     * front derive son propre accordeon depuis ces feuilles. Contrat identique
     * (la brique de style commune), keye par la forme ASCII canon du type.
     * Le membre `Etats` de la zone de saisie est VOLONTAIREMENT exclu : c'est la
     * collection de surcharges par etat, pilotee par le selecteur d'etat du
     * front, pas une feuille editable a plat. Police/Cadre restent des
     * sous-composites (le front resout leurs feuilles via composite_leaves). */
    STYLE_BRICK_LEAVES("styleZoneSaisie"),
    { "styleZoneSaisie", "Ombre",   OWL_PRES_EDITOR_COMPOSITE, "Ombre",   "Shadow"   },
    { "styleZoneSaisie", "Degrade", OWL_PRES_EDITOR_COMPOSITE, "Dégradé", "Gradient" },
    STYLE_BRICK_LEAVES("styleLibelleSaisie"),
    /* styleCadreExterieur (allege ) : un cadre n'a pas de texte -> PAS
     * de feuille Couleur (texte) ni Police. Feuilles pertinentes seules :
     * CouleurFond + Retraits + Cadre. Diverge donc de STYLE_BRICK_LEAVES. */
    { "styleCadreExterieur", "CouleurFond",   OWL_PRES_EDITOR_COLOR,     "Couleur de fond", "Background color" },
    { "styleCadreExterieur", "RetraitGauche", OWL_PRES_EDITOR_INT,       "Retrait gauche",  "Left padding"     },
    { "styleCadreExterieur", "RetraitDroit",  OWL_PRES_EDITOR_INT,       "Retrait droit",   "Right padding"    },
    { "styleCadreExterieur", "RetraitHaut",   OWL_PRES_EDITOR_INT,       "Retrait haut",    "Top padding"      },
    { "styleCadreExterieur", "RetraitBas",    OWL_PRES_EDITOR_INT,       "Retrait bas",     "Bottom padding"   },
    { "styleCadreExterieur", "Cadre",         OWL_PRES_EDITOR_COMPOSITE, "Cadre",           "Frame"            },
    { "styleCadreExterieur", "Ombre",         OWL_PRES_EDITOR_COMPOSITE, "Ombre",           "Shadow"           },
    { "styleCadreExterieur", "Degrade",       OWL_PRES_EDITOR_COMPOSITE, "Dégradé",         "Gradient"         },

    /* styleOmbre : sous-objet ombre portée du CadreExtérieur.
     * Feuilles primitives : couleur + décalages + flou + étendue. */
    { "styleOmbre", "Couleur",   OWL_PRES_EDITOR_COLOR, "Couleur",    "Color"    },
    { "styleOmbre", "DecalageX", OWL_PRES_EDITOR_INT,   "Décalage X", "Offset X" },
    { "styleOmbre", "DecalageY", OWL_PRES_EDITOR_INT,   "Décalage Y", "Offset Y" },
    { "styleOmbre", "Flou",      OWL_PRES_EDITOR_INT,   "Flou",       "Blur"     },
    { "styleOmbre", "Etendue",   OWL_PRES_EDITOR_INT,   "Étendue",    "Spread"   },

    /* styleDégradé : sous-objet dégradé de fond du CadreExtérieur.
     * Feuilles primitives : type (enum) + angle + couleurs début/fin. */
    { "styleDegrade", "Type",         OWL_PRES_EDITOR_ENUM,  "Type",          "Type",
      TYPEDEGRADE_VALUES, NULL, NULL },
    { "styleDegrade", "Angle",        OWL_PRES_EDITOR_INT,   "Angle",         "Angle"       },
    { "styleDegrade", "CouleurDebut", OWL_PRES_EDITOR_COLOR, "Couleur début", "Start color" },
    { "styleDegrade", "CouleurFin",   OWL_PRES_EDITOR_COLOR, "Couleur fin",   "End color"   },
    /* styleZoneBouton : brique du sous-element bouton (stateful).
     * Le membre Etats (collection de surcharges) est exclu comme pour la
     * ZoneSaisie : pilote par le selecteur d'etat du front, pas une feuille. */
    STYLE_BRICK_LEAVES("styleZoneBouton"),
    /* styleImageBouton (, ) : sous-element icone du bouton.
     * Deux feuilles : Image (source de l'icone, editeur image ; VARIANT
     * Image|chaine|Icone) + Position (enum posBtnImage*, editeur enum). */
    { "styleImageBouton", "Image",    OWL_PRES_EDITOR_IMAGE_FILE, "Image",    "Image" },
    { "styleImageBouton", "Position", OWL_PRES_EDITOR_ENUM, "Position", "Position",
      POSIMGBTN_VALUES, NULL, NULL },

    /* Cadre : feuilles primitives (phase 1). Les sous-composites Coin/Trait
     * du template restent en phase 2, non exposes ici. */
    { "Cadre", "Couleur",   OWL_PRES_EDITOR_COLOR, "Couleur",   "Color"      },
    { "Cadre", "Epaisseur", OWL_PRES_EDITOR_INT,   "Épaisseur", "Thickness"  },
    { "Cadre", "Trait",     OWL_PRES_EDITOR_ENUM,  "Trait",     "Line style", TRAITCADRE_VALUES, NULL, NULL },
    { "Cadre", "Arrondi",   OWL_PRES_EDITOR_INT,   "Arrondi",   "Radius"     },
    { "Cadre", "Bords",     OWL_PRES_EDITOR_ENUM,  "Bords",     "Edges"      },

    /* CadreOnglet : bordure restreinte du pane d'onglet. Feuilles
     * primitives seules (Couleur/Épaisseur/Arrondi), labels FR accentués comme
     * le Cadre complet — sans quoi le front humanise le nom ASCII « Epaisseur ». */
    { "CadreOnglet", "Couleur",   OWL_PRES_EDITOR_COLOR, "Couleur",   "Color"     },
    { "CadreOnglet", "Epaisseur", OWL_PRES_EDITOR_INT,   "Épaisseur", "Thickness" },
    { "CadreOnglet", "Arrondi",   OWL_PRES_EDITOR_INT,   "Arrondi",   "Radius"    },

    /* styleVolet : surface d'une languette (VoletActif/VoletInactif).
     * Feuilles éditables à plat : texte (Couleur), fond (CouleurFond), Police
     * (sous-composite) + Arrondi de la forme. Le membre Etats (surcharge survol)
     * est exclu : piloté par le sélecteur d'état du front, pas une feuille. */
    { "styleVolet", "Couleur",     OWL_PRES_EDITOR_COLOR,     "Couleur du texte", "Text color"       },
    { "styleVolet", "CouleurFond", OWL_PRES_EDITOR_COLOR,     "Couleur de fond",  "Background color" },
    { "styleVolet", "Police",      OWL_PRES_EDITOR_COMPOSITE, "Police",           "Font"             },
    { "styleVolet", "Arrondi",     OWL_PRES_EDITOR_INT,       "Arrondi",          "Corner radius"    },
};

#define LEAF_COUNT ((int)(sizeof(PRES_LEAVES) / sizeof(PRES_LEAVES[0])))

/* Kinds opt-in : seuls les widgets industrialises sont controles. */
static const char* const COVERED_KINDS[] = { "Saisie", "Libelle", "Bouton", "Image", "Interrupteur", "Selecteur", "Liste", "Combo", "Table", "Colonne", "ZoneRepetee", "Jauge", "Conteneur", "Fenetre", "CodeBarres", "OptionMenu", "SousMenu", "MenuSeparateur", "Onglet", "Volet", NULL };

int owl_presentation_count(void) { return PRES_COUNT; }

int owl_presentation_leaf_count(void) { return LEAF_COUNT; }

const OwlPresLeafEntry* owl_presentation_leaf_at(int index) {
    if (index < 0 || index >= LEAF_COUNT) return NULL;
    return &PRES_LEAVES[index];
}

/* Lookup d'une entree feuille par (type composite, nom de feuille). */
static const OwlPresLeafEntry* pres_leaf_find(const char* composite, const char* leaf) {
    for (int i = 0; i < LEAF_COUNT; i++) {
        if (strcasecmp(PRES_LEAVES[i].composite, composite) == 0 &&
            strcasecmp(PRES_LEAVES[i].leaf, leaf) == 0)
            return &PRES_LEAVES[i];
    }
    return NULL;
}

/* Template runtime d'un type composite (ex "Police"). Les templates builtins
 * vivent dans vm.types (cf. ). Read-only : aucune allocation, pas de
 * discipline GC roots requise. */
static const ObjInstance* pres_composite_template(const char* typeName) {
    ObjString* key = table_find_key_case_insensitive(&vm.types, typeName);
    if (!key) return NULL;
    Value v;
    if (!table_get(&vm.types, key, &v)) return NULL;
    if (!IS_INSTANCE(v)) return NULL;
    return AS_INSTANCE(v);
}

/* Vrai si un type composite est expose par au moins une entree designer
 * (editor composite) d'un kind couvert. Sert au self-test de labels : seules
 * les feuilles d'un composite reellement affiche exigent un libelle. */
static bool pres_composite_is_exposed(const char* compType) {
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!owl_presentation_kind_is_covered(e->kind)) continue;
        if (e->editor != OWL_PRES_EDITOR_COMPOSITE) continue;
        if (e->visibilite != OWL_PRES_VIS_DESIGNER) continue;
        const OwlControlPropDef* def = owl_dsl_prop_find(e->kind, e->prop);
        if (def && def->object_type && strcasecmp(def->object_type, compType) == 0)
            return true;
    }
    return false;
}

/* Vrai si le type composite possede reellement une feuille `leaf`. */
static bool pres_composite_has_leaf(const ObjInstance* tpl, const char* leaf) {
    for (int i = 0; i < tpl->fields.capacity; i++) {
        const ObjString* k = tpl->fields.entries[i].key;
        if (k && strcasecmp(k->chars, leaf) == 0) return true;
    }
    return false;
}

const OwlPresEntry* owl_presentation_at(int index) {
    if (index < 0 || index >= PRES_COUNT) return NULL;
    return &PRES[index];
}

bool owl_presentation_kind_is_covered(const char* kind) {
    if (!kind) return false;
    for (const char* const* k = COVERED_KINDS; *k != NULL; k++)
        if (strcmp(*k, kind) == 0) return true;
    return false;
}

const OwlPresEntry* owl_presentation_find(const char* kind, const char* prop) {
    if (!kind || !prop) return NULL;
    /* Resolution par identite de prop : robuste aux formes accent/casse. */
    const OwlControlPropDef* target = owl_dsl_prop_find(kind, prop);
    for (int i = 0; i < PRES_COUNT; i++) {
        if (strcmp(PRES[i].kind, kind) != 0) continue;
        if (target) {
            if (owl_dsl_prop_find(kind, PRES[i].prop) == target) return &PRES[i];
        } else if (strcasecmp(PRES[i].prop, prop) == 0) {
            return &PRES[i];
        }
    }
    return NULL;
}

const char* owl_pres_group_name(OwlPresGroup g) {
    switch (g) {
        case OWL_PRES_GROUP_IDENTITE:    return "identite";
        case OWL_PRES_GROUP_CONTENU:     return "contenu";
        case OWL_PRES_GROUP_UI:          return "ui";
        case OWL_PRES_GROUP_DISPOSITION: return "disposition";
        case OWL_PRES_GROUP_APPARENCE:   return "apparence";
        case OWL_PRES_GROUP_COMPORTEMENT:return "comportement";
        case OWL_PRES_GROUP_AIDE:        return "aide";
        case OWL_PRES_GROUP_AVANCE:      return "avance";
    }
    return "avance";
}

const char* owl_pres_importance_name(OwlPresImportance imp) {
    return imp == OWL_PRES_IMPORTANCE_AVANCE ? "avance" : "courant";
}

const char* owl_pres_editor_name(OwlPresEditor ed) {
    switch (ed) {
        case OWL_PRES_EDITOR_TEXT:        return "text";
        case OWL_PRES_EDITOR_INT:         return "int";
        case OWL_PRES_EDITOR_BOOL:        return "bool";
        case OWL_PRES_EDITOR_ENUM:        return "enum";
        case OWL_PRES_EDITOR_RADIO:       return "radio";
        case OWL_PRES_EDITOR_BOOL_COMBO:  return "bool_combo";
        case OWL_PRES_EDITOR_COLOR:       return "color";
        case OWL_PRES_EDITOR_FONT:        return "font";
        case OWL_PRES_EDITOR_IMAGE_FILE:  return "imagefile";
        case OWL_PRES_EDITOR_SHORTCUT:    return "shortcut";
        case OWL_PRES_EDITOR_MULTILANGUE: return "multilangue";
        case OWL_PRES_EDITOR_COMPOSITE:   return "composite";
        case OWL_PRES_EDITOR_POSITION:    return "position";
        case OWL_PRES_EDITOR_VARIANT:     return "variant";
    }
    return "text";
}

const char* owl_pres_visibilite_name(OwlPresVisibilite v) {
    return v == OWL_PRES_VIS_CACHE ? "cache" : "designer";
}

/* Escaper JSON minimal (les valeurs sont des identifiants ASCII ou de l'UTF-8
 * valide ; seuls " et \ et les controles doivent etre echappes). */
static void pres_json_str(FILE* out, const char* s) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 0x20)          { fprintf(out, "\\u%04x", *p); }
        else                         { fputc(*p, out); }
    }
    fputc('"', out);
}

/* === Transformateur de libelles d'enum ================================
 *
 * Derive un libelle d'affichage lisible a partir du nom de constante d'un
 * enum : retrait du prefixe commun a toutes les valeurs de l'enum, puis mise
 * en forme camelCase -> "Premier mot capitalise, suite en minuscules". Ex :
 *   alignDébut  (prefixe "align") -> "Début"
 *   typSaisieMotDePasse (prefixe "typSaisie") -> "Mot de passe"
 * FR est derive du nom FR canon (values de l'enum), EN du nameEN de la
 * constante membre — les prefixes FR et EN different (align/align vs
 * typSaisie/typeInput), donc chaque langue calcule son propre prefixe.
 *
 * La derivation echoue proprement (rend la valeur brute) quand :
 *   - aucun prefixe commun (valeurs deja lisibles : Actif/AffichageSeulement) ;
 *   - le decoupage produit un prefixe mixte (posBtnImage / posBtnTexteSeul).
 * Ces cas passent par la table d'overrides ci-dessous, sinon on omet le
 * libelle (label == valeur : le front retombe sur la valeur brute lisible). */

/* Longueur en octets d'une lettre majuscule (debut de mot camelCase) a la
 * position p : 1 pour ASCII A-Z, 2 pour une majuscule accentuee Latin-1
 * (0xC3 0x80..0x9E, hors 0x97 = signe multiplication). 0 sinon. */
static int pres_upper_len(const char* p) {
    unsigned char c = (unsigned char)p[0];
    if (c >= 'A' && c <= 'Z') return 1;
    if (c == 0xC3) {
        unsigned char c2 = (unsigned char)p[1];
        if (c2 >= 0x80 && c2 <= 0x9E && c2 != 0x97) return 2;
    }
    return 0;
}

/* Ecrit dans out (min-lowercase) le caractere a p ; retourne les octets
 * consommes. ASCII A-Z -> +32 ; majuscule accentuee Latin-1 -> +0x20 sur le
 * second octet ; tout le reste recopie tel quel. */
static int pres_write_lower(char* out, int* w, const char* p) {
    unsigned char c = (unsigned char)p[0];
    if (c >= 'A' && c <= 'Z') { out[(*w)++] = (char)(c + 32); return 1; }
    if (c == 0xC3) {
        unsigned char c2 = (unsigned char)p[1];
        if (c2 >= 0x80 && c2 <= 0x9E && c2 != 0x97) {
            out[(*w)++] = (char)0xC3; out[(*w)++] = (char)(c2 + 0x20); return 2;
        }
        out[(*w)++] = (char)c; out[(*w)++] = (char)c2; return 2;
    }
    out[(*w)++] = (char)c; return 1;
}

/* Prefixe commun d'un ensemble de valeurs d'enum, coupe a une frontiere de mot
 * camelCase : plus long prefixe commun a toutes les valeurs, retreci jusqu'a ce
 * que le caractere suivant (dans la premiere valeur) soit une majuscule (debut
 * du premier mot signifiant). Retourne la longueur en octets (0 = pas de
 * prefixe strippable). */
static int pres_common_prefix_len(const char* const* values) {
    if (!values || !values[0]) return 0;
    int n = 0; for (const char* const* v = values; *v; v++) n++;
    if (n < 2) return 0; /* une seule valeur : pas de prefixe fiable */
    const char* first = values[0];
    /* LCP octet a octet. */
    int lcp = 0;
    for (;; lcp++) {
        char ch = first[lcp];
        if (ch == '\0') break;
        bool all = true;
        for (const char* const* v = values; *v; v++) {
            if ((*v)[lcp] != ch) { all = false; break; }
        }
        if (!all) break;
    }
    /* Retrecir jusqu'a ce que first[lcp] soit un debut de mot (majuscule). */
    while (lcp > 0 && pres_upper_len(first + lcp) == 0) lcp--;
    /* Le prefixe ne doit pas engloutir toute la premiere valeur. */
    if (first[lcp] == '\0') return 0;
    return lcp;
}

/* Derive le libelle d'une valeur en retirant `prefixLen` octets puis en mettant
 * en forme le reste (camelCase -> "Premier mot, suite minuscule"). Ecrit dans
 * buf (taille bufSize). Retourne true si le libelle differe de la valeur brute
 * (sinon caller omet le libelle : deja lisible).
 *
 * Frontiere de mot = debut d'une majuscule. Deux cas :
 *   - une majuscule suivie d'une minuscule -> mot normal (Capitale + minuscules) ;
 *   - une suite de 2+ majuscules consecutives -> acronyme, conserve tel quel en
 *     un seul token (curTailleNS -> "Taille NS", pas "Taille n s"). */
static bool pres_derive_label(const char* value, int prefixLen,
                              char* buf, int bufSize) {
    if (!value) return false;
    const char* rest = value + prefixLen;
    if (*rest == '\0') return false;
    int w = 0;
    bool firstWord = true;
    const char* p = rest;
    while (*p && w < bufSize - 4) {
        int ul = pres_upper_len(p);
        if (ul == 0) {
            /* Milieu d'un mot : recopie brute. */
            buf[w++] = *p++;
            firstWord = false;
            continue;
        }
        /* Debut de mot. Espace de separation sauf pour le tout premier. */
        if (!firstWord) buf[w++] = ' ';
        /* Longueur du run de majuscules consecutives. */
        const char* q = p + ul;
        int run = 1;
        while (pres_upper_len(q) > 0) { q += pres_upper_len(q); run++; }
        /* Regle camelCase : si le run est suivi d'une minuscule (debut d'un mot
         * capitalise), la DERNIERE majuscule du run appartient a ce mot, pas a
         * l'acronyme. Ex "HFleche" -> "H" (acronyme d'1 lettre) + "Fleche". */
        bool followedByLower = (*q != '\0' && pres_upper_len(q) == 0
                                && (unsigned char)*q > ' ');
        int acroLetters = (run >= 2 && followedByLower) ? run - 1 : run;
        if (acroLetters >= 2) {
            /* Acronyme : recopie les majuscules du run telles quelles. */
            for (int k = 0; k < acroLetters; k++) {
                int l2 = pres_upper_len(p);
                for (int m = 0; m < l2; m++) buf[w++] = p[m];
                p += l2;
            }
        } else if (firstWord) {
            /* Premier mot : majuscule conservee, suite en minuscule. */
            for (int k = 0; k < ul; k++) buf[w++] = p[k];
            p += ul;
            while (*p && pres_upper_len(p) == 0) p += pres_write_lower(buf, &w, p);
        } else {
            /* Mot suivant (sentence case) : entierement en minuscule. */
            p += pres_write_lower(buf, &w, p);
            while (*p && pres_upper_len(p) == 0) p += pres_write_lower(buf, &w, p);
        }
        firstWord = false;
    }
    buf[w] = '\0';
    return strcmp(buf, value) != 0;
}

/* Table d'overrides : libelles explicites qui priment sur la derivation la ou
 * l'heuristique echoue (EN irregulier, prefixe mixte, cas speciaux). Cle =
 * (nom d'enum canon, valeur canon). Source de verite unique pour ces enums. */
typedef struct {
    const char* enum_name;
    const char* value;
    const char* labelFR;
    const char* labelEN;
} OwlEnumLabelOverride;

static const OwlEnumLabelOverride PRES_ENUM_LABEL_OVERRIDES[] = {
    /* typeBouton : EN irregulier (Interrupt/Abort/Help, pas le calque FR). */
    { "typeBouton", "btnNormal",       "Normal",       "Normal"     },
    { "typeBouton", "btnValider",      "Validation",   "Validation" },
    { "typeBouton", "btnInterruption", "Interruption", "Interrupt"  },
    { "typeBouton", "btnAbandon",      "Abandon",      "Abort"      },
    { "typeBouton", "btnAide",         "Aide",         "Help"       },
    /* Position de l'image du bouton (feuille styleImageBouton) : prefixe mixte
     * posBtnImage / posBtnTexteSeul + EN idiomatique (Icon only / Text only). */
    { "positionImageBouton", "posBtnImageGauche", "Gauche",      "Left"      },
    { "positionImageBouton", "posBtnImageDroite", "Droite",      "Right"     },
    { "positionImageBouton", "posBtnImageHaut",   "Haut",        "Top"       },
    { "positionImageBouton", "posBtnImageBas",    "Bas",         "Bottom"    },
    { "positionImageBouton", "posBtnImageSeule",  "Image seule", "Icon only" },
    { "positionImageBouton", "posBtnTexteSeul",   "Texte seul",  "Text only" },
    /* Cadrage image (Champ Image) : FR accentue perdu par la derivation
     * (cad* -> Centre/Etire/Homothetique). EN idiomatique. */
    { "cadrage", "cadCentre",       "Centré",       "Center"       },
    { "cadrage", "cadEtire",        "Étiré",        "Stretch"      },
    { "cadrage", "cadHomothetique", "Homothétique", "Proportional" },
    /* Type d'une colonne de Table (colType*) : FR accentue perdu par la
     * derivation (Numerique/Booleen). EN idiomatique court (Text/Numeric...). */
    { "colTypes", "colTypeTexte",     "Texte",     "Text"     },
    { "colTypes", "colTypeNumerique", "Numérique", "Numeric"  },
    { "colTypes", "colTypeMonétaire", "Monétaire", "Currency" },
    { "colTypes", "colTypeDate",      "Date",      "Date"     },
    { "colTypes", "colTypeHeure",     "Heure",     "Time"     },
    { "colTypes", "colTypeDurée",     "Durée",     "Duration" },
    { "colTypes", "colTypeBooleen",   "Booléen",   "Boolean"  },
    { "colTypes", "colTypeMotDePasse", "Mot de passe", "Password" },
    /* Curseur souris (combo Curseur souris) : les constantes cur* ne sont pas
     * lisibles telles quelles et la derivation heuristique reste incomplete
     * (ex "Hand" au lieu de "Pointing hand", "Caret" au lieu de "Texte", "Wait"
     * au lieu de "Busy"). Libelles amis figes ici, FR + EN idiomatique. */
    { "curseurs", "curFl\xc3\xa8""che",    "Flèche",                   "Arrow"                    },
    { "curseurs", "curMain",               "Main",                     "Pointing hand"            },
    { "curseurs", "curMainSyst\xc3\xa8me", "Main système",             "System hand"              },
    { "curseurs", "curCaret",              "Texte",                    "Text"                     },
    { "curseurs", "curCroix",              "Croix",                    "Cross"                    },
    { "curseurs", "curSablier",            "Sablier",                  "Busy"                     },
    { "curseurs", "curAide",               "Aide",                     "Help"                     },
    { "curseurs", "curInterdit",           "Interdit",                 "Forbidden"                },
    { "curseurs", "curHFl\xc3\xa8""che",   "Flèche haut",              "Up arrow"                 },
    { "curseurs", "curTaille",             "Redimensionner",           "Resize all"               },
    { "curseurs", "curTailleNS",           "Redim. vertical",          "Resize vertical"          },
    { "curseurs", "curTailleOE",           "Redim. horizontal",        "Resize horizontal"        },
    { "curseurs", "curTailleNESO",         "Redim. diagonale (NE-SO)", "Resize diagonal (NE-SW)"  },
    { "curseurs", "curTailleNOSE",         "Redim. diagonale (NO-SE)", "Resize diagonal (NW-SE)"  },
    /* Etat unifie d'un champ (radio "État initial"). Les valeurs FR sont deja
     * lisibles en mono-mot (Actif/Inactif/Grisé/AffichageSeulement), donc la
     * derivation ne gagne sur aucune -> sans libelle explicite, les kinds dont
     * le sous-ensemble est {Actif, Inactif, Grisé} (Image/Interrupteur/
     * Selecteur) n'emettaient aucun label et retombaient sur le FR brut, y
     * compris en EN. Overrides bilingues explicites : le FR reprend la valeur
     * canon, l'EN reutilise la forme EN du panel (Active/Inactive/Grayed). */
    { "etat", "Actif",           "Actif",  "Active"   },
    { "etat", "Inactif",         "Inactif", "Inactive" },
    { "etat", "Gris\xc3\xa9",    "Gris\xc3\xa9", "Grayed"   },
    /* Trait du cadre (feuille Cadre.Trait) : constantes Trait* -> libelles
     * courts FR + EN CSS-idiomatique (Solid/Dashed/Dotted). */
    { "traitCadre", "TraitContinu",          "Continu",    "Solid"  },
    { "traitCadre", "TraitAucun",            "Aucun",      "None"   },
    { "traitCadre", "TraitTiret",            "Tirets",     "Dashed" },
    { "traitCadre", "TraitPointill\xc3\xa9", "Pointillés", "Dotted" },
    { "traitCadre", "TraitDouble",           "Double",     "Double" },
    /* Largeur des volets (radio du Champ Onglet) : la derivation donne
     * "Proportionnelle" tout court, sans dire proportionnelle A QUOI. On
     * precise que la largeur suit le libelle de la languette. L'autre valeur
     * (largeurÉgale -> "Égale" / "Equal") reste derivee. */
    { "largeurVolets", "largeurProportionnelle",
      "Proportionnelle au libell\xc3\xa9", "Proportional to label" },
};
#define OVERRIDE_COUNT ((int)(sizeof(PRES_ENUM_LABEL_OVERRIDES) / sizeof(PRES_ENUM_LABEL_OVERRIDES[0])))

/* Cherche un override pour (enum_name, value). NULL si absent. Le champ
 * demande (fr/en) est renvoye via labelFR/labelEN. */
static const OwlEnumLabelOverride* pres_label_override(const char* enum_name,
                                                       const char* value) {
    if (!enum_name || !value) return NULL;
    for (int i = 0; i < OVERRIDE_COUNT; i++) {
        const OwlEnumLabelOverride* o = &PRES_ENUM_LABEL_OVERRIDES[i];
        if (strcasecmp(o->enum_name, enum_name) == 0 &&
            strcmp(o->value, value) == 0)
            return o;
    }
    return NULL;
}

/* Resout le libelle EN d'une valeur : nameEN de la constante membre si connu,
 * sinon la valeur FR (fallback). Utilise pour calculer le prefixe EN et
 * deriver les libelles EN, dont les prefixes different du FR. */
static const char* pres_value_en(const char* value) {
    const OwlConstDef* c = owl_registry_const_find(value);
    if (c && c->nameEN && *c->nameEN) return c->nameEN;
    return value;
}

/* Emet, pour un enum donne (par son nom canon + ses valeurs), les tableaux
 * "enum_subset_labels_fr" et "enum_subset_labels_en" — MAIS seulement si au
 * moins un libelle apporte (override ou derivation != valeur brute). Si tous
 * les libelles egalent la valeur brute (enum deja lisible), n'emet rien : le
 * front retombe sur la valeur. Les tableaux sont paralleles a `values`
 * (meme ordre, meme longueur), l'entree "deja lisible" y reprend la valeur
 * brute pour garder l'alignement. */
static void pres_emit_enum_labels(FILE* out, const char* enum_name,
                                  const char* const* values) {
    if (!enum_name || !values || !values[0]) return;

    /* Prefixe commun FR (sur les valeurs) et EN (sur les nameEN). */
    int prefixFR = pres_common_prefix_len(values);
    int nv = 0; for (const char* const* v = values; *v; v++) nv++;
    const char** enVals = (const char**)malloc(sizeof(char*) * (size_t)(nv + 1));
    if (!enVals) return;
    for (int i = 0; i < nv; i++) enVals[i] = pres_value_en(values[i]);
    enVals[nv] = NULL;
    int prefixEN = pres_common_prefix_len(enVals);

    /* Premiere passe : y a-t-il au moins un libelle qui apporte ? */
    bool any = false;
    char tmp[256];
    for (int i = 0; i < nv && !any; i++) {
        if (pres_label_override(enum_name, values[i])) { any = true; break; }
        if (pres_derive_label(values[i], prefixFR, tmp, sizeof(tmp))) any = true;
    }
    if (!any) { free(enVals); return; }

    /* FR. */
    fprintf(out, ",\"enum_subset_labels_fr\":[");
    for (int i = 0; i < nv; i++) {
        if (i) fputc(',', out);
        const OwlEnumLabelOverride* o = pres_label_override(enum_name, values[i]);
        if (o) { pres_json_str(out, o->labelFR); continue; }
        if (pres_derive_label(values[i], prefixFR, tmp, sizeof(tmp)))
            pres_json_str(out, tmp);
        else
            pres_json_str(out, values[i]); /* deja lisible : valeur brute */
    }
    fputc(']', out);

    /* EN. */
    fprintf(out, ",\"enum_subset_labels_en\":[");
    for (int i = 0; i < nv; i++) {
        if (i) fputc(',', out);
        const OwlEnumLabelOverride* o = pres_label_override(enum_name, values[i]);
        if (o) { pres_json_str(out, o->labelEN); continue; }
        if (pres_derive_label(enVals[i], prefixEN, tmp, sizeof(tmp)))
            pres_json_str(out, tmp);
        else
            pres_json_str(out, enVals[i]); /* deja lisible : valeur brute EN */
    }
    fputc(']', out);

    free(enVals);
}

/* Resout le libelle FR (wantEN=false) ou EN (wantEN=true) d'une valeur d'enum,
 * meme chemin que l'emission JSON : override d'abord, sinon derivation, sinon
 * valeur brute. Ecrit dans buf. Utilise par le self-test (verrou de valeur
 * exacte) pour valider le resultat sans dupliquer les libelles cote test. */
static void pres_resolve_label(const char* enum_name, const char* const* values,
                               int idx, bool wantEN, char* buf, int bufSize) {
    const char* value = values[idx];
    const OwlEnumLabelOverride* o = pres_label_override(enum_name, value);
    if (o) {
        snprintf(buf, (size_t)bufSize, "%s", wantEN ? o->labelEN : o->labelFR);
        return;
    }
    if (!wantEN) {
        int prefixFR = pres_common_prefix_len(values);
        if (!pres_derive_label(value, prefixFR, buf, bufSize))
            snprintf(buf, (size_t)bufSize, "%s", value);
        return;
    }
    /* EN : prefixe calcule sur les nameEN. */
    int nv = 0; for (const char* const* v = values; *v; v++) nv++;
    const char** enVals = (const char**)malloc(sizeof(char*) * (size_t)(nv + 1));
    if (!enVals) { snprintf(buf, (size_t)bufSize, "%s", value); return; }
    for (int i = 0; i < nv; i++) enVals[i] = pres_value_en(values[i]);
    enVals[nv] = NULL;
    int prefixEN = pres_common_prefix_len(enVals);
    if (!pres_derive_label(enVals[idx], prefixEN, buf, bufSize))
        snprintf(buf, (size_t)bufSize, "%s", enVals[idx]);
    free(enVals);
}

/* Resout le nom d'enum et les valeurs derriere une prop de presentation. Rend
 * le nom d'enum (via *outEnumName) et les valeurs (retour). Pour une prop a
 * enum_subset explicite (ex Etat), utilise ce sous-ensemble et le nom d'enum
 * de la prop. Pour une prop enum sans subset, resout l'enum du catalogue
 * semantique et prend toutes ses valeurs. NULL si la prop n'est pas un enum. */
static const char* const* pres_entry_enum_values(const OwlPresEntry* e,
                                                 const char** outEnumName) {
    *outEnumName = NULL;
    const OwlControlPropDef* def = owl_dsl_prop_find(e->kind, e->prop);
    if (!def || def->type != OWL_PROP_TYPE_ENUM || !def->enum_name) return NULL;
    *outEnumName = def->enum_name;
    if (e->enum_subset) return e->enum_subset;
    const OwlEnum* en = owl_registry_enum_find(def->enum_name);
    if (!en || !en->values) return NULL;
    return en->values;
}

void owl_presentation_emit_json(FILE* out) {
    bool first = true;
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!first) fprintf(out, ",\n");
        first = false;
        fprintf(out, "    {\"kind\":");
        pres_json_str(out, e->kind);
        fprintf(out, ",\"name\":");
        pres_json_str(out, e->prop);
        /* labelFR/labelEN : omis pour les entrees cache (NULL). Le front n'en
         * a besoin que pour les entrees designer, qu'il affiche. */
        if (e->labelFR) { fprintf(out, ",\"labelFR\":"); pres_json_str(out, e->labelFR); }
        if (e->labelEN) { fprintf(out, ",\"labelEN\":"); pres_json_str(out, e->labelEN); }
        fprintf(out, ",\"group\":");
        pres_json_str(out, owl_pres_group_name(e->groupe));
        fprintf(out, ",\"importance\":");
        pres_json_str(out, owl_pres_importance_name(e->importance));
        fprintf(out, ",\"editor\":");
        pres_json_str(out, owl_pres_editor_name(e->editor));
        fprintf(out, ",\"order\":%d", e->ordre);
        fprintf(out, ",\"visibility\":");
        pres_json_str(out, owl_pres_visibilite_name(e->visibilite));
        if (e->enum_subset) {
            fprintf(out, ",\"enum_subset\":[");
            bool firstV = true;
            for (const char* const* v = e->enum_subset; *v != NULL; v++) {
                if (!firstV) fputc(',', out);
                firstV = false;
                pres_json_str(out, *v);
            }
            fputc(']', out);
        }
        /* Libelles d'enum : derives par le transformateur (override-first) pour
         * toute prop enum, sur le sous-ensemble expose (ou toutes les valeurs
         * de l'enum si pas de subset). Emis seulement si au moins un libelle
         * apporte (sinon le front retombe sur la valeur brute lisible). */
        {
            const char* enumName = NULL;
            const char* const* vals = pres_entry_enum_values(e, &enumName);
            if (vals && enumName) pres_emit_enum_labels(out, enumName, vals);
        }
        /*  — radio 2-etats sur prop BOOL : libelles des deux options
         * (faux/vrai), bilingue. Le front rend un groupe radio libelle par le
         * titre (labelFR/labelEN) + ces deux options. */
        if (e->bool_radio_labels) {
            const OwlPresBoolRadioLabels* b = e->bool_radio_labels;
            fprintf(out, ",\"bool_radio_labels\":{\"falseFR\":");
            pres_json_str(out, b->falseFR);
            fprintf(out, ",\"trueFR\":");
            pres_json_str(out, b->trueFR);
            fprintf(out, ",\"falseEN\":");
            pres_json_str(out, b->falseEN);
            fprintf(out, ",\"trueEN\":");
            pres_json_str(out, b->trueEN);
            fputc('}', out);
        }
        /* Visibilite conditionnelle inter-propriete : le front ne rend cette
         * prop que si la prop controlante (meme kind) vaut cette valeur d'enum.
         * Emis seulement si renseigne (defaut : aucune condition). */
        if (e->visible_if_prop && e->visible_if_value) {
            fprintf(out, ",\"visible_if\":{\"prop\":");
            pres_json_str(out, e->visible_if_prop);
            fprintf(out, ",\"value\":");
            pres_json_str(out, e->visible_if_value);
            fputc('}', out);
        }
        fputc('}', out);
    }
}

void owl_presentation_leaves_emit_json(FILE* out) {
    /* Objet groupe par type composite : { "Police": [ {leaf, editor}, ... ] }.
     * Les entrees d'un meme composite sont contigues dans PRES_LEAVES. */
    bool firstComp = true;
    for (int i = 0; i < LEAF_COUNT; i++) {
        const OwlPresLeafEntry* l = &PRES_LEAVES[i];
        bool startGroup = (i == 0) ||
                          strcmp(PRES_LEAVES[i - 1].composite, l->composite) != 0;
        if (startGroup) {
            if (!firstComp) fprintf(out, "\n    ],\n");
            firstComp = false;
            fprintf(out, "    ");
            pres_json_str(out, l->composite);
            fprintf(out, ": [\n");
        } else {
            fprintf(out, ",\n");
        }
        fprintf(out, "      {\"leaf\":");
        pres_json_str(out, l->leaf);
        if (l->labelFR) { fprintf(out, ",\"labelFR\":"); pres_json_str(out, l->labelFR); }
        if (l->labelEN) { fprintf(out, ",\"labelEN\":"); pres_json_str(out, l->labelEN); }
        fprintf(out, ",\"editor\":");
        pres_json_str(out, owl_pres_editor_name(l->editor));
        /* Feuille enum : sous-ensemble de valeurs + libelles FR/EN paralleles.
         * Memes cles JSON que la couche presentation (enum_subset /
         * enum_subset_labels_fr / _en) pour que le front reutilise le meme
         * chemin de rendu que les props (ex TypeBouton). */
        if (l->enum_subset) {
            fprintf(out, ",\"enum_subset\":[");
            bool firstV = true;
            for (const char* const* v = l->enum_subset; *v; v++) {
                if (!firstV) fputc(',', out);
                firstV = false;
                pres_json_str(out, *v);
            }
            fputc(']', out);
        }
        /* Libelles de feuille enum : transformateur (override-first), meme
         * mecanisme que les props. Le nom d'enum est resolu depuis la premiere
         * valeur (reverse lookup registry) pour l'appariement des overrides. */
        if (l->enum_subset && l->enum_subset[0]) {
            const OwlEnum* owner = owl_registry_enum_owner_of_const(l->enum_subset[0]);
            const char* enumName = owner ? owner->name : NULL;
            if (enumName) pres_emit_enum_labels(out, enumName, l->enum_subset);
        }
        fputc('}', out);
    }
    if (!firstComp) fprintf(out, "\n    ]");
}

/* Palette d'etats d'un sous-element, resolue depuis le TYPE composite du
 * sous-element (ex "styleZoneBouton") : parmi ses champs, celui dont le type est
 * une collection porteuse d'etats (membre Etats -> styleZoneBoutonEtats).
 * Retourne l'index de palette (>=0) ou -1 si le type ne porte pas d'etats. */
static int pres_palette_index_for_subtype(const char* subType) {
    if (!subType) return -1;
    const OwlTypeDef* sub = owl_registry_type_find(subType);
    if (sub == NULL || sub->fields == NULL) return -1;
    for (const OwlField* g = sub->fields; g->name; g++) {
        int idx = compiler_style_palette_index_for_type(g->type);
        if (idx >= 0) return idx;
    }
    return -1;
}

/* Nom du type collection ..Etats d'un sous-element (le champ dont le type porte
 * des etats — widget via STYLE_PALETTES OU menu via MENU_ETATS_STATES), ou NULL
 * si le sous-element est statique. Source unique cote compiler.c (accesseurs
 * compiler_etats_type_*) : pas de duplication d'etats ici. */
static const char* pres_etats_type_for_subtype(const char* subType) {
    if (!subType) return NULL;
    const OwlTypeDef* sub = owl_registry_type_find(subType);
    if (sub == NULL || sub->fields == NULL) return NULL;
    for (const OwlField* g = sub->fields; g->name; g++)
        if (compiler_etats_type_state_count(g->type) > 0) return g->type;
    return NULL;
}

/* Emet le tableau JSON [ {fr,en}, ... ] des etats d'un type collection ..Etats
 * (vide si NULL / statique). Unifie widgets et menus via les accesseurs
 * compiler_etats_type_*. */
static void pres_emit_state_array(FILE* out, const char* etatsType) {
    fputc('[', out);
    int ns = etatsType ? compiler_etats_type_state_count(etatsType) : 0;
    bool firstS = true;
    for (int j = 0; j < ns; j++) {
        const char* fr = NULL;
        const char* en = NULL;
        if (!compiler_etats_type_state_at(etatsType, j, &fr, &en)) continue;
        if (!firstS) fprintf(out, ", ");
        firstS = false;
        fprintf(out, "{\"fr\":");
        pres_json_str(out, fr ? fr : "");
        fprintf(out, ",\"en\":");
        pres_json_str(out, en ? en : "");
        fputc('}', out);
    }
    fputc(']', out);
}

void owl_presentation_element_states_emit_json(FILE* out, bool byType) {
    /* Iteration depuis les types de style RACINE (compiler_style_model_*).
     * byType=true  : cle = TYPE composite du sous-element (ex "styleZoneBouton").
     *                Non-collisionnant : deux widgets partageant un nom de
     *                sous-element ("Libellé") ont des types distincts.
     * byType=false : cle = nom d'affichage du sous-element (legacy transition).
     *                Collision possible (le dernier ecrit gagne). */
    int nm = compiler_style_model_count();
    const char* seen[64];
    int seenN = 0;
    bool first = true;
    for (int i = 0; i < nm; i++) {
        const OwlTypeDef* root = owl_registry_type_find(compiler_style_model_type_at(i));
        if (root == NULL || root->fields == NULL) continue;
        for (const OwlField* f = root->fields; f->name; f++) {
            const char* key = byType
                ? f->type
                : (f->nameAccented ? f->nameAccented : f->name);
            if (!key) continue;
            bool dup = false;
            for (int s = 0; s < seenN; s++)
                if (strcmp(seen[s], key) == 0) { dup = true; break; }
            if (dup) continue;
            if (seenN < 64) seen[seenN++] = key;

            const char* etatsType = pres_etats_type_for_subtype(f->type);
            if (!first) fprintf(out, ",\n");
            first = false;
            fprintf(out, "    ");
            pres_json_str(out, key);
            fprintf(out, ": ");
            pres_emit_state_array(out, etatsType);
        }
    }
}

/*  — mapping racine de menu -> type de style. Table minimale : les deux
 * racines de menu non-materialisees et leur type de style canon. Le front resout
 * la structure via style_elements[styleType] (derivee du registre), exactement
 * comme il resout la prop Style d'un widget materialise. */
typedef struct {
    const char* rootFR;
    const char* rootEN;
    const char* styleType;
} OwlMenuStyleRoot;

static const OwlMenuStyleRoot MENU_STYLE_ROOTS[] = {
    { "MenuPrincipal",  "MainMenu",    "styleMenuPrincipal"  },
    { "MenuContextuel", "ContextMenu", "styleMenuContextuel" },
    /*  — Champ Onglet : le pont racine -> type de style, expose ici
     * depuis que le parser interceptait la prop Style en amont du catalogue.
     *  a rendu Onglet.Style OBJECT (styleOnglet) : l'entree reste comme
     * source stable pour les fronts qui lisent menu_style_roots. */
    { "Onglet",         "Tab",         "styleOnglet"         },
};

int owl_presentation_menu_style_root_count(void) {
    return (int)(sizeof(MENU_STYLE_ROOTS) / sizeof(MENU_STYLE_ROOTS[0]));
}

bool owl_presentation_menu_style_root_at(int idx, const char** outRootFR,
                                         const char** outRootEN,
                                         const char** outStyleType) {
    if (idx < 0 || idx >= owl_presentation_menu_style_root_count()) return false;
    const OwlMenuStyleRoot* r = &MENU_STYLE_ROOTS[idx];
    if (outRootFR)    *outRootFR    = r->rootFR;
    if (outRootEN)    *outRootEN    = r->rootEN;
    if (outStyleType) *outStyleType = r->styleType;
    return true;
}

int owl_presentation_self_test_run(FILE* out) {
    int violations = 0;

    /* (1) Toute entree de presentation pointe une prop reelle du catalogue. */
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!owl_presentation_kind_is_covered(e->kind)) continue;
        if (owl_dsl_prop_find(e->kind, e->prop) == NULL) {
            fprintf(out, "presentation: entree (%s.%s) ne correspond a aucune prop reelle du catalogue\n",
                    e->kind, e->prop);
            violations++;
        }
    }

    /* (2) Toute prop du catalogue (kind couvert) a une entree de presentation. */
    int dn = owl_dsl_props_count();
    for (int i = 0; i < dn; i++) {
        const OwlDSLPropEntry* d = owl_dsl_prop_at(i);
        if (!d || !d->def || !d->def->nameFR) continue;
        if (!owl_presentation_kind_is_covered(d->kind)) continue;
        if (owl_presentation_find(d->kind, d->def->nameFR) == NULL) {
            fprintf(out, "presentation: prop (%s.%s) sans entree de presentation couche 2\n",
                    d->kind, d->def->nameFR);
            violations++;
        }
    }

    /* (3) Feuilles de composite. Pour chaque composite EXPOSE (designer +
     *     editor composite) d'un kind couvert, toute feuille reelle du type
     *     composite a une entree de presentation feuille. */
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!owl_presentation_kind_is_covered(e->kind)) continue;
        if (e->editor != OWL_PRES_EDITOR_COMPOSITE) continue;
        if (e->visibilite != OWL_PRES_VIS_DESIGNER) continue;
        const OwlControlPropDef* def = owl_dsl_prop_find(e->kind, e->prop);
        const char* compType = def ? def->object_type : NULL;
        if (!compType) {
            fprintf(out, "presentation: composite expose (%s.%s) sans type composite cible (object_type)\n",
                    e->kind, e->prop);
            violations++;
            continue;
        }
        const ObjInstance* tpl = pres_composite_template(compType);
        if (!tpl) {
            fprintf(out, "presentation: type composite '%s' (%s.%s) introuvable dans les globals\n",
                    compType, e->kind, e->prop);
            violations++;
            continue;
        }
        for (int f = 0; f < tpl->fields.capacity; f++) {
            const ObjString* k = tpl->fields.entries[f].key;
            if (!k) continue;
            /* Garde de recursion : un champ dont la valeur est une instance du
             * MEME type composite que le parent est un sous-etat auto-referentiel
             * (ex : Survol/Focus de type styleSaisie dans styleSaisie). Ce sont
             * des surcharges d'etat, pas des sous-elements structurels — ils ne
             * s'exposent pas au designer comme des feuilles, donc pas d'entree
             * feuille exigee. Generalise a tout futur sous-etat auto-referentiel. */
            Value fv = tpl->fields.entries[f].value;
            if (IS_INSTANCE(fv)) {
                const ObjInstance* fi = AS_INSTANCE(fv);
                if (fi->structName && strcasecmp(fi->structName->chars, compType) == 0)
                    continue;
            }
            if (pres_leaf_find(compType, k->chars) == NULL) {
                fprintf(out, "presentation: feuille (%s.%s) sans entree de presentation feuille\n",
                        compType, k->chars);
                violations++;
            }
        }
    }

    /* (4) Toute entree feuille pointe une feuille reelle d'un type composite
     *     connu (orphan attrape). */
    for (int i = 0; i < LEAF_COUNT; i++) {
        const OwlPresLeafEntry* l = &PRES_LEAVES[i];
        const ObjInstance* tpl = pres_composite_template(l->composite);
        if (!tpl) {
            fprintf(out, "presentation: entree feuille (%s.%s) -> type composite inconnu\n",
                    l->composite, l->leaf);
            violations++;
            continue;
        }
        if (!pres_composite_has_leaf(tpl, l->leaf)) {
            fprintf(out, "presentation: entree feuille (%s.%s) ne correspond a aucune feuille reelle du type composite\n",
                    l->composite, l->leaf);
            violations++;
        }
    }

    /* (5) Libelles. Toute entree designer d'un kind couvert porte un labelFR ET
     *     un labelEN non-vides (invariant 3 : bilingue). Idem pour toute feuille
     *     d'un composite expose. Les entrees cache sont dispensees (NULL OK). */
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!owl_presentation_kind_is_covered(e->kind)) continue;
        if (e->visibilite != OWL_PRES_VIS_DESIGNER) continue;
        if (!e->labelFR || !*e->labelFR) {
            fprintf(out, "presentation: entree designer (%s.%s) sans labelFR non-vide\n",
                    e->kind, e->prop);
            violations++;
        }
        if (!e->labelEN || !*e->labelEN) {
            fprintf(out, "presentation: entree designer (%s.%s) sans labelEN non-vide\n",
                    e->kind, e->prop);
            violations++;
        }
    }
    for (int i = 0; i < LEAF_COUNT; i++) {
        const OwlPresLeafEntry* l = &PRES_LEAVES[i];
        if (!pres_composite_is_exposed(l->composite)) continue;
        if (!l->labelFR || !*l->labelFR) {
            fprintf(out, "presentation: feuille exposee (%s.%s) sans labelFR non-vide\n",
                    l->composite, l->leaf);
            violations++;
        }
        if (!l->labelEN || !*l->labelEN) {
            fprintf(out, "presentation: feuille exposee (%s.%s) sans labelEN non-vide\n",
                    l->composite, l->leaf);
            violations++;
        }
    }

    /* (6) Feuilles enum : les libelles sont derives par le transformateur
     *     (override-first) a l'emission — plus de tableaux statiques dans la
     *     feuille. Verrou de valeur exacte sur styleImageBouton.Position (source
     *     de verite = table d'overrides positionImageBouton) : les 6 valeurs
     *     canon et leurs libelles FR/EN sont figes, valides via le MEME chemin
     *     de resolution que l'emission (pres_resolve_label). Non-regression du
     *     rendu du dropdown de feuille. */
    {
        const OwlPresLeafEntry* pos = pres_leaf_find("styleImageBouton", "Position");
        if (!pos) {
            fprintf(out, "presentation: feuille styleImageBouton.Position introuvable\n");
            violations++;
        } else {
            static const char* const EXP_VALUES[] = {
                "posBtnImageGauche", "posBtnImageDroite", "posBtnImageHaut",
                "posBtnImageBas", "posBtnImageSeule", "posBtnTexteSeul", NULL
            };
            static const char* const EXP_FR[] = {
                "Gauche", "Droite", "Haut", "Bas", "Image seule", "Texte seul"
            };
            static const char* const EXP_EN[] = {
                "Left", "Right", "Top", "Bottom", "Icon only", "Text only"
            };
            const OwlEnum* owner = pos->enum_subset && pos->enum_subset[0]
                ? owl_registry_enum_owner_of_const(pos->enum_subset[0]) : NULL;
            bool okV = owner != NULL && pos->enum_subset != NULL;
            char fr[256], en[256];
            for (int k = 0; okV && EXP_VALUES[k]; k++) {
                if (!pos->enum_subset[k] || strcmp(pos->enum_subset[k], EXP_VALUES[k]) != 0) { okV = false; break; }
                pres_resolve_label(owner->name, pos->enum_subset, k, false, fr, sizeof(fr));
                pres_resolve_label(owner->name, pos->enum_subset, k, true,  en, sizeof(en));
                if (strcmp(fr, EXP_FR[k]) != 0 || strcmp(en, EXP_EN[k]) != 0) { okV = false; break; }
            }
            if (!okV) {
                fprintf(out, "presentation: feuille styleImageBouton.Position : valeurs/libelles enum attendus absents ou divergents\n");
                violations++;
            }
        }
    }

    /* (7) element_states keye par TYPE. Verrou anti-collision :
     *     l'iteration des sous-elements de tous les types racine, keyee par
     *     TYPE composite, ne doit produire aucune cle en double (sinon la
     *     collision de nom historique — deux "Libellé" — persiste). Verrou de
     *     presence : le sous-element bouton (styleZoneBouton) DOIT porter des
     *     etats (le keying par nom masquait le Bouton stateful sous le "Libellé"
     *     statique de la Saisie). */
    {
        int nm = compiler_style_model_count();
        const char* seenTypes[64];
        int seenN = 0;
        bool zoneBoutonHasStates = false;
        for (int i = 0; i < nm; i++) {
            const OwlTypeDef* root = owl_registry_type_find(compiler_style_model_type_at(i));
            if (root == NULL || root->fields == NULL) continue;
            for (const OwlField* f = root->fields; f->name; f++) {
                if (!f->type) continue;
                for (int s = 0; s < seenN; s++) {
                    if (strcmp(seenTypes[s], f->type) == 0) {
                        /* Deux racines exposent le meme TYPE de sous-element :
                         * legitime (feuilles identiques) — pas une violation. */
                        break;
                    }
                }
                bool known = false;
                for (int s = 0; s < seenN; s++)
                    if (strcmp(seenTypes[s], f->type) == 0) { known = true; break; }
                if (!known && seenN < 64) seenTypes[seenN++] = f->type;
                if (strcasecmp(f->type, "styleZoneBouton") == 0) {
                    if (pres_palette_index_for_subtype(f->type) >= 0)
                        zoneBoutonHasStates = true;
                }
            }
        }
        if (!zoneBoutonHasStates) {
            fprintf(out, "presentation: element_states par type — le sous-element bouton (styleZoneBouton) ne porte pas d'etats\n");
            violations++;
        }
    }

    /* (8) Verrou de valeur exacte sur l'enum curseurs (combo Curseur souris).
     *     Les 14 valeurs canon et leurs libelles FR/EN amis sont figes (source
     *     de verite = table d'overrides curseurs), valides via le MEME chemin de
     *     resolution que l'emission (pres_resolve_label). Garantit que le combo
     *     du designer affiche des libelles lisibles bilingues, pas les constantes
     *     brutes (curFleche, curTailleNESO, ...). */
    {
        const OwlEnum* cur = owl_registry_enum_find("curseurs");
        if (!cur || !cur->values) {
            fprintf(out, "presentation: enum curseurs introuvable\n");
            violations++;
        } else {
            static const char* const EXP_FR[] = {
                "Flèche", "Main", "Main système", "Texte", "Croix", "Sablier",
                "Aide", "Interdit", "Flèche haut", "Redimensionner",
                "Redim. vertical", "Redim. horizontal",
                "Redim. diagonale (NE-SO)", "Redim. diagonale (NO-SE)"
            };
            static const char* const EXP_EN[] = {
                "Arrow", "Pointing hand", "System hand", "Text", "Cross", "Busy",
                "Help", "Forbidden", "Up arrow", "Resize all", "Resize vertical",
                "Resize horizontal", "Resize diagonal (NE-SW)",
                "Resize diagonal (NW-SE)"
            };
            int nv = 0; for (const char* const* v = cur->values; *v; v++) nv++;
            char fr[256], en[256];
            bool ok = (nv == 14);
            for (int k = 0; ok && k < nv; k++) {
                pres_resolve_label("curseurs", cur->values, k, false, fr, sizeof(fr));
                pres_resolve_label("curseurs", cur->values, k, true,  en, sizeof(en));
                if (strcmp(fr, EXP_FR[k]) != 0 || strcmp(en, EXP_EN[k]) != 0) {
                    fprintf(out, "presentation: enum curseurs[%d]=%s -> FR '%s' / EN '%s' (attendu '%s' / '%s')\n",
                            k, cur->values[k], fr, en, EXP_FR[k], EXP_EN[k]);
                    ok = false;
                }
            }
            if (!ok) violations++;
        }
    }

    /* (9) Verrou de valeur exacte sur l'Etat radio des kinds a sous-ensemble
     *     {Actif, Inactif, Grisé} (Image, Interrupteur, Selecteur). Ces trois
     *     valeurs sont deja lisibles en FR (mono-mot), donc la derivation ne
     *     "gagne" sur aucune : sans libelle explicite, l'emission court-circuite
     *     et le front retombe sur les valeurs FR brutes (Actif/Inactif/Grisé),
     *     y compris en langue EN. Ce verrou teste l'EMISSION reelle
     *     (pres_emit_enum_labels), pas seulement la resolution unitaire :
     *     le gate `any` doit emettre les libelles EN (Active/Inactive/Grayed). */
    {
        static const char* const IMG_SUBSET[] = {
            "Actif", "Inactif", "Gris\xc3\xa9", NULL
        };
        static const char* const EXP =
            ",\"enum_subset_labels_fr\":[\"Actif\",\"Inactif\",\"Gris\xc3\xa9\"]"
            ",\"enum_subset_labels_en\":[\"Active\",\"Inactive\",\"Grayed\"]";
        FILE* tf = tmpfile();
        if (!tf) {
            fprintf(out, "presentation: tmpfile indisponible (verrou Etat image-subset)\n");
            violations++;
        } else {
            pres_emit_enum_labels(tf, "etat", IMG_SUBSET);
            long sz = ftell(tf);
            rewind(tf);
            char buf[512];
            size_t n = (sz > 0 && (size_t)sz < sizeof(buf))
                       ? fread(buf, 1, (size_t)sz, tf) : 0;
            buf[n] = '\0';
            if (strcmp(buf, EXP) != 0) {
                fprintf(out, "presentation: Etat image-subset (Image/Interrupteur/Selecteur) — libelles EN absents ou divergents : '%s' (attendu '%s')\n",
                        buf, EXP);
                violations++;
            }
            fclose(tf);
        }
    }

    /* (10) Couverture couche 2 des noeuds de menu editables. Verrou de valeur
     *     exacte : chaque genre enfant de menu (OptionMenu, SousMenu,
     *     MenuSéparateur) est couvert et expose ses props modelisees, classees
     *     dans le bon onglet (identite=Général, ui=UI), en editeur designer. Les
     *     racines MenuPrincipal/MenuContextuel n'ont pas de prop de noeud (hors
     *     perimetre). L'"Etat initial" radio (Etat, Actif/Inactif/Grisé) est
     *     modelise comme sur un widget ; Grisé est couvert par ce radio donc
     *     cache (visibility=cache, ), seul Visible reste un bool expose. */
    {
        static const struct { const char* kind; const char* prop; OwlPresGroup group; }
        MENU_EXPECTED[] = {
            { "OptionMenu",     "Libelle",   OWL_PRES_GROUP_IDENTITE },
            { "OptionMenu",     "Image",     OWL_PRES_GROUP_IDENTITE },
            { "OptionMenu",     "Visible",   OWL_PRES_GROUP_UI },
            { "OptionMenu",     "Etat",      OWL_PRES_GROUP_UI },
            { "OptionMenu",     "Raccourci", OWL_PRES_GROUP_UI },
            { "OptionMenu",     "Coche",     OWL_PRES_GROUP_UI },
            { "SousMenu",       "Libelle",   OWL_PRES_GROUP_IDENTITE },
            { "SousMenu",       "Visible",   OWL_PRES_GROUP_UI },
            { "SousMenu",       "Etat",      OWL_PRES_GROUP_UI },
            { "MenuSeparateur", "Visible",   OWL_PRES_GROUP_UI },
        };
        int nexp = (int)(sizeof(MENU_EXPECTED) / sizeof(MENU_EXPECTED[0]));
        for (int i = 0; i < nexp; i++) {
            const char* kind = MENU_EXPECTED[i].kind;
            const char* prop = MENU_EXPECTED[i].prop;
            if (!owl_presentation_kind_is_covered(kind)) {
                fprintf(out, "presentation: genre menu '%s' non couvert (COVERED_KINDS)\n", kind);
                violations++;
                continue;
            }
            const OwlPresEntry* e = owl_presentation_find(kind, prop);
            if (!e) {
                fprintf(out, "presentation: prop menu (%s.%s) sans entree couche 2\n", kind, prop);
                violations++;
                continue;
            }
            if (e->visibilite != OWL_PRES_VIS_DESIGNER) {
                fprintf(out, "presentation: prop menu (%s.%s) non exposee au designer (attendu editable)\n",
                        kind, prop);
                violations++;
            }
            if (e->groupe != MENU_EXPECTED[i].group) {
                fprintf(out, "presentation: prop menu (%s.%s) dans le groupe '%s' (attendu '%s')\n",
                        kind, prop, owl_pres_group_name(e->groupe),
                        owl_pres_group_name(MENU_EXPECTED[i].group));
                violations++;
            }
        }
        /*  — le bool Grise des noeuds menu doit etre CACHE (couvert par
         * le radio Etat), a parite des autres widgets. Verrou anti-retour au
         * designer : sinon doublon avec le radio dans l'inspecteur menu. */
        static const char* MENU_GRISE_CACHE[] = { "OptionMenu", "SousMenu", NULL };
        for (int i = 0; MENU_GRISE_CACHE[i]; i++) {
            const OwlPresEntry* e = owl_presentation_find(MENU_GRISE_CACHE[i], "Grise");
            if (!e) {
                fprintf(out, "presentation: %s.Grise sans entree couche 2 (fix/1246)\n",
                        MENU_GRISE_CACHE[i]);
                violations++;
            } else if (e->visibilite != OWL_PRES_VIS_CACHE) {
                fprintf(out, "presentation: %s.Grise doit etre visibility=cache (couvert par radio Etat, fix/1246)\n",
                        MENU_GRISE_CACHE[i]);
                violations++;
            }
        }
    }

    /* (11)  — editeurs riches promus en types de contrat couche 2.
     *     Les props source-image portent IMAGE_FILE (catalogue d'icones +
     *     fichier) et les props de raccourci clavier portent SHORTCUT (capture
     *     de frappe), pour que le front route PAR TYPE et non par nom de prop
     *     (un nouveau genre — menu — ratait l'editeur riche avec le routage par
     *     nom). Verrou de valeur exacte sur chaque prop concernee : widget,
     *     noeud de menu editable, et feuille de style (styleImageBouton.Image). */
    {
        static const struct { const char* kind; const char* prop; OwlPresEditor editor; }
        RICH_EDITOR_EXPECTED[] = {
            { "Bouton",       "Image",           OWL_PRES_EDITOR_IMAGE_FILE },
            { "Image",        "Image",           OWL_PRES_EDITOR_IMAGE_FILE },
            { "OptionMenu",   "Image",           OWL_PRES_EDITOR_IMAGE_FILE },
            { "Saisie",       "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Bouton",       "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Interrupteur", "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Selecteur",    "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Liste",        "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Combo",        "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "Table",        "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "ZoneRepetee",  "ToucheRaccourci", OWL_PRES_EDITOR_SHORTCUT },
            { "OptionMenu",   "Raccourci",       OWL_PRES_EDITOR_SHORTCUT },
        };
        int nrich = (int)(sizeof(RICH_EDITOR_EXPECTED) / sizeof(RICH_EDITOR_EXPECTED[0]));
        for (int i = 0; i < nrich; i++) {
            const char* kind = RICH_EDITOR_EXPECTED[i].kind;
            const char* prop = RICH_EDITOR_EXPECTED[i].prop;
            OwlPresEditor exp = RICH_EDITOR_EXPECTED[i].editor;
            const OwlPresEntry* e = owl_presentation_find(kind, prop);
            if (!e) {
                fprintf(out, "presentation: prop editeur riche (%s.%s) sans entree couche 2\n", kind, prop);
                violations++;
                continue;
            }
            if (e->editor != exp) {
                fprintf(out, "presentation: prop (%s.%s) en editeur '%s' (attendu '%s')\n",
                        kind, prop, owl_pres_editor_name(e->editor), owl_pres_editor_name(exp));
                violations++;
            }
        }
        /* Feuille de style : la source d'image du sous-element Image du Bouton
         * (styleImageBouton) doit aussi porter l'editeur riche. */
        bool leaf_seen = false;
        int ln = owl_presentation_leaf_count();
        for (int i = 0; i < ln; i++) {
            const OwlPresLeafEntry* l = owl_presentation_leaf_at(i);
            if (!l || strcmp(l->composite, "styleImageBouton") != 0
                   || strcmp(l->leaf, "Image") != 0) continue;
            leaf_seen = true;
            if (l->editor != OWL_PRES_EDITOR_IMAGE_FILE) {
                fprintf(out, "presentation: feuille de style (styleImageBouton.Image) en editeur '%s' (attendu 'imagefile')\n",
                        owl_pres_editor_name(l->editor));
                violations++;
            }
        }
        if (!leaf_seen) {
            fprintf(out, "presentation: feuille de style (styleImageBouton.Image) absente\n");
            violations++;
        }
    }

    /* (12) Couverture couche 2 du Champ Onglet et de ses volets.
     *     Verrou de valeur exacte : les props STRUCTURELLES editables au
     *     designer (Onglet Valeur en Contenu ; Position en disposition avec
     *     les autres reglages de la barre de languettes ; geometrie X/Y/
     *     Largeur/Hauteur/Visible en UI ; Étire/Alignement en disposition,
     *     en parite avec la Table ; Volet Libellé en identite,
     *     Disposition en disposition) sont couvertes, exposees au designer,
     *     dans le bon groupe et avec le bon editeur. Le style reste hors
     *     perimetre a ce stade (cache). */
    {
        static const struct { const char* kind; const char* prop; OwlPresGroup group; OwlPresEditor editor; }
        ONGLET_EXPECTED[] = {
            { "Onglet", "Position",    OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_ENUM },
            { "Onglet", "Valeur",      OWL_PRES_GROUP_CONTENU,     OWL_PRES_EDITOR_INT },
            { "Onglet", "X",           OWL_PRES_GROUP_UI,          OWL_PRES_EDITOR_POSITION },
            { "Onglet", "Y",           OWL_PRES_GROUP_UI,          OWL_PRES_EDITOR_POSITION },
            { "Onglet", "Largeur",     OWL_PRES_GROUP_UI,          OWL_PRES_EDITOR_INT },
            { "Onglet", "Hauteur",     OWL_PRES_GROUP_UI,          OWL_PRES_EDITOR_INT },
            { "Onglet", "Visible",     OWL_PRES_GROUP_UI,          OWL_PRES_EDITOR_BOOL },
            { "Onglet", "Etire",       OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_BOOL },
            { "Onglet", "Alignement",  OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_ENUM },
            { "Onglet", "LargeurVolets",     OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_RADIO },
            { "Onglet", "AlignementLibelle", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_RADIO },
            { "Volet",  "Libelle",     OWL_PRES_GROUP_IDENTITE,    OWL_PRES_EDITOR_MULTILANGUE },
            { "Volet",  "Disposition", OWL_PRES_GROUP_DISPOSITION, OWL_PRES_EDITOR_ENUM },
        };
        int nexp = (int)(sizeof(ONGLET_EXPECTED) / sizeof(ONGLET_EXPECTED[0]));
        for (int i = 0; i < nexp; i++) {
            const char* kind = ONGLET_EXPECTED[i].kind;
            const char* prop = ONGLET_EXPECTED[i].prop;
            if (!owl_presentation_kind_is_covered(kind)) {
                fprintf(out, "presentation: genre onglet '%s' non couvert (COVERED_KINDS)\n", kind);
                violations++;
                continue;
            }
            const OwlPresEntry* e = owl_presentation_find(kind, prop);
            if (!e) {
                fprintf(out, "presentation: prop onglet (%s.%s) sans entree couche 2\n", kind, prop);
                violations++;
                continue;
            }
            if (e->visibilite != OWL_PRES_VIS_DESIGNER) {
                fprintf(out, "presentation: prop onglet (%s.%s) non exposee au designer (attendu editable)\n",
                        kind, prop);
                violations++;
            }
            if (e->groupe != ONGLET_EXPECTED[i].group) {
                fprintf(out, "presentation: prop onglet (%s.%s) dans le groupe '%s' (attendu '%s')\n",
                        kind, prop, owl_pres_group_name(e->groupe),
                        owl_pres_group_name(ONGLET_EXPECTED[i].group));
                violations++;
            }
            if (e->editor != ONGLET_EXPECTED[i].editor) {
                fprintf(out, "presentation: prop onglet (%s.%s) en editeur '%s' (attendu '%s')\n",
                        kind, prop, owl_pres_editor_name(e->editor),
                        owl_pres_editor_name(ONGLET_EXPECTED[i].editor));
                violations++;
            }
        }
    }

    /* (13)  — visibilite conditionnelle inter-propriete (visible_if).
     *     Anti-typo : si une entree porte visible_if, la prop controlante doit
     *     etre une prop REELLE du meme kind, cette prop doit etre une ENUM, et
     *     la valeur attendue doit etre une valeur canon de cet enum. Un couple
     *     mal orthographie (prop inconnue ou valeur d'enum inexistante) casse le
     *     build plutot que de livrer une condition qui ne se declenche jamais. */
    for (int i = 0; i < PRES_COUNT; i++) {
        const OwlPresEntry* e = &PRES[i];
        if (!e->visible_if_prop && !e->visible_if_value) continue;  /* pas de condition */
        if (!e->visible_if_prop || !e->visible_if_value) {
            fprintf(out, "presentation: entree (%s.%s) visible_if incomplet (prop et value tous deux requis)\n",
                    e->kind, e->prop);
            violations++;
            continue;
        }
        const OwlControlPropDef* ctrl = owl_dsl_prop_find(e->kind, e->visible_if_prop);
        if (ctrl == NULL) {
            fprintf(out, "presentation: entree (%s.%s) visible_if.prop '%s' ne correspond a aucune prop du kind\n",
                    e->kind, e->prop, e->visible_if_prop);
            violations++;
            continue;
        }
        if (ctrl->enum_name == NULL) {
            fprintf(out, "presentation: entree (%s.%s) visible_if.prop '%s' n'est pas une enum (condition non decidable)\n",
                    e->kind, e->prop, e->visible_if_prop);
            violations++;
            continue;
        }
        const OwlEnum* en = owl_registry_enum_find(ctrl->enum_name);
        bool valueOk = false;
        if (en && en->values) {
            for (const char* const* v = en->values; *v != NULL; v++) {
                if (strcmp(*v, e->visible_if_value) == 0) { valueOk = true; break; }
            }
        }
        if (!valueOk) {
            fprintf(out, "presentation: entree (%s.%s) visible_if.value '%s' absente de l'enum '%s'\n",
                    e->kind, e->prop, e->visible_if_value, ctrl->enum_name);
            violations++;
        }
    }

    /* Garde ensemble vide : les boucles de ce controle parcourent
     * la table de presentation et le catalogue de feuilles. Une table vide les
     * fait boucler zero fois et rendre "0 violation" sans rien avoir verifie. */
    if (PRES_COUNT == 0 || LEAF_COUNT == 0) {
        fprintf(out, "presentation: CORPUS VIDE — %d entrees couche 2, %d feuilles. "
                     "Un controle qui n'examine rien ne prouve rien.\n",
                PRES_COUNT, LEAF_COUNT);
        return violations + 1;
    }

    if (violations == 0) {
        fprintf(out, "presentation: OK (%d entrees, %d feuilles, kinds couverts:",
                PRES_COUNT, LEAF_COUNT);
        for (const char* const* k = COVERED_KINDS; *k != NULL; k++)
            fprintf(out, "%s %s", (k == COVERED_KINDS) ? "" : ",", *k);
        fprintf(out, ")\n");
    }
    else
        fprintf(out, "presentation: %d violation(s) detectee(s)\n", violations);
    return violations;
}
