// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_catalog.c - Tables statiques + lookup des proprietes UI Tier 1A.
 *
 * Voir ui_catalog.h.
 */

#include "ui_catalog.h"
#include "owl_ui.h"
#include "owl_registry.h"
#include "owl_stdlib.h"
#include "vm.h"
#include "value.h"
#include "sentinels.h"
#include "event_catalog_widgets.h"   /* vue dedup events widget (evt*) */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

/* Defauts canon ( Veine Y) — raccourcis pour le champ .dft, place
 * en DERNIER dans OwlControlPropDef (apres object_type). Les macros sont
 * COMMA-PREFIXEES : a l'usage on ecrit toujours d'abord le slot object_type
 * (NULL pour une prop scalaire, "Cadre"/"Police" pour une prop composee)
 * PUIS le DFT, ex : `... descEN, NULL DFT_BOOL(1) }`. Une prop sans suffixe
 * DFT laisse .dft a zero (= OWL_PROP_DEFAULT_UNSET, sentinelle generique). */
#define DFT_NULL      , { OWL_PROP_DEFAULT_NULL,   0, NULL }
#define DFT_AUTO      , { OWL_PROP_DEFAULT_AUTO,   0, NULL }
#define DFT_BOOL(v)   , { OWL_PROP_DEFAULT_BOOL,  (v), NULL }
#define DFT_INT(v)    , { OWL_PROP_DEFAULT_INT,   (v), NULL }
#define DFT_ENUM(nm)  , { OWL_PROP_DEFAULT_ENUM,   0, (nm) }
#define DFT_STR(str)  , { OWL_PROP_DEFAULT_STRING, 0, (str) }

/*  — marque une prop comme etat runtime (lue/ecrite a l'execution,
 * sans semantique de design). Macro DESIGNEE comma-prefixee, appendue en fin
 * d'init APRES l'eventuel slot object_type + DFT, ex :
 *   `... descEN. " RUNTIME_ONLY }`            (prop sans defaut, UNSET)
 *   `... descEN.", NULL DFT_NULL RUNTIME_ONLY }`  (prop avec defaut)
 * Robuste a la position grace au designateur .runtime_only. Le designer
 * exclut ces props de l'inspector ; elles restent valides en code. */
#define RUNTIME_ONLY  , .runtime_only = true

/*  — marque une prop design-init : ecrivable dans le bloc PROPRIETES
 * (design) mais lecture seule en code imperatif. Macro DESIGNEE comma-prefixee,
 * appendue en fin d'init APRES l'eventuel slot object_type + DFT, ex :
 *   `... descEN, NULL DFT_NULL READONLY_RUNTIME }`
 * Le compilateur rejette `<widget>..<prop> = ...` hors PROPRIETES ; le
 * PROPRIETES reste autorise (control_parse_property_init ne consulte pas ce
 * flag). Inverse conceptuel de RUNTIME_ONLY. */
#define READONLY_RUNTIME  , .readonly_runtime = true

/*  — propriete universelle Note : emplacement texte libre par
 * instance, lisible et modifiable par programme sur la Fenetre et tous les
 * kinds de champ. Sert a stocker une donnee arbitraire propre a
 * l'application (contournement de limites, metadonnees maison). Type chaine,
 * defaut chaine vide. Definie ici (avant PROPS_WINDOW) car reutilisee dans
 * toutes les tables de kinds. Volontairement absente des noeuds de menu et
 * des sous-blocs Etat (catalogues dedies distincts). */
#define NOTE_PROP \
    { "Note",           NULL,     "Note",         OWL_PROP_TYPE_STRING, NULL, \
      "Emplacement de texte libre attache a l'element, lisible et modifiable " \
      "par programme. Sert a stocker une donnee arbitraire propre a " \
      "l'application. Vaut la chaine vide tant qu'aucune valeur n'est ecrite.", \
      "Free-text slot attached to the element, readable and writable from " \
      "code. Holds arbitrary application-specific data. Empty string until a " \
      "value is written.", NULL DFT_NULL }

const char* owl_prop_default_kind_name(OwlPropDefaultKind k) {
    switch (k) {
        case OWL_PROP_DEFAULT_UNSET:  return "unset";
        case OWL_PROP_DEFAULT_NULL:   return "null";
        case OWL_PROP_DEFAULT_AUTO:   return "auto";
        case OWL_PROP_DEFAULT_BOOL:   return "bool";
        case OWL_PROP_DEFAULT_INT:    return "int";
        case OWL_PROP_DEFAULT_ENUM:   return "enum";
        case OWL_PROP_DEFAULT_STRING: return "string";
    }
    return "unset";
}

/* ====================================================================
 *  Tables de proprietes par kind (sources : spec V1 §4)
 * ==================================================================== */

/* Lot F — props layout cote CONTENEUR (Fenetre, Conteneur). Definie ici
 * en tete de fichier car PROPS_WINDOW (ci-dessous) en depend.
 * Voir docs/reference/layouts.md pour la semantique complete. */
#define LAYOUT_CONTAINER_PROPS \
    { "Disposition",     NULL,     "Layout",       OWL_PROP_TYPE_ENUM, "disposition", \
      "Mode de layout des enfants (dispVerticale/Horizontale/Grille/Libre). " \
      "dispLibre est le seul mode autorisant X/Y absolu sur les enfants.", \
      "Children layout mode (dispVerticale/Horizontale/Grille/Libre). " \
      "dispLibre is the only mode allowing absolute X/Y on children.", \
      NULL DFT_ENUM("dispVerticale") }, \
    { "Padding",         NULL,     "Padding",      OWL_PROP_TYPE_INT,  NULL, \
      "Marge interieure uniforme du conteneur en pixels.", \
      "Uniform inner padding of the container in pixels.", NULL DFT_INT(0) }, \
    { "PaddingHaut",     NULL,     "PaddingTop",   OWL_PROP_TYPE_INT,  NULL, \
      "Marge interieure haut.",  "Top inner padding.", NULL DFT_INT(0) }, \
    { "PaddingBas",      NULL,     "PaddingBottom", OWL_PROP_TYPE_INT, NULL, \
      "Marge interieure bas.",   "Bottom inner padding.", NULL DFT_INT(0) }, \
    { "PaddingGauche",   NULL,     "PaddingLeft",  OWL_PROP_TYPE_INT,  NULL, \
      "Marge interieure gauche.", "Left inner padding.", NULL DFT_INT(0) }, \
    { "PaddingDroite",   NULL,     "PaddingRight", OWL_PROP_TYPE_INT,  NULL, \
      "Marge interieure droite.", "Right inner padding.", NULL DFT_INT(0) }, \
    { "Espacement",      NULL,     "Spacing",      OWL_PROP_TYPE_INT,  NULL, \
      "Espace en pixels entre enfants successifs (gap).", \
      "Pixel gap between consecutive children.", NULL DFT_INT(6) }, \
    { "AlignH",          NULL,     "AlignH",       OWL_PROP_TYPE_ENUM, "alignement", \
      "Alignement horizontal par defaut des enfants (override par leur prop Alignement).", \
      "Default horizontal alignment of children (overridden by child Alignement).", \
      NULL DFT_NULL }, \
    { "AlignV",          NULL,     "AlignV",       OWL_PROP_TYPE_ENUM, "alignement", \
      "Alignement vertical par defaut des enfants.", \
      "Default vertical alignment of children.", NULL DFT_NULL }, \
    { "Colonnes",        NULL,     "Columns",      OWL_PROP_TYPE_INT,  NULL, \
      "Nombre de colonnes en dispGrille (obligatoire si dispGrille).", \
      "Number of columns in dispGrille (mandatory for dispGrille).", NULL DFT_INT(1) }, \
    { "Lignes",          NULL,     "Rows",         OWL_PROP_TYPE_INT,  NULL, \
      "Nombre de lignes en dispGrille (optionnel, calcule sinon).", \
      "Number of rows in dispGrille (optional, computed otherwise)." }

/* Fenetre - 10 proprietes + LAYOUT_CONTAINER_PROPS (Lot F) */
static const OwlControlPropDef PROPS_WINDOW[] = {
    { "Titre",          NULL, "Title",          OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Titre dans la barre de titre. Accepte une chaine simple (coerce auto vers langue source) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Title bar text. Accepts a plain string (auto-coerced to source language) or an inline multilingual literal {langueXxx: \"...\"}." },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,     NULL,
      "Largeur de la fenetre en pixels.", "Window width in pixels.", NULL DFT_INT(640) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,     NULL,
      "Hauteur de la fenetre en pixels.", "Window height in pixels.", NULL DFT_INT(480) },
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,     NULL,
      "Position X de la fenetre. -1 = centree.", "X position. -1 = centered.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,     NULL,
      "Position Y de la fenetre. -1 = centree.", "Y position. -1 = centered.", NULL DFT_AUTO },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,    NULL,
      "Fenetre affichee.", "Window visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,    NULL,
      "Fenetre desactivee (overlay sombre + setEnabled).", "Window grayed out (dark overlay + setEnabled).", NULL DFT_BOOL(0) },
    { "Ic\xc3\xb4ne",   "Icone", "Icon",        OWL_PROP_TYPE_VARIANT, NULL,
      "Icone de la fenetre. Accepte un chemin de fichier (.ico/.png), une "
      "reference du catalogue (icones.X), ou un litteral inline teinte "
      "{icones.X, Couleur = ...}.",
      "Window icon. Accepts a file path (.ico/.png), a catalogue reference "
      "(icones.X), or an inline tinted literal {icones.X, Couleur = ...}.", NULL DFT_NULL },
    { "Redimensionnable", NULL, "Resizable",    OWL_PROP_TYPE_BOOL,    NULL,
      "Fenetre redimensionnable par l'utilisateur (defaut Vrai).",
      "User-resizable window (default true).", NULL DFT_BOOL(1) },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond de la fenetre (RVB entier). -1 = fond systeme par defaut.",
      "Window background color (RGB integer). -1 = default system background.", NULL DFT_NULL },
    /*  — arbre de menu classique applique a la fenetre. Dans le bloc
     * PROPRIETES, l'affectation est interceptee par le compilateur (choix de
     * bloc, pas valeur) ; cette entree ouvre la LECTURE et l'ECRITURE runtime
     * `FEN_X..MenuPrincipal = MNU_Autre`, qui rebatit la barre a chaud. */
    { "MenuPrincipal",  NULL, "MainMenu",       OWL_PROP_TYPE_VARIANT, NULL,
      "Arbre de menu affiche sous le titre de la fenetre. Recoit le nom d'un "
      "bloc MenuPrincipal declare dans la fenetre ; l'affecter au cours de "
      "l'execution remplace la barre de menu a chaud.",
      "Menu tree displayed under the window title. Takes the name of a "
      "MenuPrincipal block declared in the window; assigning it at run time "
      "replaces the menu bar on the fly.", NULL DFT_NULL },
    /*  — Style typé de la Fenetre (styleConteneur, reutilise le modele
     * du Champ Conteneur : un seul sous-element CadreExtérieur = fond + cadre +
     * retraits). Peint le panneau de la fenetre. Coexiste avec CouleurFond plat :
     * un fond pose par le Style prime sur CouleurFond. Cascade `FEN..Style..
     * CadreExtérieur..Prop` validee compile-time (mecanisme OBJECT prop). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé de la fenetre (styleConteneur). Sous-element : "
      "CadreExtérieur (le fond de la fenetre : couleur de fond, cadre, retraits).",
      "Typed style descriptor of the window (styleConteneur). Sub-element: "
      "OuterFrame (the window background: background color, border, paddings).",
      "styleConteneur" DFT_NULL },
    { "ValeurRenvoy\xc3\xa9" "e", "ValeurRenvoyee", "ReturnValue",
                                                OWL_PROP_TYPE_VARIANT, NULL,
      "Valeur retournee par Ouvre().", "Value returned by Ouvre()." },
    NOTE_PROP,
    LAYOUT_CONTAINER_PROPS,
};

/* Macro de reuse pour les 8 props Police (descripteur + 7 raccourcis sucre).
 * Le descripteur composé Police est défini dans stdlib_builtin_types.c
 * (canon). Les raccourcis PoliceNom/PoliceTaille/PoliceGras/
 * PoliceItalique/PoliceSoulignée/PoliceBarrée/PoliceCouleur écrivent
 * directement la sous-prop correspondante sans passer par Police..XXX. Les deux syntaxes
 * coexistent et sont équivalentes au runtime. PoliceNom a allowedBase=
 * iFamille → l'autocomplete LSP propose iPoliceLiberationSans/Arial/etc. */
#define FONT_PROPS \
    { "Police",         NULL,     "Font",         OWL_PROP_TYPE_OBJECT, NULL, \
      "Descripteur de police composé. Sous-champs : Nom, Taille, Gras, Italique, Souligné, Barré, Couleur. Equivalent aux raccourcis PoliceNom/PoliceTaille/etc.", \
      "Composite font descriptor. Sub-fields: Name, Size, Bold, Italic, Underlined, StrikeOut, Color. Equivalent to flat shortcuts PoliceNom/PoliceTaille/etc.", \
      "Police" DFT_NULL }, \
    { "PoliceNom",      NULL,     "FontName",     OWL_PROP_TYPE_STRING,  "iFamille", \
      "Nom de famille de police. Chaine libre (\"Arial\") ou constante iPolice* (iPoliceLiberationSans, iPoliceArial, ...). Equivalent à Police..Nom.", \
      "Font family name. Free string (\"Arial\") or iPolice* constant. Equivalent to Police..Nom.", \
      NULL DFT_NULL }, \
    { "PoliceTaille",   NULL,     "FontSize",     OWL_PROP_TYPE_INT,     NULL, \
      "Taille de la police en points typographiques. Equivalent à Police..Taille.", \
      "Font size in typographic points. Equivalent to Police..Size.", NULL DFT_INT(0) }, \
    { "PoliceGras",     NULL,     "FontBold",     OWL_PROP_TYPE_BOOL,    NULL, \
      "Texte en gras. Equivalent à Police..Gras.", "Bold text. Equivalent to Police..Bold.", \
      NULL DFT_BOOL(0) }, \
    { "PoliceItalique", NULL,     "FontItalic",   OWL_PROP_TYPE_BOOL,    NULL, \
      "Texte en italique. Equivalent à Police..Italique.", "Italic text. Equivalent to Police..Italic.", \
      NULL DFT_BOOL(0) }, \
    { "PoliceSoulign\xc3\xa9" "e", "PoliceSoulignee", "FontUnderlined", \
                                                   OWL_PROP_TYPE_BOOL,    NULL, \
      "Texte souligne. Equivalent à Police..Souligné.", "Underlined text. Equivalent to Police..Underlined.", \
      NULL DFT_BOOL(0) }, \
    { "PoliceBarr\xc3\xa9" "e",   "PoliceBarree",   "FontStrikeOut", \
                                                   OWL_PROP_TYPE_BOOL,    NULL, \
      "Texte barre. Equivalent à Police..Barré.", "Strikeout text. Equivalent to Police..StrikeOut.", \
      NULL DFT_BOOL(0) }, \
    { "PoliceCouleur",  NULL,     "FontColor",    OWL_PROP_TYPE_COLOR,   NULL, \
      "Couleur du texte (RGB entier). Equivalent à Police..Couleur.", \
      "Text color (RGB integer). Equivalent to Police..Color.", NULL DFT_NULL }

/*  — prop Groupe cote champ. Lecture : tableau de Groupe
 * collectant les Groupes globaux contenant ce control. Ecriture :
 * recompose l'appartenance (retire de tous, ajoute aux mentionnes).
 * Le runtime intercepte cette prop dans OP_STRUCT_GET/SET sur
 * IS_UI_CONTROL — la presence ici sert au LSP (completion, hover) et
 * a l'introspection registry. Type VARIANT : valeur = tableau de
 * Groupe (ObjArray non-group d'ObjArray is_group). Inclus dans tous
 * les kinds sauf Fenetre (une fenetre n'est pas un champ). */
#define GROUPE_PROP \
    { "Groupe",         NULL, "Group",         OWL_PROP_TYPE_VARIANT, NULL, \
      "Tableau des Groupes contenant ce champ. Lecture retourne la " \
      "liste actuelle, ecriture recompose l'appartenance.", \
      "Array of Groups containing this field. Read returns the current " \
      "list, write recomposes membership.", NULL DFT_NULL }

/*  — ToucheRaccourci sur les champs interactifs focusables (Saisie,
 * Combo, Liste, Selecteur, Interrupteur, Table, ZoneRepetee). Contrairement au
 * Bouton (dont le raccourci declenche le clic, cf. PROPS_BUTTON), ces widgets
 * n'ont pas d'action integree : le raccourci leur DONNE LE FOCUS. Pas
 * d'alternative '&' (ces champs n'ont pas de libelle integre). */
#define SHORTCUT_FOCUS_PROP \
    { "ToucheRaccourci", NULL, "ShortcutKey",   OWL_PROP_TYPE_STRING,   NULL, \
      "Raccourci clavier qui donne le focus au champ. Chaine : une touche " \
      "seule (\"F5\", \"n\") ou un ou plusieurs modificateurs suivis d'une " \
      "touche, relies par '+' : \"Alt+O\", \"Ctrl+S\", \"Shift+F1\", " \
      "\"Ctrl+Shift+T\".", \
      "Keyboard shortcut that gives focus to the field. String: a single key " \
      "(\"F5\", \"n\") or one or more modifiers followed by a key, joined " \
      "with '+': \"Alt+O\", \"Ctrl+S\", \"Shift+F1\", \"Ctrl+Shift+T\".", \
      NULL DFT_NULL }

/* Lot F — props layout cote ENFANT (widget place dans un conteneur).
 * Sans effet si parent en dispLibre. Voir docs/reference/layouts.md. */
#define LAYOUT_CHILD_PROPS \
    { "\xc3\x89tire",   "Etire",  "Stretch",      OWL_PROP_TYPE_BOOL, NULL, \
      "Le champ prend tout l'espace restant sur l'axe principal du conteneur (flex:1).", \
      "Control takes all remaining space along the container's main axis (flex:1).", \
      NULL DFT_BOOL(0) }, \
    { "Marge",           NULL,     "Margin",       OWL_PROP_TYPE_INT,  NULL, \
      "Marge externe uniforme en pixels (espace entre ce champ et ses voisins).", \
      "Uniform external margin in pixels (gap between this control and neighbors).", \
      NULL DFT_INT(0) }, \
    { "MargeHaut",       NULL,     "TopMargin",    OWL_PROP_TYPE_INT,  NULL, \
      "Marge externe haut (override de Marge).", "Top external margin (overrides Marge).", \
      NULL DFT_INT(0) }, \
    { "MargeBas",        NULL,     "BottomMargin", OWL_PROP_TYPE_INT,  NULL, \
      "Marge externe bas.",  "Bottom external margin.", NULL DFT_INT(0) }, \
    { "MargeGauche",     NULL,     "LeftMargin",   OWL_PROP_TYPE_INT,  NULL, \
      "Marge externe gauche.", "Left external margin.", NULL DFT_INT(0) }, \
    { "MargeDroite",     NULL,     "RightMargin",  OWL_PROP_TYPE_INT,  NULL, \
      "Marge externe droite.", "Right external margin.", NULL DFT_INT(0) }, \
    { "Alignement",      NULL,     "Alignment",    OWL_PROP_TYPE_ENUM, "alignement", \
      "Alignement transverse de ce champ dans son slot (alignDebut/Centre/Fin/Etire).", \
      "Cross-axis alignment of this control within its slot (alignDebut/Centre/Fin/Etire).", \
      NULL DFT_NULL }, \
    { "ColSpan",         NULL,     "ColumnSpan",   OWL_PROP_TYPE_INT,  NULL, \
      "Nombre de colonnes occupees en dispGrille (defaut 1).", \
      "Number of columns spanned in dispGrille (default 1).", NULL DFT_INT(1) }, \
    { "RowSpan",         NULL,     "RowSpan",      OWL_PROP_TYPE_INT,  NULL, \
      "Nombre de lignes occupees en dispGrille (defaut 1).", \
      "Number of rows spanned in dispGrille (default 1).", NULL DFT_INT(1) }, \
    { "LargeurMax",      NULL,     "MaxWidth",     OWL_PROP_TYPE_INT,  NULL, \
      "Largeur maximale en pixels (-1 = pas de limite).", \
      "Maximum width in pixels (-1 = no limit).", NULL DFT_INT(-1) }, \
    { "LargeurMin",      NULL,     "MinWidth",     OWL_PROP_TYPE_INT,  NULL, \
      "Largeur minimale en pixels (-1 = pas de limite).", \
      "Minimum width in pixels (-1 = no limit).", NULL DFT_INT(-1) }, \
    { "HauteurMax",      NULL,     "MaxHeight",    OWL_PROP_TYPE_INT,  NULL, \
      "Hauteur maximale en pixels (-1 = pas de limite).", \
      "Maximum height in pixels (-1 = no limit).", NULL DFT_INT(-1) }, \
    { "HauteurMin",      NULL,     "MinHeight",    OWL_PROP_TYPE_INT,  NULL, \
      "Hauteur minimale en pixels (-1 = pas de limite).", \
      "Minimum height in pixels (-1 = no limit).", NULL DFT_INT(-1) }

/* Libelle - 15 proprietes (9 + 6 Police) + LAYOUT_CHILD_PROPS (Lot F) */
static const OwlControlPropDef PROPS_LABEL[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels (depuis le coin haut-gauche du parent). -1 = auto-layout.",
      "X position in pixels (from parent top-left). -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte affiche. Accepte une chaine simple (coerce auto vers langue source) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Displayed text. Accepts a plain string (auto-coerced to source language) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "TypeLibell\xc3\xa9", "TypeLibelle", "LabelType",
                                                OWL_PROP_TYPE_ENUM,
                                                "typeLibelle",
      "Type d'affichage (TLNormal, TLMultiLigne).", "Display type.", NULL DFT_ENUM("TLNormal") },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte (RGB entier).", "Text color (RGB integer).", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond (RGB entier).", "Background color (RGB integer).", NULL DFT_NULL },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,
                                                "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "CadrageHorizontal", NULL, "HorizontalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageH",
      "Alignement horizontal (chGauche/chCentre/chDroite).",
      "Horizontal alignment (chGauche/chCentre/chDroite).", NULL DFT_NULL },
    { "CadrageVertical", NULL, "VerticalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageV",
      "Alignement vertical (cvHaut/cvCentre/cvBas).",
      "Vertical alignment (cvHaut/cvCentre/cvBas).", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre composé. Sous-champs : "
      "Couleur, Epaisseur, Trait, Arrondi, Bords + sous-objets per-bord "
      "(TraitHaut/Bas/Gauche/Droite) et per-coin (CoinHautGauche/Droite/BasGauche/Droite).",
      "Composite border descriptor. Sub-fields: "
      "Couleur, Epaisseur, Trait, Arrondi, Bords + per-side and per-corner sub-objects.",
      "Cadre" },
    { "Cadrage",        NULL, "Framing",        OWL_PROP_TYPE_ENUM, "cadrage",
      "Mode de cadrage de l'image dans le libelle "
      "(cadCentre/cadEtire/cadHomothetique).",
      "Image framing mode in the label "
      "(cadCentre/cadEtire/cadHomothetique).", NULL DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    /* Lot A — props canon (proprietes Libelle Fenetre). */
    { "AffichageActif", NULL, "DisplayEnabled", OWL_PROP_TYPE_BOOL, NULL,
      "Active le rafraichissement du champ. Le moteur d'affichage batche deja les repaints, "
      "utile principalement pour les boucles intensives.",
      "Toggles control repainting. The display engine already batches repaints — primarily "
      "useful for tight modification loops.", NULL DFT_BOOL(1) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre (entier). Plus haut = au-dessus.",
      "Intra-window z-order (integer). Higher = on top.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol (curFleche, curMain, curSablier, ...).",
      "Hover cursor shape (curFleche, curMain, curSablier, ...).", NULL DFT_NULL },
    { "Ellipse",        NULL, "Ellipsis",       OWL_PROP_TYPE_ENUM, "ellipse",
      "Mode de gestion du texte trop long (ellipseInactive/Fin/Chemin).",
      "Overflow handling for long text (ellipseInactive/Fin/Chemin).", NULL DFT_NULL },
    /* LargeurMin/Max + HauteurMin/Max sont dans LAYOUT_CHILD_PROPS (Lot F). */
    { "Message",        NULL, "StatusTip",      OWL_PROP_TYPE_STRING, NULL,
      "Texte d'aide affiche dans la barre de statut au survol.",
      "Help text shown in the status bar on hover.", NULL DFT_NULL },
    { "SensEcriture",   NULL, "TextDirection",  OWL_PROP_TYPE_ENUM, "sensEcriture",
      "Sens d'ecriture (sensDefaut/sensGaucheDroite/sensDroiteGauche).",
      "Text direction (sensDefaut/sensGaucheDroite/sensDroiteGauche).", NULL DFT_NULL },
    /* Etirement horizontal des caracteres — reglage unique deterministe
     * (etirNormal par defaut). Un champ a un seul etirement, pas un cumul. */
    { "Police\xc3\x89tirement", "PoliceEtirement", "FontStretch",
                                                OWL_PROP_TYPE_ENUM, "Police\xc3\x89tirement",
      "Etirement horizontal des caracteres (etirNormal/etirCondense/etirEtendu/etirLarge).",
      "Horizontal character stretch (etirNormal/etirCondense/etirEtendu/etirLarge).", NULL DFT_NULL },
    { "PoliceTailleMin", NULL, "FontMinSize",   OWL_PROP_TYPE_INT,  NULL,
      "Taille minimale en points si 'Reduire la police' active (fit initial à ce jour).",
      "Minimum point size when 'Shrink font' is enabled (currently initial fit only).", NULL DFT_NULL },
    /* Lot C — marges interieures (canon).
     * Espace en pixels entre le bord du widget et le texte. Mappe sur
     * QWidget::contentsMargins (left/top/right/bottom).
     * EN = *Indent (LeftIndent/...), DISTINCT des marges EXTERNES Marge*→*Margin
     * (layout enfant). NE PAS re-unifier vers *Margin : le nom EN doit rester
     * 1:1 sinon `owl translate` EN->FR ne peut plus departager (collision). */
    { "RetraitGauche",   NULL, "LeftIndent",    OWL_PROP_TYPE_INT,  NULL,
      "Espace en pixels a gauche du texte (marge interieure).",
      "Pixels of inner padding to the left of the text.", NULL DFT_NULL },
    { "RetraitDroit",    NULL, "RightIndent",   OWL_PROP_TYPE_INT,  NULL,
      "Espace en pixels a droite du texte (marge interieure).",
      "Pixels of inner padding to the right of the text.", NULL DFT_NULL },
    { "RetraitHaut",     NULL, "TopIndent",     OWL_PROP_TYPE_INT,  NULL,
      "Espace en pixels au-dessus du texte (marge interieure).",
      "Pixels of inner padding above the text.", NULL DFT_NULL },
    { "RetraitBas",      NULL, "BottomIndent",  OWL_PROP_TYPE_INT,  NULL,
      "Espace en pixels en-dessous du texte (marge interieure).",
      "Pixels of inner padding below the text.", NULL DFT_NULL },
    /* Lot E — formatAffichage (canon). 'formatTexte' (defaut) = */
    /* literal ; 'formatTexteRiche' = Markdown OWL etendu interprete. */
    { "FormatAffichage", NULL, "DisplayFormat", OWL_PROP_TYPE_ENUM, "formatAffichage",
      "Format d'interpretation du contenu Libelle (formatTexte ou formatTexteRiche).",
      "Display format for label content (formatTexte or formatTexteRiche).", NULL DFT_ENUM("formatTexte") },
    /*  — Style typé du Champ Libellé (styleLibellé). Struct de deux
     * sous-elements : Libellé (le texte du champ) et CadreExtérieur (le
     * conteneur). La cascade `LIB..Style..SousElement..Prop` est validee
     * compile-time contre les champs de styleLibellé (mecanisme OBJECT prop,
     * cf Style sur le Saisie). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleLibellé). Sous-elements : "
      "Libellé (le texte, statique) et CadreExtérieur (le conteneur, statique).",
      "Typed style descriptor of the field (styleLibellé). Sub-elements: "
      "Label (the text, static) and OuterFrame (the container, static).",
      "styleLibell\xc3\xa9" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Bouton - 18 proprietes (11 + 6 Police + Opacite) */
static const OwlControlPropDef PROPS_BUTTON[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(30) },
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte du bouton. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Button caption. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "TypeBouton",     NULL, "ButtonType",     OWL_PROP_TYPE_ENUM,
                                                "typeBouton",
      "Type (btnNormal, btnValider, btnAbandon, ...).", "Button type.", NULL DFT_ENUM("btnNormal") },
    { "ToucheRaccourci", NULL, "ShortcutKey",   OWL_PROP_TYPE_STRING,   NULL,
      "Raccourci clavier active le bouton. Chaine : une touche seule "
      "(\"F5\", \"n\") ou un ou plusieurs modificateurs suivis d'une touche, "
      "relies par '+' : \"Alt+O\", \"Ctrl+S\", \"Shift+F1\", \"Ctrl+Shift+T\". "
      "Alternative sans cette propriete : placer '&' devant une lettre du "
      "Libelle (par ex. \"&Ouvrir\") souligne cette lettre et l'active via "
      "Alt+lettre.",
      "Keyboard shortcut that triggers the button. String: a single key "
      "(\"F5\", \"n\") or one or more modifiers followed by a key, joined "
      "with '+': \"Alt+O\", \"Ctrl+S\", \"Shift+F1\", \"Ctrl+Shift+T\". "
      "Alternative without this property: put '&' before a letter of the "
      "Caption (e.g. \"&Open\") to underline that letter and trigger it "
      "with Alt+letter.", NULL DFT_NULL },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Image",          NULL, "Image",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Icone du bouton. Accepte un chemin de fichier (chaine) ou une "
      "instance Image en memoire.",
      "Button icon. Accepts a file path (string) or an in-memory Image "
      "instance.", NULL DFT_NULL },
    { "ImageTaille",    NULL, "ImageSize",      OWL_PROP_TYPE_INT,      NULL,
      "Taille de l'icone en pixels. 0 ou absent : l'icone s'adapte a la "
      "hauteur du bouton. Valeur positive : taille forcee.",
      "Icon size in pixels. 0 or absent: the icon adapts to the button "
      "height. Positive value: forced size.", NULL DFT_AUTO },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond (RGB entier).", "Background color (RGB integer).", NULL DFT_NULL },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,
                                                "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol (curFleche, curMain, curSablier, ...).",
      "Hover cursor shape (curFleche, curMain, curSablier, ...).", NULL DFT_NULL },
    /* ,  — Style typé du Champ Bouton (styleBouton). Struct de
     * trois sous-elements : Libellé (le texte, brique interactive stateful),
     * Image (l'icone : Fichier + Position) et CadreExtérieur (le conteneur). La
     * cascade `BTN..Style..SousElement..Prop` est validee compile-time contre
     * les champs de styleBouton (mecanisme OBJECT prop, cf Style sur le
     * Saisie/Libellé). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleBouton). Sous-elements : "
      "Libellé (le texte, avec etats via ..Etats[survol/appuyé/focus/grisé]), "
      "Image (l'icône : Fichier + Position) et CadreExtérieur (le conteneur, statique).",
      "Typed style descriptor of the field (styleBouton). Sub-elements: "
      "Caption (the text, with states via ..Etats[hover/pressed/focus/grayed]), "
      "Image (the icon: File + Position) and OuterFrame (the static container).",
      "styleBouton" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Saisie - 18+ proprietes */
static const OwlControlPropDef PROPS_INPUT[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(200) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Contenu saisi (propriete par defaut). 'Saisie = X' equivaut a 'Saisie..Valeur = X'. "
      "Accepte une chaine simple (figee a la langue courante) ou un litteral inline "
      "multilingue par langue (retenu, re-resolu au changement de langue).",
      "Input content (default property). 'Input = X' is equivalent to 'Input..Value = X'. "
      "Accepts a plain string (frozen to the current language) or an inline per-language "
      "multilingual literal (retained, re-resolved on language change).", NULL DFT_NULL RUNTIME_ONLY },
    { "ContenuInitial", NULL, "InitialContent", OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Contenu initial defini en design (pose comme valeur de depart du champ). "
      "Accepte une chaine simple ou un litteral inline multilingue par langue.",
      "Initial design-time content (used as the field's starting value). "
      "Accepts a plain string or an inline per-language multilingual literal.", NULL DFT_NULL READONLY_RUNTIME },
    { "TypeSaisie",     NULL, "InputType",      OWL_PROP_TYPE_ENUM,
                                                "typeSaisie",
      "Type (typSaisieTexte, typSaisieMotDePasse).", "Input type.", NULL DFT_ENUM("typSaisieTexte") },
    { "Indication",     NULL, "Placeholder",    OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte indicatif (placeholder). Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Placeholder text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "MaxLongueur",    NULL, "MaxLength",      OWL_PROP_TYPE_INT,      NULL,
      "Longueur max (0 = illimite).", "Max length (0 = unlimited).", NULL DFT_INT(0) },
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Libelle integre affiche a gauche de la zone de saisie. Accepte une chaine simple "
      "(coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Integrated label shown to the left of the input area. Accepts a plain string "
      "(auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "LargeurLibell\xc3\xa9", "LargeurLibelle", "LabelWidth", OWL_PROP_TYPE_INT, NULL,
      "Largeur en pixels de la zone du libelle integre.",
      "Width in pixels of the integrated label area.", NULL DFT_INT(0) },
    { "Curseur",        NULL, "Cursor",         OWL_PROP_TYPE_INT,      NULL,
      "Position du curseur (1-based, debut de selection).",
      "Cursor position (1-based, selection start)." RUNTIME_ONLY },
    { "FinCurseur",     NULL, "EndCursor",      OWL_PROP_TYPE_INT,      NULL,
      "Position fin de selection (1-based).",
      "Selection end (1-based)." RUNTIME_ONLY },
    { "TexteS\xc3\xa9lectionn\xc3\xa9", "TexteSelectionne", "SelectedText",
                                                OWL_PROP_TYPE_STRING,   NULL,
      "Texte selectionne (lecture/ecriture).",
      "Selected text (read/write)." RUNTIME_ONLY },
    { "BorneMin",       NULL, "MinValue",       OWL_PROP_TYPE_INT,      NULL,
      "Valeur minimale acceptee (borne basse du validateur d'un champ numerique/monetaire).",
      "Min accepted value (lower bound of a numeric/currency field validator)." },
    { "BorneMax",       NULL, "MaxValue",       OWL_PROP_TYPE_INT,      NULL,
      "Valeur maximale acceptee (borne haute du validateur d'un champ numerique/monetaire).",
      "Max accepted value (upper bound of a numeric/currency field validator)." },
    { "Modifi\xc3\xa9", "Modifie", "Modified",   OWL_PROP_TYPE_BOOL,    NULL,
      "Vrai si l'utilisateur a touche le champ depuis init/setText.",
      "True if user has touched the field since init/setText." RUNTIME_ONLY },
    { "MasqueSaisie",   NULL, "InputMask",      OWL_PROP_TYPE_STRING,   NULL,
      "Masque de saisie a positions fixes : `9` = un chiffre, `A` = une lettre, `C` = une lettre ou un chiffre ; tout autre caractere est un litteral affiche tel quel (ex. \"99/99/9999\", \"+99 999 99 99 99\").",
      "Fixed-position input mask: `9` = a digit, `A` = a letter, `C` = a letter or a digit; any other character is a literal shown as-is (e.g. \"99/99/9999\", \"+99 999 99 99 99\").", NULL DFT_NULL },
    { "MasqueAffichage", NULL, "DisplayMask",   OWL_PROP_TYPE_STRING,   NULL,
      "Masque d'affichage d'un champ Date : gabarit de jetons (JJ, MM, AAAA...) pilotant le format affiche et relu (ex. \"AAAA-MM-JJ\", \"JJ/MM/AAAA\"). Vide = format par defaut JJ/MM/AAAA.",
      "Display mask of a Date field: token template (JJ, MM, AAAA...) driving the shown and re-read format (e.g. \"AAAA-MM-JJ\", \"JJ/MM/AAAA\"). Empty = default JJ/MM/AAAA format.", NULL DFT_NULL },
    { "FormatNombre",   NULL, "NumberFormat",   OWL_PROP_TYPE_OBJECT,   NULL,
      "Format d'affichage d'un champ Numerique : descripteur compose (SeparateurDecimal, SeparateurMilliers, ChiffresApresVirgule), litteral inline `{ ... }`. Non pose = herite du FormatNombre du projet, sinon affichage plat.",
      "Numeric field display format: composite descriptor (DecimalSeparator, ThousandsSeparator, DecimalPlaces), inline literal `{ ... }`. Unset = inherits the project NumberFormat, otherwise plain display.",
      "FormatNombre" DFT_NULL },
    { "FormatMon\xc3\xa9taire", "FormatMonetaire", "CurrencyFormat", OWL_PROP_TYPE_OBJECT, NULL,
      "Format d'affichage d'un champ Monetaire : descripteur compose (Symbole, PositionSymbole, SeparateurDecimal, SeparateurMilliers, ChiffresApresVirgule, FormatNegatif), litteral inline `{ ... }`. Non pose = herite du FormatMonetaire du projet, sinon affichage plat.",
      "Currency field display format: composite descriptor (Symbol, SymbolPosition, DecimalSeparator, ThousandsSeparator, DecimalPlaces, NegativeFormat), inline literal `{ ... }`. Unset = inherits the project CurrencyFormat, otherwise plain display.",
      "FormatMon\xc3\xa9taire" DFT_NULL },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    /* Defaut Actif : un champ de saisie est actif a la creation. Metadata de
     * design (export/doc/designer) ; le runtime resout l'etat via
     * owl_ui_headless_default (base sur le nom de prop), independant de .dft. */
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,
                                                "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "CadrageHorizontal", NULL, "HorizontalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageH",
      "Alignement horizontal (chGauche/chCentre/chDroite).",
      "Horizontal alignment.", NULL DFT_NULL },
    { "CadrageVertical", NULL, "VerticalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageV",
      "Alignement vertical (cvHaut/cvCentre/cvBas).",
      "Vertical alignment.", NULL DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    { "FormatAffichage", NULL, "DisplayFormat", OWL_PROP_TYPE_ENUM, "formatAffichage",
      "Format d'interpretation du contenu Saisie (formatTexte ou formatTexteRiche).",
      "Display format for input content (formatTexte or formatTexteRiche).", NULL DFT_ENUM("formatTexte") },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol (curFleche, curMain, curSablier, ...).",
      "Hover cursor shape (curFleche, curMain, curSablier, ...).", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,  NULL,
      "Couleur de fond du champ (RVB entier). -1 = fond systeme par defaut.",
      "Input background color (RGB integer). -1 = default system background.", NULL DFT_NULL },
    /* /0833 — Style typé du champ Saisie (styleSaisie). Premiere brique
     * du modele de style par widget : agrege Couleur/CouleurFond/Retraits +
     * Police + Cadre. Les etats visuels (survol souris, focus clavier)
     * s'ecrivent via la syntaxe Style[survol] / Style[focus]. La cascade
     * `SAI..Style..SousChamp` est validee compile-time contre les champs de
     * styleSaisie (mecanisme OBJECT prop, cf Cadre sur Libellé). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleSaisie). Sous-champs : "
      "Couleur, CouleurFond, RetraitGauche/Droit/Haut/Bas, Police (composé), "
      "Cadre (composé). Etats visuels via Style[survol] / Style[focus].",
      "Typed style descriptor of the field (styleSaisie). Sub-fields: "
      "Color, BackgroundColor, LeftPadding/RightPadding/TopPadding/BottomPadding, "
      "Font (composed), Border (composed). Visual states via Style[hover] / Style[focus].",
      "styleSaisie" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Table - Phase 0 : seulement geometrie + visible/grise/bulle.
 * Phases ulterieures ajouteront Tri, MultiSelection, BarreFiltre, etc. */
static const OwlControlPropDef PROPS_TABLE[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(600) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(300) },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Libelle du champ. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Field caption. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM, "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible / AffichageSansSélection). Inactif est un synonyme d'AffichageSeulement. AffichageSansSélection est propre au Champ Table : la sélection utilisateur est désactivée, la sélection par programmation reste possible.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible / DisplayWithoutSelection). Inactive is a synonym for DisplayOnly. DisplayWithoutSelection is specific to the Table field: user selection is disabled, programmatic selection stays possible.", NULL DFT_ENUM("Actif") },
    FONT_PROPS,
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol (curFleche, curMain, curSablier, ...).",
      "Hover cursor shape (curFleche, curMain, curSablier, ...).", NULL DFT_NULL },
    { "Orientation",    NULL, "Orientation",    OWL_PROP_TYPE_ENUM, "orientationsTable",
      "Orientation de disposition du tableau (Vertical : lignes empilees).",
      "Table layout orientation (Vertical: stacked rows).", NULL DFT_ENUM("Vertical") },
    { "EtatInitial",    NULL, "InitialState",   OWL_PROP_TYPE_ENUM, "etatsTableInitial",
      "Etat initial du tableau (EnSaisie / Grisé / Sélection / NiSaisieNiSélection).",
      "Initial table state (EnSaisie / Grisé / Sélection / NiSaisieNiSélection).", NULL DFT_ENUM("EnSaisie") },
    { "ModeLoupe",      NULL, "MagnifierMode",  OWL_PROP_TYPE_ENUM, "modeLoupe",
      "Mode de gestion de la loupe du titre de colonne : rechercheCommencePar "
      "(défaut : trie la colonne et positionne sur la 1re ligne « commence par », "
      "sans masquer) ou filtreContient (masque les lignes hors motif).",
      "Magnifier handling mode of the column title: rechercheCommencePar (default: "
      "sorts the column and positions on the first « starts with » row, without "
      "hiding) or filtreContient (hides rows outside the pattern).",
      NULL DFT_ENUM("rechercheCommencePar") RUNTIME_ONLY },
    /*  — menu de la loupe (clic droit sur l'en-tete de colonne).
     * L'affectation `MenuLoupe = MNU_X` est interceptee par le compilateur
     * (choix d'un bloc MenuContextuel, pas une valeur) ; cette entree publie la
     * propriete au catalogue (completion, hover, forme canon au fmt). Attache =
     * remplace le menu loupe integre. Dans une option, MoiMeme designe la
     * colonne cliquee : les natifs de filtre (TableActiveFiltre...) s'y branchent
     * directement. */
    { "MenuLoupe",      NULL, "MagnifierMenu",  OWL_PROP_TYPE_VARIANT, NULL,
      "Menu affiche au clic droit sur l'en-tete d'une colonne (la « loupe »). "
      "Recoit le nom d'un bloc MenuContextuel declare dans la fenetre ; l'attacher "
      "remplace le menu integre de la loupe. Dans une option, MoiMeme designe la "
      "colonne cliquee.",
      "Menu shown on right-click over a column header (the « magnifier »). Takes "
      "the name of a MenuContextuel block declared in the window; attaching it "
      "replaces the built-in magnifier menu. Within an option, MySelf refers to "
      "the clicked column.", NULL DFT_NULL },
    { "ColonneExtensible", NULL, "StretchColumn", OWL_PROP_TYPE_STRING, NULL,
      "Nom de la colonne qui s'étire pour occuper l'espace horizontal restant. "
      "Vide = la dernière colonne s'étire par défaut.",
      "Name of the column that stretches to fill the remaining horizontal space. "
      "Empty = the last column stretches by default.", NULL DFT_NULL },
    { "HauteurLigne",   NULL, "RowHeight",      OWL_PROP_TYPE_INT,      NULL,
      "Hauteur d'une ligne en pixels (0 = hauteur naturelle).",
      "Row height in pixels (0 = natural height).", NULL DFT_INT(0) },
    { "HauteurTitre",   NULL, "TitleHeight",    OWL_PROP_TYPE_INT,      NULL,
      "Hauteur de la bande de titre des colonnes en pixels. 0 masque "
      "l'en-tête et rend la place à la zone de données. -1 = hauteur "
      "naturelle.",
      "Height of the column title band in pixels. 0 hides the header and "
      "gives the space back to the data area. -1 = natural height.",
      NULL DFT_AUTO },
    /*  — Style typé du Champ Table (styleTable). Struct de sous-elements :
     * CadreExtérieur, TitreColonne, LibelléLignes, LignesPaires, LignesImpaires,
     * LigneSélectionnée, LigneSurvolée, Quadrillage. Cascade `TAB..Style..X..Y`
     * validee compile-time contre les champs de styleTable (mecanisme OBJECT). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleTable). La police et la couleur "
      "globales du champ servent de base au rendu des cellules et en-têtes ; ce "
      "style les surcharge par sous-élément (Style..TitreColonne..Police, "
      "Style..LibelléLignes..Couleur…). Les deux coexistent. "
      "Sous-elements : CadreExtérieur, TitreColonne, LibelléLignes, LignesPaires, "
      "LignesImpaires, LigneSélectionnée, LigneSurvolée, Quadrillage.",
      "Typed style descriptor of the field (styleTable). The field's global font "
      "and color serve as the base for rendering cells and headers; this style "
      "overrides them per sub-element (Style..ColumnTitle..Font, "
      "Style..RowLabel..Color…). Both coexist. Sub-elements: "
      "OuterFrame, ColumnTitle, RowLabel, EvenRows, OddRows, "
      "SelectedRow, HoveredRow, Gridlines.",
      "styleTable" DFT_NULL },
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Conteneur (Lot F) — boite generique pour layout par nesting. */
static const OwlControlPropDef PROPS_CONTAINER[] = {
    /* Geometrie : le Conteneur est une boite de layout, sa taille derive
     * du moteur (children/disposition). build_control est encore un stub
     * pour ce kind (Lot F.3) — pas de littéral concret applique, on declare
     * AUTO conformement a la semantique declaree (-1 = layout-derived). */
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X (uniquement valide si parent en dispLibre).",
      "X position (only valid if parent is dispLibre).", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y (uniquement valide si parent en dispLibre).",
      "Y position (only valid if parent is dispLibre).", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels (-1 = derive du layout).",
      "Width in pixels (-1 = layout-derived).", NULL DFT_AUTO },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_AUTO },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Conteneur affiche.", "Container visible.", NULL DFT_BOOL(1) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,      NULL,
      "Z-order intra-parent.", "Intra-parent z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol (curFleche, curMain, curSablier, ...).",
      "Hover cursor shape (curFleche, curMain, curSablier, ...).", NULL DFT_NULL },
    /*  — Style typé du Champ Conteneur (styleConteneur). Un seul
     * sous-element : CadreExtérieur (la boite : CouleurFond + Cadre + Retraits).
     * La cascade `CNT..Style..CadreExtérieur..Prop` est validee compile-time
     * contre les champs de styleConteneur (mecanisme OBJECT prop, cf Style sur
     * le Champ Image). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du conteneur (styleConteneur). Sous-element : "
      "CadreExtérieur (la boite : couleur de fond, cadre, retraits, statique).",
      "Typed style descriptor of the container (styleConteneur). Sub-element: "
      "OuterFrame (the box: background color, border, paddings, static).",
      "styleConteneur" DFT_NULL },
    LAYOUT_CONTAINER_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Onglet — widget positionne qui empile des pages (volets).
 * Props geometriques + Position (barre d'onglets) + Valeur (volet actif,
 * entier 1-based). Les volets eux-memes sont des sous-blocs Volet (PROPS_VOLET),
 * pas des proprietes. */
static const OwlControlPropDef PROPS_ONGLET[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(300) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(200) },
    { "Position",       NULL, "Position",       OWL_PROP_TYPE_ENUM, "position",
      "Position de la barre d'onglets (ongHaut/ongBas/ongGauche/ongDroite).",
      "Tab bar position (ongHaut/ongBas/ongGauche/ongDroite).", NULL DFT_ENUM("ongHaut") },
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_INT,      NULL,
      "Volet actif au démarrage (entier 1-based, entre 1 et le nombre de volets).",
      "Active pane at startup (1-based integer, between 1 and the number of panes).", NULL DFT_INT(1) },
    { "LargeurVolets",  NULL, "TabWidth",       OWL_PROP_TYPE_ENUM, "largeurVolets",
      "Largeur des languettes (largeurProportionnelle = chacune selon son libellé, défaut ; largeurÉgale = toutes calées sur la plus large).",
      "Tab width (largeurProportionnelle = each sized to its caption, default; largeurÉgale = all set to the widest one).", NULL DFT_ENUM("largeurProportionnelle") },
    { "AlignementLibell\xc3\xa9", "AlignementLibelle", "LabelAlignment", OWL_PROP_TYPE_ENUM, "alignement",
      "Alignement du texte dans la languette (alignDébut = gauche, alignCentre = défaut, alignFin = droite).",
      "Tab label text alignment (alignDébut = left, alignCentre = default, alignFin = right).", NULL DFT_ENUM("alignCentre") },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,
                                                "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    /*  — Style typé du Champ Onglet (styleOnglet). Les formes littérale
     * `Style = { ... }` et statement `Style..Volet..Prop = v` restent parsees
     * par le parseur dedie (slots plats, cf compiler.c) ; cette entree catalogue
     * porte la forme REFERENCE `Style = theme.<nom>` / `Style = STY_<nom>`
     * (chemin OBJECT generique, comme tous les autres champs stylés) et donne au
     * front / LSP le pont racine -> type de style. */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleOnglet). Sous-elements : "
      "CadreExtérieur (le pane : couleur de fond, cadre), VoletActif et "
      "VoletInactif (les languettes).",
      "Typed style descriptor of the control (styleOnglet). Sub-elements: "
      "OuterFrame (the pane: background color, border), ActivePane and "
      "InactivePane (the tabs).",
      "styleOnglet" DFT_NULL },
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Volet — page d'un Champ Onglet. Sous-bloc de rôle : porte son
 * Libellé (texte de l'onglet, obligatoire) et sa Disposition (mode de layout
 * de ses enfants, comme un Conteneur). Les champs enfants sont declares dans
 * le corps du Volet (recursion, pas des proprietes). */
static const OwlControlPropDef PROPS_VOLET[] = {
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte affiché sur l'onglet de ce volet. Accepte une chaine simple ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Text shown on this pane's tab. Accepts a plain string or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Disposition",    NULL, "Layout",         OWL_PROP_TYPE_ENUM, "disposition",
      "Mode de layout des enfants du volet (dispVerticale/Horizontale/Grille/Libre).",
      "Children layout mode of the pane (dispVerticale/Horizontale/Grille/Libre).", NULL DFT_ENUM("dispVerticale") },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Volet affiché.", "Pane visible.", NULL DFT_BOOL(1) },
};

/* Champ Image — V1 : affichage d'une image avec Cadrage. V2 ajoutera
 * scrollbars, zoom (molette + +/-) et palette d'Outils. */
static const OwlControlPropDef PROPS_IMAGE_FIELD[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Image",          NULL, "Image",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Image affichee. Accepte un chemin de fichier (chaine) ou une "
      "instance Image en memoire.",
      "Displayed image. Accepts a file path (string) or an in-memory "
      "Image instance.", NULL DFT_NULL },
    { "ImageTaille",    NULL, "ImageSize",      OWL_PROP_TYPE_INT,      NULL,
      "Taille de rendu d'une icone en pixels. 0 ou absent : l'icone se "
      "rasterise a la dimension du champ. Valeur positive : taille forcee.",
      "Icon render size in pixels. 0 or absent: the icon rasterizes at the "
      "field dimension. Positive value: forced size.", NULL DFT_AUTO },
    { "Cadrage",        NULL, "Framing",        OWL_PROP_TYPE_ENUM, "cadrage",
      "Mode de cadrage de l'image dans le champ "
      "(cadCentre/cadEtire/cadHomothetique). Defaut cadHomothetique : "
      "l'image occupe le champ en preservant ses proportions.",
      "Image framing mode in the field "
      "(cadCentre/cadEtire/cadHomothetique). Default cadHomothetique: the "
      "image fills the field while preserving its aspect ratio.",
      NULL DFT_ENUM("cadHomothetique") },
    { "Zoom",           NULL, "Zoom",           OWL_PROP_TYPE_INT,      NULL,
      "Pourcentage de zoom (1..1000, defaut 100). 100 = taille native, "
      "200 = double, 50 = moitie. Modifiable au runtime.",
      "Zoom percentage (1..1000, default 100). 100 = native size, "
      "200 = double, 50 = half. Modifiable at runtime.", NULL DFT_INT(100) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_STRING,   NULL,
      "Texte d'info-bulle.", "Tooltip text.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond (zone hors image en cadHomothetique).",
      "Background color (area outside image in cadHomothetique).", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose (couleur, epaisseur, arrondi, bords).",
      "Composite border descriptor (color, thickness, radius, sides).",
      "Cadre" DFT_NULL },
    /*  — Style typé du Champ Image (styleImage). Un seul sous-element :
     * CadreExtérieur (la boite : CouleurFond + Cadre + Retraits). La source de
     * l'image et le mode d'echelle restent des props top-level (Image, Cadrage),
     * pas du Style. La cascade `IMG..Style..CadreExtérieur..Prop` est validee
     * compile-time contre les champs de styleImage (mecanisme OBJECT prop, cf
     * Style sur le Libellé). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleImage). Sous-element : "
      "CadreExtérieur (la boite : couleur de fond, cadre, retraits, statique).",
      "Typed style descriptor of the field (styleImage). Sub-element: "
      "OuterFrame (the box: background color, border, paddings, static).",
      "styleImage" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,      NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.",
      "Hover cursor shape.", NULL DFT_NULL },
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ CodeBarres — affichage output-only d'un code-barres genere. Calque
 * strict du Champ Image (backend OwlImageLabel) : l'image est rasterisee
 * depuis les props (type + contenu + correction d'erreur) via le pipeline de
 * generation, puis poussee au QLabel comme une image ordinaire.
 *
 * defaultProperty = "CodeBarres" (prop VARIANT) : `CBA = maVariableCodeBarres`
 * affecte le descripteur complet du code-barres (calque du prop "Image" du
 * Champ Image).
 *
 * CorrectionErreur est OWL_PROP_TYPE_INT (et non ENUM) : la valeur est un
 * niveau QR (constantes qrcodeNiveau*) OU un entier brut (PDF417 0-8, Aztec
 * 1-4). Un OWL_PROP_TYPE_ENUM refuserait un entier hors des valeurs enum ;
 * l'entier est le type reel du champ CorrectionErreur du type CodeBarres
 * (declare "entier" + allowedBase). */
static const OwlControlPropDef PROPS_CODEBARRE_FIELD[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(100) },
    { "CodeBarres",     NULL, "BarCodes",       OWL_PROP_TYPE_VARIANT,  NULL,
      "Descripteur complet du code-barres affiché (instance CodeBarres). "
      "L'affecter regénère l'image depuis son type, son contenu et son "
      "niveau de correction d'erreur.",
      "Full descriptor of the displayed barcode (CodeBarres instance). "
      "Assigning it regenerates the image from its type, content and "
      "error-correction level.", NULL DFT_NULL },
    { "TypeCodeBarres", NULL, "TypeBarCode",    OWL_PROP_TYPE_ENUM, "cbType",
      "Symbologie du code-barres (CB_QRCODE, CB_CODE128, ...).",
      "Barcode symbology (CB_QRCODE, CB_CODE128, ...).",
      NULL DFT_ENUM("CB_QRCODE") },
    { "Contenu",        NULL, "Content",        OWL_PROP_TYPE_STRING,   NULL,
      "Donnée à encoder dans le code-barres.",
      "Data to encode in the barcode.", NULL DFT_NULL },
    { "CorrectionErreur", NULL, "CorrectionError", OWL_PROP_TYPE_INT,   NULL,
      "Niveau de correction d'erreur. QR : constantes qrcodeNiveau* ; "
      "PDF417 : entier 0 à 8 ; Aztec : entier 1 à 4.",
      "Error-correction level. QR: qrcodeLevel* constants; PDF417: integer "
      "0 to 8; Aztec: integer 1 to 4.", NULL DFT_NULL },
    { "Option",         NULL, "Option",         OWL_PROP_TYPE_ENUM, "cbOptions",
      "Options d'affichage et d'orientation, combinables par + (CB_Texte / "
      "CB_SansTexte / CB_Etendu / CB_Vérification / "
      "CB_90 / CB_180 / CB_270).",
      "Display and orientation options, combinable with + (CB_Texte / "
      "CB_SansTexte / CB_Etendu / CB_Vérification / "
      "CB_90 / CB_180 / CB_270).", NULL DFT_NULL },
    { "CadrageHorizontal", NULL, "HorizontalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageH",
      "Alignement horizontal du code-barres dans le champ (chGauche/chCentre/chDroite).",
      "Horizontal alignment of the barcode within the field (chGauche/chCentre/chDroite).",
      NULL DFT_NULL },
    { "CadrageVertical", NULL, "VerticalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageV",
      "Alignement vertical du code-barres dans le champ (cvHaut/cvMilieu/cvBas).",
      "Vertical alignment of the barcode within the field (cvHaut/cvMilieu/cvBas).",
      NULL DFT_NULL },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_STRING,   NULL,
      "Texte d'info-bulle.", "Tooltip text.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond (zone hors code-barres).",
      "Background color (area outside the barcode).", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose (couleur, epaisseur, arrondi, bords).",
      "Composite border descriptor (color, thickness, radius, sides).",
      "Cadre" DFT_NULL },
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleCodeBarre). Sous-element : "
      "CadreExtérieur (la boite : couleur de fond, cadre, retraits, statique).",
      "Typed style descriptor of the field (styleCodeBarre). Sub-element: "
      "OuterFrame (the box: background color, border, paddings, static).",
      "styleCodeBarre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,      NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.",
      "Hover cursor shape.", NULL DFT_NULL },
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Liste — V1 mono-colonne texte simple.
 * Backend Qt6 : QListWidget. Sans modifier "Champ", `est une Liste` declare
 * une collection (cf TOKEN_LISTE dans compiler.c). Le DSL exige donc
 * `LST_X est un Champ Liste ... FIN` pour le widget. */
static const OwlControlPropDef PROPS_LIST_FIELD[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels. -1 = auto-layout.",
      "X position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels. -1 = auto-layout.",
      "Y position in pixels. -1 = auto-layout.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifié (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement.",
      "Unified state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_STRING,   NULL,
      "Texte d'info-bulle.", "Tooltip text.", NULL DFT_NULL },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte des elements (RGB entier).",
      "Item text color (RGB integer).", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond de la liste (RGB entier).",
      "List background color (RGB integer).", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose (couleur, epaisseur, arrondi, bords).",
      "Composite border descriptor (color, thickness, radius, sides).",
      "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100 (defaut 100).", "Opacity 0..100 (default 100).", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.",
      "Hover cursor shape.", NULL DFT_NULL },
    { "Ellipse",        NULL, "Ellipsis",       OWL_PROP_TYPE_ENUM, "ellipse",
      "Mode de gestion du texte trop long dans une ligne (ellipseInactive/Fin/Chemin).",
      "Overflow handling for long row text (ellipseInactive/Fin/Chemin).", NULL DFT_NULL },
    { "RetraitGauche",  NULL, "LeftPadding",    OWL_PROP_TYPE_INT,  NULL,
      "Espace en pixels a gauche du texte de chaque ligne.",
      "Pixels of inner padding to the left of each item text.", NULL DFT_NULL },
    /* Specifiques Liste */
    { "Multis\xc3\xa9lection", "Multiselection", "MultiSelection",
                                                OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = plusieurs lignes selectionnables (Ctrl/Shift). Faux = mono.",
      "True = multiple rows selectable (Ctrl/Shift). False = single.", NULL DFT_BOOL(0) },
    { "Tri\xc3\xa9""e", "Triee", "Sorted",      OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = tri alphabetique automatique a l'ajout.",
      "True = automatic alphabetical sort on insert.", NULL DFT_BOOL(0) },
    { "OptionTri",      NULL, "SortOption",     OWL_PROP_TYPE_ENUM, "chaineCompare",
      "Options de tri combinables (ccSansAccent, ccSansCasse, ccRespecteNumerique, etc.).",
      "Combinable sort options (ccSansAccent, ccSansCasse, ccRespecteNumerique, etc.).", NULL DFT_NULL },
    { "Zoom",           NULL, "Zoom",           OWL_PROP_TYPE_INT,      NULL,
      "Pourcentage 1..1000 (defaut 100). Ajuste la taille du texte des elements.",
      "Percentage 1..1000 (default 100). Scales item text size.", NULL DFT_INT(100) },
    { "Contenu",        NULL, "Content",        OWL_PROP_TYPE_STRING,   NULL,
      "Contenu complet en lecture/ecriture, lignes separees par LF.",
      "Full content for read/write, rows separated by LF.", NULL DFT_NULL },
    { "ContenuInitial", NULL, "InitialContent", OWL_PROP_TYPE_STRING,   NULL,
      "Contenu initial defini en design (multilignes, separateur LF).",
      "Initial design-time content (multiline, LF separator).", NULL DFT_NULL READONLY_RUNTIME },
    /* VARIANT intentionnel : Valeur est polymorphe (entier index OU
     * chaine rubrique memorisee selon le reglage du champ). OWL n'expose
     * que le mode index, mais le slot reste VARIANT pour rester aligne
     * sur le caractere polymorphe canon. Ne pas reclasser en scalaire.
     * UNSET assume : selection LIVE (read/write runtime), pas de littéral
     * de build (build_control lit ValeurInitiale, pas Valeur). */
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Element selectionne courant (lecture / ecriture pour positionner).",
      "Currently selected item (read / write to position)." RUNTIME_ONLY },
    { "ValeurAffich\xc3\xa9""e", "ValeurAffichee", "DisplayedValue",
                                                OWL_PROP_TYPE_STRING,   NULL,
      "Texte affiche de l'element selectionne.",
      "Displayed text of the selected item." RUNTIME_ONLY },
    { "ValeurM\xc3\xa9moris\xc3\xa9""e", "ValeurMemorisee", "StoredValue",
                                                OWL_PROP_TYPE_STRING,   NULL,
      "Valeur cachee associee a l'element selectionne (clef interne).",
      "Hidden value associated with the selected item (internal key)." RUNTIME_ONLY },
    /* VARIANT intentionnel : ValeurInitiale accepte tout type (la valeur
     * par defaut suit le type metier du champ). Polymorphe par design.
     * NULL = aucune selection initiale (absence). */
    { "ValeurInitiale", NULL, "InitialValue",   OWL_PROP_TYPE_VARIANT,  NULL,
      "Valeur par defaut a l'ouverture de la fenetre.",
      "Default value when the window opens.", NULL DFT_NULL READONLY_RUNTIME },
    { "Occurrence",     NULL, "Count",          OWL_PROP_TYPE_INT,      NULL,
      "Nombre d'elements dans la liste (lecture seule).",
      "Number of items in the list (read-only).", NULL DFT_INT(0) RUNTIME_ONLY },
    { "Vide",           NULL, "Empty",          OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai si la liste ne contient aucun element (lecture seule).",
      "True if the list contains no items (read-only).", NULL DFT_BOOL(1) RUNTIME_ONLY },
    /*  — style typé du Champ Liste (styleListe). Struct de sous-elements :
     * CadreExtérieur (le cadre + fond de la liste), LibelléLignes (le texte des
     * lignes) et LigneSélectionnée (le texte de la ligne selectionnee). La cascade
     * `LISTE..Style..SousElement..Prop` est validee compile-time contre les champs
     * de styleListe (mecanisme OBJECT prop, cf Style sur le Sélecteur). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleListe). Sous-elements : "
      "CadreExtérieur (le cadre + fond), LibelléLignes (le texte des lignes) "
      "et LigneSélectionnée (le texte de la ligne sélectionnée).",
      "Typed style descriptor of the field (styleListe). Sub-elements: "
      "OuterFrame (frame + background), RowLabel (row text) and "
      "SelectedRow (selected row text).",
      "styleListe" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Combo — V1 mono-colonne texte + dropdown.
 * Backend Qt6 : QComboBox. Reuse 90% des props de Liste, plus EnSaisie
 * et MaxLignesAffichees specifiques. */
static const OwlControlPropDef PROPS_COMBO_FIELD[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels.", "X position in pixels.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels.", "Y position in pixels.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur du champ ferme en pixels.", "Closed-state height in pixels.", NULL DFT_INT(25) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifie.", "Unified state.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte.", "Text color.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond.", "Background color.", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose.", "Composite border descriptor.", "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100.", "Opacity 0..100.", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.", "Hover cursor shape.", NULL DFT_NULL },
    /*  — libelle integre a gauche du champ combo (structure calquee
     * sur le Saisie : composite {Libellé + champ}). LargeurLibellé pilote la
     * largeur reservee au libelle. */
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Libelle integre affiche a gauche du champ combo. Accepte une chaine simple "
      "(coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Integrated label shown to the left of the combo field. Accepts a plain string "
      "(auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_STR("Combo") },
    { "LargeurLibell\xc3\xa9", "LargeurLibelle", "LabelWidth", OWL_PROP_TYPE_INT, NULL,
      "Largeur en pixels de la zone du libelle integre.",
      "Width in pixels of the integrated label area.", NULL DFT_INT(0) },
    /* Specifiques Combo */
    { "Tri\xc3\xa9""e", "Triee", "Sorted",      OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = tri alphabetique automatique.",
      "True = automatic alphabetical sort.", NULL DFT_BOOL(0) },
    { "OptionTri",      NULL, "SortOption",     OWL_PROP_TYPE_ENUM, "chaineCompare",
      "Options de tri combinables (ccSansAccent, ccSansCasse, etc.).",
      "Combinable sort options.", NULL DFT_NULL },
    { "EnSaisie",       NULL, "Editable",       OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = saisie libre dans la combo (Combo avec saisie).",
      "True = editable text field (Combo with input).", NULL DFT_BOOL(0) },
    /*  — desc alignee sur le comportement reel : defaut 0 = pas de
     * limite explicite (le QComboBox garde son maxVisibleItems ; setMaxVisibleItems
     * appele seulement si > 0). Le test verrouille ce defaut 0. */
    { "MaxLignesAffich\xc3\xa9""es", "MaxLignesAffichees", "MaxVisibleItems",
                                                OWL_PROP_TYPE_INT,      NULL,
      "Nombre max de lignes visibles dans la zone deroulee (0 = pas de limite explicite, ascenseur si la liste est plus longue).",
      "Max visible items in the dropdown (0 = no explicit limit, scrollbar if the list is longer)." },
    { "Contenu",        NULL, "Content",        OWL_PROP_TYPE_STRING,   NULL,
      "Contenu complet, lignes separees par LF.",
      "Full content, rows separated by LF.", NULL DFT_NULL },
    { "ContenuInitial", NULL, "InitialContent", OWL_PROP_TYPE_STRING,   NULL,
      "Contenu initial.", "Initial content.", NULL DFT_NULL READONLY_RUNTIME },
    /* VARIANT intentionnel : voir note Valeur sur la Liste (polymorphe
     * index/chaine canon, OWL n'expose que le mode index). UNSET assume :
     * selection LIVE (read/write runtime), pas de littéral de build. */
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Index 1-based de l'element selectionne (-1 si rien).",
      "1-based index of selected item (-1 if none)." RUNTIME_ONLY },
    { "ValeurAffich\xc3\xa9""e", "ValeurAffichee", "DisplayedValue",
                                                OWL_PROP_TYPE_STRING,   NULL,
      "Texte affiche de l'element selectionne.",
      "Displayed text of the selected item." RUNTIME_ONLY },
    { "ValeurM\xc3\xa9moris\xc3\xa9""e", "ValeurMemorisee", "StoredValue",
                                                OWL_PROP_TYPE_STRING,   NULL,
      "Valeur cachee associee a l'element selectionne.",
      "Hidden value associated with the selected item." RUNTIME_ONLY },
    /* VARIANT intentionnel : ValeurInitiale accepte tout type (defaut
     * suit le type metier du champ). Polymorphe par design. NULL = aucune
     * selection initiale (absence). */
    { "ValeurInitiale", NULL, "InitialValue",   OWL_PROP_TYPE_VARIANT,  NULL,
      "Valeur par defaut a l'ouverture.",
      "Default value.", NULL DFT_NULL READONLY_RUNTIME },
    { "Occurrence",     NULL, "Count",          OWL_PROP_TYPE_INT,      NULL,
      "Nombre d'elements.", "Number of items.", NULL DFT_INT(0) RUNTIME_ONLY },
    { "Vide",           NULL, "Empty",          OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai si vide.", "True if empty.", NULL DFT_BOOL(1) RUNTIME_ONLY },
    /*  — style typé du Champ Combo (styleCombo). Struct de cinq
     * sous-elements : Libellé (le libellé integre), CadreExtérieur (le conteneur),
     * ZoneSaisie (le champ combo), LibelléLignes (le texte des items du popup)
     * et LigneSélectionnée (l'item surligné). La cascade
     * `COMBO..Style..SousElement..Prop` est validee compile-time contre les champs
     * de styleCombo (mecanisme OBJECT prop, cf Style sur la Liste). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleCombo). Sous-elements : "
      "Libellé (libellé integre), CadreExtérieur (conteneur), ZoneSaisie (champ combo), "
      "LibelléLignes (texte des items du popup) et LigneSélectionnée (item surligné).",
      "Typed style descriptor of the field (styleCombo). Sub-elements: "
      "Label (embedded label), OuterFrame (container), InputZone (combo field), "
      "RowLabel (popup item text) and SelectedRow (highlighted item).",
      "styleCombo" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Selecteur — V1 (radio button group exclusif).
 * Backend Qt6 : QGroupBox sans titre + QButtonGroup exclusif + N
 * QRadioButton (avec optionnellement un sous-libelle / icone).
 *
 * Prop ..Options polymorphique : array de chaines (libelles simples) OU
 * array de Struct {Libelle, SousLibelle, ValeurRenvoyee, Image}. Le
 * dispatch se fait au runtime dans owl_ui_selecteur_set_options. */
static const OwlControlPropDef PROPS_SELECTEUR_FIELD[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels.", "X position in pixels.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels.", "Y position in pixels.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifie.", "Unified state.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte des libelles d'options.", "Option label text color.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond.", "Background color.", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose.", "Composite border descriptor.", "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100.", "Opacity 0..100.", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.", "Hover cursor shape.", NULL DFT_NULL },
    /* Specifiques Selecteur */
    { "Options",        NULL, "Options",        OWL_PROP_TYPE_VARIANT,  "Option",
      "Liste des options. Accepte un tableau de chaines (libelles simples) ou un tableau de Struct {Libelle, SousLibelle, ValeurRenvoyee, Image}.",
      "Options list. Accepts an array of strings (simple labels) or an array of Struct {Libelle, SousLibelle, ValeurRenvoyee, Image}.", NULL DFT_NULL },
    { "ModeAffichage",  NULL, "DisplayMode",    OWL_PROP_TYPE_ENUM, "selModeAffichage",
      "Mode d'affichage : selStandard (defaut), selSousLib, selVista, selVistaSousLib.",
      "Display mode: selStandard (default), selSousLib, selVista, selVistaSousLib.", NULL DFT_ENUM("selStandard") },
    { "NombreColonne",  NULL, "ColumnCount",    OWL_PROP_TYPE_INT,      NULL,
      "Nombre de colonnes pour disposer les options en grille (defaut 1 = vertical).",
      "Number of columns to arrange options in a grid (default 1 = vertical).", NULL DFT_INT(1) },
    /* UNSET assume : selection LIVE (read/write runtime), build lit
     * ValeurInitiale, pas Valeur. */
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Selection courante : entier (index 1-based), chaine (matche ValeurRenvoyee), -1 ou Null pour deselectionner.",
      "Current selection: integer (1-based index), string (matches ValeurRenvoyee), -1 or Null to deselect." },
    { "ValeurInitiale", NULL, "InitialValue",   OWL_PROP_TYPE_VARIANT,  NULL,
      "Valeur par defaut a l'ouverture (entier ou chaine).",
      "Default value at window open (integer or string).", NULL DFT_NULL },
    { "Occurrence",     NULL, "Count",          OWL_PROP_TYPE_INT,      NULL,
      "Nombre d'options (lecture seule).",
      "Number of options (read-only).", NULL DFT_INT(0) },
    { "HauteurOption",  NULL, "OptionHeight",   OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels de chaque ligne d'option (0 = hauteur naturelle).",
      "Height in pixels of each option row (0 = natural height).", NULL DFT_INT(0) },
    /*  — Style typé du Champ Sélecteur (styleSélecteur). Struct de
     * sous-elements : CadreExtérieur (le conteneur), TexteOptions (le texte des
     * options), Puce (le rond selectionne) et CadrePuce (la boite du rond). La
     * cascade `SEL..Style..SousElement..Prop` est validee compile-time contre
     * les champs de styleSélecteur (mecanisme OBJECT prop, cf Style sur
     * l'Interrupteur). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleSélecteur). Sous-elements : "
      "CadreExtérieur (le conteneur), TexteOptions (le texte des options), Puce "
      "(le rond selectionne) et CadrePuce (la boite du rond).",
      "Typed style descriptor of the field (styleSélecteur). Sub-elements: "
      "OuterFrame (container), OptionText (option text), Bullet (selected "
      "bullet) and BulletBox (bullet box).",
      "styleS\xc3\xa9lecteur" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Interrupteur — V1 (checkbox group multi-select).
 * Backend Qt6 : QFrame custom + N QCheckBox indépendantes (pas de
 * QButtonGroup). Reuse 90% des props du Selecteur. ..Valeur = bitmask
 * entier (canon : option N = bit N-1). Pas de ValeurRenvoyee custom
 * sur les options (l'index decide du bit). Support tri-etat. */
static const OwlControlPropDef PROPS_INTERRUPTEUR_FIELD[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels.", "X position in pixels.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels.", "Y position in pixels.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(25) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifie.", "Unified state.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte des libelles.", "Option label text color.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond.", "Background color.", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose.", "Composite border descriptor.", "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100.", "Opacity 0..100.", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.", "Hover cursor shape.", NULL DFT_NULL },
    /* Specifiques Interrupteur */
    { "Options",        NULL, "Options",        OWL_PROP_TYPE_VARIANT,  "Option",
      "Liste des options. Tableau de chaines OU tableau de Struct {Libelle, SousLibelle, Image}.",
      "Options list. Array of strings OR array of Struct {Libelle, SousLibelle, Image}.", NULL DFT_NULL },
    { "ModeAffichage",  NULL, "DisplayMode",    OWL_PROP_TYPE_ENUM, "selModeAffichage",
      "Mode d'affichage : selStandard (defaut), selSousLib, selVista, selVistaSousLib.",
      "Display mode: selStandard (default), selSousLib, selVista, selVistaSousLib.", NULL DFT_ENUM("selStandard") },
    { "NombreColonne",  NULL, "ColumnCount",    OWL_PROP_TYPE_INT,      NULL,
      "Nombre de colonnes pour disposer les options en grille (defaut 1).",
      "Number of columns to arrange options in a grid (default 1).", NULL DFT_INT(1) },
    { "HauteurOption",  NULL, "OptionHeight",   OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels de chaque ligne d'option (0 = hauteur naturelle).",
      "Height in pixels of each option row (0 = natural height).", NULL DFT_INT(0) },
    { "TroisEtats",     NULL, "ThreeStates",    OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = options tri-etat (coche/decoche/indetermine, valeur option = -1).",
      "True = tri-state options (checked/unchecked/indeterminate, option value = -1).", NULL DFT_BOOL(0) },
    /* UNSET assume : bitmask LIVE (read/write runtime), build lit
     * ValeurInitiale. */
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_INT,      NULL,
      "Bitmask des options cochees : option N -> bit N-1. Lecture/ecriture.",
      "Bitmask of checked options: option N -> bit N-1. Read/write." },
    { "ValeurInitiale", NULL, "InitialValue",   OWL_PROP_TYPE_INT,      NULL,
      "Bitmask initial.",
      "Initial bitmask.", NULL DFT_INT(0) },
    { "Occurrence",     NULL, "Count",          OWL_PROP_TYPE_INT,      NULL,
      "Nombre d'options (lecture seule).",
      "Number of options (read-only).", NULL DFT_INT(0) },
    /*  — Style typé du Champ Interrupteur (styleInterrupteur). Struct
     * de sous-elements : CadreExtérieur (le conteneur), TexteOptions (le texte
     * des cases), Coche (la marque) et CadreCoche (la boite de la coche). La
     * cascade `INT..Style..SousElement..Prop` est validee compile-time contre
     * les champs de styleInterrupteur (mecanisme OBJECT prop, cf Style sur le
     * Bouton). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleInterrupteur). Sous-elements : "
      "CadreExtérieur (le conteneur), TexteOptions (le texte des cases), Coche "
      "(la marque) et CadreCoche (la boite de la coche).",
      "Typed style descriptor of the field (styleInterrupteur). Sub-elements: "
      "OuterFrame (container), OptionText (option text), CheckMark (check mark) "
      "and CheckBox (check box).",
      "styleInterrupteur" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ Jauge — V1 (barre de progression output-only).
 * Backend Qt6 : QProgressBar. ..Valeur = entier borne a [BorneMin, BorneMax]
 * (propriete par defaut). Variantes lineaire horizontale (defaut) / verticale
 * (Vertical) + symetrique (progression depuis le centre). */
static const OwlControlPropDef PROPS_JAUGE_FIELD[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels.", "X position in pixels.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels.", "Y position in pixels.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_INT(150) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur en pixels.", "Height in pixels.", NULL DFT_INT(24) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifie.", "Unified state.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte affiche (pourcentage ou texte personnalise).",
      "Color of the displayed text (percentage or custom text).", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond de la barre.", "Bar background color.", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose.", "Composite border descriptor.", "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100.", "Opacity 0..100.", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.", "Hover cursor shape.", NULL DFT_NULL },
    /* Specifiques Jauge */
    { "Valeur",         NULL, "Value",          OWL_PROP_TYPE_INT,      NULL,
      "Position courante, bornee a [BorneMin, BorneMax]. Lecture/ecriture.",
      "Current position, clamped to [MinValue, MaxValue]. Read/write.", NULL DFT_INT(0) },
    { "BorneMin",       NULL, "MinValue",       OWL_PROP_TYPE_INT,      NULL,
      "Borne inferieure de la plage.", "Lower bound of the range.", NULL DFT_INT(0) },
    { "BorneMax",       NULL, "MaxValue",       OWL_PROP_TYPE_INT,      NULL,
      "Borne superieure de la plage.", "Upper bound of the range.", NULL DFT_INT(100) },
    { "Vertical",       NULL, "Vertical",       OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = barre verticale (remplissage de bas en haut) ; Faux = horizontale.",
      "True = vertical bar (fills bottom to top); False = horizontal.", NULL DFT_BOOL(0) },
    { "Sym\xc3\xa9trique", "Symetrique", "Symmetric", OWL_PROP_TYPE_BOOL,  NULL,
      "Vrai = progression depuis le centre vers les bords (valeurs negatives/positives).",
      "True = fills from the center toward the edges (negative/positive values).", NULL DFT_BOOL(0) },
    { "CouleurJauge",   NULL, "ProgressBarColor", OWL_PROP_TYPE_COLOR,  NULL,
      "Couleur de la portion remplie de la barre.",
      "Color of the filled portion of the bar.", NULL DFT_NULL },
    { "TexteJauge",     NULL, "TextProgressBar", OWL_PROP_TYPE_STRING,  NULL,
      "Texte affiche a la place du pourcentage (vide = pourcentage automatique).",
      "Text shown instead of the percentage (empty = automatic percentage).", NULL DFT_NULL },
    { "CadrageHorizontal", NULL, "HorizontalAlignment", OWL_PROP_TYPE_ENUM,
                                                "cadrageH",
      "Alignement horizontal du texte de progression dans la barre "
      "(chGauche/chCentre/chDroite).",
      "Horizontal alignment of the progress text within the bar "
      "(chGauche/chCentre/chDroite).", NULL DFT_ENUM("chCentre") },
    /*  — Style typé du Champ Jauge (styleJauge). Struct de sous-elements :
     * CadreExtérieur (piste + bordure + fond), Remplissage (barre de progression),
     * Libellé (texte du pourcentage). Cascade `JAU..Style..X..Y` validee
     * compile-time contre les champs de styleJauge (mecanisme OBJECT). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleJauge). Sous-elements : "
      "CadreExtérieur, Remplissage, Libellé.",
      "Typed style descriptor of the field (styleJauge). Sub-elements: "
      "OuterFrame, Fill, Label.",
      "styleJauge" DFT_NULL },
    FONT_PROPS,
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* Champ ZoneRépétée (Looper) — V1 par programmation. */
static const OwlControlPropDef PROPS_ZR_FIELD[] = {
    SHORTCUT_FOCUS_PROP,
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X en pixels.", "X position in pixels.", NULL DFT_AUTO },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y en pixels.", "Y position in pixels.", NULL DFT_AUTO },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur totale du champ en pixels.",
      "Total field width in pixels.", NULL DFT_INT(100) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur totale du champ en pixels.",
      "Total field height in pixels.", NULL DFT_INT(25) },
    { "Visible",        NULL, "Visible",        OWL_PROP_TYPE_BOOL,     NULL,
      "Champ affiche.", "Control visible.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9",   "Grise", "Grayed",    OWL_PROP_TYPE_BOOL,     NULL,
      "Champ desactive.", "Control grayed out.", NULL DFT_BOOL(0) },
    { "Etat",           NULL, "State",          OWL_PROP_TYPE_ENUM,     "etat",
      "Etat unifie.", "Unified state.", NULL DFT_ENUM("Actif") },
    { "Bulle",          NULL, "Tooltip",        OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'info-bulle. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Tooltip text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond du champ.", "Background color.", NULL DFT_NULL },
    { "Cadre",          NULL, "Border",         OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de cadre compose.", "Composite border descriptor.", "Cadre" DFT_NULL },
    { "Opacit\xc3\xa9", "Opacite", "Opacity",   OWL_PROP_TYPE_INT,      NULL,
      "Opacite 0..100.", "Opacity 0..100.", NULL DFT_INT(100) },
    { "Altitude",       NULL, "Altitude",       OWL_PROP_TYPE_INT,  NULL,
      "Z-order intra-fenetre.", "Intra-window z-order.", NULL DFT_NULL },
    { "CurseurSouris",  NULL, "MouseCursor",    OWL_PROP_TYPE_ENUM, "curseurs",
      "Forme du curseur de survol.", "Hover cursor shape.", NULL DFT_NULL },
    /* Specifiques ZoneRepetee */
    { "Sens",           NULL, "Direction",      OWL_PROP_TYPE_ENUM, "zrSens",
      "Sens de repetition : zrVertical (hauteur du template fait foi).",
      "Repetition direction: zrVertical (template height drives layout).",
      NULL DFT_ENUM("zrVertical") },
    { "Multis\xc3\xa9lection", "Multiselection", "MultiSelection",
                                                OWL_PROP_TYPE_BOOL,     NULL,
      "Vrai = plusieurs lignes selectionnables (Ctrl/Shift+clic). "
      "Faux (defaut) = selection unique.",
      "True = multi-row selection (Ctrl/Shift+click). False (default) = single.", NULL DFT_BOOL(0) },
    /* UNSET assume : ligne selectionnee LIVE (read/write runtime). */
    { "LigneS\xc3\xa9lectionn\xc3\xa9""e", "LigneSelectionnee", "SelectedRow",
                                                OWL_PROP_TYPE_INT,      NULL,
      "Indice 1-based de la repetition courante (0 si aucune). "
      "Lecture/ecriture : ecrire repositionne et notifie surSelectionLigne.",
      "1-based index of the current repetition (0 if none). "
      "Read/write: writing repositions and triggers onRowSelection." RUNTIME_ONLY },
    { "Occurrence",     NULL, "Count",          OWL_PROP_TYPE_INT,      NULL,
      "Nombre de repetitions (lecture seule).",
      "Number of repetitions (read-only).", NULL DFT_INT(0) RUNTIME_ONLY },
    /* AUTO : override d'un calcul auto (bounding box du template). Absence
     * = hauteur derivee par le moteur. */
    { "HauteurRepetition", NULL, "RepetitionHeight", OWL_PROP_TYPE_INT, NULL,
      "Hauteur d'une repetition en pixels (override le calcul auto base "
      "sur le bounding box des sous-champs du template). Posee par le "
      "designer via le slider de la ligne de separation.",
      "Repetition height in pixels (overrides the auto bounding-box "
      "calculation). Set by the designer's row-separator slider.", NULL DFT_AUTO },
    /*  — Style typé du Champ Zone répétée (styleZoneRépétée). Cascade
     * `ZR..Style..X..Y` validee compile-time contre les champs de
     * styleZoneRépétée (mecanisme OBJECT). Defauts sobres (fonds transparents,
     * cadres invisibles). */
    { "Style",          NULL, "Style",          OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé du champ (styleZoneRépétée). Sous-elements : "
      "CadreExtérieur, CadreInterne, LignesImpaires, LignesPaires, "
      "LigneSélectionnée, LigneSurvolée, Séparateur.",
      "Typed style descriptor of the field (styleZoneRépétée). Sub-elements: "
      "OuterFrame, InnerFrame, OddRows, EvenRows, "
      "SelectedRow, HoveredRow, Separator.",
      /* object_type = forme CANON ACCENTUEE du type (le registry stocke
       * styleZoneRépétée sous son nom accentue ; l'ASCII briserait la
       * resolution pres_composite_template dans la bijection presentation). */
      "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e" DFT_NULL },
    LAYOUT_CHILD_PROPS,
    GROUPE_PROP,
    NOTE_PROP,
};

/* colonne (sub-element de Table) — V1 minimaliste : Libellé + Type + Largeur. */
static const OwlControlPropDef PROPS_COLUMN[] = {
    /* Libelle absent -> l'en-tete retombe sur le nom de la colonne (build :
     * title=col->name). NULL = absence d'en-tete explicite. */
    { "Libell\xc3\xa9", "Libelle", "Caption",      OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte d'en-tete de la colonne. Accepte une chaine simple (coerce auto) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Column header text. Accepts a plain string (auto-coerced) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    { "Type",           NULL, "Type",              OWL_PROP_TYPE_ENUM,
                                                   "colTypes",
      "Type d'affichage des cellules (colTypeTexte/colTypeNumerique/colTypeMonetaire/colTypeDate/colTypeHeure/colTypeDuree/colTypeBooleen/colTypeMotDePasse).",
      "Cell display type (colTypeText/colTypeNumeric/colTypeCurrency/colTypeDate/colTypeTime/colTypeDuration/colTypeBoolean/colTypePassword).", NULL DFT_ENUM("colTypeTexte") },
    /* Largeur colonne : build lit colWidth=-1 (auto-sizing Qt). AUTO. */
    { "Largeur",        NULL, "Width",             OWL_PROP_TYPE_INT,      NULL,
      "Largeur en pixels.", "Width in pixels.", NULL DFT_AUTO },
    { "Visible",        NULL, "Visible",           OWL_PROP_TYPE_BOOL,     NULL,
      "Colonne affichee.", "Column visible.", NULL DFT_BOOL(1) },
    { "Etat",           NULL, "State",             OWL_PROP_TYPE_ENUM,     "etat",
      "Etat de la colonne (Actif / AffichageSeulement / Grisé). Inactif est un synonyme d'AffichageSeulement.",
      "Column state (Active / DisplayOnly / Grayed). Inactive is a synonym for DisplayOnly.", NULL DFT_ENUM("Actif") },
    /* Miroir du MasqueSaisie du champ Saisie (PROPS_SAISIE), au niveau colonne :
     * cadre la forme des valeurs saisies dans les cellules de la colonne. */
    { "MasqueSaisie",   NULL, "InputMask",         OWL_PROP_TYPE_STRING,   NULL,
      "Masque de saisie appliqué à l'édition des cellules de la colonne (texte).",
      "Input mask applied when editing the column's cells (text).", NULL DFT_NULL },
    /*  — Style typé de la Colonne (styleColonne) : surcharge locale du
     * styleTable, colonne par colonne. Cascade `COL..Style..X..Y` validee
     * compile-time contre les champs de styleColonne (mecanisme OBJECT). */
    { "Style",          NULL, "Style",             OWL_PROP_TYPE_OBJECT,   NULL,
      "Descripteur de style typé de la colonne (styleColonne) : surcharge locale "
      "du style de la table. Sous-elements : TitreColonne, LibelléLignes, "
      "LignesPaires, LignesImpaires, LigneSélectionnée, LigneSurvolée.",
      "Typed style descriptor of the column (styleColonne): local override of the "
      "table style. Sub-elements: ColumnTitle, RowLabel, EvenRows, "
      "OddRows, SelectedRow, HoveredRow.",
      "styleColonne" DFT_NULL },
    /*  — mise en forme runtime de la colonne entiere via le receveur nu
     * `COL_X..Couleur` / `COL_X..CouleurFond` (desucre vers le proxy-colonne,
     * ). RUNTIME_ONLY : non declarables dans le bloc `colonne` (le style
     * typé y reste la voie declarative) mais lisibles/ecrivables en code et
     * exposees a la completion + `explain Colonne`. Ce sont les DEUX seules props
     * que le receveur nu accepte a l'ecriture ; les autres restent refusees. */
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte de toute la colonne (RGB entier).",
      "Text color of the whole column (RGB integer).", NULL DFT_NULL RUNTIME_ONLY },
    { "CouleurFond",    NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,   NULL,
      "Couleur de fond de toute la colonne (RGB entier).",
      "Background color of the whole column (RGB integer).", NULL DFT_NULL RUNTIME_ONLY },
};

/*  — mise en forme d'une cellule ou d'une ligne de Table.
 *
 * Genre synthetique : aucun bloc DSL ne l'ouvre. Ces deux proprietes ne vivent
 * qu'en code, sur un receveur indexe (`TABLE_X[ligne][colonne]..Couleur`,
 * `TABLE_X[ligne]..CouleurFond`). Elles sont volontairement ABSENTES de
 * PROPS_TABLE et de PROPS_COLUMN : au niveau du tableau comme de la colonne, la
 * mise en forme passe par le descripteur Style typé, pas par une couleur plate.
 * Les ajouter la ouvrirait deux surfaces publiques de plus, non demandees. */
static const OwlControlPropDef PROPS_TABLE_CELL[] = {
    { "Couleur",     NULL, "Color",           OWL_PROP_TYPE_COLOR, NULL,
      "Couleur du texte de la cellule ou de la ligne (RGB entier).",
      "Text color of the cell or row (RGB integer).",
      NULL DFT_NULL RUNTIME_ONLY },
    { "CouleurFond", NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR, NULL,
      "Couleur de fond de la cellule ou de la ligne (RGB entier).",
      "Background color of the cell or row (RGB integer).",
      NULL DFT_NULL RUNTIME_ONLY },
};

const OwlControlPropDef* owl_table_cell_catalog_find(const char* prop_name) {
    if (prop_name == NULL) return NULL;
    for (int i = 0;
         i < (int)(sizeof(PROPS_TABLE_CELL) / sizeof(PROPS_TABLE_CELL[0]));
         i++) {
        const OwlControlPropDef* p = &PROPS_TABLE_CELL[i];
        if (strcasecmp(p->nameFR, prop_name) == 0) return p;
        if (p->nameEN != NULL && strcasecmp(p->nameEN, prop_name) == 0) return p;
    }
    return NULL;
}

/* ====================================================================
 *  Menu classique de fenetre
 *
 *  Trois tables, une par sorte de noeud. Volontairement etroites : un
 *  noeud de menu n'a ni geometrie ni couleur propre (le rendu suit la
 *  barre de menus native). Toute autre propriete est refusee a la
 *  compilation, pas avalee.
 * ==================================================================== */

static const OwlControlPropDef PROPS_MENU_SUBMENU[] = {
    { "Libell\xc3\xa9", "Libelle", "Caption", OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte affiche du menu deroulant. Accepte une chaine simple (coerce auto) "
      "ou un litteral inline multilingue {langueXxx: \"...\"}. Le caractere '&' "
      "place devant une lettre en fait le raccourci mnemonique du menu.",
      "Displayed text of the drop-down menu. Accepts a plain string (auto-coerced) "
      "or an inline multilingual literal {langueXxx: \"...\"}. A '&' before a letter "
      "turns it into the mnemonic shortcut of the menu.", NULL DFT_NULL },
    { "Visible", NULL, "Visible", OWL_PROP_TYPE_BOOL, NULL,
      "Menu deroulant affiche dans son parent.",
      "Drop-down menu displayed inside its parent.", NULL DFT_BOOL(1) },
    { "Gris\xc3\xa9", "Grise", "Grayed", OWL_PROP_TYPE_BOOL, NULL,
      "Menu deroulant grise : visible mais non ouvrable.",
      "Grayed drop-down menu: visible but cannot be opened.", NULL DFT_BOOL(0) },
    { "Etat", NULL, "State", OWL_PROP_TYPE_ENUM, "etat",
      "Etat initial du menu deroulant (enum unifie partage avec les champs). "
      "Variante utile sur un noeud de menu : Actif (ouvrable), AffichageSeulement "
      "(visible, non ouvrable ; libelle 'Inactif' cote inspecteur) et Grisé "
      "(visible, non ouvrable). Invisible reste pilotable mais le masquage passe "
      "plutot par Visible. Ecrase Grisé quand specifie.",
      "Initial state of the drop-down menu (unified enum shared with controls). "
      "Useful variant on a menu node: Actif (openable), AffichageSeulement (visible, "
      "not openable; labelled 'Inactif' in the inspector) and Grayed (visible, not "
      "openable). Invisible stays available but hiding usually goes through Visible. "
      "Overrides Grayed when specified.", NULL DFT_ENUM("Actif") },
};

static const OwlControlPropDef PROPS_MENU_OPTION[] = {
    { "Libell\xc3\xa9", "Libelle", "Caption", OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte affiche de l'option. Accepte une chaine simple (coerce auto) ou un "
      "litteral inline multilingue {langueXxx: \"...\"}. Le caractere '&' place "
      "devant une lettre en fait le raccourci mnemonique de l'option.",
      "Displayed text of the option. Accepts a plain string (auto-coerced) or an "
      "inline multilingual literal {langueXxx: \"...\"}. A '&' before a letter turns "
      "it into the mnemonic shortcut of the option.", NULL DFT_NULL },
    { "Raccourci", NULL, "Shortcut", OWL_PROP_TYPE_STRING, NULL,
      "Accelerateur clavier de l'option, sous la forme \"Ctrl+N\", \"Maj+F5\", "
      "\"Ctrl+Alt+Suppr\" : des modificateurs (Ctrl, Alt, Maj) separes par '+' "
      "suivis d'une touche.",
      "Keyboard accelerator of the option, written \"Ctrl+N\", \"Shift+F5\", "
      "\"Ctrl+Alt+Del\": modifiers (Ctrl, Alt, Shift) separated by '+' followed "
      "by a key.", NULL DFT_NULL },
    { "Image", NULL, "Image", OWL_PROP_TYPE_VARIANT, NULL,
      "Icone affichee a gauche du texte de l'option (chemin de fichier ou "
      "reference du catalogue d'icones).",
      "Icon displayed to the left of the option text (file path or icon catalogue "
      "reference).", NULL DFT_NULL },
    { "Coch\xc3\xa9", "Coche", "Checked", OWL_PROP_TYPE_BOOL, NULL,
      "Option cochee : une marque est affichee a cote de son texte.",
      "Checked option: a mark is displayed next to its text.", NULL DFT_BOOL(0) },
    { "Gris\xc3\xa9", "Grise", "Grayed", OWL_PROP_TYPE_BOOL, NULL,
      "Option grisee : visible mais non selectionnable.",
      "Grayed option: visible but not selectable.", NULL DFT_BOOL(0) },
    { "Visible", NULL, "Visible", OWL_PROP_TYPE_BOOL, NULL,
      "Option affichee dans son menu.",
      "Option displayed inside its menu.", NULL DFT_BOOL(1) },
    { "Etat", NULL, "State", OWL_PROP_TYPE_ENUM, "etat",
      "Etat initial de l'option (enum unifie partage avec les champs). Variante "
      "utile sur un noeud de menu : Actif (cliquable), AffichageSeulement (visible, "
      "non cliquable ; libelle 'Inactif' cote inspecteur) et Grisé (visible, non "
      "cliquable). Invisible reste pilotable mais le masquage passe plutot par "
      "Visible. Ecrase Grisé quand specifie.",
      "Initial state of the option (unified enum shared with controls). Useful "
      "variant on a menu node: Actif (clickable), AffichageSeulement (visible, not "
      "clickable; labelled 'Inactif' in the inspector) and Grayed (visible, not "
      "clickable). Invisible stays available but hiding usually goes through "
      "Visible. Overrides Grayed when specified.", NULL DFT_ENUM("Actif") },
};

static const OwlControlPropDef PROPS_MENU_SEPARATOR[] = {
    { "Visible", NULL, "Visible", OWL_PROP_TYPE_BOOL, NULL,
      "Separateur affiche dans son menu.",
      "Separator displayed inside its menu.", NULL DFT_BOOL(1) },
};

/*  — fiches des deux RACINES de menu. Elles manquaient : seules les
 * trois sortes de noeud (SousMenu / OptionMenu / MenuSeparateur) en avaient
 * une, donc rien ne publiait le pont racine -> type de style, et le
 * compilateur rejouait la correspondance en comparaisons de texte tandis que
 * le formateur ne pouvait pas resoudre la cascade du tout.
 *
 * Une fiche ne presuppose aucune geometrie : Volet (Libellé / Disposition /
 * Visible) et Colonne (fiche non posable qui porte deja un `Style` typé vers
 * styleColonne, ) en sont le precedent. Les deux racines de menu
 * n'acceptent qu'une seule propriete, la propriete universelle `Style` — la
 * fiche dit donc exactement ce que le parseur accepte, sans rien inventer. */
static const OwlControlPropDef PROPS_MENU_PRINCIPAL[] = {
    { "Style", NULL, "Style", OWL_PROP_TYPE_OBJECT, NULL,
      "Descripteur de style typé de la barre de menus (styleMenuPrincipal). "
      "Sous-elements : Barre (la bande horizontale), SousMenu (le panneau "
      "déroulant) et OptionMenu (une rangée de menu).",
      "Typed style descriptor of the menu bar (styleMainMenu). Sub-elements: "
      "Bar (the horizontal band), Submenu (the drop-down panel) and MenuOption "
      "(a menu row).",
      "styleMenuPrincipal" DFT_NULL },
};

static const OwlControlPropDef PROPS_MENU_CONTEXTUEL[] = {
    { "Style", NULL, "Style", OWL_PROP_TYPE_OBJECT, NULL,
      "Descripteur de style typé du menu contextuel (styleMenuContextuel). "
      "Sous-elements : SousMenu (le panneau déroulant) et OptionMenu (une "
      "rangée de menu). Pas de surface Barre.",
      "Typed style descriptor of the context menu (styleContextMenu). "
      "Sub-elements: Submenu (the drop-down panel) and MenuOption (a menu "
      "row). No Bar surface.",
      "styleMenuContextuel" DFT_NULL },
};

static bool prop_name_match(const OwlControlPropDef* def, const char* user_name) {
    if (def->nameAscii && strcasecmp(def->nameAscii, user_name) == 0) return true;
    if (strcasecmp(def->nameFR, user_name) == 0) return true;
    if (def->nameEN && strcasecmp(def->nameEN, user_name) == 0) return true;
    return false;
}

static const OwlControlPropDef* find_in(const OwlControlPropDef* table, size_t n,
                                        const char* name) {
    for (size_t i = 0; i < n; i++) {
        if (prop_name_match(&table[i], name)) return &table[i];
    }
    return NULL;
}

const OwlControlPropDef* owl_window_catalog_find(const char* prop_name) {
    return find_in(PROPS_WINDOW, sizeof(PROPS_WINDOW)/sizeof(PROPS_WINDOW[0]),
                   prop_name);
}

/* DSL Etat V2 — proprietes du conteneur Etat top-level. */
static const OwlControlPropDef PROPS_ETAT[] = {
    { "Format",      NULL, "Format",       OWL_PROP_TYPE_ENUM,  "etatFormat",
      "Format de page (formatA4 par defaut, A3, A5, Letter, Legal).",
      "Page format (formatA4 default, A3, A5, Letter, Legal).", NULL DFT_ENUM("formatA4") },
    { "Orientation", NULL, "Orientation",  OWL_PROP_TYPE_ENUM,  "etatOrientation",
      "Orientation page (orientPortrait par defaut, orientPaysage).",
      "Page orientation (orientPortrait default, orientLandscape).", NULL DFT_ENUM("orientPortrait") },
    /* Marges : apply_etat_properties lit chaque marge avec defaut 0 mm
     * (appliquees seulement si au moins une marge est posee). */
    { "MargeHaut",   NULL, "TopMargin",    OWL_PROP_TYPE_INT,   NULL,
      "Marge haute en mm.",                "Top margin in mm.", NULL DFT_INT(0) },
    { "MargeBas",    NULL, "BottomMargin", OWL_PROP_TYPE_INT,   NULL,
      "Marge basse en mm.",                "Bottom margin in mm.", NULL DFT_INT(0) },
    { "MargeGauche", NULL, "LeftMargin",   OWL_PROP_TYPE_INT,   NULL,
      "Marge gauche en mm.",               "Left margin in mm.", NULL DFT_INT(0) },
    { "MargeDroite", NULL, "RightMargin",  OWL_PROP_TYPE_INT,   NULL,
      "Marge droite en mm.",               "Right margin in mm.", NULL DFT_INT(0) },
};

/* DSL Etat V2 — proprietes des sous-blocs (EnTeteDocument, Corps, ...).
 *
 * Un bloc d'Etat est essentiellement un conteneur visuel (cadre + fond),
 * pas un element de texte. La proprie'te' `Couleur` (du texte) n'a donc
 * pas de sens au niveau du bloc — elle se de'finira sur les champs (V2.5+).
 * La proprie'te' `Cadre` canon est un *type de variable* complexe
 * (Arrondi, Bords, CoinXxx, TraitXxx) — re'serve' V3 quand on aura le
 * type Cadre. */
static const OwlControlPropDef PROPS_ETAT_BLOCK[] = {
    /* run_etat_block lit Hauteur avec defaut 0 (espace reserve nul = le
     * body pilote l'avancee du curseur). */
    { "Hauteur",     NULL, "Height",          OWL_PROP_TYPE_INT,    NULL,
      "Hauteur reservee au bloc en mm.",
      "Block reserved height in mm.", NULL DFT_INT(0) },
    { "Visible",     NULL, "Visible",         OWL_PROP_TYPE_BOOL,   NULL,
      "Bloc visible (Vrai par defaut). Faux = bloc skip a l'impression.",
      "Block visible (default True). False = skip on print.", NULL DFT_BOOL(1) },
    { "CouleurFond", NULL, "BackgroundColor", OWL_PROP_TYPE_COLOR,  NULL,
      "Couleur de fond du bloc (RVB entier).",
      "Block background color (RGB integer).", NULL DFT_NULL },
    /* Props canon — sécabilité / saut / pied de page.
     * Secable : UNSET assume — prop acceptee au compile mais SANS effet
     * runtime a ce jour (gestion d'espace vertical = chantier V2.x). Pas de
     * littéral de lecture a sourcer. A peupler quand le runtime branchera la
     * sécabilité (defaut canon = Vrai). */
    { "S\xc3\xa9""cable", "Secable", "Splittable",  OWL_PROP_TYPE_BOOL, NULL,
      "Bloc sécable : si l'espace en bas de page est insuffisant, le bloc peut être coupé entre 2 pages (Vrai). Faux = report en page suivante.",
      "Splittable block: if remaining page space is insufficient, the block content can span across pages (True). False = move to next page." },
    { "SautDePageApresBloc", NULL, "PageBreakAfterBlock", OWL_PROP_TYPE_BOOL, NULL,
      "Vrai = un saut de page est généré automatiquement après l'impression du bloc.",
      "True = an automatic page break is generated after printing the block.", NULL DFT_BOOL(0) },
};

/* PROPS_LIBELLE_ETAT — Libellé WYSIWYG dans un bloc Etat (canon).
 *
 * Tronc commun avec PROPS_LABEL (Fenêtre) : positionnement (X/Y/Largeur/
 * Hauteur), Libellé (texte), Couleur, FONT_PROPS (Police composé +
 * raccourcis sucre).
 *
 * Specificites Etat (vs Fenêtre) :
 *   - Pas de Bulle / ToucheRaccourci (interactivité Fenêtre)
 *   - Pas de TypeLibellé / CadrageHorizontal/Vertical (mode rendering Qt)
 *   - + Alignement (combinable iGauche+iCentreH+iDroite + iHaut+iCentreV+iBas)
 *   - + Cadre (futur : style de bordure imprimee, V2)
 *
 * Le runtime stdlib_pdf.c::render_etat_label lit dirctement les raccourcis
 * (PoliceNom/PoliceTaille/etc). Le descripteur Police (composé) est lu en
 * fallback. */
static const OwlControlPropDef PROPS_LIBELLE_ETAT[] = {
    /* Coordonnees mm locales au bandeau : render_etat_label lit X/Y/Largeur/
     * Hauteur avec defaut 0. X/Y=0 = origine du bandeau. Largeur/Hauteur=0
     * declenchent une zone fallback (impression non clippee) au rendu. */
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X locale au bandeau parent (mm). 0 = bord gauche du bandeau.",
      "X position local to parent band (mm). 0 = band left edge.", NULL DFT_INT(0) },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y locale au bandeau parent (mm). 0 = haut du bandeau.",
      "Y position local to parent band (mm). 0 = band top edge.", NULL DFT_INT(0) },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur du libellé en mm.", "Label width in mm.", NULL DFT_INT(0) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur du libellé en mm.", "Label height in mm.", NULL DFT_INT(0) },
    { "Libell\xc3\xa9", "Libelle", "Caption",   OWL_PROP_TYPE_MULTILINGUE, NULL,
      "Texte du libelle. Accepte une chaine simple (coerce auto vers langue source) ou un litteral inline multilingue {langueXxx: \"...\"}.",
      "Label text. Accepts a plain string (auto-coerced to source language) or an inline multilingual literal {langueXxx: \"...\"}.", NULL DFT_NULL },
    /* Couleur : UNSET assume — DIVERGENCE declaration<->runtime. La prop est
     * declaree (desc "0 = noir") mais render_etat_label ne lit PAS "Couleur"
     * (le texte sort en couleur de police par defaut = noir). A brancher cote
     * stdlib_pdf OU re-documenter ; pas de littéral a sourcer aujourd'hui. */
    { "Couleur",        NULL, "Color",          OWL_PROP_TYPE_COLOR,    NULL,
      "Couleur du texte (RGB packed BGR ; 0 = noir).",
      "Text color (RGB packed BGR ; 0 = black)." },
    /* Specifique Etat : Alignement combinable bitmask iGauche/iCentreH/iDroite
     * + iHaut/iCentreV/iBas (cf E_IAlign). render_etat_label lit defaut 0
     * (aucun flag = haut-gauche). NULL = aucun alignement explicite. */
    { "Alignement",     NULL, "Alignment",      OWL_PROP_TYPE_ENUM,     "iAlignement",
      "Alignement texte combinable (iGauche|iCentreH|iDroite + iHaut|iCentreV|iBas).",
      "Combinable text alignment (iLeft|iCenterH|iRight + iTop|iCenterV|iBottom).", NULL DFT_NULL },
    { "FormatAffichage", NULL, "DisplayFormat", OWL_PROP_TYPE_ENUM, "formatAffichage",
      "Format d'interpretation du contenu Libelle État (formatTexte ou formatTexteRiche).",
      "Display format for Report label content (formatTexte or formatTexteRiche).", NULL DFT_ENUM("formatTexte") },
    /* Police composé + 6 raccourcis sucre (PoliceNom/Taille/Gras/...). */
    FONT_PROPS,
};

/* PROPS_IMAGE_ETAT — Image WYSIWYG dans un bloc Etat (canon).
 *
 * Coordonnees en mm (cohérent avec PROPS_LIBELLE_ETAT). Pas d'events
 * souris (pas d'interactivite sur PDF). AvantImpression event hook
 * gere separement comme un event de bloc.
 *
 * V1 minimal : positionnement + Image (path/ObjImage) + Cadrage. Pas
 * de Cadre composé, pas d'Animation, pas de Zoom (irrelevant en PDF). */
static const OwlControlPropDef PROPS_IMAGE_ETAT[] = {
    /* Coordonnees mm locales au bandeau : render_etat_image lit X/Y/Largeur/
     * Hauteur avec defaut 0 (Largeur/Hauteur=0 -> zone fallback au rendu). */
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X locale au bandeau parent (mm). 0 = bord gauche du bandeau.",
      "X position local to parent band (mm). 0 = band left edge.", NULL DFT_INT(0) },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y locale au bandeau parent (mm). 0 = haut du bandeau.",
      "Y position local to parent band (mm). 0 = band top edge.", NULL DFT_INT(0) },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur de l'image en mm.", "Image width in mm.", NULL DFT_INT(0) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur de l'image en mm.", "Image height in mm.", NULL DFT_INT(0) },
    { "Image",          NULL, "Image",          OWL_PROP_TYPE_VARIANT,  NULL,
      "Image affichee. Accepte un chemin de fichier (chaine) ou une "
      "instance Image en memoire.",
      "Displayed image. Accepts a file path (string) or an in-memory "
      "Image instance.", NULL DFT_NULL },
    /* render_etat_image lit Cadrage avec defaut 0 = cadCentre (applique). */
    { "Cadrage",        NULL, "Framing",        OWL_PROP_TYPE_ENUM, "cadrage",
      "Mode de cadrage de l'image dans la zone "
      "(cadCentre/cadEtire/cadHomothetique).",
      "Image framing mode in the area "
      "(cadCentre/cadEtire/cadHomothetique).", NULL DFT_ENUM("cadCentre") },
};

/* PROPS_CODEBARRE_ETAT — Champ CodeBarres WYSIWYG dans un bloc Etat.
 *
 * Coordonnees mm (coherent avec PROPS_IMAGE_ETAT). render_etat_codebarre
 * (stdlib_pdf.c) lit X/Y/Largeur/Hauteur + les props plates de generation
 * (TypeCodeBarres/Contenu/CorrectionErreur/Option). Rendu homothetique force
 * (un code-barres ne se deforme jamais) : pas de prop Cadrage. Pas d'events ni
 * de style (PDF statique). Table reduite face a PROPS_CODEBARRE_FIELD (Fenetre). */
static const OwlControlPropDef PROPS_CODEBARRE_ETAT[] = {
    { "X",              NULL, "X",              OWL_PROP_TYPE_INT,      NULL,
      "Position X locale au bandeau parent (mm). 0 = bord gauche du bandeau.",
      "X position local to parent band (mm). 0 = band left edge.", NULL DFT_INT(0) },
    { "Y",              NULL, "Y",              OWL_PROP_TYPE_INT,      NULL,
      "Position Y locale au bandeau parent (mm). 0 = haut du bandeau.",
      "Y position local to parent band (mm). 0 = band top edge.", NULL DFT_INT(0) },
    { "Largeur",        NULL, "Width",          OWL_PROP_TYPE_INT,      NULL,
      "Largeur du code-barres en mm.", "Barcode width in mm.", NULL DFT_INT(0) },
    { "Hauteur",        NULL, "Height",         OWL_PROP_TYPE_INT,      NULL,
      "Hauteur du code-barres en mm.", "Barcode height in mm.", NULL DFT_INT(0) },
    { "Contenu",        NULL, "Content",        OWL_PROP_TYPE_STRING,   NULL,
      "Donnee a encoder dans le code-barres.",
      "Data to encode in the barcode.", NULL DFT_NULL },
    { "TypeCodeBarres", NULL, "TypeBarCode",    OWL_PROP_TYPE_ENUM, "cbType",
      "Symbologie du code-barres (CB_QRCODE, CB_CODE128, ...).",
      "Barcode symbology (CB_QRCODE, CB_CODE128, ...).",
      NULL DFT_ENUM("CB_QRCODE") },
    { "CorrectionErreur", NULL, "CorrectionError", OWL_PROP_TYPE_INT,   NULL,
      "Niveau de correction d'erreur. QR : constantes qrcodeNiveau* ; "
      "PDF417 : entier 0 a 8 ; Aztec : entier 1 a 4.",
      "Error-correction level. QR: qrcodeLevel* constants; PDF417: integer "
      "0 to 8; Aztec: integer 1 to 4.", NULL DFT_NULL },
    { "Option",         NULL, "Option",         OWL_PROP_TYPE_ENUM, "cbOptions",
      "Options de generation, combinables par + (CB_Texte / CB_SansTexte / "
      "CB_Etendu / CB_Vérification / CB_90 / CB_180 / CB_270).",
      "Generation options, combinable with + (CB_Text / CB_NoText / CB_Extended / "
      "CB_Check / CB_90 / CB_180 / CB_270).", NULL DFT_NULL },
};
#undef FONT_PROPS

const OwlControlPropDef* owl_etat_catalog_find(const char* prop_name) {
    return find_in(PROPS_ETAT, sizeof(PROPS_ETAT)/sizeof(PROPS_ETAT[0]), prop_name);
}

const OwlControlPropDef* owl_etat_block_catalog_find(const char* prop_name) {
    return find_in(PROPS_ETAT_BLOCK,
                   sizeof(PROPS_ETAT_BLOCK)/sizeof(PROPS_ETAT_BLOCK[0]),
                   prop_name);
}

const OwlControlPropDef* owl_control_catalog_find(OwlControlKind kind,
                                                   const char* prop_name) {
    switch (kind) {
        case OWL_CONTROL_LABEL:
            return find_in(PROPS_LABEL, sizeof(PROPS_LABEL)/sizeof(PROPS_LABEL[0]),
                           prop_name);
        case OWL_CONTROL_BUTTON:
            return find_in(PROPS_BUTTON, sizeof(PROPS_BUTTON)/sizeof(PROPS_BUTTON[0]),
                           prop_name);
        case OWL_CONTROL_INPUT:
            return find_in(PROPS_INPUT, sizeof(PROPS_INPUT)/sizeof(PROPS_INPUT[0]),
                           prop_name);
        case OWL_CONTROL_TABLE:
            return find_in(PROPS_TABLE, sizeof(PROPS_TABLE)/sizeof(PROPS_TABLE[0]),
                           prop_name);
        case OWL_CONTROL_CONTAINER:
            return find_in(PROPS_CONTAINER,
                           sizeof(PROPS_CONTAINER)/sizeof(PROPS_CONTAINER[0]),
                           prop_name);
        case OWL_CONTROL_IMAGE:
            return find_in(PROPS_IMAGE_FIELD,
                           sizeof(PROPS_IMAGE_FIELD)/sizeof(PROPS_IMAGE_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_LISTE:
            return find_in(PROPS_LIST_FIELD,
                           sizeof(PROPS_LIST_FIELD)/sizeof(PROPS_LIST_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_COMBO:
            return find_in(PROPS_COMBO_FIELD,
                           sizeof(PROPS_COMBO_FIELD)/sizeof(PROPS_COMBO_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_SELECTEUR:
            return find_in(PROPS_SELECTEUR_FIELD,
                           sizeof(PROPS_SELECTEUR_FIELD)/sizeof(PROPS_SELECTEUR_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_INTERRUPTEUR:
            return find_in(PROPS_INTERRUPTEUR_FIELD,
                           sizeof(PROPS_INTERRUPTEUR_FIELD)/sizeof(PROPS_INTERRUPTEUR_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_ZONE_REPETEE:
            return find_in(PROPS_ZR_FIELD,
                           sizeof(PROPS_ZR_FIELD)/sizeof(PROPS_ZR_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_JAUGE:
            return find_in(PROPS_JAUGE_FIELD,
                           sizeof(PROPS_JAUGE_FIELD)/sizeof(PROPS_JAUGE_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_CODEBARRE:
            return find_in(PROPS_CODEBARRE_FIELD,
                           sizeof(PROPS_CODEBARRE_FIELD)/sizeof(PROPS_CODEBARRE_FIELD[0]),
                           prop_name);
        case OWL_CONTROL_ONGLET:
            return find_in(PROPS_ONGLET,
                           sizeof(PROPS_ONGLET)/sizeof(PROPS_ONGLET[0]),
                           prop_name);
        case OWL_CONTROL_VOLET:
            return find_in(PROPS_VOLET,
                           sizeof(PROPS_VOLET)/sizeof(PROPS_VOLET[0]),
                           prop_name);
        case OWL_CONTROL_WINDOW:
            /* Pour la fenetre, utiliser owl_window_catalog_find directement. */
            return find_in(PROPS_WINDOW,
                           sizeof(PROPS_WINDOW)/sizeof(PROPS_WINDOW[0]),
                           prop_name);
    }
    return NULL;
}

const OwlControlPropDef* owl_menu_catalog_find(OwlMenuNodeKind kind,
                                                const char* prop_name) {
    switch (kind) {
        case OWL_MENU_NODE_SUBMENU:
            return find_in(PROPS_MENU_SUBMENU,
                           sizeof(PROPS_MENU_SUBMENU)/sizeof(PROPS_MENU_SUBMENU[0]),
                           prop_name);
        case OWL_MENU_NODE_OPTION:
            return find_in(PROPS_MENU_OPTION,
                           sizeof(PROPS_MENU_OPTION)/sizeof(PROPS_MENU_OPTION[0]),
                           prop_name);
        case OWL_MENU_NODE_SEPARATOR:
            return find_in(PROPS_MENU_SEPARATOR,
                           sizeof(PROPS_MENU_SEPARATOR)/sizeof(PROPS_MENU_SEPARATOR[0]),
                           prop_name);
    }
    return NULL;
}

const char* owl_menu_catalog_prop_list(OwlMenuNodeKind kind) {
    switch (kind) {
        case OWL_MENU_NODE_SUBMENU:
            return "Libell\xc3\xa9, Visible, Gris\xc3\xa9";
        case OWL_MENU_NODE_OPTION:
            return "Libell\xc3\xa9, Raccourci, Image, Coch\xc3\xa9, Gris\xc3\xa9, Visible";
        case OWL_MENU_NODE_SEPARATOR:
            return "Visible";
    }
    return "";
}

bool owl_ui_prop_is_universal_readonly(const char* prop_name) {
    if (prop_name == NULL) return false;
    return strcasecmp(prop_name, "Nom") == 0 ||
           strcasecmp(prop_name, "Name") == 0;
}

bool owl_ui_prop_is_universal(const char* prop_name) {
    if (prop_name == NULL) return false;
    return strcasecmp(prop_name, "Style") == 0 ||
           owl_ui_prop_is_universal_readonly(prop_name);
}

/*  — validation par UNION pour recepteur dynamique (MoiMême/MonChamp)
 * dont le kind reel n'est connu qu'au runtime. Une prop est acceptee des
 * qu'elle existe sur AU MOINS UN kind de champ (seuil >= 1, zero faux positif
 * — a distinguer du seuil >= 2 de la completion). Les props universelles
 * (Nom/Style) sont trivialement acceptees. La Fenetre (OWL_CONTROL_WINDOW)
 * est exclue : ce n'est pas un champ. */
bool owl_ui_prop_exists_on_any_control_kind(const char* prop_name) {
    if (prop_name == NULL) return false;
    if (owl_ui_prop_is_universal(prop_name)) return true;
    for (int k = OWL_CONTROL_LABEL; k < OWL_CONTROL_WINDOW; k++) {
        if (owl_control_catalog_find((OwlControlKind)k, prop_name) != NULL) {
            return true;
        }
    }
    return false;
}

const char* owl_prop_type_name(OwlPropType t) {
    switch (t) {
        case OWL_PROP_TYPE_INT:       return "entier";
        case OWL_PROP_TYPE_STRING:    return "chaine";
        case OWL_PROP_TYPE_BOOL:      return "booleen";
        case OWL_PROP_TYPE_VARIANT:   return "tout";
        case OWL_PROP_TYPE_POSITION:  return "position (entier, entier)";
        case OWL_PROP_TYPE_COLOR:     return "couleur (entier RGB)";
        case OWL_PROP_TYPE_ENUM:      return "valeur d'enum";
        case OWL_PROP_TYPE_MULTILINGUE: return "cha\xc3\xae" "neMultilingue";
        case OWL_PROP_TYPE_OBJECT:    return "descripteur compose";
    }
    return "<inconnu>";
}

/* ====================================================================
 *  Enregistrement enums + constantes dans le registry
 * ==================================================================== */

/* TypeBouton */
static const char* const E_TypeBouton_values[] = {
    "btnNormal", "btnValider", "btnInterruption", "btnAbandon", "btnAide", NULL
};
static const OwlEnum E_TypeBouton = {
    .name="typeBouton", .nameAscii="typeBouton", .nameEN="buttonType", .category="UI",
    .descFR="Type de bouton (5 valeurs).",
    .descEN="Button type (5 values).",
    .values=E_TypeBouton_values, .combinable=false
};

/* PositionImageBouton ( lot B) — position de l'icone par rapport au
 * texte d'un Champ Bouton, feuille de style Style..Image..Position. 6 valeurs
 * mutuellement exclusives. posBtnImageGauche = defaut. */
static const char* const E_PositionImageBouton_values[] = {
    "posBtnImageGauche", "posBtnImageDroite", "posBtnImageHaut",
    "posBtnImageBas", "posBtnImageSeule", "posBtnTexteSeul", NULL
};
const OwlEnum E_PositionImageBouton = {
    .name="positionImageBouton", .nameAscii="positionImageBouton",
    .nameEN="buttonImagePosition", .category="UI",
    .descFR="Position de l'icone par rapport au texte d'un bouton (6 valeurs).",
    .descEN="Icon position relative to a button's text (6 values).",
    .values=E_PositionImageBouton_values, .combinable=false
};

/* TypeLibelle (V1 : 2 valeurs ; TLHTML/TLRTF/TLMarkdown/TLImage en V1.5+) */
static const char* const E_TypeLibelle_values[] = {
    "TLNormal", "TLMultiLigne", NULL
};
static const OwlEnum E_TypeLibelle = {
    .name="typeLibell\xc3\xa9", .nameAscii="typeLibelle", .nameEN="labelType", .category="UI",
    .descFR="Type de libelle (2 valeurs : mono-ligne ou multi-ligne).",
    .descEN="Label type (2 values: single-line or multi-line).",
    .values=E_TypeLibelle_values, .combinable=false
};

/* formatAffichage — bucket E (canon). 2 valeurs V1 :
 * 'texte' (literal) et 'texteRiche' (Markdown étendu OWL → HTML Qt6). */
static const char* const E_FormatAffichage_values[] = {
    "formatTexte", "formatTexteRiche", NULL
};
static const OwlEnum E_FormatAffichage = {
    .name="formatAffichage", .nameAscii="formatAffichage", .nameEN="displayFormat", .category="UI",
    .descFR="Format d'interpretation du contenu texte (literal ou Markdown OWL etendu).",
    .descEN="Text content interpretation format (literal or extended OWL Markdown).",
    .values=E_FormatAffichage_values, .combinable=false
};

/* TypeSaisie — canon (préfixe typSaisie*).
 * Canon complet = 7 valeurs (typSaisieDate/Durée/Heure/Monétaire/Num/
 * NumériquePrécis/Texte) ; OWL câble Texte + Num + NumériquePrécis +
 * Monétaire + Date + Heure + Durée (canon) plus les extensions DSL
 * MotDePasse/Multiligne. typSaisieNum -> owlRéel (flottant), tandis que
 * typSaisieNumériquePrécis -> owlNumérique (décimal exact, mpdecimal). */
static const char* const E_TypeSaisie_values[] = {
    "typSaisieTexte", "typSaisieMotDePasse", "typSaisieMultiligne",
    "typSaisieNum", "typSaisieMon\xc3\xa9taire", "typSaisieDate",
    "typSaisieHeure", "typSaisieDur\xc3\xa9" "e",
    "typSaisieNum\xc3\xa9riquePr\xc3\xa9" "cis", NULL
};
static const OwlEnum E_TypeSaisie = {
    .name="typeSaisie", .nameAscii="typeSaisie", .nameEN="inputType", .category="UI",
    .descFR="Type de saisie (texte, mot de passe, multiligne, numerique, monetaire, date, heure, duree).",
    .descEN="Input type (text, password, multiline, numeric, currency, date, time, duration).",
    .values=E_TypeSaisie_values, .combinable=false
};

/* CadrageHorizontal — exposé via extern pour allowedBase consommé dans stdlib_ui.c. */
static const char* const E_CadrageH_values[] = { "chGauche", "chCentre", "chDroite", NULL };
const OwlEnum E_CadrageH = {
    .name="cadrageH", .nameAscii="cadrageH", .nameEN="horizontalAlignment", .category="UI",
    .descFR="Alignement horizontal (texte dans champ).",
    .descEN="Horizontal alignment (text in field).",
    .values=E_CadrageH_values, .combinable=false
};

/* CadrageVertical — exposé via extern pour allowedBase consommé dans stdlib_ui.c.
 * cvMilieu = alias de cvCentre (canon alternatif), accepte par le compiler
 * via allowedBase et resolu au meme sentinel OWL_CV_CENTRE. */
static const char* const E_CadrageV_values[] = { "cvHaut", "cvCentre", "cvMilieu", "cvBas", NULL };
const OwlEnum E_CadrageV = {
    .name="cadrageV", .nameAscii="cadrageV", .nameEN="verticalAlignment", .category="UI",
    .descFR="Alignement vertical (texte dans champ).",
    .descEN="Vertical alignment (text in field).",
    .values=E_CadrageV_values, .combinable=false
};

/* toastDuree — durée d'affichage d'un Toast. 2 valeurs sentinel-zone.
 * Exposé via extern pour allowedBase consommé dans stdlib_ui.c. */
static const char* const E_toastDuree_values[] = { "toastCourt", "toastLong", NULL };
const OwlEnum E_toastDuree = {
    .name="toastDuree", .nameAscii="toastDuree", .nameEN="toastDuration", .category="UI",
    .descFR="Durée d'affichage d'un message Toast (court ou long).",
    .descEN="Toast message display duration (short or long).",
    .values=E_toastDuree_values, .combinable=false
};

/* Cadrage image dans un Libelle (et plus tard Champ Image). */
static const char* const E_Cadrage_values[] = {
    "cadCentre", "cadEtire", "cadHomothetique", NULL
};
static const OwlEnum E_Cadrage = {
    .name="cadrage", .nameAscii="cadrage", .nameEN="imageFraming", .category="UI",
    .descFR="Cadrage d'une image dans un champ qui en accepte une "
            "(cadCentre, cadEtire, cadHomothetique).",
    .descEN="Image framing inside an image-bearing field "
            "(cadCentre, cadEtire, cadHomothetique).",
    .values=E_Cadrage_values, .combinable=false
};

/* Etat (champ ou fenetre) - unifie Visible/Grise/ReadOnly. */
/* Inactif reste une constante compilable (alias d'AffichageSeulement, meme
 * sentinel OWL_ETAT_AFFICHAGE_SEUL) mais n'est PAS listee ici : l'enum expose
 * 4 etats distincts, l'alias est documente en prose dans la desc. */
static const char* const E_Etat_values[] = {
    "Actif", "AffichageSeulement", "Gris\xc3\xa9", "Invisible",
    "AffichageSansS\xc3\xa9lection", NULL
};
static const OwlEnum E_Etat = {
    .name="etat", .nameAscii="etat", .nameEN="fieldState", .category="UI",
    .descFR="Etat unifié d'un champ (Actif / AffichageSeulement / Grisé / Invisible). Inactif est un synonyme d'AffichageSeulement. AffichageSansSélection est spécifique au Champ Table : affiché, aucune sélection utilisateur (la sélection par programmation reste possible).",
    .descEN="Unified field state (Active / DisplayOnly / Grayed / Invisible). Inactive is a synonym for DisplayOnly. DisplayWithoutSelection is specific to the Table field: displayed, no user selection (programmatic selection stays possible).",
    .values=E_Etat_values, .combinable=false
};

/* Disposition (Lot F) — sentinel-zone : protege contre 'Disposition = 1'. */
static const char* const E_Disposition_values[] = {
    "dispVerticale", "dispHorizontale", "dispGrille", "dispLibre", NULL
};
static const OwlEnum E_Disposition = {
    .name="disposition", .nameAscii="disposition", .nameEN="layout", .category="UI",
    .descFR="Mode de layout des enfants d'un conteneur.",
    .descEN="Children layout mode for a container.",
    .values=E_Disposition_values, .combinable=false,
};

/* Position — position de la barre d'onglets d'un Champ Onglet.
 * 4 valeurs plates ongHaut(défaut)/ongBas/ongGauche/ongDroite. */
static const char* const E_Position_values[] = {
    "ongHaut", "ongBas", "ongGauche", "ongDroite", NULL
};
static const OwlEnum E_Position = {
    .name="position", .nameAscii="position", .nameEN="position", .category="UI",
    .descFR="Position de la barre d'onglets d'un Champ Onglet.",
    .descEN="Tab bar position of an Onglet field.",
    /* force_small_int : les 4 valeurs 0..3 sont la semantique runtime directe
     * (ordre haut/bas/gauche/droite du backend d'onglets), pas des sentinels. */
    .values=E_Position_values, .combinable=false, .force_small_int=true,
};

/* Alignement (Lot F) — sentinel-zone. */
static const char* const E_Alignement_values[] = {
    "alignD\xc3\xa9""but", "alignCentre", "alignFin",
    "align\xc3\x89tire", NULL
};
static const OwlEnum E_Alignement = {
    .name="alignement", .nameAscii="alignement", .nameEN="alignment", .category="UI",
    .descFR="Alignement transverse (debut/centre/fin/etire).",
    .descEN="Cross-axis alignment (start/center/end/stretch).",
    .values=E_Alignement_values, .combinable=false,
};

/* LargeurVolets — mode de largeur des languettes d'un Champ Onglet.
 * 2 valeurs mutuellement exclusives, sentinel-zone via OWL_LARGVOL_*.
 * largeurProportionnelle = defaut (comportement QTabBar natif). */
static const char* const E_LargeurVolets_values[] = {
    "largeurProportionnelle", "largeur\xc3\x89gale", NULL
};
static const OwlEnum E_LargeurVolets = {
    .name="largeurVolets", .nameAscii="largeurVolets", .nameEN="tabWidth", .category="UI",
    .descFR="Mode de largeur des languettes d'un Champ Onglet (proportionnelle/egale).",
    .descEN="Tab width mode of an Onglet field (proportional/equal).",
    .values=E_LargeurVolets_values, .combinable=false,
};

/* CurseurSouris — curseur de survol (canon). 14 valeurs.
 * Mappe directement sur Qt::CursorShape cote ui_qt.cpp. */
static const char* const E_Curseurs_values[] = {
    "curFl\xc3\xa8""che", "curMain", "curMainSyst\xc3\xa8me",
    "curCaret", "curCroix", "curSablier", "curAide", "curInterdit",
    "curHFl\xc3\xa8""che", "curTaille",
    "curTailleNS", "curTailleOE", "curTailleNESO", "curTailleNOSE", NULL
};
const OwlEnum E_Curseurs = {
    .name="curseurs", .nameAscii="curseurs", .nameEN="cursors", .category="UI",
    .descFR="Forme du curseur de survol.",
    .descEN="Mouse hover cursor shape.",
    .values=E_Curseurs_values, .combinable=false
};

/* Ellipse — gestion overflow texte (canon). */
static const char* const E_Ellipse_values[] = {
    "ellipseInactive", "ellipseFin", "ellipseChemin", NULL
};
static const OwlEnum E_Ellipse = {
    .name="ellipse", .nameAscii="ellipse", .nameEN="ellipsis", .category="UI",
    .descFR="Mode de gestion de l'ellipse pour texte trop long.",
    .descEN="Ellipsis mode for overflowing text.",
    .values=E_Ellipse_values, .combinable=false
};

/* sensEcriture — direction d'ecriture forcee. Enum unifie UI + i18n :
 * partage avec stdlib_i18n.c::LangueSensEcriture (consumer via OwlParam.allowedBase).
 * Canonical accentue "sensEcriture" pour rester coherent avec la convention
 * OWL (forme accentuee = canon, nameAscii = fallback resolution). */
static const char* const E_sensEcriture_values[] = {
    "sensD\xc3\xa9""faut", "sensGaucheDroite", "sensDroiteGauche", NULL
};
const OwlEnum E_sensEcriture = {
    .name="sens\xc3\x89""criture", .nameAscii="sensEcriture", .nameEN="writingDirection", .category="Internationalisation",
    .descFR="Sens d'ecriture du texte (defaut langue, LTR, RTL).",
    .descEN="Text writing direction (language default, LTR, RTL).",
    .values=E_sensEcriture_values, .combinable=false
};

/* TraitCadre — style du trait du cadre. Subset OWL (5 valeurs) qui mappent
 * directement sur les 5 styles Qt CSS supportes (solid/dashed/dotted/double/
 * none). Divergence assumee vs canon qui en propose 13 :
 * les variantes Moyen/Epais/Mixte/MixteTiretDouble n'apportent rien de
 * visuel sans QPainter custom (cf docs/VISION.md §5bis). */
static const char* const E_TraitCadre_values[] = {
    "TraitContinu", "TraitAucun", "TraitTiret", "TraitPointill\xc3\xa9",
    "TraitDouble",
    NULL
};
const OwlEnum E_TraitCadre = {
    .name="traitCadre", .nameAscii="traitCadre", .nameEN="frameLineStyle", .category="UI",
    .descFR="Style du trait du cadre (continu/tiret/pointille/...).",
    .descEN="Border line style (solid/dash/dot/...).",
    .values=E_TraitCadre_values, .combinable=false
};

/* typeDégradé — type de remplissage dégradé d'un CadreExtérieur.
 * 3 valeurs mutuellement exclusives, sentinel-zone. dégradéAucun = défaut
 * (pas de dégradé : le fond uni CouleurFond reste). Linéaire = dégradé
 * directionnel (angle -> coords QSS) ; radial = dégradé centré (angle ignoré). */
static const char* const E_TypeDegrade_values[] = {
    "d\xc3\xa9grad\xc3\xa9""Aucun", "d\xc3\xa9grad\xc3\xa9""Lin\xc3\xa9""aire",
    "d\xc3\xa9grad\xc3\xa9Radial",
    NULL
};
const OwlEnum E_TypeDegrade = {
    .name="typeD\xc3\xa9grad\xc3\xa9", .nameAscii="typeDegrade", .nameEN="gradientType", .category="UI",
    .descFR="Type de remplissage degrade d'un cadre (aucun/lineaire/radial).",
    .descEN="Gradient fill type of a frame (none/linear/radial).",
    .values=E_TypeDegrade_values, .combinable=false
};

/* BordsCadre — combinable bitwise (canon). BordTous = BordHaut+Droite+Bas+Gauche. */
static const char* const E_BordsCadre_values[] = {
    "BordAucun", "BordHaut", "BordDroite", "BordBas", "BordGauche", "BordTous",
    NULL
};
const OwlEnum E_BordsCadre = {
    .name="bordsCadre", .nameAscii="bordsCadre", .nameEN="frameBorders", .category="UI",
    .descFR="Bords du cadre presents (combinable bitwise).",
    .descEN="Border edges shown (combinable bitwise).",
    .values=E_BordsCadre_values, .combinable=true
};

/* colTypes — type d'une colonne dans un Champ Table.
 * Valeurs sentinel-zone pour eviter collision avec int 0..N que pourraient
 * taper a la main des utilisateurs. */
static const char* const E_ColTypes_values[] = {
    "colTypeTexte", "colTypeNumerique", "colTypeMon\xc3\xa9taire", "colTypeDate",
    "colTypeHeure", "colTypeDur\xc3\xa9""e", "colTypeBooleen", "colTypeMotDePasse", NULL
};
static const OwlEnum E_ColTypes = {
    .name="colTypes", .nameAscii="colTypes", .nameEN="colTypes", .category="UI",
    .descFR="Type d'une colonne de Table.",
    .descEN="Table column type.",
    .values=E_ColTypes_values, .combinable=false,
};

/* orientationsTable — orientation de disposition d'un Champ Table.
 * V1 : Vertical seul (lignes empilees). Sentinel-zone. */
static const char* const E_OrientationsTable_values[] = {
    "Vertical", NULL
};
static const OwlEnum E_OrientationsTable = {
    .name="orientationsTable", .nameAscii="orientationsTable",
    .nameEN="tableOrientations", .category="UI",
    .descFR="Orientation de disposition d'un Champ Table (Vertical : lignes empilees).",
    .descEN="Layout orientation of a Table field (Vertical: stacked rows).",
    .values=E_OrientationsTable_values, .combinable=false,
};

/* etatsTableInitial — etat initial d'interactivite d'un Champ
 * Table. Grisé reutilise le const de l'enum `etat`. Sentinel-zone. */
static const char* const E_EtatsTableInitial_values[] = {
    "EnSaisie", "Gris\xc3\xa9", "S\xc3\xa9lection", "NiSaisieNiS\xc3\xa9lection", NULL
};
static const OwlEnum E_EtatsTableInitial = {
    .name="etatsTableInitial", .nameAscii="etatsTableInitial",
    .nameEN="tableInitialStates", .category="UI",
    .descFR="Etat initial d'un Champ Table (EnSaisie : editable ; Grisé : "
            "desactive ; Sélection : selectionnable sans saisie ; "
            "NiSaisieNiSélection : lecture seule).",
    .descEN="Initial state of a Table field (EnSaisie: editable; Grisé: "
            "disabled; Sélection: selectable without editing; "
            "NiSaisieNiSélection: read-only).",
    .values=E_EtatsTableInitial_values, .combinable=false,
};

/* modeLoupe — mode de gestion de la loupe du titre de colonne d'un
 * Champ Table. Deux modes exposes dans cette tranche (miroir du combo « Mode
 * loupe » de l'editeur) : Filtre-contient (masque les lignes hors motif) et
 * Recherche-commence-par (trie + positionne sans masquer). Les valeurs reutilisent
 * les constantes deja enregistrees par les natives voisines (filtreContient =
 * TableActiveFiltre ; rechercheCommencePar = TableCherche) — pas de nouveau
 * sentinel. Le jeu complet des 12 filtre* + rechercheDefaut documente pour la
 * propriete est reporte a une tranche ulterieure (menu contextuel). */
static const char* const E_ModeLoupe_values[] = {
    "filtreContient", "rechercheCommencePar", NULL
};
static const OwlEnum E_ModeLoupe = {
    .name="modeLoupe", .nameAscii="modeLoupe", .nameEN="magnifierMode",
    .category="UI",
    .descFR="Mode de gestion de la loupe du titre de colonne d'un Champ Table : "
            "filtreContient (masque les lignes hors motif) ou rechercheCommencePar "
            "(trie la colonne et positionne sur la 1re ligne « commence par », sans masquer).",
    .descEN="Magnifier handling mode for a Table field column title: filtreContient "
            "(hides rows outside the pattern) or rechercheCommencePar (sorts the column "
            "and positions on the first « starts with » row, without hiding).",
    .values=E_ModeLoupe_values, .combinable=false,
};

/* TableOccurrence info type (canon 3074021). */
static const char* const E_ToInfo_values[] = {
    "toTotal", "toAffich\xc3\xa9""e", "toColonne", "toVisible", NULL
};
const OwlEnum E_ToInfo = {
    .name="toInfo", .nameAscii="toInfo", .nameEN="toInfo", .category="UI",
    .descFR="Type d'occurrence pour TableOccurrence.",
    .descEN="Info type for TableOccurrence.",
    .values=E_ToInfo_values, .combinable=false
};

/* TableSelect info type (canon 3074020). */
static const char* const E_TsInfo_values[] = {
    "tsLigne", "tsColonne", "tsLigneCellule", "tsColonneCellule", NULL
};
const OwlEnum E_TsInfo = {
    .name="tsInfo", .nameAscii="tsInfo", .nameEN="tsInfo", .category="UI",
    .descFR="Type d'info pour TableSelect.",
    .descEN="Info type for TableSelect.",
    .values=E_TsInfo_values, .combinable=false
};

/* TableInfoXY info type (canon 3074007). */
static const char* const E_TiXY_values[] = {
    "tiNumLigne", "tiNumColonne", "tiNomColonne", "tiOrigineEcran", NULL
};
const OwlEnum E_TiXY = {
    .name="tiInfoXY", .nameAscii="tiInfoXY", .nameEN="tiInfoXY", .category="UI",
    .descFR="Type d'info pour TableInfoXY.",
    .descEN="Info type for TableInfoXY.",
    .values=E_TiXY_values, .combinable=false
};

/* selModeAffichage — modes d'affichage d'un Champ Selecteur.
 * 4 valeurs canon : standard, avec sous-libelles, look Vista (image
 * par option), look Vista avec sous-libelles. */
static const char* const E_SelMode_values[] = {
    "selStandard", "selSousLib", "selVista", "selVistaSousLib", NULL
};
static const OwlEnum E_SelMode = {
    .name="selModeAffichage", .nameAscii="selModeAffichage", .nameEN="selectorDisplayMode", .category="UI",
    .descFR="Mode d'affichage du Champ Selecteur (Standard / SousLib / Vista / VistaSousLib).",
    .descEN="Selecteur display mode (Standard / SousLib / Vista / VistaSousLib).",
    .values=E_SelMode_values, .combinable=false
};

/* zrSens — sens de repetition d'un Champ ZoneRepetee.
 * Vertical seul = repetition vers le bas, hauteur du template fait foi.
 * Sentinel-zone (aligne sur orientationsTable : une seule valeur). */
static const char* const E_ZrSens_values[] = {
    "zrVertical", NULL
};
static const OwlEnum E_ZrSens = {
    .name="zrSens", .nameAscii="zrSens", .nameEN="repeatDirection", .category="UI",
    .descFR="Sens de repetition du Champ ZoneRepetee.",
    .descEN="Repetition direction of the Looper field.",
    .values=E_ZrSens_values, .combinable=false
};

/* ActionSouris — action utilisateur simulee par TestSimuleSouris.
 * 9 valeurs : 4 actions sans coords (clic/dclic G/D) consommees par la
 * forme TestSimuleSouris(champ, action), et 5 actions avec coords
 * (bouton G/D, survol, molette haut/bas) consommees par la surcharge
 * TestSimuleSouris(champ, action, x, y). Sentinel-zone.
 * Expose via extern (owl_stdlib.h) pour stdlib_ui.c::p_test_simule_souris. */
static const char* const E_ActionSouris_values[] = {
    "ClicGauche", "ClicDroit", "DoubleClicGauche", "DoubleClicDroit",
    "BoutonGauche", "BoutonDroit", "SurvolSouris", "MoletteHaut", "MoletteBas",
    NULL
};
const OwlEnum E_ActionSouris = {
    .name="actionSouris", .nameAscii="actionSouris", .nameEN="mouseAction", .category="Test",
    .descFR="Action souris simulee par TestSimuleSouris. Clics/double-clics (sans coords) ou actions a coordonnees (bouton press, survol, molette).",
    .descEN="Mouse action simulated by TestSimuleSouris. Clicks/double-clicks (no coords) or coordinate-based actions (button press, hover, wheel).",
    .values=E_ActionSouris_values, .combinable=false
};

/* PoliceEtirement — etirement horizontal des caracteres (apparence police).
 * Enum sentinel-zone, mutuellement exclusif : un champ a UN etirement, pas
 * un cumul. etirNormal = etat par defaut. Expose via extern (owl_stdlib.h)
 * pour la prop de champ PoliceEtirement (allowedBase) et le cablage runtime.
 * Divergence de modelisation assumee (ADR) : un seul reglage deterministe
 * remplace plusieurs interrupteurs separes incompatibles entre eux. */
static const char* const E_PoliceEtirement_values[] = {
    "etirNormal", "etirCondens\xc3\xa9", "etir\xc3\x89tendu", "etirLarge", NULL
};
const OwlEnum E_PoliceEtirement = {
    .name="Police\xc3\x89tirement", .nameAscii="PoliceEtirement", .nameEN="fontStretch", .category="UI",
    .descFR="Etirement horizontal des caracteres (etirNormal, etirCondense, etirEtendu, etirLarge).",
    .descEN="Horizontal stretch of characters (etirNormal, etirCondense, etirEtendu, etirLarge).",
    .values=E_PoliceEtirement_values, .combinable=false
};

/* td* — ListeDeplaceLigne / TableDeplaceLigne (canon).
 *
 * Split en 2 enums car les 2 sous-familles ont des semantiques opposees :
 *
 *  - tdOperation (combinable=true, bitmask 0/1/2) : <Operation>
 *    tdDeplace + tdEchange + tdAfficheDestination — OR-able.
 *
 *  - tdCibleRelative (combinable=false, sentinel-zone) : <Destination>
 *    tdPremier, tdDernier, tdSuivant, ... — choix EXCLUSIF. Note : le
 *    param Destination accepte aussi un entier 1-based libre, donc le
 *    cablage du LSP utilisera acceptsLiterals=true (cf C.3).
 */
static const char* const E_tdOperation_values[] = {
    "tdD\xc3\xa9place", "tdEchange", "tdAfficheDestination", NULL
};
const OwlEnum E_tdOperation = {
    .name="tdOperation", .nameAscii="tdOperation", .nameEN="tdOperation", .category="UI",
    .descFR="Operations combinables pour ListeDeplaceLigne / TableDeplaceLigne.",
    .descEN="Combinable operations for ListMoveLine / TableMoveLine.",
    .values=E_tdOperation_values, .combinable=true
};

static const char* const E_tdCibleRelative_values[] = {
    "tdPremier", "tdDernier", "tdPr\xc3\xa9""c\xc3\xa9""dent", "tdSuivant",
    "tdPagePr\xc3\xa9""c\xc3\xa9""dente", "tdPageSuivante", NULL
};
const OwlEnum E_tdCibleRelative = {
    .name="tdCibleRelative", .nameAscii="tdCibleRelative", .nameEN="tdRelativeTarget", .category="UI",
    .descFR="Cibles relatives mutuellement exclusives pour <Indice destination> de ListeDeplaceLigne / TableDeplaceLigne.",
    .descEN="Mutually exclusive relative targets for <DestinationIndex> of ListMoveLine / TableMoveLine.",
    .values=E_tdCibleRelative_values, .combinable=false
};

/* TablePosition option (canon 3074022). */
static const char* const E_TaInfo_values[] = {
    "taPremier", "taVisible", NULL
};
const OwlEnum E_TaInfo = {
    .name="taInfo", .nameAscii="taInfo", .nameEN="taInfo", .category="UI",
    .descFR="Option de positionnement pour TablePosition.",
    .descEN="Position option for TablePosition.",
    .values=E_TaInfo_values, .combinable=false
};

/* iDestination — destination d'impression (canon).
 * Seules les formes canoniques (accentuées) apparaissent dans l'enum :
 * les alias ASCII (iPDFGenerique, etc.) sont auto-acceptés au runtime
 * via le scanner-strip + register_consts auto-alias, donc inutiles ici
 * — leur présence dans la liste polluerait l'autocomplete LSP. */
static const char* const E_IDest_values[] = {
    "iImprimante", "iPDFG\xc3\xa9n\xc3\xa9rique", "iPDF",
    "iImageM\xc3\xa9moire", NULL
};
const OwlEnum E_IDest = {
    .name="iDestination", .nameAscii="iDestination", .nameEN="iDestination", .category="Etats",
    .descFR="Destination de l'impression.",
    .descEN="Print output destination.",
    .values=E_IDest_values, .combinable=false
};

/* iCreePolice — style (combinable bitwise). Forme canon uniquement, alias
 * ASCII auto au runtime (cf E_IDest). */
static const char* const E_IStyle_values[] = {
    "iNormal", "iGras", "iItalique", "iSoulign\xc3\xa9",
    "iBarr\xc3\xa9", NULL
};
const OwlEnum E_IStyle = {
    .name="iStyle", .nameAscii="iStyle", .nameEN="iFontStyle", .category="Etats",
    .descFR="Style de police pour iCreePolice (combinable).",
    .descEN="Font style for iCreateFont (combinable).",
    .values=E_IStyle_values, .combinable=true
};

/* iCreePolice — famille de police. */
static const char* const E_IFamily_values[] = {
    "iD\xc3\xa9""faut", "iRoman", "iSwiss", "iCourier",
    "iPoliceLiberationSans", "iPoliceLiberationSerif",
    "iPoliceLiberationMono", "iPoliceDejaVuSans",
    /* Aliases Microsoft (rendus via map_legacy_family). */
    "iPoliceArial", "iPoliceTimesNewRoman", "iPoliceCourierNew",
    "iPoliceVerdana", "iPoliceTahoma", "iPoliceCalibri",
    "iPoliceComicSansMS", "iPoliceGeorgia", "iPoliceTrebuchetMS",
    "iPoliceHelvetica",
    NULL
};
const OwlEnum E_IFamily = {
    .name="iFamille", .nameAscii="iFamille", .nameEN="iFontFamily", .category="Etats",
    .descFR="Famille de police (alternative au nom string).",
    .descEN="Font family (alternative to string name).",
    .values=E_IFamily_values, .combinable=false
};

/* Resout une sentinelle de famille iPolice* / iSwiss / iRoman / iCourier vers
 * la fonte EMBARQUEE equivalente (assets/fonts.qrc, enregistree cote UI et
 * PDF). But : rendu deterministe sans dependre d'une fonte systeme. Les alias
 * Microsoft (Arial, Calibri, ...) pointent vers la fonte libre de metriques
 * compatibles. Retourne :
 *   - le nom de famille embarquee pour une sentinelle connue,
 *   - "" pour iDefaut (garde la police heritee / defaut applicatif),
 *   - NULL si `v` n'est pas une sentinelle iFamille (chaine libre a laisser
 *     passer telle quelle par l'appelant). */
const char* owl_ui_ipol_embedded_family(long long v) {
    switch (v) {
        case OWL_IPOL_DEFAUT:
            return "";
        case OWL_IPOL_SWISS:
        case OWL_IPOL_LIBERATION_SANS:
        case OWL_IPOL_ARIAL:
        case OWL_IPOL_VERDANA:
        case OWL_IPOL_TAHOMA:
        case OWL_IPOL_CALIBRI:
        /*  — iPoliceComicSansMS resout ici vers Liberation Sans, qui est
         * AUSSI la police applicative par defaut (cf. QApplication::setFont
         * "Liberation Sans" 10pt). Consequence : poser iPoliceComicSansMS sur un
         * widget deja en defaut ne change RIEN de visible (meme famille) -> ce
         * n'est PAS un bug de rendu, c'est le substitut = defaut. Un vrai Comic
         * Sans n'est pas embarque (pas de fonte libre equivalente vendoree). */
        case OWL_IPOL_COMIC_SANS_MS:
        case OWL_IPOL_TREBUCHET_MS:
        case OWL_IPOL_HELVETICA:
            return "Liberation Sans";
        case OWL_IPOL_ROMAN:
        case OWL_IPOL_LIBERATION_SERIF:
        case OWL_IPOL_TIMES_NEW_ROMAN:
        case OWL_IPOL_GEORGIA:
            return "Liberation Serif";
        case OWL_IPOL_COURIER:
        case OWL_IPOL_LIBERATION_MONO:
        case OWL_IPOL_COURIER_NEW:
            return "Liberation Mono";
        case OWL_IPOL_DEJAVU_SANS:
            return "DejaVu Sans";
        default:
            return NULL;
    }
}

/* iParametrePDF.Protection (combinaison Format + Protection). Reuse les
 * constantes iPDFA et iProtection deja exposees. Pas un nouvel enum canon
 * canon, mais un pseudo-enum union pour l'allowedBase du champ Protection. */
static const char* const E_pdfProtection_values[] = {
    "iPDFA", "iPDFA1b", "iPDFA3b",
    "iProtectionAucune", "iProtectionImpression", "iProtectionModification",
    "iProtectionS\xc3\xa9lection", "iProtectionCommentaire",
    NULL
};
const OwlEnum E_pdfProtection = {
    .name="pdfProtection", .nameAscii="pdfProtection", .nameEN="pdfProtection", .category="PDF",
    .descFR="Format + protection du PDF (combinable).",
    .descEN="PDF format + protection (combinable).",
    .values=E_pdfProtection_values, .combinable=true
};

/* iLigne / iCadre — style de trait. */
static const char* const E_ITrait_values[] = {
    "iContinu", "iTiret", "iPointill\xc3\xa9", "iMixte", NULL
};
const OwlEnum E_ITrait = {
    .name="iTrait", .nameAscii="iTrait", .nameEN="iLineStyle", .category="Etats",
    .descFR="Style de trait pour iLigne / iCadre / iEllipse.",
    .descEN="Stroke style for iLigne / iCadre / iEllipse.",
    .values=E_ITrait_values, .combinable=false
};

/* iCadre — brosse de remplissage. */
static const char* const E_IBrosse_values[] = {
    "iPleine", "iHorizontale", "iVerticale", "iCrois\xc3\xa9""e",
    "iDiagonaleDescendante", "iDiagonaleMontante",
    "iCrois\xc3\xa9""eDiagonale", NULL
};
const OwlEnum E_IBrosse = {
    .name="iBrosse", .nameAscii="iBrosse", .nameEN="iBrush", .category="Etats",
    .descFR="Brosse de remplissage pour iCadre / iEllipse.",
    .descEN="Fill brush for iCadre / iEllipse.",
    .values=E_IBrosse_values, .combinable=false
};

/* iCadre — bordure visible. */
static const char* const E_IBord_values[] = {
    "iBordTous", "iBordHaut", "iBordBas",
    "iBordGauche", "iBordDroit", "iBordAucun", NULL
};
const OwlEnum E_IBord = {
    .name="iBord", .nameAscii="iBord", .nameEN="iBorder", .category="Etats",
    .descFR="Bordure visible pour iCadre.",
    .descEN="Visible border for iCadre.",
    .values=E_IBord_values, .combinable=false
};

/* iImprimeZone — alignement (combinable horizontal + vertical via +). */
static const char* const E_IAlign_values[] = {
    "iGauche", "iDroite", "iCentreH",
    "iHaut", "iBas", "iCentreV", NULL
};
const OwlEnum E_IAlign = {
    .name="iAlignement", .nameAscii="iAlignement", .nameEN="iAlignment", .category="Etats",
    .descFR="Alignement texte dans iImprimeZone (combinable iCentreH + iCentreV).",
    .descEN="Text alignment for iImprimeZone (combinable iCentreH + iCentreV).",
    .values=E_IAlign_values, .combinable=true
};

/* iImprimeImage — echelle / mode affichage. */
static const char* const E_IEchelle_values[] = {
    "iAffichageNormal", "iAffichageHomoth\xc3\xa9tique",
    "iAffichageHomoCentr\xc3\xa9", NULL
};
const OwlEnum E_IEchelle = {
    .name="iEchelle", .nameAscii="iEchelle", .nameEN="iScale", .category="Etats",
    .descFR="Mode d'affichage pour iImprimeImage (Normal etire / Homothetique proportionnel / HomoCentre proportionnel centre).",
    .descEN="Display mode for iPrintImage (Normal stretch / Homothetic proportional / HomoCenter proportional centered).",
    .values=E_IEchelle_values, .combinable=false
};

/* iInfoImprimante — type d'information demandee. */
static const char* const E_IInfoImpr_values[] = {
    "iNomImprimante", "iNomDriver", "iNomPort", NULL
};
const OwlEnum E_IInfoImpr = {
    .name="iInfoImprimante", .nameAscii="iInfoImprimante", .nameEN="iPrinterInfo", .category="Etats",
    .descFR="Type d'information demande a iInfoImprimante.",
    .descEN="Information type requested from iInfoPrinter.",
    .values=E_IInfoImpr_values, .combinable=false
};

/* DSL Etat V2 — Format de page (propriete Etat..Format). */
static const char* const E_EtatFormat_values[] = {
    "formatA3", "formatA4", "formatA5", "formatLetter", "formatLegal", NULL
};
const OwlEnum E_EtatFormat = {
    .name="etatFormat", .nameAscii="etatFormat", .nameEN="reportFormat", .category="Etats",
    .descFR="Format de page de l'État (A3, A4, A5, Letter, Legal).",
    .descEN="Report page format (A3, A4, A5, Letter, Legal).",
    .values=E_EtatFormat_values, .combinable=false
};

/* DSL Etat V2 — Orientation (propriete Etat..Orientation). */
static const char* const E_EtatOrient_values[] = {
    "orientPortrait", "orientPaysage", NULL
};
const OwlEnum E_EtatOrient = {
    .name="etatOrientation", .nameAscii="etatOrientation", .nameEN="reportOrientation", .category="Etats",
    .descFR="Orientation de l'État (Portrait par defaut, Paysage).",
    .descEN="Report orientation (Portrait by default, Landscape).",
    .values=E_EtatOrient_values, .combinable=false
};

/* TableActiveFiltre type (canon 1000017149). */
static const char* const E_FiltreType_values[] = {
    "filtreContient", "filtreCommencePar", "filtreTerminePar", "filtreEgal",
    "filtreDiff\xc3\xa9rent",
    "filtreInf\xc3\xa9rieur",
    "filtreInf\xc3\xa9rieurOuEgal",
    "filtreSup\xc3\xa9rieur",
    "filtreSup\xc3\xa9rieurOuEgal",
    "filtreNeContientPas", "filtreNeCommencePasPar", "filtreNeTerminePasPar",
    NULL
};
const OwlEnum E_FiltreType = {
    .name="filtreType", .nameAscii="filtreType", .nameEN="filterType", .category="UI",
    .descFR="Type de filtre pour TableActiveFiltre.",
    .descEN="Filter type for TableActiveFiltre.",
    .values=E_FiltreType_values, .combinable=false
};

#define C_UI(nm, val, dfr, den) {nm, "UI", INTEGER_VAL(val), dfr, den}
/* Variante avec nameEN explicite (invariant bilingue n°3). L'alias EN
 * runtime est auto-genere par owl_registry (Lot 0) ; poser .nameEN suffit. */
#define C_UI_EN(nm, val, dfr, den, en) {nm, "UI", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef UI_CONSTS[] = {
    /* TypeBouton — canon : btn* (etait TB* en V1, renommé pour conformite). */
    C_UI_EN("btnNormal",       OWL_BTN_NORMAL,       "Bouton standard.", "Standard button.", "btnNormal"),
    C_UI_EN("btnValider",      OWL_BTN_VALIDER,      "Bouton de validation (Entree).", "Validation button (Enter).", "btnValidate"),
    C_UI_EN("btnInterruption", OWL_BTN_INTERRUPTION, "Bouton interruption.", "Interrupt button.", "btnInterrupt"),
    C_UI_EN("btnAbandon",      OWL_BTN_ABANDON,      "Bouton d'annulation (Echap).", "Cancel button (Escape).", "btnAbort"),
    C_UI_EN("btnAide",         OWL_BTN_AIDE,         "Bouton d'aide (F1).", "Help button (F1).", "btnHelp"),
    /* PositionImageBouton ( lot B) — feuille Style..Image..Position. */
    C_UI_EN("posBtnImageGauche", OWL_POSBTNIMG_GAUCHE,     "Icone a gauche du texte (defaut).", "Icon left of the text (default).", "posBtnImageLeft"),
    C_UI_EN("posBtnImageDroite", OWL_POSBTNIMG_DROITE,     "Icone a droite du texte.", "Icon right of the text.", "posBtnImageRight"),
    C_UI_EN("posBtnImageHaut",   OWL_POSBTNIMG_HAUT,       "Icone au-dessus du texte.", "Icon above the text.", "posBtnImageTop"),
    C_UI_EN("posBtnImageBas",    OWL_POSBTNIMG_BAS,        "Icone sous le texte.", "Icon below the text.", "posBtnImageBottom"),
    C_UI_EN("posBtnImageSeule",  OWL_POSBTNIMG_SEULE,      "Icone seule (texte masque).", "Icon only (text hidden).", "posBtnIconOnly"),
    C_UI_EN("posBtnTexteSeul",   OWL_POSBTNIMG_TEXTE_SEUL, "Texte seul (icone masquee).", "Text only (icon hidden).", "posBtnTextOnly"),
    /* TypeLibelle */
    C_UI_EN("TLNormal",       OWL_TL_NORMAL,     "Texte mono-ligne.", "Single-line text.", "TLNormal"),
    C_UI_EN("TLMultiLigne",   OWL_TL_MULTILIGNE, "Texte multi-ligne avec wrap.", "Multi-line text with wrap.", "TLMultiLine"),
    /* formatAffichage (bucket E) */
    C_UI_EN("formatTexte",       OWL_FAFF_TEXTE,       "Texte literal sans interpretation.",
                                  "Literal text without interpretation.", "formatText"),
    C_UI_EN("formatTexteRiche",  OWL_FAFF_TEXTE_RICHE, "Markdown OWL etendu : **gras**, *italique*, [c:rouge]…[/c], [s:14]…[/s], [j:justifie]…[/j].",
                                  "Extended OWL Markdown: **bold**, *italic*, [c:red]…[/c], [s:14]…[/s], [j:justify]…[/j].", "formatRichText"),
    /* TypeSaisie — canon (typSaisie*). */
    C_UI_EN("typSaisieTexte",      OWL_TYPESAISIE_TEXTE,      "Saisie texte standard.", "Standard text input.", "typeInputText"),
    C_UI_EN("typSaisieMotDePasse", OWL_TYPESAISIE_MOTDEPASSE, "Saisie mot de passe (caracteres masques).", "Password input (masked).", "typeInputPassword"),
    C_UI_EN("typSaisieMultiligne", OWL_TYPESAISIE_MULTILIGNE, "Saisie texte multi-ligne.", "Multi-line text input.", "typeInputMultiline"),
    C_UI_EN("typSaisieNum",        OWL_TYPESAISIE_NUM,        "Saisie numerique (entier ou reel, frappe restreinte au numerique).", "Numeric input (integer or real, keystrokes restricted to numbers).", "typeInputNumeric"),
    C_UI_EN("typSaisieMon\xc3\xa9taire", OWL_TYPESAISIE_MONETAIRE, "Saisie monetaire (valeur avec symbole/format devise).", "Currency input (value with currency symbol/format).", "typeInputCurrency"),
    C_UI_EN("typSaisieDate",       OWL_TYPESAISIE_DATE,       "Saisie de date (affichage JJ/MM/AAAA, valeur relue en date).", "Date input (JJ/MM/AAAA display, value read back as a date).", "typeInputDate"),
    C_UI_EN("typSaisieHeure",      OWL_TYPESAISIE_HEURE,      "Saisie d'heure (affichage HH:MI par defaut, valeur relue en heure).", "Time input (HH:MI display by default, value read back as a time).", "typeInputTime"),
    C_UI_EN("typSaisieDur\xc3\xa9" "e", OWL_TYPESAISIE_DUREE,     "Saisie de duree (affichage HH:MM:SS par defaut, valeur relue en duree ; heures non plafonnees a 23).", "Duration input (HH:MM:SS display by default, value read back as a duration; hours not capped at 23).", "typeInputDuration"),
    C_UI_EN("typSaisieNum\xc3\xa9riquePr\xc3\xa9" "cis", OWL_TYPESAISIE_NUM_PRECIS, "Saisie numerique precise (valeur relue en decimal exact, sans arrondi de precision finie).", "Precise numeric input (value read back as an exact decimal, without finite-precision rounding).", "typeInputNumericPrecise"),
    /* CadrageHorizontal */
    /* Disposition (Lot F) — sentinel-zone via OWL_DISP_*. */
    C_UI_EN("dispVerticale",   OWL_DISP_VERTICALE,
         "Empile les enfants verticalement (axe principal = vertical).",
         "Stack children vertically (main axis = vertical).", "dispVertical"),
    C_UI_EN("dispHorizontale", OWL_DISP_HORIZONTALE,
         "Aligne les enfants horizontalement (axe principal = horizontal).",
         "Lay out children horizontally (main axis = horizontal).", "dispHorizontal"),
    C_UI_EN("dispGrille",      OWL_DISP_GRILLE,
         "Grille a Colonnes/Lignes (cellules egales par defaut).",
         "Grid with Colonnes/Lignes (equal cells by default).", "dispGrid"),
    C_UI_EN("dispLibre",       OWL_DISP_LIBRE,
         "Positionnement libre X/Y (les enfants doivent specifier X et Y).",
         "Free X/Y positioning (children must specify X and Y).", "dispFree"),
    /* Alignement (Lot F) — sentinel-zone via OWL_ALIGN_*. */
    /* Position — barre d'onglets d'un Champ Onglet. */
    C_UI_EN("ongHaut",   OWL_POS_HAUT,
         "Barre d'onglets en haut (defaut).", "Tab bar on top (default).", "ongTop"),
    C_UI_EN("ongBas",    OWL_POS_BAS,
         "Barre d'onglets en bas.", "Tab bar at the bottom.", "ongBottom"),
    C_UI_EN("ongGauche", OWL_POS_GAUCHE,
         "Barre d'onglets a gauche.", "Tab bar on the left.", "ongLeft"),
    C_UI_EN("ongDroite", OWL_POS_DROITE,
         "Barre d'onglets a droite.", "Tab bar on the right.", "ongRight"),
    /* LargeurVolets — largeur des languettes d'un Champ Onglet. */
    C_UI_EN("largeurProportionnelle", OWL_LARGVOL_PROPORTIONNELLE,
         "Chaque languette dimensionnee selon son libelle (defaut).",
         "Each tab sized to its own caption (default).", "widthProportional"),
    C_UI_EN("largeur\xc3\x89gale",    OWL_LARGVOL_EGALE,
         "Toutes les languettes a la meme largeur (celle de la plus large).",
         "All tabs at the same width (that of the widest one).", "widthEqual"),
    C_UI_EN("alignD\xc3\xa9""but",  OWL_ALIGN_DEBUT,
         "Aligne au debut de l'axe transverse (haut ou gauche).",
         "Align to start of cross-axis (top or left).", "alignStart"),
    C_UI_EN("alignCentre",          OWL_ALIGN_CENTRE,
         "Centre sur l'axe transverse.",
         "Center on cross-axis.", "alignCenter"),
    C_UI_EN("alignFin",             OWL_ALIGN_FIN,
         "Aligne a la fin de l'axe transverse (bas ou droite).",
         "Align to end of cross-axis (bottom or right).", "alignEnd"),
    C_UI_EN("align\xc3\x89tire",    OWL_ALIGN_ETIRE,
         "Etire pour occuper toute la dimension transverse.",
         "Stretch to fill cross-axis.", "alignStretch"),
    C_UI_EN("chGauche",       OWL_CH_GAUCHE, "Aligne a gauche.", "Left aligned.", "chLeft"),
    C_UI_EN("chCentre",       OWL_CH_CENTRE, "Centre.",          "Centered.", "chCenter"),
    C_UI_EN("chDroite",       OWL_CH_DROITE, "Aligne a droite.", "Right aligned.", "chRight"),
    /* CadrageVertical */
    C_UI_EN("cvHaut",         OWL_CV_HAUT,   "Aligne en haut.",  "Top aligned.", "cvTop"),
    C_UI_EN("cvCentre",       OWL_CV_CENTRE, "Centre.",          "Centered.", "cvCenter"),
    C_UI_EN("cvMilieu",       OWL_CV_CENTRE, "Centre vertical (alias de cvCentre).",
                                          "Vertical middle (alias of cvCentre).", "cvMiddle"),
    C_UI_EN("cvBas",          OWL_CV_BAS,    "Aligne en bas.",   "Bottom aligned.", "cvBottom"),
    /* toastDuree — modes de duree pour ToastAffiche. */
    C_UI_EN("toastCourt",     OWL_TOAST_COURT, "Affichage bref du toast (environ 2 secondes).",
                                            "Short toast display (about 2 seconds).", "toastShort"),
    C_UI_EN("toastLong",      OWL_TOAST_LONG,  "Affichage prolonge du toast (environ 5 secondes).",
                                            "Long toast display (about 5 seconds).", "toastLong"),
    /* Cadrage image (prop Cadrage du Libelle quand il porte une Image). */
    C_UI_EN("cadCentre",       OWL_CADIMG_CENTRE,       "Image affichee a sa taille native, centree, debord coupe.",
                                "Image at native size, centered, overflow clipped.", "cadCenter"),
    C_UI_EN("cadEtire",        OWL_CADIMG_ETIRE,        "Image deformee pour remplir entierement le champ.",
                                "Image stretched to fill the field (aspect ratio not preserved).", "cadStretch"),
    C_UI_EN("cadHomothetique", OWL_CADIMG_HOMOTHETIQUE, "Image scalee en preservant le ratio, centree, bandes autour.",
                                "Image scaled with aspect ratio preserved, centered, letterboxed.", "cadProportional"),
    /* Etat — Actif/AffichageSeulement/Grise/Invisible.
     * AffichageSeulement = Inactif (alias canon, meme valeur sentinel). */
    C_UI_EN("Actif",              OWL_ETAT_ACTIF,          "Champ actif.",                        "Active.", "Active"),
    C_UI_EN("AffichageSeulement", OWL_ETAT_AFFICHAGE_SEUL, "Champ en affichage seul (R-only).",   "Display-only.", "DisplayOnly"),
    C_UI_EN("Inactif",            OWL_ETAT_AFFICHAGE_SEUL, "Alias d'AffichageSeulement.",         "Alias of DisplayOnly.", "Inactive"),
    C_UI_EN("Gris\xc3\xa9",       OWL_ETAT_GRISE,          "Champ grise (non interactif).",       "Grayed.", "Grayed"),
    C_UI_EN("Invisible",          OWL_ETAT_INVISIBLE,      "Champ invisible.",                    "Invisible.", "Invisible"),
    C_UI_EN("AffichageSansS\xc3\xa9lection", OWL_ETAT_AFFICHAGE_SANS_SELECTION,
            "Champ Table affiche, aucune selection utilisateur ; la selection par programmation reste possible.",
            "Table field displayed, no user selection; programmatic selection stays possible.",
            "DisplayWithoutSelection"),
    /* Cadre legacy enum (cadreAucun/cadreSimple/cadre3DEnfonce/cadre3DSorti)
     * supprime — utiliser le type compose Cadre (canon). */
    /* CurseurSouris (canon). 14 formes. Indices contigus 0..13
     * exposes uniquement vers les setters runtime ; le mapping vers Qt::CursorShape
     * est fait cote ui_qt.cpp. Garder l'ordre stable (depend de l'index). */
    C_UI_EN("curFl\xc3\xa8""che",   OWL_CUR_FLECHE,        "Fleche standard (defaut).",          "Default arrow.",        "curArrow"),
    C_UI_EN("curMain",              OWL_CUR_MAIN,          "Main pointeur (lien cliquable).",    "Pointing hand.",        "curHand"),
    C_UI_EN("curMainSyst\xc3\xa8me",OWL_CUR_MAIN_SYSTEME,  "Main systeme (drag).",                "System hand (drag).",  "curSystemHand"),
    C_UI_EN("curCaret",             OWL_CUR_CARET,         "Curseur de saisie (I-beam).",        "Text I-beam cursor.",   "curCaret"),
    C_UI_EN("curCroix",             OWL_CUR_CROIX,         "Croix standard.",                    "Crosshair.",            "curCross"),
    C_UI_EN("curSablier",           OWL_CUR_SABLIER,       "Sablier (attente).",                 "Hourglass (busy).",     "curWait"),
    C_UI_EN("curAide",              OWL_CUR_AIDE,          "Aide contextuelle (?).",             "What's this help.",     "curHelp"),
    C_UI_EN("curInterdit",          OWL_CUR_INTERDIT,      "Signe interdit.",                    "Forbidden.",            "curForbidden"),
    C_UI_EN("curHFl\xc3\xa8""che",  OWL_CUR_HFLECHE,       "Fleche vers le haut.",               "Up arrow.",             "curUpArrow"),
    C_UI_EN("curTaille",            OWL_CUR_TAILLE,        "Fleche en croix (resize 4 sens).",    "Size all (4-way).",    "curSize"),
    C_UI_EN("curTailleNS",          OWL_CUR_TAILLE_NS,     "Resize Nord-Sud.",                    "Size vertical (NS).",   "curSizeNS"),
    C_UI_EN("curTailleOE",          OWL_CUR_TAILLE_OE,     "Resize Ouest-Est.",                   "Size horizontal (OE).", "curSizeWE"),
    C_UI_EN("curTailleNESO",        OWL_CUR_TAILLE_NESO,   "Resize Nord-Est / Sud-Ouest.",        "Size NE-SW (back diag).","curSizeNESW"),
    C_UI_EN("curTailleNOSE",        OWL_CUR_TAILLE_NOSE,   "Resize Nord-Ouest / Sud-Est.",        "Size NW-SE (fwd diag).", "curSizeNWSE"),
    /* Ellipse (canon). */
    C_UI_EN("ellipseInactive",  OWL_ELL_INACTIVE, "Pas d'ellipse (defaut).",       "No ellipsis (default).", "ellipseNone"),
    C_UI_EN("ellipseFin",       OWL_ELL_FIN,      "Caracteres '...' a la fin.",    "'...' at end.", "ellipseEnd"),
    C_UI_EN("ellipseChemin",    OWL_ELL_CHEMIN,   "Caracteres '...' au milieu.",   "'...' in middle (path-style).", "ellipsePath"),
    /* sensEcriture — partage avec stdlib_i18n.c (LangueSensEcriture). */
    C_UI_EN("sensD\xc3\xa9""faut",  OWL_SE_DEFAUT,        "Sens d'ecriture par defaut (langue projet).",
                                  "Default text direction (project language).", "sensDefault"),
    C_UI_EN("sensGaucheDroite",     OWL_SE_GAUCHE_DROITE, "Sens d'ecriture gauche-a-droite (LTR).",
                                  "Left-to-right writing direction (LTR).", "sensLeftRight"),
    C_UI_EN("sensDroiteGauche",     OWL_SE_DROITE_GAUCHE, "Sens d'ecriture droite-a-gauche (RTL).",
                                  "Right-to-left writing direction (RTL).", "sensRightLeft"),
    /* TraitCadre — style du trait (canon, 13 valeurs). V1 :
     * indices stables 0..12, mapping Qt::PenStyle dans ui_qt.cpp.
     * Variantes "Moyen"/"Epais" partagent le meme PenStyle, l'epaisseur change
     * ailleurs (prop Cadre..Epaisseur). */
    /* OWL : 5 styles directs CSS Qt6 (solid/none/dashed/dotted/double).
     * Variantes canon Moyen/Epais/Mixte* retirees — divergence assumee. */
    C_UI_EN("TraitContinu",               OWL_TC_CONTINU,    "Trait continu (defaut, mappe CSS solid).",
                                             "Solid line (default).", "LineSolid"),
    C_UI_EN("TraitAucun",                 OWL_TC_AUCUN,      "Pas de trait (cadre invisible).",
                                             "No line (invisible border).", "LineNone"),
    C_UI_EN("TraitTiret",                 OWL_TC_TIRET,      "Trait en tirets (CSS dashed).",
                                             "Dashed line.", "LineDash"),
    C_UI_EN("TraitPointill\xc3\xa9",      OWL_TC_POINTILLE,  "Trait en pointilles (CSS dotted).",
                                             "Dotted line.", "LineDotted"),
    C_UI_EN("TraitDouble",                OWL_TC_DOUBLE,     "Trait double (CSS double).",
                                             "Double line.", "LineDouble"),
    /* typeDégradé — type de remplissage dégradé d'un CadreExtérieur. */
    C_UI_EN("d\xc3\xa9grad\xc3\xa9""Aucun",   OWL_GRAD_AUCUN,    "Pas de degrade (defaut, le fond uni reste).",
                                             "No gradient (default, flat fill remains).", "gradientNone"),
    C_UI_EN("d\xc3\xa9grad\xc3\xa9""Lin\xc3\xa9""aire", OWL_GRAD_LINEAIRE, "Degrade lineaire directionnel (selon l'angle).",
                                             "Directional linear gradient (angle-driven).", "gradientLinear"),
    C_UI_EN("d\xc3\xa9grad\xc3\xa9Radial",    OWL_GRAD_RADIAL,   "Degrade radial centre (angle ignore).",
                                             "Centered radial gradient (angle ignored).", "gradientRadial"),
    /* BordsCadre — combinable bitwise (canon). */
    C_UI_EN("BordAucun",     0, "Aucun bord.",                "No edges.", "BorderNone"),
    C_UI_EN("BordHaut",      1, "Bord haut.",                 "Top edge.", "BorderTop"),
    C_UI_EN("BordDroite",    2, "Bord droit.",                "Right edge.", "BorderRight"),
    C_UI_EN("BordBas",       4, "Bord bas.",                  "Bottom edge.", "BorderBottom"),
    C_UI_EN("BordGauche",    8, "Bord gauche.",               "Left edge.", "BorderLeft"),
    C_UI_EN("BordTous",     15, "Tous les bords (defaut).",   "All edges (default).", "BorderAll"),
    /* Confirmer / OuiNon - retours et defauts. Aligne doc 3021005/3021007. */
    C_UI_EN("Non",            0, "Reponse Non (et fermeture par X de OuiNon).", "No answer (and X-close in YesNo).", "No"),
    C_UI_EN("Oui",            1, "Reponse Oui.", "Yes answer.", "Yes"),
    C_UI_EN("Annuler",        2, "Reponse Annuler (Confirmer uniquement).", "Cancel answer (Confirm only).", "Cancel"),
    /* colTypes — sentinel zone (cf sentinels.h). */
    C_UI_EN("colTypeTexte",      OWL_COL_TYPE_TEXTE,     "Colonne de type chaine.",   "Text column.",    "colTypeText"),
    C_UI_EN("colTypeNumerique",  OWL_COL_TYPE_NUMERIQUE, "Colonne entiere ou reelle.","Numeric column.", "colTypeNumeric"),
    C_UI_EN("colTypeMon\xc3\xa9taire", OWL_COL_TYPE_MONETAIRE, "Colonne monetaire.",   "Currency column.","colTypeCurrency"),
    C_UI_EN("colTypeDate",       OWL_COL_TYPE_DATE,      "Colonne date.",             "Date column.",    "colTypeDate"),
    C_UI_EN("colTypeHeure",      OWL_COL_TYPE_HEURE,     "Colonne heure.",            "Time column.",    "colTypeTime"),
    C_UI_EN("colTypeDur\xc3\xa9""e", OWL_COL_TYPE_DUREE, "Colonne duree.",            "Duration column.","colTypeDuration"),
    C_UI_EN("colTypeBooleen",    OWL_COL_TYPE_BOOLEEN,   "Colonne booleen (case).",   "Boolean (checkbox) column.", "colTypeBoolean"),
    C_UI_EN("colTypeMotDePasse", OWL_COL_TYPE_MOTDEPASSE, "Colonne mot de passe : affichage masque, saisie a echo masque.", "Password column: masked display, masked-echo entry.", "colTypePassword"),

    /* orientationsTable — orientation d'un Champ Table. */
    C_UI_EN("Vertical",          OWL_ORIENT_TABLE_VERTICAL, "Lignes empilees verticalement (defaut).", "Rows stacked vertically (default).", "Vertical"),
    /* etatsTableInitial — etat initial d'un Champ Table. Grisé
     * reutilise le const de l'enum etat (meme sentinel OWL_ETAT_GRISE). */
    C_UI_EN("EnSaisie",              OWL_ETAT_TABLE_EN_SAISIE, "Table editable (defaut).", "Editable table (default).", "InEdit"),
    C_UI_EN("S\xc3\xa9lection",      OWL_ETAT_TABLE_SELECTION, "Table selectionnable sans saisie.", "Selectable table without editing.", "Selection"),
    C_UI_EN("NiSaisieNiS\xc3\xa9lection", OWL_ETAT_TABLE_NI_NI, "Table en lecture seule (ni saisie ni selection).", "Read-only table (no editing, no selection).", "NeitherEditNorSelect"),

    /* TableOccurrence — info type (canon 3074021). */
    C_UI_EN("toTotal",         OWL_TO_TOTAL,    "Nombre total de lignes (defaut).", "Total row count (default).", "toTotal"),
    C_UI_EN("toAffich\xc3\xa9""e", OWL_TO_AFFICHEE, "Nombre de lignes non vides visibles.", "Non-empty visible rows.", "toDisplayed"),
    C_UI_EN("toColonne",       OWL_TO_COLONNE,  "Nombre de colonnes.", "Column count.", "toColumn"),
    C_UI_EN("toVisible",       OWL_TO_VISIBLE,  "Nombre de lignes visibles (cadre).", "Visible row count (slot).", "toVisible"),

    /* TableSelect — info type (canon 3074020). */
    C_UI_EN("tsLigne",         OWL_TS_LIGNE,           "Numero de la ligne selectionnee (defaut).", "Selected row number (default).", "tsRow"),
    C_UI_EN("tsColonne",       OWL_TS_COLONNE,         "Numero de la colonne selectionnee.", "Selected column number.", "tsColumn"),
    C_UI_EN("tsLigneCellule",  OWL_TS_LIGNE_CELLULE,   "Numero de ligne pour la cellule selectionnee.", "Selected cell row.", "tsRowCell"),
    C_UI_EN("tsColonneCellule",OWL_TS_COLONNE_CELLULE, "Numero de colonne pour la cellule selectionnee.", "Selected cell column.", "tsColumnCell"),

    /* TableInfoXY — info type (canon 3074007). */
    C_UI_EN("tiNumLigne",      OWL_TIXY_NUM_LIGNE,    "Numero de ligne au point (X,Y).", "Row number at point (X,Y).", "tiRowNum"),
    C_UI_EN("tiNumColonne",    OWL_TIXY_NUM_COLONNE,  "Numero (creation) de colonne au point.", "Column index at point.", "tiColumnNum"),
    C_UI_EN("tiNomColonne",    OWL_TIXY_NOM_COLONNE,  "Nom de la colonne au point.", "Column name at point.", "tiColumnName"),
    C_UI_EN("tiOrigineEcran",  OWL_TIXY_ORIGINE_ECRAN,"Coords par rapport a l'ecran (combinable).", "Screen-relative coords (combinable).", "tiScreenOrigin"),

    /* TablePosition — option (canon 3074022). */
    C_UI_EN("taPremier",       OWL_TA_PREMIER,  "Affiche la ligne en premier (defaut).", "Show row first (default).", "taFirst"),
    C_UI_EN("taVisible",       OWL_TA_VISIBLE,  "Rend la ligne visible (au plus pres).", "Make row visible (closest).", "taVisible"),

    /* ListeDeplaceLigne / TableDeplaceLigne — operations + cibles relatives (canon). */
    C_UI_EN("tdD\xc3\xa9place",       OWL_TD_DEPLACE,    "Deplace la ligne vers la destination (defaut).", "Move row to destination (default).", "tdMove"),
    C_UI_EN("tdEchange",              OWL_TD_ECHANGE,    "Echange source et destination.",                  "Swap source and destination.", "tdSwap"),
    C_UI_EN("tdAfficheDestination",   OWL_TD_AFFICHE_DESTINATION,
         "Force l'affichage de la ligne deplacee (combinable).",
         "Force scrolling to make moved row visible (combinable).", "tdShowDestination"),
    C_UI_EN("tdPremier",              OWL_TD_PREMIER,    "Deplace en premiere position.", "Move to first position.", "tdFirst"),
    C_UI_EN("tdDernier",              OWL_TD_DERNIER,    "Deplace en derniere position.", "Move to last position.", "tdLast"),
    C_UI_EN("tdPr\xc3\xa9""c\xc3\xa9""dent", OWL_TD_PRECEDENT, "Deplace d'une position vers le haut.", "Move one position up.", "tdPrevious"),
    C_UI_EN("tdSuivant",              OWL_TD_SUIVANT,    "Deplace d'une position vers le bas.", "Move one position down.", "tdNext"),
    C_UI_EN("tdPagePr\xc3\xa9""c\xc3\xa9""dente", OWL_TD_PAGE_PRECEDENTE, "Deplace d'une page vers le haut.", "Move one page up.", "tdPreviousPage"),
    C_UI_EN("tdPageSuivante",         OWL_TD_PAGE_SUIVANTE, "Deplace d'une page vers le bas.", "Move one page down.", "tdNextPage"),
    C_UI_EN("ligneS\xc3\xa9lectionn\xc3\xa9""e", OWL_LIGNE_SELECTIONNEE,
         "Reference la ligne actuellement selectionnee (en source ou destination).",
         "References the currently selected row (as source or destination).", "selectedRow"),

    /* selModeAffichage — Champ Selecteur (canon). */
    C_UI_EN("selStandard",         OWL_SELMODE_STANDARD,
         "Affichage standard : radio buttons + libelles seuls.",
         "Standard display: radio buttons + labels only.", "selStandard"),
    C_UI_EN("selSousLib",          OWL_SELMODE_SOUSLIB,
         "Standard + sous-libelle (texte secondaire sous chaque option).",
         "Standard + sub-label (secondary text below each option).", "selSubCaption"),
    C_UI_EN("selVista",            OWL_SELMODE_VISTA,
         "Look Vista : icone par option a gauche du libelle.",
         "Vista look: icon per option to the left of the label.", "selVista"),
    C_UI_EN("selVistaSousLib",     OWL_SELMODE_VISTA_SOUSLIB,
         "Look Vista avec icone + sous-libelle.",
         "Vista look with icon + sub-label.", "selVistaSubCaption"),

    /* zrSens — Champ ZoneRepetee (Vertical seul). */
    C_UI_EN("zrVertical",   OWL_ZRSENS_VERTICAL,
         "Repetition verticale. La hauteur du template fait foi.",
         "Vertical repetition. Template height drives layout.", "zrVertical"),

    /* ActionSouris — action simulee par TestSimuleSouris. */
    C_UI_EN("ClicGauche",       OWL_ACTION_SOURIS_CLIC_G,
         "Clic souris bouton gauche (simule par TestSimuleSouris).",
         "Left mouse button click (simulated by TestSimuleSouris).", "LeftClick"),
    C_UI_EN("ClicDroit",        OWL_ACTION_SOURIS_CLIC_D,
         "Clic souris bouton droit (simule par TestSimuleSouris).",
         "Right mouse button click (simulated by TestSimuleSouris).", "RightClick"),
    C_UI_EN("DoubleClicGauche", OWL_ACTION_SOURIS_DCLIC_G,
         "Double-clic souris bouton gauche (simule par TestSimuleSouris).",
         "Left mouse button double-click (simulated by TestSimuleSouris).", "LeftDoubleClick"),
    C_UI_EN("DoubleClicDroit",  OWL_ACTION_SOURIS_DCLIC_D,
         "Double-clic souris bouton droit (simule par TestSimuleSouris).",
         "Right mouse button double-click (simulated by TestSimuleSouris).", "RightDoubleClick"),
    C_UI_EN("BoutonGauche",     OWL_ACTION_SOURIS_BTN_G,
         "Pression du bouton gauche a coordonnees (x, y) ; declenche surBoutonGauche via TestSimuleSouris(champ, action, x, y).",
         "Left button press with coords (x, y); triggers surBoutonGauche via TestSimuleSouris(field, action, x, y).", "LeftButton"),
    C_UI_EN("BoutonDroit",      OWL_ACTION_SOURIS_BTN_D,
         "Pression du bouton droit a coordonnees (x, y) ; declenche surBoutonDroit via TestSimuleSouris(champ, action, x, y).",
         "Right button press with coords (x, y); triggers surBoutonDroit via TestSimuleSouris(field, action, x, y).", "RightButton"),
    /* Nom distinct de l'etat de style `survol` (Style[survol]) pour eviter que
     * le formatter canonicalise l'etat vers cette constante (collision casse). */
    C_UI_EN("SurvolSouris",     OWL_ACTION_SOURIS_SURVOL,
         "Survol curseur a coordonnees (x, y) ; declenche surSurvol via TestSimuleSouris(champ, action, x, y).",
         "Mouse hover at coords (x, y); triggers surSurvol via TestSimuleSouris(field, action, x, y).", "MouseHover"),
    C_UI_EN("MoletteHaut",      OWL_ACTION_SOURIS_MOLETTE_H,
         "Cran de molette vers le haut a (x, y) ; declenche surMolette(mouvement=+1, x, y) via TestSimuleSouris.",
         "Wheel tick up at (x, y); triggers surMolette(movement=+1, x, y) via TestSimuleSouris.", "WheelUp"),
    C_UI_EN("MoletteBas",       OWL_ACTION_SOURIS_MOLETTE_B,
         "Cran de molette vers le bas a (x, y) ; declenche surMolette(mouvement=-1, x, y) via TestSimuleSouris.",
         "Wheel tick down at (x, y); triggers surMolette(movement=-1, x, y) via TestSimuleSouris.", "WheelDown"),

    /* PoliceEtirement — etirement horizontal des caracteres (apparence). */
    C_UI_EN("etirNormal",   OWL_ETIR_NORMAL,
         "Etirement nominal des caracteres (etat par defaut).",
         "Nominal character stretch (default state).", "etirNormal"),
    C_UI_EN("etirCondens\xc3\xa9", OWL_ETIR_CONDENSE,
         "Caracteres condenses (espacement reduit).",
         "Condensed characters (reduced spacing).", "etirCondensed"),
    C_UI_EN("etir\xc3\x89tendu",  OWL_ETIR_ETENDU,
         "Caracteres etendus (espacement augmente).",
         "Expanded characters (increased spacing).", "etirExtended"),
    C_UI_EN("etirLarge",    OWL_ETIR_LARGE,
         "Caracteres elargis (espacement maximum).",
         "Wide characters (maximum spacing).", "etirWide"),

    /* iDestination — seule la sortie PDF est honoree (cf ).
     * iImprimante reste exposee comme valeur de retour/fallback. */
    C_UI_EN("iImprimante",            OWL_IDEST_IMPRIMANTE,    "Imprimante systeme (defaut).",   "System printer (default).", "iPrinter"),
    C_UI_EN("iPDFG\xc3\xa9n\xc3\xa9rique", OWL_IDEST_PDF_GENERIQUE, "Destination PDF (driver-independant).", "PDF (driver-independent).", "iGenericPDF"),
    C_UI_EN("iPDF",                   OWL_IDEST_PDF,           "Ancien PDF (déprécié).",         "Legacy PDF (deprecated).", "iPDF"),
    C_UI_EN("iImageM\xc3\xa9moire",   OWL_IDEST_IMAGE_MEMOIRE,
         "Rasterise chaque page de l'Etat dans un tableau de Image (extension OWL).",
         "Rasterizes each Report page into an array of Image (OWL extension).", "iMemoryImage"),

    /* iParametrePDF — format/encodage. */
    C_UI_EN("iPDFNiveauGris",         OWL_PDF_NIVEAU_GRIS,
         "PDF généré en noir et blanc (rendu interne en niveaux de gris).",
         "PDF generated in grayscale (internal grayscale rendering).", "iPDFGrayscale"),
    C_UI_EN("iPDFA",                  OWL_PDF_A1B,
         "PDF/A (alias de iPDFA1b).",
         "PDF/A (alias for iPDFA1b).", "iPDFA"),
    C_UI_EN("iPDFA1b",                OWL_PDF_A1B,
         "PDF/A-1b (archivage long terme).",
         "PDF/A-1b (long-term archival).", "iPDFA1b"),
    C_UI_EN("iPDFA3b",                OWL_PDF_A3B,
         "PDF/A-3b (archivage avec attachments). NON supporté par le rendu PDF de base — via post-traitement qpdf prévu.",
         "PDF/A-3b (archival with attachments). NOT supported by base PDF rendering — via qpdf post-processing planned.", "iPDFA3b"),

    /* iParametrePDF — signets (qpdf post-traitement). */
    C_UI_EN("iSignetAucun",           OWL_PDF_SIGNET_AUCUN,
         "Aucun signet dans le PDF (défaut).",
         "No bookmark in PDF (default).", "iBookmarkNone"),
    C_UI_EN("iSignetPage",            OWL_PDF_SIGNET_PAGE,
         "Un signet par page (\"Page N\").",
         "One bookmark per page (\"Page N\").", "iBookmarkPage"),

    /* iParametrePDF — protection (chiffrement + ACL). NON supporté par
     * Qt6 — via qpdf prévu. Constantes acceptées pour conformité syntaxe. */
    C_UI_EN("iProtectionAucune",      OWL_PDF_PROT_AUCUNE,
         "Aucune protection (défaut).", "No protection (default).", "iProtectionNone"),
    C_UI_EN("iProtectionImpression",  OWL_PDF_PROT_IMPRESSION,
         "Bloque l'impression. NON supporté par le rendu PDF de base — via post-traitement qpdf prévu.",
         "Block printing. NOT supported by base PDF rendering — via qpdf post-processing planned.", "iProtectionPrinting"),
    C_UI_EN("iProtectionModification", OWL_PDF_PROT_MODIFICATION,
         "Bloque la modification. NON supporté par le rendu PDF de base — via post-traitement qpdf prévu.",
         "Block modification. NOT supported by base PDF rendering — via qpdf post-processing planned.", "iProtectionModification"),
    C_UI_EN("iProtectionS\xc3\xa9lection", OWL_PDF_PROT_SELECTION,
         "Bloque la copie/sélection. NON supporté par le rendu PDF de base — via post-traitement qpdf prévu.",
         "Block copy/selection. NOT supported by base PDF rendering — via qpdf post-processing planned.", "iProtectionSelection"),
    C_UI_EN("iProtectionCommentaire", OWL_PDF_PROT_COMMENTAIRE,
         "Bloque l'ajout/modification de commentaires. NON supporté par le rendu PDF de base — via post-traitement qpdf prévu.",
         "Block comment add/modify. NOT supported by base PDF rendering — via qpdf post-processing planned.", "iProtectionComment"),

    /* iCreePolice — style (combinable bitwise). */
    C_UI_EN("iNormal",            OWL_IPOL_NORMAL,    "Police normale.",       "Plain font.", "iNormal"),
    C_UI_EN("iGras",              OWL_IPOL_GRAS,      "Police grasse.",        "Bold font.", "iBold"),
    C_UI_EN("iItalique",          OWL_IPOL_ITALIQUE,  "Police italique.",      "Italic font.", "iItalic"),
    C_UI_EN("iSoulign\xc3\xa9",   OWL_IPOL_SOULIGNE,  "Police soulignee.",     "Underlined font.", "iUnderlined"),
    C_UI_EN("iBarr\xc3\xa9",      OWL_IPOL_BARRE,     "Police barree.",        "Strikethrough font.", "iStrikeOut"),

    /* iCreePolice — famille (alternative au nom string). */
    C_UI_EN("iD\xc3\xa9""faut",   OWL_IPOL_DEFAUT,    "Famille systeme par defaut.", "System default family.", "iDefault"),
    C_UI_EN("iRoman",             OWL_IPOL_ROMAN,     "Famille serif (Times-like).", "Serif family (Times-like).", "iRoman"),
    C_UI_EN("iSwiss",             OWL_IPOL_SWISS,     "Famille sans-serif (Arial-like).", "Sans-serif family (Arial-like).", "iSwiss"),
    C_UI_EN("iCourier",           OWL_IPOL_COURIER,   "Famille monospace.",    "Monospace family.", "iCourier"),
    /* Constantes OWL pour les fontes officiellement embarquees. Prefixees
     * iPoliceXxx pour distinguer des familles canon (iSwiss/iRoman/...)
     * et signaler qu'elles ciblent une fonte specifique embarquee. */
    C_UI_EN("iPoliceLiberationSans",  OWL_IPOL_LIBERATION_SANS,  "Liberation Sans (sans-serif embarquee, equivalent metric Arial).",
                                                              "Liberation Sans (embedded sans-serif, metric-compatible with Arial).", "iFontLiberationSans"),
    C_UI_EN("iPoliceLiberationSerif", OWL_IPOL_LIBERATION_SERIF, "Liberation Serif (serif embarquee, equivalent metric Times).",
                                                              "Liberation Serif (embedded serif, metric-compatible with Times).", "iFontLiberationSerif"),
    C_UI_EN("iPoliceLiberationMono",  OWL_IPOL_LIBERATION_MONO,  "Liberation Mono (monospace embarquee, equivalent metric Courier).",
                                                              "Liberation Mono (embedded monospace, metric-compatible with Courier).", "iFontLiberationMono"),
    C_UI_EN("iPoliceDejaVuSans",      OWL_IPOL_DEJAVU_SANS,      "DejaVu Sans (couverture Unicode large, fallback symboles).",
                                                              "DejaVu Sans (broad Unicode coverage, symbols fallback).", "iFontDejaVuSans"),
    /* Aliases familieres Microsoft : noms exposes pour le confort utilisateur.
     * Aucun fichier de police Microsoft n'est embarque — le rendu utilise la
     * fonte Liberation/DejaVu embarquee via map_legacy_family (cf pdf_qt.cpp). */
    C_UI_EN("iPoliceArial",           OWL_IPOL_ARIAL,            "Arial (rendu : Liberation Sans embarquee).",
                                                              "Arial (rendered as embedded Liberation Sans).", "iFontArial"),
    C_UI_EN("iPoliceTimesNewRoman",   OWL_IPOL_TIMES_NEW_ROMAN,  "Times New Roman (rendu : Liberation Serif embarquee).",
                                                              "Times New Roman (rendered as embedded Liberation Serif).", "iFontTimesNewRoman"),
    C_UI_EN("iPoliceCourierNew",      OWL_IPOL_COURIER_NEW,      "Courier New (rendu : Liberation Mono embarquee).",
                                                              "Courier New (rendered as embedded Liberation Mono).", "iFontCourierNew"),
    C_UI_EN("iPoliceVerdana",         OWL_IPOL_VERDANA,          "Verdana (rendu : Liberation Sans embarquee).",
                                                              "Verdana (rendered as embedded Liberation Sans).", "iFontVerdana"),
    C_UI_EN("iPoliceTahoma",          OWL_IPOL_TAHOMA,           "Tahoma (rendu : Liberation Sans embarquee).",
                                                              "Tahoma (rendered as embedded Liberation Sans).", "iFontTahoma"),
    C_UI_EN("iPoliceCalibri",         OWL_IPOL_CALIBRI,          "Calibri (rendu : Liberation Sans embarquee).",
                                                              "Calibri (rendered as embedded Liberation Sans).", "iFontCalibri"),
    C_UI_EN("iPoliceComicSansMS",     OWL_IPOL_COMIC_SANS_MS,    "Comic Sans MS (rendu : Liberation Sans embarquee).",
                                                              "Comic Sans MS (rendered as embedded Liberation Sans).", "iFontComicSansMS"),
    C_UI_EN("iPoliceGeorgia",         OWL_IPOL_GEORGIA,          "Georgia (rendu : Liberation Serif embarquee).",
                                                              "Georgia (rendered as embedded Liberation Serif).", "iFontGeorgia"),
    C_UI_EN("iPoliceTrebuchetMS",     OWL_IPOL_TREBUCHET_MS,     "Trebuchet MS (rendu : Liberation Sans embarquee).",
                                                              "Trebuchet MS (rendered as embedded Liberation Sans).", "iFontTrebuchetMS"),
    C_UI_EN("iPoliceHelvetica",       OWL_IPOL_HELVETICA,        "Helvetica (rendu : Liberation Sans embarquee).",
                                                              "Helvetica (rendered as embedded Liberation Sans).", "iFontHelvetica"),

    /* iLigne / iCadre / iEllipse — style trait. */
    C_UI_EN("iContinu",           OWL_ITRAIT_CONTINU,    "Trait continu (defaut).", "Solid line (default).", "iContinuous"),
    C_UI_EN("iTiret",             OWL_ITRAIT_TIRET,      "Trait en tirets.",         "Dashed line.", "iDashed"),
    C_UI_EN("iPointill\xc3\xa9",  OWL_ITRAIT_POINTILLE,  "Trait pointille.",         "Dotted line.", "iDotted"),
    C_UI_EN("iMixte",             OWL_ITRAIT_MIXTE,      "Trait mixte tirets-points.","Dash-dot line.", "iDotAndDash"),

    /* iCadre / iEllipse — brosse de remplissage. */
    C_UI_EN("iPleine",                  OWL_IBROSSE_PLEINE,       "Remplissage plein (defaut).",   "Solid fill (default).", "iSolid"),
    C_UI_EN("iHorizontale",             OWL_IBROSSE_HORIZONTALE,  "Hachures horizontales.",        "Horizontal hatching.", "iHorizontal"),
    C_UI_EN("iVerticale",               OWL_IBROSSE_VERTICALE,    "Hachures verticales.",          "Vertical hatching.", "iVertical"),
    C_UI_EN("iCrois\xc3\xa9""e",        OWL_IBROSSE_CROISEE,      "Hachures croisees.",            "Crossed hatching.", "iCrossed"),
    C_UI_EN("iDiagonaleDescendante",    OWL_IBROSSE_DIAG_DESC,    "Hachures diagonales descendantes.", "Diagonal down hatching.", "iDownwardDiagonal"),
    C_UI_EN("iDiagonaleMontante",       OWL_IBROSSE_DIAG_MONT,    "Hachures diagonales montantes.","Diagonal up hatching.", "iUpwardDiagonal"),
    C_UI_EN("iCrois\xc3\xa9""eDiagonale",OWL_IBROSSE_CROISEE_DIAG,"Hachures diagonales croisees.", "Crossed diagonal hatching.", "iDiagonalCrossed"),

    /* iCadre — bordure visible. */
    C_UI_EN("iBordTous",          OWL_IBORD_TOUS,    "Toutes bordures (defaut).",  "All borders (default).", "iBorderAll"),
    C_UI_EN("iBordHaut",          OWL_IBORD_HAUT,    "Bordure haut seule.",        "Top border only.", "iBorderTop"),
    C_UI_EN("iBordBas",           OWL_IBORD_BAS,     "Bordure bas seule.",         "Bottom border only.", "iBorderBottom"),
    C_UI_EN("iBordGauche",        OWL_IBORD_GAUCHE,  "Bordure gauche seule.",      "Left border only.", "iBorderLeft"),
    C_UI_EN("iBordDroit",         OWL_IBORD_DROIT,   "Bordure droite seule.",      "Right border only.", "iBorderRight"),
    C_UI_EN("iBordAucun",         OWL_IBORD_AUCUN,   "Aucune bordure.",            "No border.", "iBorderNone"),

    /* iImprimeZone — alignement (combinable horizontal + vertical). */
    C_UI_EN("iGauche",            OWL_IALIGN_GAUCHE,    "Alignement horizontal a gauche (defaut).", "Horizontal left alignment (default).", "iLeft"),
    C_UI_EN("iDroite",            OWL_IALIGN_DROITE,    "Alignement horizontal a droite.",          "Horizontal right alignment.", "iRight"),
    C_UI_EN("iCentreH",           OWL_IALIGN_CENTRE_H,  "Centrage horizontal.",                     "Horizontal centering.", "iCenterH"),
    C_UI_EN("iHaut",              OWL_IALIGN_HAUT,      "Alignement vertical en haut (defaut).",    "Vertical top alignment (default).", "iTop"),
    C_UI_EN("iBas",               OWL_IALIGN_BAS,       "Alignement vertical en bas.",              "Vertical bottom alignment.", "iBottom"),
    C_UI_EN("iCentreV",           OWL_IALIGN_CENTRE_V,  "Centrage vertical.",                       "Vertical centering.", "iCenterV"),

    /* iImprimeImage — mode affichage / echelle. */
    C_UI_EN("iAffichageNormal",                 OWL_IECHELLE_NORMAL,       "Image etiree dans le rectangle (defaut).", "Image stretched in the rectangle (default).", "iNormalDisplay"),
    C_UI_EN("iAffichageHomoth\xc3\xa9tique",    OWL_IECHELLE_HOMOTHETIQUE, "Image proportionnelle ancree haut-gauche.","Proportional, top-left anchor.", "iHomotheticDisplay"),
    C_UI_EN("iAffichageHomoCentr\xc3\xa9",      OWL_IECHELLE_HOMOCENTRE,   "Image proportionnelle centree.",           "Proportional, centered.", "iHomoCenteredDisplay"),

    /* iInfoImprimante — type d'information. */
    C_UI_EN("iNomImprimante",                   OWL_IINFOIMPR_NOM,         "Nom de l'imprimante (defaut).",            "Printer name (default).", "iPrinterName"),
    C_UI_EN("iNomDriver",                       OWL_IINFOIMPR_DRIVER,      "Nom du driver d'imprimante.",              "Printer driver name.", "iDriverName"),
    C_UI_EN("iNomPort",                         OWL_IINFOIMPR_PORT,        "Nom du port d'impression.",                "Print port name.", "iDeviceName"),

    /* DSL Etat V2 — formats de page (propriete Etat..Format). */
    C_UI_EN("formatA3",       OWL_ETAT_FORMAT_A3,     "Format A3 (297x420 mm).",       "A3 format (297x420 mm).", "formatA3"),
    C_UI_EN("formatA4",       OWL_ETAT_FORMAT_A4,     "Format A4 (210x297 mm, defaut).", "A4 format (210x297 mm, default).", "formatA4"),
    C_UI_EN("formatA5",       OWL_ETAT_FORMAT_A5,     "Format A5 (148x210 mm).",       "A5 format (148x210 mm).", "formatA5"),
    C_UI_EN("formatLetter",   OWL_ETAT_FORMAT_LETTER, "US Letter (8.5x11 pouces).",    "US Letter (8.5x11 inches).", "formatLetter"),
    C_UI_EN("formatLegal",    OWL_ETAT_FORMAT_LEGAL,  "US Legal (8.5x14 pouces).",     "US Legal (8.5x14 inches).", "formatLegal"),

    /* DSL Etat V2 — orientation (propriete Etat..Orientation). */
    C_UI_EN("orientPortrait", OWL_ETAT_ORIENT_PORTRAIT, "Orientation portrait (defaut).", "Portrait orientation (default).", "orientPortrait"),
    C_UI_EN("orientPaysage",  OWL_ETAT_ORIENT_PAYSAGE,  "Orientation paysage.",            "Landscape orientation.", "orientLandscape"),

    /* TableActiveFiltre — type de filtre (canon 1000017149). */
    C_UI_EN("filtreContient",         OWL_FILTRE_CONTIENT,        "Lignes dont la valeur contient le pattern.",       "Rows where value contains the pattern.", "filterContains"),
    C_UI_EN("filtreCommencePar",      OWL_FILTRE_COMMENCE_PAR,    "Lignes dont la valeur commence par le pattern.",   "Rows where value starts with the pattern.", "filterStartsWith"),
    C_UI_EN("filtreTerminePar",       OWL_FILTRE_TERMINE_PAR,     "Lignes dont la valeur se termine par le pattern.", "Rows where value ends with the pattern.", "filterEndsWith"),
    C_UI_EN("filtreEgal",             OWL_FILTRE_EGAL,            "Lignes dont la valeur est egale au pattern.",      "Rows where value equals the pattern.", "filterEqual"),
    C_UI_EN("filtreDiff\xc3\xa9rent", OWL_FILTRE_DIFFERENT,       "Lignes dont la valeur est differente du pattern.", "Rows where value differs from the pattern.", "filterDifferent"),
    C_UI_EN("filtreInf\xc3\xa9rieur", OWL_FILTRE_INFERIEUR,       "Valeur inferieure au pattern.", "Value < pattern.", "filterLess"),
    C_UI_EN("filtreInf\xc3\xa9rieurOuEgal", OWL_FILTRE_INFERIEUR_EGAL, "Valeur <= pattern.", "Value <= pattern.", "filterLessOrEqual"),
    C_UI_EN("filtreSup\xc3\xa9rieur", OWL_FILTRE_SUPERIEUR,       "Valeur superieure.", "Value > pattern.", "filterGreater"),
    C_UI_EN("filtreSup\xc3\xa9rieurOuEgal", OWL_FILTRE_SUPERIEUR_EGAL, "Valeur >= pattern.", "Value >= pattern.", "filterGreaterOrEqual"),
    C_UI_EN("filtreNeContientPas",    OWL_FILTRE_NE_CONTIENT_PAS, "Valeur ne contient pas le pattern.", "Value does NOT contain pattern.", "filterDoesNotContain"),
    C_UI_EN("filtreNeCommencePasPar", OWL_FILTRE_NE_COMMENCE_PAS, "Valeur ne commence pas par le pattern.", "Value does NOT start with pattern.", "filterDoesNotStartWith"),
    C_UI_EN("filtreNeTerminePasPar",  OWL_FILTRE_NE_TERMINE_PAS,  "Valeur ne se termine pas par le pattern.", "Value does NOT end with pattern.", "filterDoesNotEndWith"),
};
#undef C_UI

/* ====================================================================
 *  — DSL de service HTTP : proprietes des blocs ServiceREST et
 * OperationHTTP. Cablees ici pour l'autocomplete LSP + la validation
 * compile-time des valeurs d'enum. Les proprietes de type liste (UrlBase,
 * Chemin, Reponses, Requete) sont declarees VARIANT : leur contenu (segments
 * litteraux, valeurs d'enum) est valide par le parser DSL, pas par le type
 * scalaire du catalogue.
 * ==================================================================== */
static const OwlControlPropDef PROPS_SERVICE_REST[] = {
    { "UrlBase",          NULL, "BaseUrl",          OWL_PROP_TYPE_VARIANT, NULL,
      "Prefixe d'URL du service, en segments litteraux (ex [\"api\", \"v1\"]). "
      "Aucun segment ne contient de separateur « / ».",
      "Service URL prefix, as literal segments (e.g. [\"api\", \"v1\"]). "
      "No segment contains a « / » separator.", NULL DFT_NULL },
    { "Authentification", NULL, "Authentication",   OWL_PROP_TYPE_ENUM, "httpAuthentification",
      "Schema d'authentification applique aux operations du service.",
      "Authentication scheme applied to the service operations.",
      NULL DFT_ENUM("httpAuthAucune") },
    { "Cl\xc3\xa9PubliqueJWT", "ClePubliqueJWT", "JWTPublicKey", OWL_PROP_TYPE_STRING, NULL,
      "Cle publique RS256 (format PEM) verifiant la signature des jetons JWT "
      "quand l'authentification est httpAuthJWT.",
      "RS256 public key (PEM format) verifying the signature of JWT tokens "
      "when authentication is httpAuthJWT.", NULL DFT_NULL },
    { "URLJWKS", NULL, "JWKSURL", OWL_PROP_TYPE_STRING, NULL,
      "Adresse d'un jeu de cles JWKS (JSON Web Key Set) fournissant les cles "
      "publiques RS256 de l'emetteur. Les cles sont recuperees a la demande, "
      "mises en cache et selectionnees par le kid du jeton, ce qui suit la "
      "rotation des cles. Exclusive de ClePubliqueJWT. En http://, l'adresse "
      "doit rester locale (127.0.0.1, localhost, ::1) ; sinon https://.",
      "Address of a JWKS (JSON Web Key Set) providing the issuer's RS256 "
      "public keys. Keys are fetched on demand, cached and selected by the "
      "token kid, which follows key rotation. Mutually exclusive with "
      "JWTPublicKey. Over http://, the address must stay local (127.0.0.1, "
      "localhost, ::1); otherwise https://.", NULL DFT_NULL },
    { "\xc3\x89metteurJWT", "EmetteurJWT", "JWTIssuer", OWL_PROP_TYPE_STRING, NULL,
      "Emetteur attendu (claim iss) des jetons JWT acceptes quand "
      "l'authentification est httpAuthJWT.",
      "Expected issuer (iss claim) of accepted JWT tokens when authentication "
      "is httpAuthJWT.", NULL DFT_NULL },
    { "AudienceJWT", NULL, "JWTAudience", OWL_PROP_TYPE_STRING, NULL,
      "Audience attendue (claim aud) des jetons JWT acceptes quand "
      "l'authentification est httpAuthJWT. Le jeton doit cibler cette audience "
      "(egale a la chaine aud, ou presente dans le tableau aud).",
      "Expected audience (aud claim) of accepted JWT tokens when authentication "
      "is httpAuthJWT. The token must target this audience (equal to the aud "
      "string, or present in the aud array).", NULL DFT_NULL },
};

static const OwlControlPropDef PROPS_OPERATION_HTTP[] = {
    { "M\xc3\xa9thode",   "Methode", "Method",       OWL_PROP_TYPE_ENUM, "httpMethodes",
      "Verbe HTTP de l'operation.", "HTTP verb of the operation.",
      NULL DFT_ENUM("httpGet") },
    { "Chemin",           NULL, "Path",             OWL_PROP_TYPE_VARIANT, NULL,
      "Chemin de l'operation en segments : chaines litterales et parametres "
      "(identifiants nus). Aucun segment ne contient de « / ».",
      "Operation path as segments: literal strings and parameters (bare "
      "identifiers). No segment contains a « / ».", NULL DFT_NULL },
    { "R\xc3\xa9ponses",  "Reponses", "Responses",   OWL_PROP_TYPE_VARIANT, NULL,
      "Liste des codes de statut que l'operation peut renvoyer.",
      "List of status codes the operation may return.", NULL DFT_NULL },
    { "Code\xc3\x89tat",  "CodeEtat", "StatusCode",  OWL_PROP_TYPE_ENUM, "httpCodeEtat",
      "Code de statut nominal de l'operation. Doit figurer dans Reponses.",
      "Nominal status code of the operation. Must be listed in Responses.",
      NULL DFT_ENUM("httpOK") },
    { "Requ\xc3\xaate",   "Requete", "Query",        OWL_PROP_TYPE_VARIANT, NULL,
      "Parametres de query string de l'operation.",
      "Query-string parameters of the operation.", NULL DFT_NULL },
    { "Authentification", NULL, "Authentication",   OWL_PROP_TYPE_ENUM, "httpAuthentification",
      "Exempte l'operation de l'authentification du service (httpAuthAucune).",
      "Opts the operation out of the service authentication (httpAuthNone).",
      NULL DFT_ENUM("httpAuthAucune") },
    { "TypeContenu",      NULL, "ContentType",      OWL_PROP_TYPE_ENUM, "typeMime",
      "Type de contenu attendu du corps de la requete. "
      "typeMimeFormulaireMultiPartie active la reception de fichiers via des "
      "parametres FichierTeleverse.",
      "Expected content type of the request body. "
      "typeMimeFormulaireMultiPartie enables receiving files through "
      "UploadedFile parameters.",
      NULL DFT_NULL },
    { "ExclureDeOpenAPI", NULL, "ExcludeFromOpenAPI", OWL_PROP_TYPE_BOOL, NULL,
      "Retire l'operation du document OpenAPI genere sans changer son routage : "
      "elle reste servie normalement, mais n'apparait pas dans le contrat.",
      "Removes the operation from the generated OpenAPI document without "
      "changing its routing: it is still served normally but does not appear "
      "in the contract.",
      NULL DFT_BOOL(0) },
    { "Flux",             NULL, "Stream",             OWL_PROP_TYPE_ENUM, "httpFlux",
      "Mode de diffusion progressive de l'operation. Absente = reponse normale ; "
      "fluxSSE emet des evenements (framing SSE), fluxOctets diffuse des octets "
      "bruts.",
      "Progressive delivery mode of the operation. Absent = normal response; "
      "fluxSSE emits events (SSE framing), fluxOctets streams raw bytes.",
      NULL DFT_NULL },
    { "D\xc3\xa9laiMax",  "DelaiMax", "MaxDelay",     OWL_PROP_TYPE_INT, NULL,
      "Borne de duree d'un flux en secondes (0 = illimite). Sans effet si "
      "l'operation n'est pas un flux.",
      "Stream duration limit in seconds (0 = unlimited). No effect if the "
      "operation is not a stream.",
      NULL DFT_INT(0) },
};

/* ============================================================
 * Index plat des proprietes DSL (Phase A — alignment registry).
 *
 * Realloc dynamique. Vie : pour la duree du process. Pas de free
 * (constants des tables PROPS_* sont static, pas d'ownership).
 * ============================================================ */

static OwlDSLPropEntry* g_dsl_props = NULL;
static int g_dsl_props_count = 0;
static int g_dsl_props_capacity = 0;

static void register_dsl_props_table(const char* kind,
                                      const OwlControlPropDef* props,
                                      int count) {
    if (g_dsl_props_count + count > g_dsl_props_capacity) {
        int new_cap = (g_dsl_props_capacity == 0) ? 64 : g_dsl_props_capacity * 2;
        while (new_cap < g_dsl_props_count + count) new_cap *= 2;
        g_dsl_props = (OwlDSLPropEntry*)realloc(g_dsl_props,
            (size_t)new_cap * sizeof(OwlDSLPropEntry));
        g_dsl_props_capacity = new_cap;
    }
    for (int i = 0; i < count; i++) {
        g_dsl_props[g_dsl_props_count].kind = kind;
        g_dsl_props[g_dsl_props_count].def  = &props[i];
        g_dsl_props_count++;
    }
}

int owl_dsl_props_count(void) { return g_dsl_props_count; }

const OwlDSLPropEntry* owl_dsl_prop_at(int index) {
    if (index < 0 || index >= g_dsl_props_count) return NULL;
    return &g_dsl_props[index];
}

const OwlControlPropDef* owl_dsl_prop_find(const char* kind, const char* prop_name) {
    if (!kind || !prop_name) return NULL;
    for (int i = 0; i < g_dsl_props_count; i++) {
        if (strcmp(g_dsl_props[i].kind, kind) != 0) continue;
        const OwlControlPropDef* p = g_dsl_props[i].def;
        if (p->nameFR && strcasecmp(p->nameFR, prop_name) == 0) return p;
        if (p->nameAscii && strcasecmp(p->nameAscii, prop_name) == 0) return p;
        if (p->nameEN && strcasecmp(p->nameEN, prop_name) == 0) return p;
    }
    return NULL;
}

/* ============================================================
 * Famille evt* — constantes d'evenement widget.
 *
 * Generees 1:1 depuis la vue dedupliquee du catalogue d'events widget
 * (event_catalog_widgets.c) : chaque event unique donne une constante evt*
 * (strip du prefixe sur/on -> prefixe evt), de valeur OWL_EVT_BASE + index.
 * Ajouter un event au catalogue expose sa constante automatiquement, sans
 * edition ici. L'enum E_EvtWidget porte la liste (filtrage LSP via allowedBase
 * sur le parametre evt de DeclencheEvenement / TriggerEvent).
 *
 * Stockage a duree de vie process (pas de free) : les OwlConstDef et le tableau
 * de valeurs de l'enum doivent survivre au registre. Reconstruit a chaque init
 * (idempotent sur le double vm_init de --vscode-data).
 * ============================================================ */
#define OWL_EVT_MAX 128
static char        g_evt_name_fr[OWL_EVT_MAX][96];
static char        g_evt_name_en[OWL_EVT_MAX][96];
static OwlConstDef g_evt_consts[OWL_EVT_MAX];
static const char* g_evt_enum_values[OWL_EVT_MAX + 1];

OwlEnum E_EvtWidget = {
    .name = "\xc3\xa9""v\xc3\xa9""nementWidget",   /* événementWidget */
    .nameAscii = "evenementWidget",
    .nameEN = "widgetEvent",
    .category = "UI",
    .descFR = "Evenement declenchable par code sur un champ (DeclencheEvenement). "
              "La constante porte la signature de l'evenement (parametres attendus).",
    .descEN = "Event that can be triggered on a control by code (TriggerEvent). "
              "The constant carries the event signature (expected parameters).",
    .values = NULL,          /* rempli a l'init par register_widget_event_constants */
    .combinable = false,
};

/* Strip le prefixe (sur/on) et prepose evt. `src` commence par `prefix`
 * (garanti par le catalogue widget : tout event canon est surXxx / onXxx). */
static void build_evt_name(char* dst, size_t dstsz,
                           const char* src, const char* prefix, int prefixLen) {
    const char* rest = src ? src : "";
    if (src && (int)strlen(src) >= prefixLen &&
        strncasecmp(src, prefix, (size_t)prefixLen) == 0) {
        rest = src + prefixLen;
    }
    snprintf(dst, dstsz, "evt%s", rest);
}

static void register_widget_event_constants(void) {
    int n = owl_widget_event_dedup_count();
    if (n > OWL_EVT_MAX) n = OWL_EVT_MAX;
    for (int i = 0; i < n; i++) {
        const OwlEventDef* d = owl_widget_event_dedup_at(i);
        build_evt_name(g_evt_name_fr[i], sizeof(g_evt_name_fr[i]),
                       d->canonical, "sur", 3);
        build_evt_name(g_evt_name_en[i], sizeof(g_evt_name_en[i]),
                       d->nameEN ? d->nameEN : d->canonical, "on", 2);
        g_evt_consts[i].name     = g_evt_name_fr[i];
        g_evt_consts[i].category = "UI";
        g_evt_consts[i].value    = INTEGER_VAL(OWL_EVT_BASE + i);
        g_evt_consts[i].descFR   = d->descFR;
        g_evt_consts[i].descEN   = d->descEN;
        g_evt_consts[i].nameEN   = g_evt_name_en[i];
        g_evt_enum_values[i]     = g_evt_name_fr[i];
    }
    g_evt_enum_values[n] = NULL;
    E_EvtWidget.values = g_evt_enum_values;
    owl_registry_register_consts(g_evt_consts, n);
}

/* Forward decl — defini dans event_catalog_widgets.c. */
void event_catalog_widgets_init(void);

void stdlib_ui_catalog_init(void) {
    /* Cas du double vm_init (--vscode-data : main + print_vscode_data) :
     * vm_init() commence par owl_registry_free() qui clear le registry, donc
     * il faut bien re-register les enums/consts. Mais notre index plat
     * g_dsl_props est local au fichier (non clear par owl_registry_free) :
     * on le reset manuellement ici sinon on doublerait l'index. */
    g_dsl_props_count = 0;
    event_catalog_widgets_init();
    /*  — genere la famille evt* depuis la vue dedup construite par
     * event_catalog_widgets_init(). Doit tourner AVANT register_enums (E_EvtWidget
     * dans ENUMS[]) et register_consts pour que l'enum voie ses valeurs. */
    register_widget_event_constants();

    static const OwlEnum* ENUMS[] = {
        &E_TypeBouton, &E_PositionImageBouton, &E_TypeLibelle, &E_TypeSaisie,
        &E_CadrageH, &E_CadrageV, &E_toastDuree, &E_Cadrage, &E_Etat,
        &E_Curseurs, &E_Ellipse, &E_sensEcriture,
        &E_TraitCadre, &E_BordsCadre, &E_TypeDegrade, &E_FormatAffichage,
        &E_Disposition, &E_Alignement, &E_Position, &E_LargeurVolets,
        &E_ColTypes, &E_OrientationsTable, &E_EtatsTableInitial, &E_ModeLoupe,
        &E_ToInfo, &E_TsInfo, &E_TiXY, &E_tdOperation, &E_tdCibleRelative,
        &E_TaInfo, &E_FiltreType, &E_SelMode,
        &E_ZrSens, &E_ActionSouris, &E_PoliceEtirement,
        &E_IDest, &E_IStyle, &E_IFamily, &E_ITrait, &E_IBrosse, &E_IBord, &E_IAlign, &E_IEchelle, &E_IInfoImpr,
        &E_EtatFormat, &E_EtatOrient,
        &E_pdfProtection,
        &E_EvtWidget,
    };
    owl_registry_register_enums(ENUMS, (int)(sizeof(ENUMS)/sizeof(ENUMS[0])));
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * pour les noms accentues (cf owl_registry.c). */
    owl_registry_register_consts(UI_CONSTS, (int)(sizeof(UI_CONSTS)/sizeof(UI_CONSTS[0])));

    /* Phase A : enregistre les proprietes DSL dans l'index plat. Les kinds
     * sont les meme strings que ceux retournes par owl_control_kind_name()
     * pour les WIDGETS, plus "Fenetre"/"Etat"/"blocEtat" pour les conteneurs.
     * (cf rule 7/8/9 du self-test registry). */
    register_dsl_props_table("Fenetre",  PROPS_WINDOW,
        (int)(sizeof(PROPS_WINDOW)/sizeof(PROPS_WINDOW[0])));
    register_dsl_props_table("Libelle",  PROPS_LABEL,
        (int)(sizeof(PROPS_LABEL)/sizeof(PROPS_LABEL[0])));
    register_dsl_props_table("Bouton",   PROPS_BUTTON,
        (int)(sizeof(PROPS_BUTTON)/sizeof(PROPS_BUTTON[0])));
    register_dsl_props_table("Saisie",   PROPS_INPUT,
        (int)(sizeof(PROPS_INPUT)/sizeof(PROPS_INPUT[0])));
    register_dsl_props_table("Table",    PROPS_TABLE,
        (int)(sizeof(PROPS_TABLE)/sizeof(PROPS_TABLE[0])));
    register_dsl_props_table("Conteneur", PROPS_CONTAINER,
        (int)(sizeof(PROPS_CONTAINER)/sizeof(PROPS_CONTAINER[0])));
    /*  — PROPS_IMAGE_FIELD n'etait pas index avant ce fix :
     * owl_dsl_prop_find("Image", ...) retournait NULL pour toutes les props
     * du widget Champ Image (Image, Cadrage, Zoom, etc.), aveuglant la
     * regle 25 du self-test et tout autre consommateur de l'index plat. */
    register_dsl_props_table("Image",    PROPS_IMAGE_FIELD,
        (int)(sizeof(PROPS_IMAGE_FIELD)/sizeof(PROPS_IMAGE_FIELD[0])));
    /* Clé = forme ascii canon du widget (cf nameAscii||nameFR de CONTROL_KINDS).
     * Colonne n'a pas de nameAscii (pas d'accent) -> ascii = nameFR = "Colonne".
     * Doit matcher pour que le doc HTML / list props rattachent ses props. */
    register_dsl_props_table("Colonne",  PROPS_COLUMN,
        (int)(sizeof(PROPS_COLUMN)/sizeof(PROPS_COLUMN[0])));
    /*  — genre synthetique : la cellule (et la ligne) d'une Table n'est
     * pas un bloc declarable, mais un receveur indexe en code. L'inscrire dans
     * l'index plat suffit a le rendre visible du LSP (catalogue genere) et du
     * formatter, sans dupliquer la liste. */
    register_dsl_props_table("CelluleTable", PROPS_TABLE_CELL,
        (int)(sizeof(PROPS_TABLE_CELL)/sizeof(PROPS_TABLE_CELL[0])));
    register_dsl_props_table("Liste",    PROPS_LIST_FIELD,
        (int)(sizeof(PROPS_LIST_FIELD)/sizeof(PROPS_LIST_FIELD[0])));
    register_dsl_props_table("Combo",    PROPS_COMBO_FIELD,
        (int)(sizeof(PROPS_COMBO_FIELD)/sizeof(PROPS_COMBO_FIELD[0])));
    register_dsl_props_table("Selecteur", PROPS_SELECTEUR_FIELD,
        (int)(sizeof(PROPS_SELECTEUR_FIELD)/sizeof(PROPS_SELECTEUR_FIELD[0])));
    register_dsl_props_table("Interrupteur", PROPS_INTERRUPTEUR_FIELD,
        (int)(sizeof(PROPS_INTERRUPTEUR_FIELD)/sizeof(PROPS_INTERRUPTEUR_FIELD[0])));
    register_dsl_props_table("ZoneRepetee", PROPS_ZR_FIELD,
        (int)(sizeof(PROPS_ZR_FIELD)/sizeof(PROPS_ZR_FIELD[0])));
    register_dsl_props_table("Jauge",    PROPS_JAUGE_FIELD,
        (int)(sizeof(PROPS_JAUGE_FIELD)/sizeof(PROPS_JAUGE_FIELD[0])));
    register_dsl_props_table("CodeBarres", PROPS_CODEBARRE_FIELD,
        (int)(sizeof(PROPS_CODEBARRE_FIELD)/sizeof(PROPS_CODEBARRE_FIELD[0])));
    /*  — cles = forme ascii canon des kinds Onglet/Volet (cf CONTROL_KINDS). */
    register_dsl_props_table("Onglet",   PROPS_ONGLET,
        (int)(sizeof(PROPS_ONGLET)/sizeof(PROPS_ONGLET[0])));
    register_dsl_props_table("Volet",    PROPS_VOLET,
        (int)(sizeof(PROPS_VOLET)/sizeof(PROPS_VOLET[0])));
    register_dsl_props_table("Etat",     PROPS_ETAT,
        (int)(sizeof(PROPS_ETAT)/sizeof(PROPS_ETAT[0])));
    register_dsl_props_table("blocEtat", PROPS_ETAT_BLOCK,
        (int)(sizeof(PROPS_ETAT_BLOCK)/sizeof(PROPS_ETAT_BLOCK[0])));
    register_dsl_props_table("LibelleEtat", PROPS_LIBELLE_ETAT,
        (int)(sizeof(PROPS_LIBELLE_ETAT)/sizeof(PROPS_LIBELLE_ETAT[0])));
    register_dsl_props_table("ImageEtat", PROPS_IMAGE_ETAT,
        (int)(sizeof(PROPS_IMAGE_ETAT)/sizeof(PROPS_IMAGE_ETAT[0])));
    /*  — cles = forme ascii canon des kinds menu (cf CONTROL_KINDS). */
    register_dsl_props_table("MenuPrincipal", PROPS_MENU_PRINCIPAL,
        (int)(sizeof(PROPS_MENU_PRINCIPAL)/sizeof(PROPS_MENU_PRINCIPAL[0])));
    register_dsl_props_table("MenuContextuel", PROPS_MENU_CONTEXTUEL,
        (int)(sizeof(PROPS_MENU_CONTEXTUEL)/sizeof(PROPS_MENU_CONTEXTUEL[0])));
    register_dsl_props_table("SousMenu", PROPS_MENU_SUBMENU,
        (int)(sizeof(PROPS_MENU_SUBMENU)/sizeof(PROPS_MENU_SUBMENU[0])));
    register_dsl_props_table("OptionMenu", PROPS_MENU_OPTION,
        (int)(sizeof(PROPS_MENU_OPTION)/sizeof(PROPS_MENU_OPTION[0])));
    register_dsl_props_table("MenuSeparateur", PROPS_MENU_SEPARATOR,
        (int)(sizeof(PROPS_MENU_SEPARATOR)/sizeof(PROPS_MENU_SEPARATOR[0])));
    register_dsl_props_table("CodeBarresEtat", PROPS_CODEBARRE_ETAT,
        (int)(sizeof(PROPS_CODEBARRE_ETAT)/sizeof(PROPS_CODEBARRE_ETAT[0])));
    /*  — blocs DSL du service HTTP declaratif. */
    register_dsl_props_table("ServiceREST", PROPS_SERVICE_REST,
        (int)(sizeof(PROPS_SERVICE_REST)/sizeof(PROPS_SERVICE_REST[0])));
    register_dsl_props_table("OperationHTTP", PROPS_OPERATION_HTTP,
        (int)(sizeof(PROPS_OPERATION_HTTP)/sizeof(PROPS_OPERATION_HTTP[0])));
}

/* ====================================================================
 *   — Valeurs par defaut headless
 * ====================================================================
 * Quand un ObjUiControl materialise au compile-time n'a pas encore reçu
 * de set sur une prop ET n'a pas de widget Qt attache (mode CLI/test ou
 * pre-Ouvre en GUI), OP_STRUCT_GET appelle cette fonction pour produire
 * une valeur sensible :
 *   - Visible / Actif       → Vrai (sémantique implicite canon : un widget
 *                              cree est visible/actif par defaut)
 *   - BOOL autres           → Faux
 *   - STRING (Texte/Libellé/Titre/...) → ""
 *   - INT / COLOR / POSITION → 0
 *   - ENUM / VARIANT / inconnue → Null (pas de defaut canonique)
 * ==================================================================== */
static bool default_visible_or_actif(const char* prop) {
    /* Comparaisons case-insensitive sur les noms canoniques FR + alias EN. */
    if (strcasecmp(prop, "Visible") == 0) return true;
    if (strcasecmp(prop, "Actif")   == 0) return true;
    if (strcasecmp(prop, "Active")  == 0) return true;
    if (strcasecmp(prop, "Enabled") == 0) return true;
    return false;
}

Value owl_ui_headless_default(ObjUiControl* c, const char* prop) {
    if (!c || !prop) return NIL_VAL;
    const OwlControlPropDef* def =
        owl_control_catalog_find((OwlControlKind)c->kind, prop);
    if (!def) return NIL_VAL;
    /*  lot B — la prop 'Image' (chemin d'icone) lit "" par defaut
     * (pas d'icone), symetrique du getter GUI owl_ui_button_get_icon/label.
     * Le type reste VARIANT (accepte aussi ObjImage/ObjIcone en ecriture),
     * seul le defaut de lecture back est une chaine vide. */
    if (strcasecmp(prop, "Image") == 0) return OBJ_VAL(obj_copy_string("", 0));
    /*  — ModeLoupe/MagnifierMode : la lecture d'un enum non ecrit rend
     * NIL par defaut (case ENUM ci-dessous), mais le canon expose un defaut
     * concret (rechercheCommencePar). On resout le defaut vers son entier
     * sentinel pour que `TAB..ModeLoupe` relise le mode Recherche a l'ouverture,
     * sans avoir a seeder le store. Symetrique du const rechercheCommencePar. */
    if (strcasecmp(prop, "ModeLoupe") == 0 || strcasecmp(prop, "MagnifierMode") == 0)
        return INTEGER_VAL(OWL_RCHT_COMMENCE_PAR);
    switch (def->type) {
        case OWL_PROP_TYPE_BOOL:
            return BOOL_VAL(default_visible_or_actif(def->nameFR));
        case OWL_PROP_TYPE_STRING:
        case OWL_PROP_TYPE_MULTILINGUE:
            return OBJ_VAL(obj_copy_string("", 0));
        case OWL_PROP_TYPE_INT:
        case OWL_PROP_TYPE_COLOR:
            return INTEGER_VAL(0);
        case OWL_PROP_TYPE_POSITION:
        case OWL_PROP_TYPE_VARIANT:
        case OWL_PROP_TYPE_ENUM:
        default:
            return NIL_VAL;
    }
}
