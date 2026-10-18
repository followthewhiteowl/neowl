// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_decl.h - Intermediate Representation des declarations UI (DSL fenetre v1).
 *
 * Apres parsing par compiler.c, chaque "Fenetre FEN_X ... FIN" produit un
 * OwlWindowDecl stocke dans vm.windows. Les natives Ouvre/OuvreFille
 * consomment cet IR pour creer les widgets Qt via owl_ui.h.
 */

#ifndef OWL_UI_DECL_H
#define OWL_UI_DECL_H

#include "value.h"
#include "object.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types de champs supportes en V1 ---- */

typedef enum {
    OWL_CONTROL_LABEL,    /* Libelle */
    OWL_CONTROL_BUTTON,   /* Bouton */
    OWL_CONTROL_INPUT,    /* Saisie */
    OWL_CONTROL_TABLE,    /* Table (Champ Table V1) */
    OWL_CONTROL_CONTAINER,/* Conteneur (Lot F) — boite generique pour layout
                           * par nesting. Equivalent <div> / Qt QWidget vide. */
    OWL_CONTROL_IMAGE,    /* Champ Image — affichage d'une image (path string ou
                           * ObjImage en memoire) avec Cadrage. V2 ajoutera
                           * scroll/zoom/Outils. */
    OWL_CONTROL_LISTE,    /* Champ Liste — backend QListWidget. V1 mono-colonne
                           * texte simple, mono ou multisel. V2 : Liste fichier
                           * (HFSQL), multicolonne, Liste Image. */
    OWL_CONTROL_COMBO,    /* Champ Combo — backend QComboBox. Reuse 90% de la
                           * famille Liste (ListeAjoute, etc. fonctionnent
                           * indifferemment). Specifiques : ComboOuvre,
                           * ComboFerme, prop EnSaisie. */
    OWL_CONTROL_SELECTEUR,/* Champ Selecteur — radio group exclusif. Backend
                           * QButtonGroup + QRadioButton. 4 modes d'affichage
                           * (standard, sous-libelles, Vista image, Vista +
                           * sous-libelles). Prop ..Options polymorphique :
                           * array de strings ou array de structs
                           * {Libelle, SousLibelle, ValeurRenvoyee, Image}. */
    OWL_CONTROL_INTERRUPTEUR, /* Champ Interrupteur — checkbox group multi-select.
                           * Backend QCheckBox (no group, indep). Memes 4 modes
                           * que Selecteur + tri-etat (TroisEtats : coche /
                           * decoche / indetermine). ..Valeur = bitmask entier
                           * (canon : option N = bit N-1). */
    OWL_CONTROL_ZONE_REPETEE, /* Champ ZoneRépétée (Looper) — sous-champs
                           * declares une fois en template, instancies a chaque
                           * AjouteLigne. Backend QScrollArea + QVBoxLayout
                           * (Sens=zrVertical, unique valeur).
                           * Indexation ZR[i].sub..prop renvoie un proxy ciblant
                           * le sub-control de la repetition i. V1 : par
                           * programmation uniquement, 1 niveau d'imbrication
                           * ZR-dans-ZR max. */
    OWL_CONTROL_JAUGE,    /* Champ Jauge — barre de progression output-only.
                           * Backend QProgressBar. Variantes lineaire H/V +
                           * symetrique (progression depuis le centre). ..Valeur
                           * = entier borne a [BorneMin, BorneMax]. */
    OWL_CONTROL_CODEBARRE,/* Champ CodeBarres — affichage output-only d'un
                           * code-barres genere (type + contenu + ECC via le
                           * pipeline zint). Calque strict du Champ Image :
                           * backend OwlImageLabel (QLabel), l'image est
                           * rasterisee depuis les props et poussee par
                           * owl_ui_label_set_image_rgba. Le nom `CodeBarres`
                           * designe aussi un type runtime (variable) -> il est
                           * ambigu (cf owl_control_kind_is_ambiguous). */
    OWL_CONTROL_ONGLET,   /* Champ Onglet — organise le contenu en pages
                           * ("volets") empilees, un seul visible a la fois.
                           * Widget positionne (X/Y/Largeur/Hauteur). Ses
                           * enfants directs sont exclusivement des sous-blocs
                           * Volet (OWL_CONTROL_VOLET). Backend Qt QTabWidget
                           * (lot 2). Nom EN "Tab" — resolution contextuelle vs
                           * la constante TAB (le kind ne gagne qu'apres
                           * `est un Champ`). */
    OWL_CONTROL_VOLET,    /* Volet — sous-bloc de rôle d'un Champ Onglet (une
                           * page). Structurel (is_widget=false, pas de prefixe
                           * `Champ`), mais point de recursion enfants comme un
                           * Conteneur : porte son Libellé + sa Disposition +
                           * des champs enfants arbitraires. */
    OWL_CONTROL_WINDOW,   /* Fenetre — virtuel, pas un sous-champ. Sert au
                           * dispatch runtime FEN_X..Prop via ObjUiControl. */
} OwlControlKind;

/* ---- Initialisation d'une propriete : "Titre = valeur" ----
 *
 * Une propriete peut etre :
 *  - is_constant=true  : valeur fixee au compile-time (literal entier,
 *                        chaine, booleen, constante nommee)
 *  - is_constant=false : expression dynamique, ObjFunction* eval qui
 *                        produira la valeur juste avant l'init du widget
 */

typedef struct {
    char*           name;          /* "Titre", "Largeur", ... (alloue) */
    Value           const_value;   /* Si is_constant : la valeur fixe */
    ObjFunction*    eval;          /* Si !is_constant : bytecode eval */
    bool            is_constant;
    int             line;          /* Pour erreurs runtime */
} OwlPropertyInit;

/* ---- Bloc de code attache a un champ ou a la fenetre ----
 *
 * "PROCEDURE Initialisation() ... FIN" -> OwlEventBlock {
 *     name="Initialisation", impl=ObjFunction*, line=12 }
 */

typedef struct {
    char*           name;          /* "Initialisation", "Clic", ... (alloue) */
    ObjFunction*    impl;          /* Bytecode du bloc */
    int             line;
} OwlEventBlock;

/* ---- Declaration d'une colonne de Table (Champ Table V1) ----
 *
 * "COL_Nom est une colonne PROPRIETES Libelle = ... Type = ... FIN FIN"
 * dans le sous-bloc COLONNES d'une Table.
 */

typedef struct {
    char*             name;             /* "COL_Nom" (alloue) */
    OwlPropertyInit*  properties;       /* Libelle, Type, Largeur, ... */
    int               property_count;
    int               property_capacity;
    int               line;
} OwlColumnDecl;

/*  — brique de style plate d'un volet d'onglet (VoletActif /
 * VoletInactif, base ou état survol). Miroir de OwlMenuStyleBrick (défini plus
 * bas), découplé et placé avant OwlControlDecl car membre par valeur. Les
 * couleurs portent l'encodage OWL_MENU_COL_PACK (BGR + alpha). Sentinelle -1
 * (couleurs/ints) / NULL (police_nom) = « non posé » -> repli au rendu QSS. */
typedef struct {
    int64_t couleur;        /* texte, BGR (+ alpha) ; -1 = non posé */
    int64_t couleur_fond;   /* fond, BGR (+ alpha) ; -1 = non posé */
    char*   police_nom;     /* famille ; NULL = non posé */
    int     police_taille;  /* points ; -1 = non posé */
    int     police_gras;    /* -1 non posé, 0/1 */
    int     police_italique;/* -1 non posé, 0/1 */
    int     police_souligne;/* -1 non posé, 0/1 */
    int     police_barre;   /* -1 non posé, 0/1 */
    int     arrondi;        /*  : arrondi des coins de la languette, px ; -1 = non posé */
} OwlOngletStyleBrick;

/*  — sous-élément CadreExtérieur du style Onglet (la boite du pane,
 * QTabWidget::pane). Fond + bordure (Cadre..Couleur/Épaisseur/Arrondi). Slots
 * plats const-folded, comme la brique de volet. Sentinelle -1 = « non posé »
 * -> repli au défaut du thème au rendu QSS. */
typedef struct {
    int64_t couleur_fond;   /* fond du pane, BGR (+ alpha) ; -1 = non posé */
    int64_t cadre_couleur;  /* couleur de la bordure, BGR (+ alpha) ; -1 = non posé */
    int     cadre_epaisseur;/* épaisseur de la bordure, px ; -1 = non posé */
    int     cadre_arrondi;  /* arrondi des coins, px ; -1 = non posé */
} OwlOngletCadreExt;

/* ---- Declaration d'un sous-champ ----
 *
 * "Bouton btnOK ... FIN" dans une fenetre.
 */

typedef struct OwlControlDecl {
    OwlControlKind     kind;
    char*              name;            /* "btnOK", "lblUser" (alloue) */
    char*              parent_name;     /* Lot F : nom du Conteneur parent
                                         * (ou NULL si enfant direct de la
                                         * Fenetre). Utilise au runtime pour
                                         * resoudre le QWidget parent. */
    OwlPropertyInit*   properties;      /* Tableau dynamique */
    int                property_count;
    int                property_capacity;
    OwlEventBlock*     events;          /* Tableau dynamique */
    int                event_count;
    int                event_capacity;
    /* Phase 6 : PROCEDURE custom dans body widget. Memes type que events
     * mais distinguees pour resolution implicite + nom qualifie au runtime.
     * Stockees comme global '<ctrl>_<proc>' au moment du build_control. */
    OwlEventBlock*     user_procs;
    int                user_proc_count;
    int                user_proc_capacity;
    /* Champ Table V1 : sous-bloc COLONNES { COL_X est une colonne ... FIN }.
     * Vide pour les autres kinds. */
    OwlColumnDecl*     columns;
    int                column_count;
    int                column_capacity;
    /*  — top-level executable du champ : tout le code de niveau 0
     * dans le body du champ (declarations + statements), compile dans une
     * ObjFunction implicite executee a l'ouverture (apres le top-level
     * fenetre, dans l'ordre d'ecriture). NULL si le champ n'a pas de code
     * top-level. */
    ObjFunction*       top_level;
    /*  — menu contextuel attache au champ via `MenuContextuel = <nom>`
     * (clic droit sur le champ). NULL si aucun. Le nom reference un bloc
     * `<nom> est un MenuContextuel` de la fenetre ; resolu au parse (differe a
     * la fermeture du body fenetre, comme main_menu_name). */
    char*              context_menu_name;
    int                context_menu_line;
    /*  — menu de la LOUPE d'un Champ Table, attache via la propriete
     * `MenuLoupe = <nom>` (clic droit sur l'en-tete de colonne). Distinct de
     * context_menu_name (menu du corps) : quand present, il REMPLACE le menu
     * loupe integre. Le nom reference un bloc `<nom> est un MenuContextuel` de
     * la fenetre ; meme resolution differee que context_menu_name. */
    char*              magnifier_menu_name;
    int                magnifier_menu_line;
    /*  — Champ Onglet : capture compile-time de la propriete Valeur
     * (volet actif, 1-based) pour la borner contre le nombre de volets une fois
     * le body parse. ong_valeur_set=false tant que Valeur n'est pas ecrite (le
     * defaut implicite 1 n'est jamais hors bornes des qu'il existe un volet).
     * ong_valeur_col/line = position du token VALEUR pour le diagnostic. */
    bool               ong_valeur_set;
    long               ong_valeur;
    int                ong_valeur_line;
    int                ong_valeur_col;
    /*  — style typé du Champ Onglet (styleOnglet). Slots plats
     * const-folded au parse de la propriété `Style` du bloc PROPRIETES.
     * ong_has_style : au moins une prop posée. CadreExtérieur = fond + bordure
     * du pane (QTabWidget::pane) ; VoletActif/VoletInactif = en-têtes des
     * languettes (base + surcharge état survol). Sentinelle -1/NULL = « non posé ». */
    int                 ong_has_style;
    OwlOngletCadreExt   ong_cadre_ext;
    OwlOngletStyleBrick ong_actif_base;
    OwlOngletStyleBrick ong_actif_survol;
    OwlOngletStyleBrick ong_inactif_base;
    OwlOngletStyleBrick ong_inactif_survol;
    /*  — position (line/col, 1-based ; 0 = non posé) d'une borne de
     * valeur numérique (BorneMax/BorneMin) déclarée sur un Champ Saisie. Sert
     * au warning cross-propriété différé (l'InputType peut suivre la borne dans
     * le bloc) : émis après le parse complet du body si le type de saisie
     * effectif n'est pas bornable. */
    int                bound_max_line, bound_max_col;
    int                bound_min_line, bound_min_col;
    /*  — position (line/col, 1-based ; 0 = non posé) d'un
     * FormatAffichage déclaré sur un Champ Saisie. Sert au warning
     * cross-propriété différé (le TypeSaisie peut suivre FormatAffichage dans
     * le bloc) : émis après le parse complet du body si le format est enrichi
     * alors que le type de saisie effectif n'est pas multiligne. */
    int                faff_line, faff_col;
    int                line;
} OwlControlDecl;

/* ---- Menu classique de fenetre ----
 *
 * `MNU_X est un MenuPrincipal ... FIN` declare un arbre de menu nomme,
 * bloc enfant de la Fenetre. La fenetre choisit lequel s'applique via la
 * propriete d'affectation `MenuPrincipal = MNU_X` de son bloc PROPRIETES
 * (plusieurs MenuPrincipal declarables, un seul affecte).
 *
 * L'arbre est fait de 3 sortes de noeuds :
 *   - SousMenu       : conteneur deroulant (top-level ou imbrique)
 *   - OptionMenu     : item terminal, porte l'action surSelection()
 *   - MenuSeparateur : trait de separation, sans libelle ni action
 */

typedef enum {
    OWL_MENU_NODE_SUBMENU,
    OWL_MENU_NODE_OPTION,
    OWL_MENU_NODE_SEPARATOR,
} OwlMenuNodeKind;

typedef struct OwlMenuNode {
    OwlMenuNodeKind      kind;
    char*                name;             /* "SM_Fichier", "OPT_Quitter" */
    OwlPropertyInit*     properties;
    int                  property_count;
    int                  property_capacity;
    OwlEventBlock*       events;           /* surSelection (OptionMenu) */
    int                  event_count;
    int                  event_capacity;
    struct OwlMenuNode** children;         /* SousMenu uniquement */
    int                  child_count;
    int                  child_capacity;
    int                  line;
} OwlMenuNode;

/*  — encodage d'un slot couleur du bloc Style menu (int64). Une
 * couleur OPAQUE (couleur nommee, RVB(...), Couleur(r,v,b)) tient sur 24 bits
 * BGR (0..0xFFFFFF) : octets hauts a 0, rendue en rgb(). Une couleur
 * SEMI-TRANSPARENTE (Couleur(r,v,b,a) avec a<255) pose le flag
 * OWL_MENU_COL_ALPHA (bit 32) + l'octet alpha en bits 24-31, rendue en rgba().
 * Sentinelle -1 = non pose. Convention partagee mot pour mot avec owl_ui.h
 * (structs miroir volontairement decouplees). */
#define OWL_MENU_COL_ALPHA (1LL << 32)
#define OWL_MENU_COL_PACK(bgr, a) \
    ((a) >= 255 ? (int64_t)((bgr) & 0xFFFFFF) \
                : (OWL_MENU_COL_ALPHA | ((int64_t)((a) & 0xFF) << 24) \
                                      | (int64_t)((bgr) & 0xFFFFFF)))

/*  (ticket C) — surcharge de style d'une surface texte du menu
 * (Barre / OptionMenu, base ou etat survol). Slots plats const-folded au parse.
 * Sentinelle -1 (ints) / NULL (police_nom) = « non pose » -> repli sur le
 * defaut du theme au rendu QSS. */
typedef struct {
    int64_t couleur;        /* texte, BGR (+ alpha encode) ; -1 = non pose */
    int64_t couleur_fond;   /* fond, BGR (+ alpha encode) ; -1 = non pose */
    char* police_nom;       /* famille de police ; NULL = non pose */
    int   police_taille;    /* points ; -1 = non pose */
    int   police_gras;      /* -1 non pose, 0/1 */
    int   police_italique;  /* -1 non pose, 0/1 */
    int   police_souligne;  /* -1 non pose, 0/1 */
    int   police_barre;     /* barre (strikethrough) ; -1 non pose, 0/1 */
} OwlMenuStyleBrick;

typedef struct {
    char*          name;                   /* "MNU_Principal" */
    OwlMenuNode**  children;               /* noeuds de premier niveau */
    int            child_count;
    int            child_capacity;
    int            line;
    /*  — arbre de menu contextuel (clic droit) plutot qu'un
     * MenuPrincipal (barre). 0 = MenuPrincipal, 1 = MenuContextuel. Un menu
     * contextuel n'a pas de surface Barre dans son bloc Style et exige au
     * moins un noeud (pas de menu contextuel vide). */
    int            is_context;
    /*  (ticket C) — style typé du menu (bloc `Style`). Slots plats,
     * sentinelle « non pose » -> repli au defaut du theme. has_style : au
     * moins une prop a ete posee. */
    int               has_style;
    OwlMenuStyleBrick barre_base;
    OwlMenuStyleBrick barre_survol;
    OwlMenuStyleBrick option_base;
    OwlMenuStyleBrick option_survol;
    int64_t           sousmenu_couleur_fond;        /* -1 = non pose */
    int64_t           sousmenu_couleur_separateur;   /* -1 = non pose */
    /*  lot 3-ter — bordure + arrondi du panneau (props plates). */
    int               sousmenu_arrondi;             /* -1 = non pose */
    int               sousmenu_epaisseur;           /* -1 = non pose */
    int64_t           sousmenu_couleur_bordure;      /* -1 = non pose */
    /*  — forme REFERENCE `Style = theme.<nom>` (ou `= STY_<nom>`, ou
     * toute expression). L'expression est compilee au parse, resolue au montage
     * de la fenetre : l'instance obtenue est la BASE que les slots plats
     * ci-dessus surchargent. has_style_ref : une reference a ete posee. */
    int               has_style_ref;
    OwlPropertyInit   style_ref;
} OwlMenuDecl;

/* ---- Variable propre a la fenetre ----
 *
 * "nbTentatives est un entier = 0" dans le corps de Fenetre.
 * Garde le nom + type + valeur initiale optionnelle.
 */

typedef struct {
    char*           name;          /* "nbTentatives" (alloue) */
    char*           type_name;     /* "entier", "chaine", ... (alloue) */
    ObjFunction*    init;          /* Bytecode init optionnel */
    int             line;
} OwlWindowVariable;

/* ---- Parametre de fenetre (`FEN_X est une Fenetre(<name> est un <type>)`) ----
 *
 * Symetrie stricte avec OwlEtatParam : les params declares sur la 1ere ligne
 * sont accessibles comme globales dans tout le scope de la fenetre (top-level
 * + champs + events + PROCEDUREs). Au runtime, Ouvre(FEN_X, val1, ...) injecte
 * leurs valeurs avant l'execution du top-level fenetre. */
typedef struct {
    char*  name;          /* "niveau", "etiquette" (alloue) */
    char*  type_name;     /* "entier", "chaine", NULL si non type (alloue ou NULL) */
    Value  default_value; /* Litteral par defaut si has_default, sinon NIL_VAL */
    bool   has_default;   /* true si `= <litteral>` fourni a la declaration */
    int    line;
} OwlWindowParam;

/* ---- Declaration de fenetre top-level ----
 *
 * Stockage : table globale vm.windows indexee par nom (FEN_X).
 * Cree au moment du parsing. Consommee par les natives Ouvre/OuvreFille.
 */

/* Tag struct explicite : permet une forward declaration depuis
 * object.h pour les ObjWindowInstance.decl sans tirer ui_decl.h dans
 * object.h (qui creerait un cycle d'inclusion). */
typedef struct OwlWindowDecl {
    Obj                obj;             /* Heritage Obj pour GC */
    char*              name;            /* "FEN_Login" (alloue) */
    OwlPropertyInit*   properties;      /* Init des proprietes type Fenetre */
    int                property_count;
    int                property_capacity;
    OwlWindowVariable* variables;       /* Variables propres a la fenetre */
    int                variable_count;
    int                variable_capacity;
    OwlControlDecl*    controls;        /* Sous-champs */
    int                control_count;
    int                control_capacity;
    /*  — arbres de menu classique declares dans la fenetre. */
    OwlMenuDecl**      menus;
    int                menu_count;
    int                menu_capacity;
    /* Nom du MenuPrincipal affecte via la propriete `MenuPrincipal = X`
     * du bloc PROPRIETES (NULL si la fenetre n'a pas de menu). */
    char*              main_menu_name;
    int                main_menu_line;
    /*  — menu contextuel attache a la fenetre elle-meme (clic droit
     * sur une zone vide) via `MenuContextuel = <nom>` du bloc PROPRIETES.
     * NULL si aucun. Meme resolution differee que main_menu_name. */
    char*              context_menu_name;
    int                context_menu_line;
    OwlEventBlock*     events;          /* Initialisation, Fermeture, etc. */
    int                event_count;
    int                event_capacity;
    /* Phase 6 : PROCEDURE custom dans body fenetre. */
    OwlEventBlock*     user_procs;
    int                user_proc_count;
    int                user_proc_capacity;
    /* Params declares sur la 1ere ligne `Fenetre(p1 est un type, ...)`. Vide si
     * fenetre sans params. Injectes au runtime par Ouvre(FEN_X, val1, ...). */
    OwlWindowParam*    params;
    int                param_count;
    int                param_capacity;
    /*  — top-level executable de la fenetre : tout le code de
     * niveau 0 dans le body de la fenetre (declarations globales +
     * statements), compile dans une ObjFunction implicite executee a
     * l'ouverture EN PREMIER (avant les top-levels des champs). Ses
     * declarations de variables sont GLOBALES (visibles dans champs /
     * procedures / events). NULL si pas de code top-level. */
    ObjFunction*       top_level;
    int                line;
} OwlWindowDecl;

/* ---- API ---- */

/* Allocation / liberation */
OwlWindowDecl*  owl_window_decl_new(const char* name, int line);
void            owl_window_decl_free(OwlWindowDecl* w);

OwlControlDecl* owl_control_decl_new(OwlControlKind kind, const char* name, int line);
void            owl_control_decl_free(OwlControlDecl* c);
/* Lot F : tag un sous-controle avec le nom de son Conteneur parent. NULL ou "" = enfant direct fenetre. */
void            owl_control_decl_set_parent_name(OwlControlDecl* c, const char* parent_name);

void            owl_property_init_free(OwlPropertyInit* p);
void            owl_event_block_free(OwlEventBlock* e);
void            owl_window_variable_free(OwlWindowVariable* v);

/* Append helpers (resize les tableaux dynamiques) */
void            owl_window_decl_add_property(OwlWindowDecl* w, const char* name,
                                              Value const_value, ObjFunction* eval,
                                              bool is_constant, int line);
void            owl_window_decl_add_control(OwlWindowDecl* w, OwlControlDecl* c);
void            owl_window_decl_add_event(OwlWindowDecl* w, const char* name,
                                           ObjFunction* impl, int line);
void            owl_window_decl_add_user_proc(OwlWindowDecl* w, const char* name,
                                               ObjFunction* impl, int line);
/*  — peuplement variables[] (mise en service de l'infrastructure
 * dormante pour les Groupes top-level fenetre). type_name = "Groupe" pour
 * l'instant ; future extension possible aux autres declarations
 * windowed-only. */
void            owl_window_decl_add_variable(OwlWindowDecl* w, const char* name,
                                              const char* type_name, int line);

/* Declare un parametre de fenetre (`Fenetre(<name> [est un <type>] [= <defaut>])`).
 * has_default=false + default_value ignore quand le param n'a pas de defaut. */
void            owl_window_decl_add_param(OwlWindowDecl* w, const char* name,
                                           const char* type_name,
                                           Value default_value, bool has_default,
                                           int line);

void            owl_control_decl_add_property(OwlControlDecl* c, const char* name,
                                                Value const_value, ObjFunction* eval,
                                                bool is_constant, int line);
void            owl_control_decl_add_event(OwlControlDecl* c, const char* name,
                                             ObjFunction* impl, int line);
void            owl_control_decl_add_user_proc(OwlControlDecl* c, const char* name,
                                                ObjFunction* impl, int line);

/* Champ Table : declarations de colonnes. Le caller doit avoir verifie
 * que c->kind == OWL_CONTROL_TABLE. */
OwlColumnDecl*  owl_control_decl_add_column(OwlControlDecl* c, const char* name, int line);
/*  — refus des doublons de colonnes. */
const OwlColumnDecl* owl_control_decl_find_column(const OwlControlDecl* c, const char* name);
void            owl_column_decl_add_property(OwlColumnDecl* col, const char* name,
                                              Value const_value, ObjFunction* eval,
                                              bool is_constant, int line);
/*  — arbre de menu classique. */
OwlMenuDecl*    owl_window_decl_add_menu(OwlWindowDecl* w, const char* name, int line);
void            owl_window_decl_set_main_menu(OwlWindowDecl* w, const char* name, int line);
/*  — attache d'un menu contextuel (`MenuContextuel = <nom>`) sur un
 * champ ou sur la fenetre. Resolution differee a la fermeture du body. */
void            owl_control_decl_set_context_menu(OwlControlDecl* c, const char* name, int line);
void            owl_window_decl_set_context_menu(OwlWindowDecl* w, const char* name, int line);
/*  — attache du menu de la loupe (`MenuLoupe = <nom>`) sur un Champ
 * Table. Meme resolution differee que le menu contextuel. */
void            owl_control_decl_set_magnifier_menu(OwlControlDecl* c, const char* name, int line);
/*  — remet les slots plats de style Onglet a « non pose ». Appele quand
 * la propriete Style ENTIERE est (re)affectee par reference. */
void            owl_control_decl_reset_onglet_style(OwlControlDecl* c);
/*  — pendant menu du couple ci-dessus : remet les slots plats de style
 * du menu a « non pose », puis enregistre l'expression de la reference. */
void            owl_menu_decl_reset_style(OwlMenuDecl* m);
void            owl_menu_decl_set_style_ref(OwlMenuDecl* m, ObjFunction* eval, int line);
OwlMenuNode*    owl_menu_decl_add_child(OwlMenuDecl* m, OwlMenuNodeKind kind,
                                        const char* name, int line);
OwlMenuNode*    owl_menu_node_add_child(OwlMenuNode* parent, OwlMenuNodeKind kind,
                                        const char* name, int line);
void            owl_menu_node_add_property(OwlMenuNode* n, const char* name,
                                           Value const_value, ObjFunction* eval,
                                           bool is_constant, int line);
void            owl_menu_node_add_event(OwlMenuNode* n, const char* name,
                                        ObjFunction* impl, int line);
bool            owl_menu_node_has_property(const OwlMenuNode* n, const char* name);
/*  — refus des doublons de noeuds et d'évènements de menu. */
const OwlMenuNode* owl_menu_children_find(OwlMenuNode* const* children,
                                          int count, const char* name);
bool            owl_menu_node_has_event(const OwlMenuNode* n, const char* name);
/* Nom canon FR/EN affichable d'une sorte de noeud (messages de compilation). */
const char*     owl_menu_node_kind_name_fr(OwlMenuNodeKind kind);

/* Lookup */
const OwlMenuDecl*     owl_window_decl_find_menu(const OwlWindowDecl* w, const char* name);
const OwlControlDecl*  owl_window_decl_find_control(const OwlWindowDecl* w, const char* name);

/* Conversion control kind / nom */
const char*    owl_control_kind_name(OwlControlKind kind);
/* Forme canon accentuee (nameFR) pour les messages user-facing. Contrairement
 * a owl_control_kind_name (forme ASCII, slots runtime), retourne « Libellé »,
 * « Sélecteur », « ZoneRépétée » — identique a la forme affichee a la compile. */
const char*    owl_control_kind_name_fr(OwlControlKind kind);
OwlControlKind owl_control_kind_from_name(const char* name);

/* Lot F — single source of truth FR/ASCII/EN pour les widget kinds + sous-blocs
 * DSL. Consomme par main.c::dump_vscode_data pour generer la grammaire
 * TextMate de coloration auto. Tableau statique termine par {NULL,...,false}. */
typedef struct {
    const char* nameFR;      /* Forme canon avec accents (ex: "Fenêtre"). */
    const char* nameAscii;   /* Alias ASCII si different (ex: "Fenetre"), sinon NULL. */
    const char* nameEN;      /* Forme EN (ex: "Window"). */
    bool        is_widget;   /* true = widget reel (Bouton, Conteneur...) ;
                              * false = sous-bloc DSL (EnTeteDocument, Corps...). */
    /* Propriete par defaut visee par `WIDGET = X` (affectation directe au
     * widget, hors bloc PROPRIETES). Convention canon : universellement
     * "Valeur" pour les widgets scalaires (Libelle/Bouton/Saisie/Image/Combo/Liste
     * tous equivalents a ..Valeur). NULL pour les widgets composes (Table avec
     * lignes TAB-separees, Interrupteur indexe par option, ZoneRepetee
     * via API) — le compilo refuse l'affectation directe avec un message
     * pointant l'alternative.
     *
     * Source unique de verite pour :
     *   - compiler.c : translate `WIDGET = X` en `WIDGET..<defaultProperty> = X`
     *   - cli_doc.c (owl explain) : affiche "propriete par defaut : ..X"
     *   - LSP hover : mentionne la prop par defaut
     *   - registry_self_test rule 25 : invariant garde (tout widget
     *     assignable a defaultProperty non-NULL ET cette prop est exposee
     *     dans son PROPS_<KIND>_FIELD catalog). */
    const char* defaultProperty;
    /* Préfixe de nommage canonique du type, souligné inclus :
     * "BTN_" pour Bouton, "SAI_" pour Saisie, "COL_" pour Colonne, etc.
     * NULL pour les sortes sans préfixe conventionnel (Fenêtre, Onglet,
     * CodeBarres, Volet) et tous les sous-blocs structurels (états, menus,
     * Itération). Lu par le compilo quand un projet active la propriété
     * opt-in ActiveAvertissementConventionDeNommage : un champ dont le nom ne
     * commence pas par ce
     * préfixe déclenche un avertissement non bloquant. Donnée PASSIVE : la
     * convention de préfixe n'est jamais imposée par le compilo hors flag. */
    const char* canonicalPrefix;
    /* Description user-facing, bilingue. Affichée par le doc HTML
     * (owl --dump-doc-html) et exploitable par cli_doc. Règle 7 : pas de
     * mention de produit tiers, texte reformulé pour l'utilisateur final. */
    const char* descFR;
    const char* descEN;
    /* Genre grammatical du nom FR (, achevement de ). Donnee
     * PASSIVE lue par `owl translate` pour accorder l'article de la
     * declaration (`a Pane` -> `un Volet`, `a Window` -> `une Fenêtre`). Le
     * compilo et `owl fmt` l'ignorent.
     *
     * Vit ICI, sur la table source de la sorte, et NON dans une table du
     * formateur : les deux graphies (FR/EN) et le genre se lisent du meme
     * enregistrement, donc une sorte ajoutee ne peut plus arriver a moitie
     * (regle 57 du self-test l'exige non-NONE).
     *
     * Type `int` et non `OwlGenre` : ui_decl.c est compile AUSSI dans owl-lsp,
     * qui ne linke pas le registre. Meme frontiere que OwlFmtCanon::type_genre.
     * Valeurs : 0 = OWL_GENRE_NONE, 1 = OWL_GENRE_M, 2 = OWL_GENRE_F. */
    int         genre;
} OwlControlKindInfo;

/* Genre grammatical d'une sorte, valeurs de OwlGenre (owl_registry.h) sans
 * dependre du registre. Utilise par CONTROL_KINDS et par le self-test. */
#define OWL_KIND_GENRE_NONE 0
#define OWL_KIND_GENRE_M    1
#define OWL_KIND_GENRE_F    2
const OwlControlKindInfo* owl_control_kinds_table(void);

/* Lookup unifie : un token (UTF-8, casse libre, accents possibles) matche-t-il
 * un widget kind canon ? Si oui, *out_kind recoit le OwlControlKind, *out_info
 * pointe vers l'entree CONTROL_KINDS (utile pour acceder a nameFR/Ascii/EN).
 * Both out_kind et out_info acceptent NULL si pas necessaire.
 *
 * Source de verite unique pour formatter.c, compiler.c (check_ident_*),
 * lsp_handlers.c. Implementation : strip accents + lowercase + scan
 * CONTROL_KINDS, puis mapping nom canon -> OwlControlKind. */
bool owl_control_kind_from_token(const char* token, int token_len,
                                  OwlControlKind* out_kind,
                                  const OwlControlKindInfo** out_info);

/* Variante sans token : retourne l'OwlControlKind d'une entree CONTROL_KINDS
 * deja resolue (utile pour le LSP qui itere la table). */
bool owl_control_kind_of_info(const OwlControlKindInfo* info,
                               OwlControlKind* out_kind);

/*  — entree CONTROL_KINDS d'un OwlControlKind (widgets seulement ;
 * NULL pour les kinds non mappes ou hors table). */
const OwlControlKindInfo* owl_control_kind_info(OwlControlKind kind);

/*  — entree CONTROL_KINDS resolue par nom canon FR/Ascii/EN, y
 * compris les sous-blocs is_widget=false (ex "Colonne"). NULL si aucun match. */
const OwlControlKindInfo* owl_control_kind_info_by_name(const char* name);

/* DSL Fenetre v1 (Phase E.X) — dispatch 'ctrl..Prop' depuis OP_STRUCT_GET.
 * Implementation owl-gui-only via owl_ui_register_get_prop(). En mode CLI
 * (owl.exe sans GUI), le default retourne NIL_VAL. */
typedef Value (*OwlUiControlGetPropFn)(ObjUiControl* c, const char* prop);
typedef bool  (*OwlUiControlSetPropFn)(ObjUiControl* c, const char* prop, Value v);

Value owl_ui_control_get_prop(ObjUiControl* c, const char* prop);
void  owl_ui_register_get_prop(OwlUiControlGetPropFn fn);

bool  owl_ui_control_set_prop(ObjUiControl* c, const char* prop, Value v);
void  owl_ui_register_set_prop(OwlUiControlSetPropFn fn);

/* V1.5-D : hook appele par OP_STRUCT_SET sur n'importe quelle ObjInstance.
 * Permet a stdlib_ui de re-apply Qt6 quand une instance attachee a un
 * widget (Cadre/Coin/Trait/Police/Style) est modifiee via chained set.
 * default no-op. */
typedef void (*OwlUiInstanceModifiedFn)(ObjInstance* inst);
void  owl_ui_on_instance_modified(ObjInstance* inst);
void  owl_ui_register_on_instance_modified(OwlUiInstanceModifiedFn fn);

/*  lot 2 — Qt observe le store d'instance.
 *
 * apply_scalar : appele apres une ecriture scalaire dans le store d'une
 *   ObjUiControlInstance qui a un widget Qt monte. Re-applique la valeur au
 *   QWidget (setter Qt) pour que l'imperatif soit reflete en GUI comme il
 *   l'est en headless. default no-op (mode CLI).
 * register_composite : appele quand une instance composee (Cadre/Police)
 *   est materialisee dans le store d'une ObjUiControlInstance montee. Relie
 *   l'arbre d'instance composee a son widget pour que owl_ui_on_instance_modified
 *   retrouve le QWidget lors d'un chained set. default no-op (mode CLI). */
typedef void (*OwlUiInstanceScalarAppliedFn)(struct ObjUiControlInstance* uici,
                                             const char* prop, Value v);
void  owl_ui_apply_instance_scalar(struct ObjUiControlInstance* uici,
                                   const char* prop, Value v);
void  owl_ui_register_apply_instance_scalar(OwlUiInstanceScalarAppliedFn fn);

typedef void (*OwlUiRegisterCompositeFn)(struct ObjUiControlInstance* uici,
                                         ObjInstance* composite);
void  owl_ui_register_instance_composite(struct ObjUiControlInstance* uici,
                                         ObjInstance* composite);
void  owl_ui_register_register_composite(OwlUiRegisterCompositeFn fn);

/*  — logique metier declenchee APRES une ecriture scalaire dans le
 * store d'une ObjUiControlInstance (call-site OP_STRUCT_SET / IS_UI_CONTROL_REF
 * de vm.c). Analog direct d'owl_ui_apply_instance_scalar, mais appele
 * INCONDITIONNELLEMENT (headless ET GUI) car la logique vit au niveau store,
 * pas au niveau widget. `old` est l'ancienne valeur du store (avant le set),
 * `neuf` la valeur ecrite. Sert au Champ Onglet : tirer surChangementVolet sur
 * un changement reel de ..Valeur, et reporter le volet actif quand le volet
 * courant est masque. default no-op (aucun autre kind concerne). */
typedef void (*OwlUiControlLogicAfterSetFn)(struct ObjUiControlInstance* uici,
                                            const char* prop,
                                            Value old, Value neuf);
void  owl_ui_control_logic_after_set(struct ObjUiControlInstance* uici,
                                     const char* prop, Value old, Value neuf);
void  owl_ui_register_control_logic_after_set(OwlUiControlLogicAfterSetFn fn);

/*  lot C — Qt observe le store d'instance fenetre.
 *
 * apply_window_scalar : appele apres une ecriture scalaire dans le store
 *   d'une ObjWindowInstance qui a un widget Qt monte. Re-applique la valeur
 *   au QMainWindow (helpers owl_ui_window_set_*). Symetrique strict de
 *   owl_ui_apply_instance_scalar pour les sous-controles. default no-op
 *   (mode CLI / headless). */
typedef void (*OwlUiWindowScalarAppliedFn)(struct ObjWindowInstance* wi,
                                           const char* prop, Value v);
void  owl_ui_apply_window_scalar(struct ObjWindowInstance* wi,
                                 const char* prop, Value v);
void  owl_ui_register_apply_window_scalar(OwlUiWindowScalarAppliedFn fn);

/*  — lecture symetrique : rend la geometrie que la fenetre occupe
 * VRAIMENT quand elle porte un widget Qt monte. Retourne false si la prop
 * n'est pas concernee ou si la fenetre n'est pas montee ; l'appelant retombe
 * alors sur son flow standard (store, puis declaratif). default false. */
typedef bool (*OwlUiWindowLiveGetFn)(struct ObjWindowInstance* wi,
                                     const char* prop, Value* out);
bool  owl_ui_window_live_get(struct ObjWindowInstance* wi,
                             const char* prop, Value* out);
void  owl_ui_register_window_live_get(OwlUiWindowLiveGetFn fn);

/* S.4 — hook pour l'indexation widget[i] (Champ Selecteur) qui retourne
 * un proxy d'option avec back-ref. Default = NIL_VAL (pas d'indexation). */
typedef Value (*OwlUiControlGetSubitemFn)(ObjUiControl* c, int index_1based);
Value owl_ui_control_get_subitem(ObjUiControl* c, int index_1based);
void  owl_ui_register_get_subitem(OwlUiControlGetSubitemFn fn);

/*  — hook pour l'indexation ZR[i] quand le receveur est une
 * ObjUiControlInstance (cas DSL Fenetre : ZR global = ObjUiControlRef
 * resolu vers son instance). Retourne le proxy de repetition :
 *   - GUI (uici->widget != NULL) : ObjUiControl(CONTAINER) sur le QWidget
 *     rep (STRUCT_GET/SET route vers le sub-widget par objectName).
 *   - headless : l'ObjInstance "rep" du store d'items (STRUCT_GET/SET
 *     route vers ses champs par nom de sous-controle).
 * NIL_VAL si pas une ZR, idx hors-bornes, ou pas de rep a cet indice. */
typedef Value (*OwlUiControlInstanceGetSubitemFn)(struct ObjUiControlInstance* uici,
                                                  int index_1based);
Value owl_ui_control_instance_get_subitem(struct ObjUiControlInstance* uici,
                                          int index_1based);
void  owl_ui_register_instance_get_subitem(OwlUiControlInstanceGetSubitemFn fn);

/*  — hook appele par OP_STRUCT_SET (vm.c, branche ObjInstance
 * generique) pour coercer la valeur ecrite dans une cellule de rep ZR
 * selon le contrat de valeur de son sous-controle. Une rep ZR headless
 * est taggee `$zrsrc` (back-ref uici) par zr_build_headless_rep ; le hook
 * retrouve le sous-controle nomme `field` dans le decl fenetre et, si
 * c'est une Saisie, coerce `v` en chaine (parite SAI..Valeur ).
 * Toute autre instance (struct user, rep Table typee, sous-controle non
 * Saisie) : `v` retournee inchangee. Default = identite (mode CLI). */
typedef Value (*OwlUiZrRepCoerceFn)(ObjInstance* rep, const char* field, Value v);
Value owl_ui_zr_rep_coerce_cell(ObjInstance* rep, const char* field, Value v);
void  owl_ui_register_zr_rep_coerce(OwlUiZrRepCoerceFn fn);

/*  — hook OP_STRUCT_GET/SET (vm.c, branche ObjInstance generique)
 * pour materialiser PARESSEUSEMENT une cellule de rep ZR en ObjUiControl
 * (widget==NULL, headless_store = store de props par cellule). Permet
 * `ZR[i].champ..Prop` (get/set) hors fenetre Qt : le `.champ` rend un
 * ObjUiControl, le `..Prop` suivant tombe dans la branche IS_UI_CONTROL.
 *
 * Garde STRICTE (isole Table) : la rep doit etre taggee `$zrsrc` ET son
 * uici source de kind ZoneRepetee ; `field` doit matcher un sous-controle
 * de la ZR. Sinon -> NIL_VAL (le caller retombe sur table_get standard).
 *
 * Idempotent : si le champ est deja un ObjUiControl, le retourne tel quel.
 * Sinon materialise (seed = scalaire courant sous la prop par defaut) et
 * remplace le champ dans la rep (inst_field_set_value). Default = NIL_VAL
 * (mode CLI). */
typedef Value (*OwlUiZrRepCellSubcontrolFn)(ObjInstance* rep,
                                            const char* field);
Value owl_ui_zr_rep_cell_subcontrol(ObjInstance* rep, const char* field);
void  owl_ui_register_zr_rep_cell_subcontrol(OwlUiZrRepCellSubcontrolFn fn);

/*  — valeur par defaut d'une prop quand le widget n'est pas
 * attache ET que le headless_store n'a pas (encore) cette prop. Derive
 * du type catalog : BOOL Visible/Actif=Vrai sinon Faux ; STRING="" ;
 * INT/COLOR=0 ; autres=Nul. Implementee dans ui_catalog.c (besoin du
 * lookup catalog par kind). */
Value owl_ui_headless_default(ObjUiControl* c, const char* prop);

/*  — Count items widgets-a-items : function pointers registered par
 * stdlib_ui_init (mode GUI). En mode CLI (owl-cli) ou owl-lsp : defaults
 * = 0, le compilateur refuse anyway les programmes UI (Ouvre/...) hard
 * compile error. Pattern adopte pour eviter de linker stdlib_ui.c +
 * object.c (lourd, GC, etc.) cote lsp qui ne fait que du parse statique.
 *
 *  — generalises (Liste/Combo + Selecteur/Interrupteur/ZoneRepetee) :
 *   - widget_items_count(kind, widget) : dispatch vers le bon
 *     owl_ui_<X>_count selon le kind.
 *   - headless_widget_items_count_*    : lecture du ObjArray store
 *     (kind-agnostic ; meme clef OWL_UI_LIST_ITEMS_KEY pour tous). */
typedef int (*OwlUiWidgetItemsCountFn)(int kind, void* widget);
int   owl_ui_widget_items_count(int kind, void* widget_or_null);
void  owl_ui_register_widget_items_count(OwlUiWidgetItemsCountFn fn);

typedef int (*OwlUiHeadlessWidgetItemsCountUicFn)(ObjUiControl* uic);
int   owl_ui_headless_widget_items_count_uic(ObjUiControl* uic);
void  owl_ui_register_headless_widget_items_count_uic(OwlUiHeadlessWidgetItemsCountUicFn fn);

typedef int (*OwlUiHeadlessWidgetItemsCountUiciFn)(struct ObjUiControlInstance* uici);
int   owl_ui_headless_widget_items_count_uici(struct ObjUiControlInstance* uici);
void  owl_ui_register_headless_widget_items_count_uici(OwlUiHeadlessWidgetItemsCountUiciFn fn);

/*  — Cle reservee du headless_store pour le tableau d'items
 * widgets-a-items. Stocke un ObjArray de Value. Pas de collision avec une
 * prop user : prefixe '__owl_'.  etend Sel/Int/ZR (meme clef). */
#define OWL_UI_LIST_ITEMS_KEY "__owl_items__"

/*  — proxy-colonne pour la forme col-first `TBL.COL_Nom[ligne]`.
 * `TBL.COL_Nom` resout (dans OP_STRUCT_GET) en un ObjInstance leger portant
 * ces 2 marqueurs internes ; `[ligne]` (OP_ARRAY_GET) les detecte, re-resout
 * la Table via la ref et lit la cellule depuis le store partage avec la forme
 * ligne-first -> meme valeur typee, meme OOB soft. Prefixe '__owl_'
 * : pas de collision avec un champ user. */
#define OWL_TABLE_COLPROXY_REF_KEY "__owl_tablecolref__"
#define OWL_TABLE_COLPROXY_COL_KEY "__owl_tablecol__"

/*  — mise en forme par cellule / par ligne de Table.
 *
 * OWL_TABLE_REP_SRC_KEY tague la rep de ligne du store d'items avec une
 * back-ref vers l'ObjUiControlInstance de la Table (miroir strict de `$zrsrc`,
 * ). Sans ce tag, une rep de ligne est indistinguable d'un struct
 * user : ni OP_ARRAY_GET (descripteur de cellule) ni OP_STRUCT_GET/SET (mise
 * en forme de ligne) ne pourraient se declencher sans faux positif.
 *
 * OWL_TABLE_CELLDESC_* portent le descripteur de cellule rendu par
 * `TABLE_X[ligne][colonne]` : la rep de ligne cible du stockage + le nom canon
 * de la colonne (accents strippes, meme forme que les cles de cellule posees
 * par la construction de la rep).
 *
 * OWL_TABLE_FMT_KEY_PREFIX prefixe les cles de stockage effectives, de la forme
 * `$fmt.<colonne>.<PropCanonFR>` ; partie colonne vide = niveau ligne. La prop
 * est toujours stockee sous sa forme canon FR pour que `Couleur` et `Color`
 * visent le meme casier (invariant 3).
 *
 * Prefixe '$' : ne peut pas collisionner avec un nom de colonne (identifiant). */
#define OWL_TABLE_REP_SRC_KEY      "$tblsrc"
#define OWL_TABLE_CELLDESC_REP_KEY "$tblcellrep"
#define OWL_TABLE_CELLDESC_COL_KEY "$tblcellcol"
#define OWL_TABLE_FMT_KEY_PREFIX   "$fmt"

/*  — mise en forme PAR COLONNE (forme nue `COL_A..Couleur`).
 *
 * La forme nue desugar (Lot A) vers `TABLE.COL_A`, qui resout en un
 * proxy-colonne ObjInstance EPHEMERE (, recree a chaque acces). La
 * couleur ne peut donc pas vivre sur le proxy jetable : elle est stockee sur un
 * HOLDER persistant unique, porte par la Table dans son headless_store sous
 * cette cle reservee. Les fields du holder portent les memes cles
 * `$fmt.<col>.<PropCanonFR>` que les reps de ligne : vm_table_fmt_get/set
 * fonctionnent sans changement, seul le RECEVEUR change (holder vs rep). Le
 * holder survit entre deux acces (read-back deterministe) et vit la ou les
 * lignes futures pourront le lire (rendu Qt = lot ulterieur). Prefixe '$' : pas
 * de collision avec une prop user. */
#define OWL_TABLE_COLFMT_HOLDER_KEY "$colfmt"

/*  — etat des filtres utilisateur d'un Champ Table, interrogeable
 * headless (TableColonnesFiltrees / TableDesactiveFiltre). Stocke dans le
 * headless_store de la Table un ObjArray de reps ObjInstance nues (une par
 * colonne filtree) portant les champs `col` (identifiant canon de colonne),
 * `type` (constante filtre*) et `val` (valeur du filtre). Le widget Qt reste
 * la source de verite pour le masquage visuel (rendu = lot ulterieur) ; ce
 * store sert la lecture d'etat par programme. Prefixe '$' : pas de collision
 * avec un nom de colonne (identifiant). */
#define OWL_TABLE_FILTERS_KEY "$tblfilters"

/*  — hook appele APRES le stockage d'une propriete de mise en forme,
 * pour la pousser vers le QStandardItem correspondant. Le store reste la source
 * de verite (la relecture ne passe jamais par Qt) : ce hook est un pur effet de
 * bord d'affichage, d'ou le default no-op en mode CLI / headless.
 *
 * `col` vide = niveau ligne (toutes les colonnes de la ligne). `propCanon` est
 * la forme canon FR (`Couleur` = texte, `CouleurFond` = fond). La rep porte sa
 * back-ref `$tblsrc` : l'implementation en deduit la Table, et l'indice de
 * ligne par identite de pointeur dans le store d'items (une rep ne connait pas
 * son rang, et un rang memorise deviendrait faux au premier
 * TableSupprime/TableInsere). */
typedef void (*OwlUiTableFmtAppliedFn)(ObjInstance* rep, const char* col,
                                       const char* propCanon, Value v);
void owl_ui_table_fmt_applied(ObjInstance* rep, const char* col,
                              const char* propCanon, Value v);
void owl_ui_register_table_fmt_applied(OwlUiTableFmtAppliedFn fn);

/*  — masque de saisie d'une colonne poussé au widget (miroir GUI du store
 * OWL-side). Hook enregistré comme owl_ui_table_fmt_applied : owl_core reste libre
 * de tout symbole GUI (la CLI sans Qt linke le default no-op). `widget` = le
 * QTableView monté (jamais NULL côté appelant), `col_1based` l'index colonne. */
typedef void (*OwlUiTableColmaskAppliedFn)(void* widget, int col_1based,
                                           const char* wl_mask);
void owl_ui_table_colmask_applied(void* widget, int col_1based,
                                  const char* wl_mask);
void owl_ui_register_table_colmask_applied(OwlUiTableColmaskAppliedFn fn);

#ifdef __cplusplus
}
#endif

#endif /* OWL_UI_DECL_H */
