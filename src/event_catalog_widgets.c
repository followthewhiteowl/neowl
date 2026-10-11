// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* event_catalog_widgets.c — catalogue des events DSL widget.
 *
 * Source de verite unique pour les events disponibles sur chaque kind
 * de widget (Bouton, Libelle, Saisie, Table, Conteneur, Image). Consomme
 * par :
 *   - compiler.c::check_event_signature  (check compile-time)
 *   - stdlib_ui.c::register_widget_events (cabling runtime)
 *   - lsp_handlers.c                       (autocomplete + hover)
 *   - registry_self_test.c regle 8         (validation infra)
 *
 * AJOUTER UN EVENT ICI = automatique partout (compile, runtime, LSP, doc).
 *
 * Convention nommage canon : prefixe "sur" en FR, "on" en EN.
 * Les noms de parametres sont des contrats publics — exposes en hover
 * LSP, donc stables une fois publies. Y reflechir avant de renommer.
 */

#include "owl_event_catalog.h"
#include "event_catalog_widgets.h"
#include "ui_decl.h"   /* OwlControlKind */

#include <string.h>

/* ============================================================
 * Parametres canon mutualises (events partages entre widgets)
 * ============================================================ */

/* Position de la souris dans le widget — utilise par tous les events
 * souris (Clic, BoutonGauche, Survol, MenuContextuel, ...). */
static const OwlEventParam P_souris_xy[] = {
    { "x", "entier",
      "Position horizontale du curseur dans le champ, en pixels.",
      "Horizontal cursor position within the field, in pixels." },
    { "y", "entier",
      "Position verticale du curseur dans le champ, en pixels.",
      "Vertical cursor position within the field, in pixels." },
};

/* Roulette de la souris : mouvement signe + position.
 * NB : on n'utilise pas "pas" car c'est un keyword scanner (NOT/STEP). */
static const OwlEventParam P_molette[] = {
    { "mouvement", "entier",
      "Mouvement signe de la molette : positif = vers le haut, "
      "negatif = vers le bas. La magnitude reflete l'amplitude.",
      "Signed wheel movement: positive = up, negative = down. Magnitude "
      "reflects amplitude." },
    { "x", "entier",
      "Position horizontale du curseur lors du defilement.",
      "Horizontal cursor position during the scroll." },
    { "y", "entier",
      "Position verticale du curseur lors du defilement.",
      "Vertical cursor position during the scroll." },
};

/* Ascenseur (un seul axe a la fois). */
static const OwlEventParam P_ascenseur[] = {
    { "position", "entier",
      "Position courante de la cage de l'ascenseur, en pixels depuis "
      "le debut du contenu.",
      "Current scrollbar thumb position, in pixels from content start." },
};

/* Changement de taille. */
static const OwlEventParam P_resize[] = {
    { "largeur", "entier",
      "Nouvelle largeur du champ apres redimensionnement, en pixels.",
      "New field width after resize, in pixels." },
    { "hauteur", "entier",
      "Nouvelle hauteur du champ apres redimensionnement, en pixels.",
      "New field height after resize, in pixels." },
};

/* Deplacement (move) — coin haut-gauche du champ. */
static const OwlEventParam P_move[] = {
    { "x", "entier",
      "Nouvelle abscisse du coin haut-gauche du champ, en pixels.",
      "New top-left X position of the field, in pixels." },
    { "y", "entier",
      "Nouvelle ordonnee du coin haut-gauche du champ, en pixels.",
      "New top-left Y position of the field, in pixels." },
};

/* Touche clavier — code de la touche.
 * Pour une touche speciale (fleche, Echap, F1...) : code tk* canon
 * (base haute facon Qt::Key). Pour une touche textuelle (lettre, chiffre,
 * espace) : code ASCII, identique a Asc("X"). */
static const OwlEventParam P_touche[] = {
    { "touche", "entier",
      "Code de la touche : constante tk* (fleches, Echap, F1...) pour les "
      "touches speciales, ou code ASCII (identique a Asc(\"X\")) pour les "
      "touches textuelles.",
      "Key code: a tk* constant (arrows, Esc, F1...) for special keys, or "
      "the ASCII code (same as Asc(\"X\")) for textual keys." },
};

/* Indice de ligne 1-based — partage par surAffichageLigne (ZoneRepetee et
 * Table). Le payload runtime est copie dans une variable nommee accessible
 * dans le body de l'event. */
static const OwlEventParam P_zr_indice[] = {
    { "indice", "entier",
      "Indice 1-based de la ligne en cours d'affichage.",
      "1-based index of the row being displayed." },
};

/* ============================================================
 * Familles d'events mutualisees
 *
 * Chaque famille est une table OwlEventDef[] enregistree separement par
 * widget via owl_event_catalog_register. Le catalogue est CUMULATIF : un
 * widget peut empiler plusieurs familles + ses events specifiques.
 *
 * Convention : nommer FAM_<NOM>_EVENTS pour la table, et exposer la
 * macro REGISTER_FAM_<NOM>(kind) qui appelle register avec count.
 * ============================================================ */

/* FOCUS — surEntree / surSortie (focus in / focus out, 0 args).
 * Applicable a tous les champs interactifs (Saisie, Liste, Combo,
 * Table, Bouton, Image, Libelle si interactif). */
static const OwlEventDef FAM_FOCUS_EVENTS[] = {
    { .canonical = "surEntr\xc3\xa9""e", .nameEN = "onEntry",
      .descFR = "Declenche a la prise de focus du champ.",
      .descEN = "Fires when the field gains focus.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surSortie", .nameEN = "onExit",
      .descFR = "Declenche a la perte de focus du champ.",
      .descEN = "Fires when the field loses focus.",
      .paramCount = 0, .params = NULL },
};

/* MOUSE_BASIC — surClic / surDoubleClic (0 args).
 * Sans payload pour V1 : la position se lit via SourisPosX()/PosY()
 * canon si besoin. Applicable a presque tous les widgets. */
static const OwlEventDef FAM_MOUSE_BASIC_EVENTS[] = {
    { .canonical = "surClic", .nameEN = "onClick",
      .descFR = "Declenche au clic gauche dans le champ.",
      .descEN = "Fires on left click within the field.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surDoubleClic", .nameEN = "onDoubleClick",
      .descFR = "Declenche au double-clic gauche dans le champ.",
      .descEN = "Fires on left double-click within the field.",
      .paramCount = 0, .params = NULL },
};

/* MOUSE_ADVANCED — events souris detailles avec coordonnees.
 * Pour les widgets qui veulent gerer le drag/drop, le tracking de
 * position, etc. Image les expose tous ; Liste/Combo/Table peuvent
 * opter pour une partie selon besoin. */
static const OwlEventDef FAM_MOUSE_ADVANCED_EVENTS[] = {
    { .canonical = "surEntr\xc3\xa9""eSurvol", .nameEN = "onMouseEnter",
      .descFR = "Declenche quand le curseur entre dans la zone du champ.",
      .descEN = "Fires when the cursor enters the field area.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surSortieSurvol", .nameEN = "onMouseLeave",
      .descFR = "Declenche quand le curseur quitte la zone du champ.",
      .descEN = "Fires when the cursor exits the field area.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surSurvol", .nameEN = "onMouseMove",
      .descFR = "Declenche en continu pendant le survol du curseur.",
      .descEN = "Fires continuously while the cursor moves over the field.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surBoutonGauche", .nameEN = "onLeftButtonDown",
      .descFR = "Declenche a la pression du bouton gauche de la souris.",
      .descEN = "Fires when the left mouse button is pressed.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surRelacheBoutonGauche", .nameEN = "onLeftButtonUp",
      .descFR = "Declenche au relachement du bouton gauche.",
      .descEN = "Fires when the left mouse button is released.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surBoutonDroit", .nameEN = "onRightButtonDown",
      .descFR = "Declenche a la pression du bouton droit.",
      .descEN = "Fires when the right mouse button is pressed.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surRelacheBoutonDroit", .nameEN = "onRightButtonUp",
      .descFR = "Declenche au relachement du bouton droit.",
      .descEN = "Fires when the right mouse button is released.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surDoubleClicDroit", .nameEN = "onRightDoubleClick",
      .descFR = "Declenche au double-clic avec le bouton droit.",
      .descEN = "Fires on right-button double-click.",
      .paramCount = 2, .params = P_souris_xy },
    { .canonical = "surMolette", .nameEN = "onWheel",
      .descFR = "Declenche au mouvement de la molette de la souris.",
      .descEN = "Fires when the mouse wheel is rotated.",
      .paramCount = 3, .params = P_molette },
};

/* MENU — surMenuContextuel(x, y).
 * Pour les widgets qui exposent un menu contextuel custom. */
static const OwlEventDef FAM_MENU_EVENTS[] = {
    { .canonical = "surMenuContextuel", .nameEN = "onContextMenu",
      .descFR = "Declenche a l'ouverture du menu contextuel "
                "(clic droit ou touche menu).",
      .descEN = "Fires when the context menu is requested "
                "(right-click or menu key).",
      .paramCount = 2, .params = P_souris_xy },
};

/* SCROLL — ascenseurs H et V.
 * Pour un widget dote d'une vraie zone defilante. Aucun champ n'en a une a ce
 * jour : la famille reste definie (elle se re-enregistre en une ligne le jour
 * ou un champ scrollable arrive) mais n'est enregistree sur aucun kind, donc
 * les constantes evtAscenseur* ne sont pas generees. */
__attribute__((unused))
static const OwlEventDef FAM_SCROLL_EVENTS[] = {
    { .canonical = "surAscenseurH", .nameEN = "onHScroll",
      .descFR = "Declenche au mouvement de l'ascenseur horizontal.",
      .descEN = "Fires when the horizontal scrollbar moves.",
      .paramCount = 1, .params = P_ascenseur },
    { .canonical = "surAscenseurV", .nameEN = "onVScroll",
      .descFR = "Declenche au mouvement de l'ascenseur vertical.",
      .descEN = "Fires when the vertical scrollbar moves.",
      .paramCount = 1, .params = P_ascenseur },
};

/* LAYOUT — surDeplacement / surChangementTaille.
 * Pour les widgets qui veulent reagir a leur repositionnement /
 * redimensionnement (rare en V1 mais utile pour layouts custom). */
static const OwlEventDef FAM_LAYOUT_EVENTS[] = {
    { .canonical = "surD\xc3\xa9""placement", .nameEN = "onMove",
      .descFR = "Declenche quand le champ est repositionne dans son parent.",
      .descEN = "Fires when the field is repositioned in its parent.",
      .paramCount = 2, .params = P_move },
    { .canonical = "surChangementTaille", .nameEN = "onResize",
      .descFR = "Declenche au redimensionnement du champ.",
      .descEN = "Fires when the field is resized.",
      .paramCount = 2, .params = P_resize },
};

/* KEYBOARD — surToucheAppuyee / surToucheRelachee.
 * Applicable a tout champ interactif focusable ( : Saisie, Bouton,
 * Table, Liste, Combo, Selecteur, Interrupteur, ZoneRepetee, + Image et
 * CodeBarres pour le canvas) et a la Fenetre (via le catalogue lifecycle,
 * voir FENETRE_LIFECYCLE_EVENTS). Le code de la
 * touche est passe en parametre. C'est l'event canon pour piloter au
 * clavier les touches sans caractere textuel (fleches directionnelles)
 * que surAChaqueModification ne couvre pas. */
static const OwlEventDef FAM_KEYBOARD_EVENTS[] = {
    { .canonical = "surToucheAppuy\xc3\xa9""e", .nameEN = "onKeyDown",
      .descFR = "Declenche a l'appui d'une touche du clavier. Le code de la "
                "touche est passe en parametre (constante tk* ou code ASCII).",
      .descEN = "Fires when a keyboard key is pressed. The key code is passed "
                "as a parameter (tk* constant or ASCII code).",
      .paramCount = 1, .params = P_touche },
    { .canonical = "surToucheRel\xc3\xa2""ch\xc3\xa9""e", .nameEN = "onKeyUp",
      .descFR = "Declenche au relachement d'une touche du clavier. Le code de "
                "la touche est passe en parametre (constante tk* ou code ASCII).",
      .descEN = "Fires when a keyboard key is released. The key code is passed "
                "as a parameter (tk* constant or ASCII code).",
      .paramCount = 1, .params = P_touche },
};

/* Macro helper : enregistre une famille pour un kind donne. Utilise dans
 * event_catalog_widgets_init() ci-dessous. */
#define REG_FAM(kind, fam) \
    owl_event_catalog_register((kind), (fam), \
        (int)(sizeof(fam) / sizeof((fam)[0])))

/* ============================================================
 * Events SPECIFIQUES par widget — au-dela des familles FAM_* mutualisees.
 *
 * Le tronc commun (surClic, surEntree, surSortie, surMolette, etc.)
 * est registre via les FAM_* ci-dessus dans event_catalog_widgets_init.
 * Ici on ne declare que ce qui est PROPRE au widget et ne peut pas etre
 * factorise (Saisie a surValidation, Combo a surOuverture/Fermeture, etc.).
 * ============================================================ */

/* Champ Bouton : pas d'event specifique (surClic est dans FAM_MOUSE_BASIC). */

/* Champ Libelle : pas d'event specifique. */

/* Champ Saisie — events texte. surEntree/surSortie viennent de FAM_FOCUS. */
static const OwlEventDef INPUT_SPECIFIC_EVENTS[] = {
    { .canonical  = "surAChaqueModification",
      .nameEN     = "onEachChange",
      .descFR     = "Declenche a chaque modification du contenu (frappe "
                    "clavier, coller, suppression).",
      .descEN     = "Fires on every content change (keystroke, paste, delete).",
      .paramCount = 0, .params = NULL },
    { .canonical  = "surValidation",
      .nameEN     = "onValidation",
      .descFR     = "Declenche a la validation de la saisie (touche Entree).",
      .descEN     = "Fires when the input is validated (Enter key).",
      .paramCount = 0, .params = NULL },
};

/* Champ Table — events specifiques table. surClic/surDoubleClic via FAM_MOUSE_BASIC. */
static const OwlEventDef TABLE_SPECIFIC_EVENTS[] = {
    { .canonical = "surChangementSelection", .nameEN = "onSelectionChange",
      .descFR    = "Declenche quand la ligne selectionnee change.",
      .descEN    = "Fires when the selected row changes.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surEntr\xc3\xa9""eLigne", .nameEN = "onRowEnter",
      .descFR    = "Declenche quand la souris entre sur une ligne de la table.",
      .descEN    = "Fires when the mouse enters a table row.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surSortieLigne", .nameEN = "onRowExit",
      .descFR    = "Declenche quand la souris quitte une ligne de la table.",
      .descEN    = "Fires when the mouse exits a table row.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surAffichageLigne", .nameEN = "onRowDisplay",
      .descFR    = "Declenche au rendu de chaque ligne de la table. Utilise pour "
                   "personnaliser l'apparence (couleur, visibilite) d'une ligne.",
      .descEN    = "Fires when each table row is rendered. Used to customize "
                   "row appearance (color, visibility) per row.",
      .paramCount = 1, .params = P_zr_indice },
};

/* Champ Image — entierement couvert par les familles FAM_MOUSE_BASIC +
 * FAM_MOUSE_ADVANCED + FAM_MENU + FAM_SCROLL + FAM_LAYOUT. Pas d'events
 * specifiques V1. */

/* Champ Liste — events specifiques (surSelection).
 * surEntree / surSortie viennent de FAM_FOCUS. */
static const OwlEventDef LIST_SPECIFIC_EVENTS[] = {
    { .canonical = "surS\xc3\xa9lection", .nameEN = "onSelection",
      .descFR = "Declenche quand la ligne selectionnee change "
                "(clic, navigation clavier, ou setter programmatique).",
      .descEN = "Fires when the selected row changes "
                "(click, keyboard navigation, or programmatic setter).",
      .paramCount = 0, .params = NULL },
};

/* Champ Selecteur — events specifiques (surAChaqueModification = canon).
 * surEntree/surSortie via FAM_FOCUS. */
static const OwlEventDef SELECTEUR_SPECIFIC_EVENTS[] = {
    { .canonical  = "surAChaqueModification",
      .nameEN     = "onEachChange",
      .descFR     = "Declenche quand l'option selectionnee change.",
      .descEN     = "Fires when the selected option changes.",
      .paramCount = 0, .params = NULL },
};

/* Champ Interrupteur — meme events que Selecteur (surAChaqueModification
 * declenche quand UNE option change). FAM_FOCUS pour Entree/Sortie.
 * Note WIP autre agent : déclaré mais pas encore référencé dans le widget. */
__attribute__((unused))
static const OwlEventDef INTERRUPTEUR_SPECIFIC_EVENTS[] = {
    { .canonical  = "surAChaqueModification",
      .nameEN     = "onEachChange",
      .descFR     = "Declenche a chaque changement d'etat d'une option (coche/decoche).",
      .descEN     = "Fires whenever an option state changes (check/uncheck).",
      .paramCount = 0, .params = NULL },
};

/* Champ Combo — events specifiques (Selection, modification saisie,
 * Ouverture/Fermeture dropdown). surEntree/surSortie via FAM_FOCUS. */
static const OwlEventDef COMBO_SPECIFIC_EVENTS[] = {
    { .canonical = "surS\xc3\xa9lection", .nameEN = "onSelection",
      .descFR = "Declenche quand l'element selectionne change.",
      .descEN = "Fires when the selected item changes.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surAChaqueModification", .nameEN = "onEachChange",
      .descFR = "Declenche a chaque modification de la saisie (combo en saisie).",
      .descEN = "Fires on each input change (editable combo).",
      .paramCount = 0, .params = NULL },
    { .canonical = "surOuverture", .nameEN = "onOpen",
      .descFR = "Declenche a l'ouverture de la zone deroulee. Utile pour remplir la combo a la volee.",
      .descEN = "Fires when the dropdown opens. Useful to populate on demand.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surFermeture", .nameEN = "onClose",
      .descFR = "Declenche a la fermeture de la zone deroulee.",
      .descEN = "Fires when the dropdown closes.",
      .paramCount = 0, .params = NULL },
};

/* Champ ZoneRepetee — events specifiques.
 * surAffichageLigne(indice) : declenche pour chaque repetition au moment de
 *   son rendu. C'est la l'endroit canon pour customiser l'apparence d'une
 *   ligne (couleur conditionnelle, visibilite d'un sous-champ, etc.).
 * surS\xc3\xa9lectionLigne(indice) : declenche quand l'utilisateur change la
 *   ligne selectionnee (clic ou setter LigneSelectionnee). */
static const OwlEventDef ZR_SPECIFIC_EVENTS[] = {
    { .canonical = "surAffichageLigne", .nameEN = "onRowDisplay",
      .descFR = "Declenche au rendu de chaque repetition. Utilise pour "
                "personnaliser l'apparence (couleur, visibilite) d'une ligne.",
      .descEN = "Fires when each repetition is rendered. Used to customize "
                "row appearance (color, visibility) per repetition.",
      .paramCount = 1, .params = P_zr_indice },
    { .canonical = "surS\xc3\xa9lectionLigne", .nameEN = "onRowSelection",
      .descFR = "Declenche quand la repetition selectionnee change.",
      .descEN = "Fires when the selected repetition changes.",
      .paramCount = 1, .params = P_zr_indice },
};

/* Champ Onglet — event specifique : surChangementVolet, tire
 * quand le volet affiche change (clic sur un onglet ou setter ..Valeur). Pas
 * d'event par-volet en statique. Le declenchement runtime vient avec le rendu
 * Qt (lot 2) ; ici on enregistre la signature pour que le compilateur accepte
 * la PROCEDURE dans le body du Champ Onglet. */
static const OwlEventDef ONGLET_SPECIFIC_EVENTS[] = {
    { .canonical = "surChangementVolet", .nameEN = "onPaneChange",
      .descFR = "Declenche quand le volet affiche change (clic sur un onglet "
                "ou modification de la propriete Valeur).",
      .descEN = "Fires when the displayed pane changes (tab click or change "
                "of the Valeur property).",
      .paramCount = 0, .params = NULL },
};

/* ============================================================
 * Init — appele depuis stdlib_ui_catalog_init()
 *
 * Pour chaque widget : on empile (via REG_FAM, qui APPEND grace au
 * stockage en chunks dans owl_event_catalog.c) les familles d'events
 * communes applicables, puis les events specifiques au widget.
 * Convention : une famille = un theme coherent ; pas de doublon entre
 * familles ni entre famille et specifique (le self-test regle 8 hurle
 * sinon).
 * ============================================================ */
/* ============================================================
 * Lifecycle Fenêtre — events de cycle de vie au niveau fenêtre.
 *
 * Note importante : `Initialisation` n'est PAS un event ici. Le top-level
 * du body de la déclaration `FEN_X est une Fenêtre ... FIN` joue ce rôle
 * implicitement — toute instruction écrite directement dans le body (hors
 * d'un bloc PROCEDURE nommé) est exécutée à l'initialisation. C'est une
 * convention OWL préservée explicitement.
 *
 * Le compileur (cf. compiler.c::is_predefined_window_block) tolère
 * `Initialisation` comme alias legacy de `FinInitialisation` mais ne le
 * documente pas comme event canon.
 * ============================================================ */
static const OwlEventDef FENETRE_LIFECYCLE_EVENTS[] = {
    { .canonical = "FinInitialisation", .nameEN = "EndInitialization",
      .descFR = "Tire une fois après création de tous les sous-champs et "
                "exposition de la fenêtre. Place idéale pour configurer "
                "dynamiquement les widgets, charger des données ou appeler "
                "des natives qui exigent que la fenêtre soit prête. Si "
                "absent, l'initialisation se termine sans action.",
      .descEN = "Fires once after all child controls are created and the "
                "window is exposed. Ideal spot to configure widgets "
                "dynamically, load data, or call natives that require the "
                "window to be ready. When absent, initialization completes "
                "with no action.",
      .paramCount = 0, .params = NULL },

    { .canonical = "Fermeture", .nameEN = "Closing",
      .descFR = "Tire juste avant que la fenêtre ne se ferme — dernière "
                "occasion d'observer son état (sauvegarde, cleanup, log). "
                "Ne peut pas annuler la fermeture ; utiliser "
                "AutorisationFermeture pour ce besoin.",
      .descEN = "Fires just before the window closes — the last chance to "
                "observe its state (save, cleanup, log). Cannot cancel the "
                "close; use AutorisationFermeture for that purpose.",
      .paramCount = 0, .params = NULL },

    { .canonical = "AutorisationFermeture", .nameEN = "CloseAuthorization",
      .descFR = "Demande au handler s'il autorise la fermeture. Retourner "
                "Faux annule la fermeture (cas typique : confirmer une "
                "perte de données non sauvegardées). Retourner Vrai (ou "
                "ne rien retourner) laisse la fermeture se poursuivre, "
                "ce qui déclenchera ensuite Fermeture.",
      .descEN = "Asks the handler whether the window may close. Returning "
                "False cancels the close (typical case: confirm unsaved "
                "changes). Returning True (or nothing) lets the close "
                "proceed, which then triggers Closing.",
      .paramCount = 0, .params = NULL },

    { .canonical = "DemandeMiseAJour", .nameEN = "RequestRefresh",
      .descFR = "Tire quand le runtime ou un appel explicite signale qu'une "
                "donnée affichée doit être rafraîchie (changement externe, "
                "rechargement). Implémenter pour relire les sources et "
                "réaffecter les champs visibles.",
      .descEN = "Fires when the runtime or an explicit call signals that "
                "displayed data must be refreshed (external change, reload). "
                "Implement to re-read sources and reassign visible fields.",
      .paramCount = 0, .params = NULL },

    /*  — clavier au niveau fenetre. Cible principale : les fleches
     * directionnelles (Snake) qui n'ont pas de caractere textuel. Un event
     * clavier non consomme par un champ focuse (champ sans handler) remonte
     * ici (modele bubble-up, cf. propagation runtime). */
    { .canonical = "surToucheAppuy\xc3\xa9""e", .nameEN = "onKeyDown",
      .descFR = "Declenche a l'appui d'une touche du clavier au niveau de la "
                "fenetre. Recoit aussi les touches d'un champ focuse qui n'a "
                "pas son propre handler. Le code de la touche est passe en "
                "parametre (constante tk* ou code ASCII).",
      .descEN = "Fires when a keyboard key is pressed at window level. Also "
                "receives keys from a focused field that has no handler of its "
                "own. The key code is passed as a parameter (tk* constant or "
                "ASCII code).",
      .paramCount = 1, .params = P_touche },

    { .canonical = "surToucheRel\xc3\xa2""ch\xc3\xa9""e", .nameEN = "onKeyUp",
      .descFR = "Declenche au relachement d'une touche du clavier au niveau de "
                "la fenetre. Le code de la touche est passe en parametre "
                "(constante tk* ou code ASCII).",
      .descEN = "Fires when a keyboard key is released at window level. The key "
                "code is passed as a parameter (tk* constant or ASCII code).",
      .paramCount = 1, .params = P_touche },

    /*  — redimensionnement de la fenetre. Tire quand la taille
     * change (ecriture de ..Largeur/..Hauteur, FenTaille, Maximise...).
     * Payload : nouvelle largeur + hauteur en pixels. */
    { .canonical = "surChangementTaille", .nameEN = "onResize",
      .descFR = "Declenche quand la fenetre est redimensionnee. Recoit la "
                "nouvelle largeur et la nouvelle hauteur en pixels.",
      .descEN = "Fires when the window is resized. Receives the new width and "
                "height in pixels.",
      .paramCount = 2, .params = P_resize },
};

/* ============================================================
 * Lifecycle Etat — events de cycle de vie d'un rapport d'impression.
 *
 * Note importante : `Ouverture` n'est PAS un event ici. Comme pour
 * `Initialisation` côté Fenêtre, le top-level du body de la
 * déclaration `ETA_X est un Etat ... FIN` joue ce rôle implicitement.
 *
 * Ordre d'exécution canon (rapport canon 3046032) :
 *   1. Ouverture (implicite, top-level body)
 *   2. EnTeteDocument
 *   3. Pour chaque page :
 *      a. HautDePage
 *      b. Pour chaque enregistrement filtré :
 *         - Filtre  → retourne Faux pour ignorer cet enreg
 *         - Lecture → calcul de valeurs dérivées pour Corps
 *         - Corps
 *      c. BasDePage
 *   4. FinDeDocument
 *   5. Fermeture
 * ============================================================ */
static const OwlEventDef ETAT_LIFECYCLE_EVENTS[] = {
    { .canonical = "Filtre", .nameEN = "Filter",
      .descFR = "Appelé avant chaque rendu Corps pour décider si "
                "l'enregistrement courant doit être inclus dans le rapport. "
                "Retourner Vrai (ou ne rien retourner) laisse l'enregistrement "
                "passer ; retourner Faux le saute. Permet d'implémenter un "
                "tri/filtrage métier sans modifier la source de données.",
      .descEN = "Called before each Body render to decide whether the "
                "current record is included in the report. Return True (or "
                "nothing) lets the record through; return False skips it. "
                "Useful to implement business filtering without altering "
                "the data source.",
      .paramCount = 0, .params = NULL },

    { .canonical = "Lecture", .nameEN = "Read",
      .descFR = "Appelé après Filtre (si l'enregistrement passe) et avant "
                "Corps. Place canon pour calculer les valeurs dérivées qui "
                "seront affichées par les libellés WYSIWYG du Corps "
                "(totaux courants, formatages spéciaux, lookups externes).",
      .descEN = "Called after Filter (if the record passes) and before "
                "Body. Canonical spot to compute derived values that "
                "Body WYSIWYG labels will display (running totals, "
                "special formatting, external lookups).",
      .paramCount = 0, .params = NULL },

    { .canonical = "Fermeture", .nameEN = "Closing",
      .descFR = "Appelé une fois après FinDeDocument, juste avant la "
                "libération du rapport. Place pour le cleanup (fermeture "
                "de connexions, log final, sauvegarde d'état). "
                "Ne peut pas annuler la fin du rapport.",
      .descEN = "Called once after DocumentFooter, just before the report "
                "is released. Spot for cleanup (close connections, final "
                "log, save state). Cannot cancel report completion.",
      .paramCount = 0, .params = NULL },
};

/* Events de cycle de vie des sous-contrôles d'un bloc État (Libellé,
 * Image déclarés dans Corps/EnTeteDocument/...). Canon.
 * Consommés par le compilateur pour valider les PROCEDURE déclarées à
 * l'intérieur d'un sous-contrôle. */
static const OwlEventDef ETAT_SUBCONTROL_LIFECYCLE_EVENTS[] = {
    { .canonical = "AvantImpression", .nameEN = "BeforePrint",
      .descFR = "Appelé avant chaque impression du sous-contrôle. "
                "Permet d'ajuster dynamiquement la valeur affichée ou "
                "la mise en forme en fonction du contexte courant.",
      .descEN = "Called before each render of the sub-control. Allows "
                "dynamic adjustment of the displayed value or formatting "
                "based on the current context.",
      .paramCount = 0, .params = NULL },
};

/* Events d'une OptionMenu du menu classique de fenetre.
 * surSelection est declenche indifferemment par un clic ou par la validation
 * clavier de l'option (mnemonique / accelerateur). Consomme par le
 * compilateur pour valider les PROCEDURE declarees dans une OptionMenu. */
static const OwlEventDef MENU_OPTION_EVENTS[] = {
    { .canonical = "surS\xc3\xa9lection", .nameEN = "onSelection",
      .descFR = "Declenche quand l'option de menu est selectionnee "
                "(clic, mnemonique ou raccourci clavier).",
      .descEN = "Fires when the menu option is selected "
                "(click, mnemonic or keyboard shortcut).",
      .paramCount = 0, .params = NULL },
};

/* Events d'un bloc CollectionDeProcedures. Terminaison est
 * appele une fois a la fermeture du projet, pour liberer les ressources
 * partagees par la collection. Le code d'init de la collection vit au
 * top-level du bloc (pas d'event nomme). Consomme par le LSP ; le hook
 * runtime vit dans project_run.c. */
static const OwlEventDef COLLECTION_EVENTS[] = {
    { .canonical = "Terminaison", .nameEN = "Termination",
      .descFR = "Appele une fois a la fermeture du projet, pour liberer les "
                "ressources partagees par la collection (fermeture de "
                "connexions, journalisation finale, sauvegarde d'etat). "
                "L'initialisation, elle, vit au top-level du bloc.",
      .descEN = "Called once when the project closes, to release the "
                "resources shared by the collection (close connections, "
                "final logging, save state). Initialization instead lives "
                "at the top level of the block.",
      .paramCount = 0, .params = NULL },
};

/* Events de niveau ServiceREST. Hooks declares directement dans
 * le corps d'un `IDENT est un ServiceREST`. Le code d'init du service vit au
 * top-level du bloc (pas d'event nomme). Consomme par le compilateur (source
 * unique de validation, en remplacement de la liste en dur service_event_classify)
 * et par le LSP (coloration event natif + completion). Construction OWL. */
static const OwlEventDef SERVICE_EVENTS[] = {
    { .canonical = "surAuthentification", .nameEN = "onAuthentication",
      .descFR = "Vérifie l'identité de l'appelant avant l'exécution d'une "
                "opération du service. Reçoit le jeton d'authentification "
                "présenté par le client et retourne Vrai pour accepter la "
                "requête, Faux pour la rejeter.",
      .descEN = "Checks the caller identity before any service operation runs. "
                "Receives the authentication token presented by the client and "
                "returns True to accept the request, False to reject it.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surAutorisation", .nameEN = "onAuthorization",
      .descFR = "Décide si l'appelant authentifié a le droit d'accéder à "
                "l'opération demandée. Retourne Vrai pour autoriser l'accès, "
                "Faux pour le refuser.",
      .descEN = "Decides whether the authenticated caller may access the "
                "requested operation. Returns True to allow access, False to "
                "deny it.",
      .paramCount = 0, .params = NULL },
    { .canonical = "surErreur", .nameEN = "onError",
      .descFR = "Déclenché quand une opération du service lève une erreur non "
                "gérée. Permet de journaliser l'incident et de construire une "
                "réponse d'erreur personnalisée renvoyée au client.",
      .descEN = "Fires when a service operation raises an unhandled error. Lets "
                "you log the incident and build a custom error response returned "
                "to the client.",
      .paramCount = 0, .params = NULL },
    { .canonical = "FinInitialisation", .nameEN = "EndInitialization",
      .descFR = "Tiré une fois après la mise en place du service, avant le "
                "traitement de la première requête. Place idéale pour préparer "
                "les ressources partagées (connexions, caches, configuration).",
      .descEN = "Fires once after the service is set up, before the first "
                "request is handled. Ideal spot to prepare shared resources "
                "(connections, caches, configuration).",
      .paramCount = 0, .params = NULL },
    { .canonical = "Fermeture", .nameEN = "Closing",
      .descFR = "Tiré une fois à l'arrêt du service, après la dernière requête "
                "traitée. Place pour libérer les ressources partagées (fermeture "
                "de connexions, journalisation finale, sauvegarde d'état).",
      .descEN = "Fires once when the service stops, after the last request is "
                "handled. Spot to release shared resources (close connections, "
                "final logging, save state).",
      .paramCount = 0, .params = NULL },
};

/* Events de niveau OpérationHTTP. surRequête traite une requête
 * reçue par l'opération et construit la réponse. Ses paramètres réels (segments
 * de chemin, paramètres de requête, corps) sont déclarés au cas par cas dans la
 * signature de la PROCEDURE : le catalogue n'en fige aucun (paramCount 0), il
 * documente le hook et son retour chaîne. */
static const OwlEventDef OPERATION_EVENTS[] = {
    { .canonical = "surRequ\xc3\xaa""te", .nameEN = "onRequest",
      .descFR = "Traite une requête reçue par l'opération et construit la "
                "réponse renvoyée au client. Reçoit les paramètres déclarés dans "
                "sa signature (segments de chemin, paramètres de requête, corps) "
                "et retourne le contenu de la réponse sous forme de chaîne.",
      .descEN = "Handles a request received by the operation and builds the "
                "response returned to the client. Receives the parameters "
                "declared in its signature (path segments, query parameters, "
                "body) and returns the response content as a string.",
      .paramCount = 0, .params = NULL },
};

/* ============================================================
 * Vue dedupliquee des events widget
 *
 * Un meme event peut etre partage par plusieurs widgets (surClic sur Bouton,
 * Libelle, Table... ; surAChaqueModification sur Saisie, Combo, Selecteur...).
 * On collecte ici la liste des events UNIQUES (par nom canonique), avec un
 * index stable = ordre de premiere apparition en parcourant les kinds. Cet
 * index est la cle de mapping avec les constantes evt* (OWL_EVT_BASE + index).
 *
 * On ne stocke que des POINTEURS vers les OwlEventDef statiques deja
 * enregistres dans owl_event_catalog : pas de copie, pas d'ownership.
 * ============================================================ */
#define OWL_WIDGET_EVENT_DEDUP_MAX 128

static const OwlEventDef* g_dedup[OWL_WIDGET_EVENT_DEDUP_MAX];
static int                g_dedup_count = 0;

static bool dedup_already_present(const char* canonical) {
    for (int i = 0; i < g_dedup_count; i++) {
        if (g_dedup[i]->canonical && canonical &&
            strcmp(g_dedup[i]->canonical, canonical) == 0) {
            return true;
        }
    }
    return false;
}

/* Reconstruit la vue dedupliquee en parcourant tous les widget kinds du
 * catalogue owl_event_catalog (indexe par OwlControlKind). Appelee en fin
 * d'init, apres l'enregistrement de toutes les familles. Idempotente : le
 * re-vm_init (--vscode-data) repart de zero. */
static void build_widget_event_dedup(void) {
    g_dedup_count = 0;
    for (int k = 0; k <= (int)OWL_CONTROL_WINDOW; k++) {
        int n = owl_event_catalog_count((OwlControlKind)k);
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_event_catalog_at((OwlControlKind)k, i);
            if (!d || !d->canonical) continue;
            if (dedup_already_present(d->canonical)) continue;
            if (g_dedup_count < OWL_WIDGET_EVENT_DEDUP_MAX) {
                g_dedup[g_dedup_count++] = d;
            }
        }
    }
}

int owl_widget_event_dedup_count(void) { return g_dedup_count; }

const OwlEventDef* owl_widget_event_dedup_at(int index) {
    if (index < 0 || index >= g_dedup_count) return NULL;
    return g_dedup[index];
}

void event_catalog_widgets_init(void) {
    /* --- Fenêtre : lifecycle events (catalogue parallele, hors
     *     OwlControlKind — pas de payload type, consommation directe
     *     compilo/LSP/dump, cf.  source unique). --- */
    owl_window_lifecycle_catalog_register(
        FENETRE_LIFECYCLE_EVENTS,
        sizeof(FENETRE_LIFECYCLE_EVENTS) /
        sizeof(FENETRE_LIFECYCLE_EVENTS[0]));

    /* --- Etat : lifecycle events (catalogue parallèle, pas dans
     *     OwlControlKind — cf. audit OWL_CONTROL_ETAT). --- */
    owl_etat_event_catalog_register(
        ETAT_LIFECYCLE_EVENTS,
        sizeof(ETAT_LIFECYCLE_EVENTS) /
        sizeof(ETAT_LIFECYCLE_EVENTS[0]));

    /* --- Sous-contrôles d'État (Libellé/Image dans un bloc) :
     *     AvantImpression (le code d'init vit dans le top-level du champ).
     *     Consommé par le compilateur (etat_control_parse_procedure) pour
     *     valider les PROCEDURE déclarées à l'intérieur du sous-contrôle. --- */
    owl_etat_subcontrol_event_catalog_register(
        ETAT_SUBCONTROL_LIFECYCLE_EVENTS,
        sizeof(ETAT_SUBCONTROL_LIFECYCLE_EVENTS) /
        sizeof(ETAT_SUBCONTROL_LIFECYCLE_EVENTS[0]));


    /* --- OptionMenu (menu classique) : surSelection. --- */
    owl_menu_option_event_catalog_register(
        MENU_OPTION_EVENTS,
        sizeof(MENU_OPTION_EVENTS) / sizeof(MENU_OPTION_EVENTS[0]));

    /* --- CollectionDeProcedures : Terminaison. --- */
    owl_collection_event_catalog_register(
        COLLECTION_EVENTS,
        sizeof(COLLECTION_EVENTS) / sizeof(COLLECTION_EVENTS[0]));

    /* --- ServiceREST : hooks authn/authz/erreur/lifecycle. --- */
    owl_service_event_catalog_register(
        SERVICE_EVENTS, sizeof(SERVICE_EVENTS) / sizeof(SERVICE_EVENTS[0]));

    /* --- OpérationHTTP : surRequête. --- */
    owl_operation_event_catalog_register(
        OPERATION_EVENTS, sizeof(OPERATION_EVENTS) / sizeof(OPERATION_EVENTS[0]));

    /* --- Champ Bouton : events souris de base + clavier. --- */
    REG_FAM(OWL_CONTROL_BUTTON, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_BUTTON, FAM_KEYBOARD_EVENTS);

    /* --- Champ Libelle : souris seule (texte cliquable). Pas de clavier :
     *     le champ n'est pas focusable, aucune frappe ne peut lui parvenir. --- */
    REG_FAM(OWL_CONTROL_LABEL, FAM_MOUSE_BASIC_EVENTS);

    /* --- Champ Saisie : focus + ses events texte specifiques + clavier. --- */
    REG_FAM(OWL_CONTROL_INPUT, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_INPUT, INPUT_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_INPUT, FAM_KEYBOARD_EVENTS);

    /* --- Champ Table : focus + souris de base + ses events ligne + clavier
     *     ( : piloter les lignes au clavier — Suppr sur la ligne
     *     courante, navigation aux fleches). --- */
    REG_FAM(OWL_CONTROL_TABLE, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_TABLE, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_TABLE, TABLE_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_TABLE, FAM_KEYBOARD_EVENTS);

    /* --- Champ Image : tout le set souris + layout + menu.
     *     Pas de FAM_SCROLL_EVENTS : le backend est un widget
     *     plein, sans QScrollArea ni QScrollBar. Aucun ascenseur a piloter,
     *     donc aucun surAscenseur* declenchable. Si l'Image gagne un jour une
     *     zone defilante, la famille se re-enregistre en une ligne. --- */
    REG_FAM(OWL_CONTROL_IMAGE, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_IMAGE, FAM_MOUSE_ADVANCED_EVENTS);
    REG_FAM(OWL_CONTROL_IMAGE, FAM_MENU_EVENTS);
    REG_FAM(OWL_CONTROL_IMAGE, FAM_LAYOUT_EVENTS);
    /*  — clavier sur Image : extension OWL (canon n'a pas d'event clavier
     * sur Image) pour le canvas de jeu (capter les fleches directement sur la
     * zone de dessin). L'Image devient focusable au runtime (install_key_filter).
     * Pour capter des touches des l'ouverture : DonneFocus(IMG_x) en
     * FinInitialisation. */
    REG_FAM(OWL_CONTROL_IMAGE, FAM_KEYBOARD_EVENTS);

    /* --- Champ Liste : focus + selection + clavier. --- */
    REG_FAM(OWL_CONTROL_LISTE, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_LISTE, LIST_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_LISTE, FAM_KEYBOARD_EVENTS);

    /* --- Champ Combo : focus + ses events specifiques (selection,
     *     ouverture/fermeture dropdown, modification saisie) + clavier. --- */
    REG_FAM(OWL_CONTROL_COMBO, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_COMBO, COMBO_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_COMBO, FAM_KEYBOARD_EVENTS);

    /* --- Champ Selecteur : focus + souris de base + AChaqueModification
     *     + clavier. --- */
    REG_FAM(OWL_CONTROL_SELECTEUR, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_SELECTEUR, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_SELECTEUR, SELECTEUR_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_SELECTEUR, FAM_KEYBOARD_EVENTS);

    /* --- Champ Interrupteur : meme couverture que Selecteur. --- */
    REG_FAM(OWL_CONTROL_INTERRUPTEUR, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_INTERRUPTEUR, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_INTERRUPTEUR, INTERRUPTEUR_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_INTERRUPTEUR, FAM_KEYBOARD_EVENTS);

    /* --- Champ ZoneRepetee : focus + souris de base + events ligne
     *     + clavier. --- */
    REG_FAM(OWL_CONTROL_ZONE_REPETEE, FAM_FOCUS_EVENTS);
    REG_FAM(OWL_CONTROL_ZONE_REPETEE, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_ZONE_REPETEE, ZR_SPECIFIC_EVENTS);
    REG_FAM(OWL_CONTROL_ZONE_REPETEE, FAM_KEYBOARD_EVENTS);

    /* --- Champ CodeBarres : calque strict de l'Image (meme backend
     *     OwlImageLabel) -> meme set d'events souris + layout + menu +
     *     clavier, et pas plus d'ascenseur que l'Image. --- */
    REG_FAM(OWL_CONTROL_CODEBARRE, FAM_MOUSE_BASIC_EVENTS);
    REG_FAM(OWL_CONTROL_CODEBARRE, FAM_MOUSE_ADVANCED_EVENTS);
    REG_FAM(OWL_CONTROL_CODEBARRE, FAM_MENU_EVENTS);
    REG_FAM(OWL_CONTROL_CODEBARRE, FAM_LAYOUT_EVENTS);
    REG_FAM(OWL_CONTROL_CODEBARRE, FAM_KEYBOARD_EVENTS);

    /* OWL_CONTROL_CONTAINER : pas d'event en V1. */
    /* OWL_CONTROL_JAUGE : champ output-only, aucun event utilisateur. */

    /* --- Champ Onglet : event de changement de volet. Pas de clavier :
     *     la navigation entre volets est deja couverte par surChangementVolet,
     *     et le clavier brut demanderait d'abord de trancher son interaction
     *     avec la navigation native entre onglets. --- */
    REG_FAM(OWL_CONTROL_ONGLET, ONGLET_SPECIFIC_EVENTS);
    /* OWL_CONTROL_VOLET : sous-bloc structurel, pas d'event propre. */

    /*  — construit la vue dedupliquee consommee par la famille evt*
     *  (DeclencheEvenement / TriggerEvent). Doit rester en DERNIER : toutes les
     *  familles ci-dessus doivent etre enregistrees avant. */
    build_widget_event_dedup();
}
