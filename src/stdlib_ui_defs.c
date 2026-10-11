// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ui_defs.c - Donnees de registre des natives UI (108).
 *
 * Partie Qt-FREE du module UI (cf. stdlib_ui_defs.h). Contient l'array DEFS
 * (108 natives) + ses tables OwlParam/OwlSignature/OwlMethodBinding + les enums
 * et constantes UI, SANS aucune implementation C : chaque entree DEFS a
 * .impl=NULL a la declaration.
 *
 * - Cote GUI (owl.exe, owl_runtime_gui) : stdlib_ui_impls_wire() (stdlib_ui.c)
 *   remplit les .impl AVANT stdlib_ui_defs_register() -> register_module voit
 *   des impls non-NULL -> chemin normal -> registre byte-identique a l'historique.
 * - Cote console (owl-cli, owl-build) : les .impl restent NULL. Le compilateur
 *   consulte uniquement les signatures/enums/arite (jamais .impl), donc la
 *   resolution des natives UI est identique au GUI. Le portail de compile
 *   d'owl-build et `owl-cli --check` sont des passes compile-only : .impl=NULL
 *   n'est jamais dereference.
 *
 * Les enums (E_touche, E_fsel, E_FenTaille, E_rechercheTable, E_ThemeSnapshot)
 * et leurs constantes migrent ici depuis stdlib_ui_init (ou elles etaient gatees
 * OWL_HAS_GUI) et sont enregistrees inconditionnellement par
 * stdlib_ui_defs_register() : le portail console valide l'appartenance d'enum
 * des parametres a l'identique du GUI. Les enums partages avec le catalogue core
 * (E_ActionSouris, E_CadrageH, ...) restent definis dans ui_catalog.c et ne sont
 * ici que references (extern via owl_stdlib.h).
 *
 * Le corps (donnees de registre) est extrait tel quel de stdlib_ui.c : seules
 * les implementations Qt (fn_*) sont restees la-bas. Des commentaires decrivant
 * ces fn ont pu suivre les donnees lors du split. */

#include "stdlib_ui_defs.h"
#include "owl_stdlib.h"       /* extern E_ActionSouris/E_CadrageH/... (ui_catalog) */
#include "owl_registry.h"
#include "owl_ui.h"           /* OWL_TK_MOUSE_LEFT/RIGHT (codes sentinelles souris) */
#include "sentinels.h"        /* OWL_FSEL_OUVRE/CREE/EXISTE/MULTI */

#include <string.h>

static const OwlParam p_msg[]    = {{"message", "cha\xc3\xae" "ne", "Valeur a afficher (une ligne par argument)"}, {0}};
static const OwlParam p_choice[] = {{"defautOuMessage", "chaîne|entier", "Bouton par defaut OU premier message"}, {0}};
static const OwlParam p_fen[]    = {{"fenetre", "tout", "Nom de la fenetre (FEN_X) ou chaine"}, {0}};
static const OwlParam p_close[]  = {{"argsOpt", "tout", "Fenetre optionnelle puis valeurs renvoyees"}, {0}};
static const OwlParam p_fin[]    = {{"argsOpt", "tout", "Messages puis booleen codeFermeture optionnel"}, {0}};

/* Enums definis dans ui_catalog.c, consommes via allowedBase. */
extern const OwlEnum E_toastDuree;
extern const OwlEnum E_CadrageH;
extern const OwlEnum E_CadrageV;
/*  — enum genere (valeurs remplies a l'init) : non-const. */
extern OwlEnum E_EvtWidget;

static const OwlParam p_toast[] = {
    {"message",     "cha\xc3\xae" "ne", "Message a afficher (multiligne supporte)."},
    {"duree",       "entier", "Duree d'affichage (toastCourt | toastLong).",
     .allowedBase=&E_toastDuree},
    {"cadrageV",    "entier", "Alignement vertical (cvHaut | cvCentre | cvMilieu | cvBas).",
     .allowedBase=&E_CadrageV},
    {"cadrageH",    "entier", "Alignement horizontal (chGauche | chCentre | chDroite).",
     .allowedBase=&E_CadrageH},
    {"couleurFond", "entier", "Couleur de fond (RVB ou constante predefinie). -1 = par defaut."},
    {0}
};

static const OwlSignature sig_msg[]    = {{-1, p_msg,    "",       ""}, {0}};
static const OwlSignature sig_choice[] = {{-1, p_choice, "entier", "Oui (1), Non (0), Annuler (2)"}, {0}};

/* fsel* — mode d'ouverture de fSelecteur. Combinable bitwise (canon :
 * defaut = fselOuvre + fselExiste). Sous-ensemble assume : pas de fselChange
 * (message pump Windows) ni fCheminUNICODE (OWL UTF-8 partout). */
static const char* const E_fsel_values[] = {
    "fselOuvre", "fselCr\xc3\xa9" "e", "fselExiste", "fselMulti", NULL
};
static const OwlEnum E_fsel = {
    .name="fsel", .nameAscii="fsel", .nameEN="fsel", .category="Fichiers",
    .descFR="Mode d'ouverture du selecteur de fichiers (combinable).",
    .descEN="File selector open mode (combinable).",
    .values=E_fsel_values, .combinable=true, .force_small_int=true
};

#define C_FSEL(nm, val, dfr, den, en) {nm, "Fichiers", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef FSEL_CONSTS[] = {
    C_FSEL("fselOuvre",  OWL_FSEL_OUVRE,
           "Ouvre le selecteur pour choisir un fichier existant (defaut).",
           "Opens the selector to pick an existing file (default).", "fselOpen"),
    C_FSEL("fselCr\xc3\xa9" "e", OWL_FSEL_CREE,
           "Ouvre le selecteur en enregistrement (choix d'un fichier a creer).",
           "Opens the selector in save mode (choose a file to create).", "fselCreate"),
    C_FSEL("fselExiste", OWL_FSEL_EXISTE,
           "Impose que le fichier choisi existe deja (combinable avec fselOuvre).",
           "Requires the chosen file to already exist (combinable with fselOpen).", "fselExists"),
    C_FSEL("fselMulti",  OWL_FSEL_MULTI,
           "Autorise la selection de plusieurs fichiers (chemins separes par CRLF).",
           "Allows selecting several files (paths separated by CRLF).", "fselMulti"),
};

static const OwlParam p_fsel[] = {
    {"repInitial",     "cha\xc3\xae" "ne", "Repertoire d'ouverture, ou chemin complet du fichier par defaut (syntaxe 2)."},
    {"fichierDefaut",  "cha\xc3\xae" "ne", "Nom du fichier preselectionne, ou titre (syntaxe 2)."},
    {"titre",          "cha\xc3\xae" "ne", "Titre de la boite de dialogue."},
    {"typesFichiers",  "cha\xc3\xae" "ne", "Filtre des types de fichiers (description + TAB + motifs)."},
    {"extDefaut",      "cha\xc3\xae" "ne", "Extension ajoutee par defaut si l'utilisateur n'en saisit pas."},
    {"mode",           "entier", "Mode d'ouverture (fselOuvre | fselCree | fselExiste | fselMulti).",
     .allowedBase=&E_fsel},
    {0}
};
static const OwlSignature sig_fsel[] = {
    {4, p_fsel, "cha\xc3\xae" "ne", "Chemin selectionne (syntaxe 2 : chemin complet defaut, titre, filtre, ext)"},
    {5, p_fsel, "cha\xc3\xae" "ne", "Chemin selectionne, \"\" si annule"},
    {6, p_fsel, "cha\xc3\xae" "ne", "+ Mode ; en fselMulti, chemins joints par CRLF"},
    {0}
};

static const OwlParam p_frepsel[] = {
    {"repDefaut",   "cha\xc3\xae" "ne", "Repertoire ouvert au demarrage du selecteur."},
    {"titre",       "cha\xc3\xae" "ne", "Titre de la boite de dialogue."},
    {"commentaires","cha\xc3\xae" "ne", "Commentaire affiche en haut du selecteur (optionnel)."},
    {"racine",      "cha\xc3\xae" "ne", "Repertoire racine de l'arborescence proposee (optionnel)."},
    {0}
};
static const OwlSignature sig_frepsel[] = {
    {2, p_frepsel, "cha\xc3\xae" "ne", "Chemin du repertoire selectionne, \"\" si annule"},
    {3, p_frepsel, "cha\xc3\xae" "ne", "+ Commentaire"},
    {4, p_frepsel, "cha\xc3\xae" "ne", "+ Repertoire racine"},
    {0}
};

/*  : TestModeHeadless — arite variable 0 (getter) ou 1 (setter). */
static const OwlParam p_test_headless[] = {
    {"actif", "boolean", "Optionnel. Si fourni : active (Vrai) ou desactive (Faux) le mode headless. Si omis : retourne l'etat courant."},
    {0}
};
/*  : formes discretes {0,1}. Getter (0 arg) / setter (1 arg).
 * L'init de declaration typee (`b est un booleen = TestModeHeadless()`)
 * resout desormais le retour d'une native multi-formes. */
static const OwlSignature sig_test_headless[] = {
    {0, p_test_headless, "booleen", "Etat courant du mode headless"},
    {1, p_test_headless, "rien",    "Active ou desactive le mode headless"},
    {0}
};

/*  +  : TestSimuleSouris a 2 surcharges.
 *  - (champ, action) : actions sans coords (ClicGauche/Droit, DoubleClic*).
 *  - (champ, action, x, y) : actions a coordonnees (BoutonGauche/Droit,
 *    SurvolSouris, MoletteHaut/Bas). Pour la molette, le mouvement (+1/-1) est
 *    derive de l'enum, pas un argument utilisateur.
 * action est contraint a l'enum ActionSouris (allowedBase). */
static const OwlParam p_test_simule_souris[] = {
    {"champ",  "tout", "Champ cible (Bouton, Saisie, Image, etc.) dont declencher le callback."},
    {"action", "entier", "Valeur de l'enum ActionSouris.",
     .allowedBase=&E_ActionSouris},
    {0}
};
static const OwlParam p_test_simule_souris_xy[] = {
    {"champ",  "tout", "Champ cible (typiquement Champ Image) dont declencher le callback a coordonnees."},
    {"action", "entier", "Valeur de l'enum ActionSouris a coords (BoutonGauche, BoutonDroit, SurvolSouris, MoletteHaut, MoletteBas).",
     .allowedBase=&E_ActionSouris},
    {"x", "entier", "Coordonnee X (pixels relatifs au champ) transmise au callback."},
    {"y", "entier", "Coordonnee Y (pixels relatifs au champ) transmise au callback."},
    {0}
};
static const OwlSignature sig_test_simule_souris[] = {
    {2, p_test_simule_souris,    "rien", ""},
    {4, p_test_simule_souris_xy, "rien", ""},
    {0}
};

/*  — DeclencheEvenement / TriggerEvent. Arite variadique (-1) :
 * (cible, evt) + parametres optionnels selon la signature de l'event. Le
 * parametre evt est contraint a l'enum evt* (allowedBase -> filtrage LSP).
 * L'arite/type des params est validee au compile-time (compiler.c) contre la
 * signature portee par la constante evt*. */
static const OwlParam p_declenche_evenement[] = {
    {"cible", "tout", "Champ cible (objet SAI_x/BTN_x) ou nom du champ en chaine."},
    {"evt",   "entier", "Evenement a declencher (constante evt*).",
     .allowedBase=&E_EvtWidget},
    {0}
};
static const OwlSignature sig_declenche_evenement[] = {
    {-1, p_declenche_evenement, "rien", ""},
    {0}
};

/*  : TestSimuleSaisie(champ, texte) — ecrit `texte` dans
 * `champ..Texte` puis declenche le callback Modification du champ. */
static const OwlParam p_test_simule_saisie[] = {
    {"champ", "tout",   "Champ Saisie cible."},
    {"texte", "chaine", "Texte a saisir."},
    {0}
};
static const OwlSignature sig_test_simule_saisie[] = {
    {2, p_test_simule_saisie, "rien", ""},
    {0}
};

/*  : TestSimuleMenu(fenetre, cheminOption) — declenche le
 * surSelection() de l'OptionMenu nommee dans le menu principal de la fenetre. */
static const OwlParam p_test_simule_menu[] = {
    {"fenetre",      "tout",   "Fenetre dont le menu principal porte l'option a declencher."},
    {"cheminOption", "chaine", "Nom de l'OptionMenu a declencher (ex. \"OPT_Nouveau\"), resolue recursivement dans l'arbre du menu."},
    {0}
};
/*  — forme a 3 arguments : le 3e fixe la colonne contexte d'un menu de
 * loupe (proxy TABLE.COL), qui devient MoiMeme dans le surSelection declenche. */
static const OwlParam p_test_simule_menu_col[] = {
    {"fenetre",      "tout",   "Fenetre dont le menu porte l'option a declencher."},
    {"cheminOption", "chaine", "Nom de l'OptionMenu a declencher (ex. \"OPT_Filtrer\")."},
    {"colonne",      "tout",   "Colonne de Table (forme TABLE.COL_X) qui devient MoiMeme dans le surSelection : contexte d'un menu de loupe."},
    {0}
};
static const OwlSignature sig_test_simule_menu[] = {
    {2, p_test_simule_menu,     "rien", ""},
    {3, p_test_simule_menu_col, "rien", ""},
    {0}
};

/*  : ChampsDe(parent) — noms des champs directs d'une fenetre ou d'un
 * champ conteneur, dans l'ordre de creation. Retour : tableau de chaines. */
static const OwlParam p_champs_de[] = {
    {"parent", "tout", "Fenetre ou champ conteneur (ZoneRepetee, Onglet, ...) dont on liste les champs directs."},
    {0}
};
static const OwlSignature sig_champs_de[] = {
    {1, p_champs_de, "tableau", "tableau des noms des champs directs, dans l'ordre de creation"},
    {0}
};

/*  : DonneFocus([champ]) — arite variable 0 (champ courant) ou 1
 * (champ cible : reference de champ ou nom en chaine). */
static const OwlParam p_donne_focus[] = {
    {"champ", "tout", "Optionnel. Champ a focaliser (reference SAI_X, forme qualifiee FEN_X.SAI_Y, ou nom en chaine). Omis : focus au champ en cours."},
    {0}
};
static const OwlSignature sig_donne_focus[] = {
    {0, p_donne_focus, "rien", "Focus au champ en cours"},
    {1, p_donne_focus, "rien", "Focus au champ cible"},
    {0}
};
/*  — __DebugFeuilleStyle(champ) : feuille QSS courante (test-only). */
static const OwlParam p_debug_feuille_style[] = {
    {"champ", "tout", "Champ dont on lit la feuille de style Qt."},
    {0}
};
static const OwlSignature sig_debug_feuille_style[] = {
    {1, p_debug_feuille_style, "chaine", "Feuille de style QSS du champ"},
    {0}
};
/*  — __DebugFeuilleStyleMenu() : feuille QSS de la barre de menu de la
 * fenetre courante (test-only). */
static const OwlSignature sig_debug_feuille_style_menu[] = {
    {0, NULL, "chaine", "Feuille de style QSS de la barre de menu"},
    {0}
};
/*  — __DebugFeuilleStyleMenuContextuel(nom) : feuille QSS du panneau
 * d'un menu contextuel declare dans la fenetre courante (test-only). */
static const OwlParam p_debug_feuille_style_menu_ctx[] = {
    {"menu", "chaine", "Nom du bloc MenuContextuel de la fenêtre courante."},
    {0}
};
static const OwlSignature sig_debug_feuille_style_menu_ctx[] = {
    {1, p_debug_feuille_style_menu_ctx, "chaine",
     "Feuille de style QSS du panneau du menu contextuel"},
    {0}
};
/*  — __DebugPoliceBarreMenu() / __DebugPoliceSousMenu(sousMenu) :
 * police EFFECTIVE (setFont) d'un widget de menu, descripteur
 * "famille|taille|gras|italique". */
static const OwlSignature sig_debug_police_barre_menu[] = {
    {0, NULL, "chaine", "Descripteur famille|taille|gras|italique de la barre de menu"},
    {0}
};
static const OwlParam p_debug_police_sousmenu[] = {
    {"sousMenu", "chaine", "Libellé du SousMenu dont on lit la police (mnémonique '&' ignoré)."},
    {0}
};
static const OwlSignature sig_debug_police_sousmenu[] = {
    {1, p_debug_police_sousmenu, "chaine", "Descripteur famille|taille|gras|italique du déroulant"},
    {0}
};
/*  — __DebugPoliceChamp(champ) : police EFFECTIVE d'un champ,
 * descripteur "famille|taille|gras|italique". */
static const OwlParam p_debug_police_champ[] = {
    {"champ", "tout", "Champ dont on lit la police effective (descripteur)."},
    {0}
};
static const OwlSignature sig_debug_police_champ[] = {
    {1, p_debug_police_champ, "chaine", "Descripteur famille|taille|gras|italique de la police du champ"},
    {0}
};
/*  — __DebugDernierMessage() : dernier message compose par une boite
 * de message (Info/Erreur/Avertissement/OuiNon/Confirmer). */
static const OwlSignature sig_debug_dernier_message[] = {
    {0, NULL, "chaine", "Dernier message compose par une boite de message"},
    {0}
};
/*  — __DebugEncreBarreMenu() : hauteur d'encre peinte (px) de la barre. */
static const OwlSignature sig_debug_encre_barre_menu[] = {
    {0, NULL, "entier", "Hauteur d'encre (glyphes peints) du texte de la barre de menu, en pixels"},
    {0}
};
/*  — __CaptureFenetre(fen, chemin) : capture offscreen vers PNG. */
static const OwlParam p_capture_fenetre[] = {
    {"fenêtre", "tout",   "Fenêtre ouverte à capturer (FEN_X) ou nom (chaîne)."},
    {"chemin",  "chaine", "Chemin du fichier PNG à écrire."},
    {0}
};
static const OwlSignature sig_capture_fenetre[] = {
    {2, p_capture_fenetre, "booleen", "Vrai si le PNG est écrit (fenêtre non vide)"},
    {0}
};
/*  — __CaptureSousMenu(fen, nomSousMenu, chemin) : capture d'un panneau
 * de sous-menu deroule vers PNG, alpha preserve. */
static const OwlParam p_capture_sousmenu[] = {
    {"fenêtre",     "tout",   "Fenêtre ouverte dont le menu porte le sous-menu (FEN_X) ou nom (chaîne)."},
    {"nomSousMenu", "chaine", "Libellé du SousMenu à capturer (mnémonique '&' ignoré)."},
    {"chemin",      "chaine", "Chemin du fichier PNG à écrire."},
    {0}
};
static const OwlSignature sig_capture_sousmenu[] = {
    {3, p_capture_sousmenu, "booleen", "Vrai si le PNG est écrit (sous-menu trouvé)"},
    {0}
};
/*  — __DebugMenuIcone(fen, sousMenu, libelleOption) : icône résolue ? */
static const OwlParam p_debug_menu_icone[] = {
    {"fenêtre",       "tout",   "Fenêtre dont le menu porte l'option (FEN_X) ou nom (chaîne)."},
    {"sousMenu",      "chaine", "Libellé du SousMenu contenant l'option (mnémonique '&' ignoré)."},
    {"libelléOption", "chaine", "Libellé de l'OptionMenu ciblée (mnémonique '&' ignoré)."},
    {0}
};
static const OwlSignature sig_debug_menu_icone[] = {
    {3, p_debug_menu_icone, "booleen", "Vrai si l'option porte une icône effectivement résolue"},
    {0}
};
/*  — __DebugTableCelluleEditable(champ, ligne, colonne) : editabilite
 * REELLE d'une cellule de Table (item Qt::ItemIsEditable ET vue pas en
 * NoEditTriggers). */
static const OwlParam p_debug_table_cellule_editable[] = {
    {"champ",   "tout",   "Champ Table dont on lit l'éditabilité d'une cellule."},
    {"ligne",   "entier", "Numéro de ligne 1-based."},
    {"colonne", "entier", "Numéro de colonne 1-based."},
    {0}
};
static const OwlSignature sig_debug_table_cellule_editable[] = {
    {3, p_debug_table_cellule_editable, "booleen",
     "Vrai si un double-clic ouvrirait un éditeur sur cette cellule"},
    {0}
};
/*  — __DebugTableCelluleMasque(champ, ligne, colonne) : masque effectif
 * (forme canon) de l'éditeur transitoire d'une cellule de Table. */
static const OwlParam p_debug_table_cellule_masque[] = {
    {"champ",   "tout",   "Champ Table dont on lit le masque de l'éditeur d'une cellule."},
    {"ligne",   "entier", "Numéro de ligne 1-based."},
    {"colonne", "entier", "Numéro de colonne 1-based."},
    {0}
};
static const OwlSignature sig_debug_table_cellule_masque[] = {
    {3, p_debug_table_cellule_masque, "chaîne",
     "Masque de saisie posé sur l'éditeur de cette cellule, vide si aucun"},
    {0}
};
/*  — __DebugTableCelluleValideSaisie(champ, ligne, colonne, texte) :
 * Vrai ssi le validateur de l'éditeur transitoire juge `texte` acceptable. */
static const OwlParam p_debug_table_cellule_valide_saisie[] = {
    {"champ",   "tout",   "Champ Table dont on interroge le validateur de l'éditeur d'une cellule."},
    {"ligne",   "entier", "Numéro de ligne 1-based."},
    {"colonne", "entier", "Numéro de colonne 1-based."},
    {"texte",   "chaîne", "Texte candidat soumis au validateur de la cellule."},
    {0}
};
static const OwlSignature sig_debug_table_cellule_valide_saisie[] = {
    {4, p_debug_table_cellule_valide_saisie, "booleen",
     "Vrai si le validateur de cette cellule accepte le texte (Vrai si aucun validateur)"},
    {0}
};
/*  — __DebugTableCelluleTexteAffiche(champ, ligne, colonne) : texte
 * RENDU d'une cellule de Table (DisplayRole formate par le delegate), hors edition. */
static const OwlParam p_debug_table_cellule_texte_affiche[] = {
    {"champ",   "tout",   "Champ Table dont on lit le texte affiché d'une cellule."},
    {"ligne",   "entier", "Numéro de ligne 1-based."},
    {"colonne", "entier", "Numéro de colonne 1-based."},
    {0}
};
static const OwlSignature sig_debug_table_cellule_texte_affiche[] = {
    {3, p_debug_table_cellule_texte_affiche, "chaîne",
     "Texte tel que rendu par la cellule hors édition (format appliqué), vide si introuvable"},
    {0}
};
/*  — sondes test-only du filtre checklist « valeurs distinctes ». */
static const OwlParam p_debug_table_filtre_in[] = {
    {"colonne",   "tout",   "Colonne de Table (forme TABLE.COL) où poser le filtre IN."},
    {"valeursRC", "chaîne", "Valeurs à retenir, jointes par RC (ensemble coché)."},
    {0}
};
static const OwlSignature sig_debug_table_filtre_in[] = {
    {2, p_debug_table_filtre_in, "rien", ""},
    {0}
};
static const OwlParam p_debug_table_colonne_visible[] = {
    {"colonne", "tout", "Colonne de Table (forme TABLE.COL) dont on lit les lignes visibles."},
    {0}
};
static const OwlSignature sig_debug_table_colonne_visible[] = {
    {1, p_debug_table_colonne_visible, "chaîne",
     "Valeurs de la colonne pour les lignes non masquées, jointes par RC"},
    {0}
};
static const OwlParam p_debug_table_valeurs_distinctes[] = {
    {"colonne", "tout", "Colonne de Table (forme TABLE.COL) dont on énumère les valeurs distinctes."},
    {0}
};
static const OwlSignature sig_debug_table_valeurs_distinctes[] = {
    {1, p_debug_table_valeurs_distinctes, "chaîne",
     "Valeurs distinctes (ordre de 1ère apparition, RC) ou vide au-delà du seuil"},
    {0}
};
static const OwlParam p_debug_table_filtre_type[] = {
    {"colonne", "tout", "Colonne de Table (forme TABLE.COL) dont on lit le type de filtre actif."},
    {0}
};
static const OwlSignature sig_debug_table_filtre_type[] = {
    {1, p_debug_table_filtre_type, "entier",
     "Type (constante filtre*) du filtre actif sur la colonne, 0 si aucun"},
    {0}
};
/*  — __DebugOmbreFlou(champ) : rayon de flou de l'ombre courante. */
static const OwlParam p_debug_ombre_flou[] = {
    {"champ", "tout", "Champ dont on lit le rayon de flou de l'ombre portée."},
    {0}
};
static const OwlSignature sig_debug_ombre_flou[] = {
    {1, p_debug_ombre_flou, "entier", "Rayon de flou de l'ombre courante (-1 si aucune)"},
    {0}
};
/*  — __SimuleInteraction(champ, code) : pousse un evenement souris. */
static const OwlParam p_simule_interaction[] = {
    {"champ", "tout",   "Champ cible dont exercer l'event filter d'ombre."},
    {"code",  "entier", "0=survol, 1=quitte, 2=appui, 3=relache."},
    {0}
};
static const OwlSignature sig_simule_interaction[] = {
    {2, p_simule_interaction, "rien", ""},
    {0}
};
/*  — __DebugPositionImageBouton(champ) : entier E_PositionImageBouton
 * effectivement applique au bouton par le QProxyStyle (test-only). */
static const OwlParam p_debug_position_image_bouton[] = {
    {"champ", "tout", "Champ Bouton dont on lit la position d'image appliquee."},
    {0}
};
static const OwlSignature sig_debug_position_image_bouton[] = {
    {1, p_debug_position_image_bouton, "entier", "Position d'image appliquee (posBtnImage*)"},
    {0}
};
/*  — __DebugBoutonIconeSurvol(champ) : booleen indiquant qu'une icone
 * de survol est armee (event filter + icone stockee) sur le bouton (test-only). */
static const OwlParam p_debug_bouton_icone_survol[] = {
    {"champ", "tout", "Champ Bouton dont on verifie l'armement de l'icone de survol."},
    {0}
};
static const OwlSignature sig_debug_bouton_icone_survol[] = {
    {1, p_debug_bouton_icone_survol, "booleen", "Vrai si une icone de survol est armee"},
    {0}
};
/*  — clavier : ToucheEnfoncee(code) / EnvoieTouche(code). */
/*  — enum source du param `touche` (defini plus bas avec les
 * constantes clavier). acceptsLiterals=true : le LSP suggere les touches
 * mais n'impose rien (Asc("X") et variables restent eligibles). */
static const OwlEnum E_touche;
static const OwlParam p_touche_enfoncee[] = {
    {"touche", "entier",
     "Code de la touche a tester (constante tk*/te* ou code ASCII via Asc(\"X\")).",
     .allowedBase=&E_touche, .acceptsLiterals=true},
    {0}
};
static const OwlSignature sig_touche_enfoncee[] = {
    {1, p_touche_enfoncee, "booleen",
     "Vrai si la touche est physiquement enfoncee a l'instant de l'appel."},
    {0}
};
static const OwlParam p_envoie_touche[] = {
    {"touche", "entier",
     "Code de la touche a injecter (constante tk*/te* ou code ASCII via Asc(\"X\")).",
     .allowedBase=&E_touche, .acceptsLiterals=true},
    {0}
};
static const OwlSignature sig_envoie_touche[] = {
    {1, p_envoie_touche, "rien", ""},
    {0}
};

static const OwlSignature sig_ouvre[]  = {{-1, p_fen,    "tout","ValeurRenvoyee de la fenetre"}, {0}};
static const OwlSignature sig_close[]  = {{-1, p_close,  "",       ""}, {0}};
static const OwlSignature sig_demande_maj_ihm[] = {
    {0, NULL, "rien", "Execute l'evenement DemandeMiseAJour de la fenetre courante"},
    {0}};
static const OwlSignature sig_fin[]    = {{-1, p_fin,    "",       ""}, {0}};
/*  : formes discretes {1,2,3,4,5}. Les 3 params optionnels portant
 * un enum (E_toastDuree/E_CadrageV/E_CadrageH) restent autocompletes : le
 * snippet LSP choisit desormais la forme la plus RICHE (cf. lsp_handlers.c
 * build_native_snippet_skip), pas la plus courte. */
static const OwlParam p_sablier[] = {
    {"activation", "bool\xc3\xa9" "en", "Vrai active le sablier, Faux restitue le curseur pr\xc3\xa9" "c\xc3\xa9" "dent"},
    {0}
};
static const OwlSignature sig_sablier[] = {
    {0, NULL,      "rien", "active le sablier"},
    {1, p_sablier, "rien", "active (Vrai) ou restitue (Faux) le curseur"},
    {0}};

static const OwlSignature sig_toast[]  = {
    {1, p_toast, "", "message seul"},
    {2, p_toast, "", "message + duree"},
    {3, p_toast, "", "message + duree + cadrage vertical"},
    {4, p_toast, "", "message + duree + cadrage vertical + horizontal"},
    {5, p_toast, "", "message + duree + cadrages + couleur de fond"},
    {0}};

/* Champ Table — signatures canon. */
static const OwlParam p_table_add[]  = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"valeurs", "tout", "Une valeur par colonne (variadic)"},
    {0}
};
static const OwlParam p_table_rm[]   = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"ligne",   "entier",  "Numero de ligne 1-based"},
    {0}
};
static const OwlParam p_table_simple[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {0}
};
/* Enums UI catalog exposés (cf src/ui_catalog.c) : référencés ici via
 * allowedBase pour que l'autocomplete LSP propose les bonnes valeurs
 * sur le param `info` / `type` / `option`. */
extern const OwlEnum E_ToInfo;
extern const OwlEnum E_TsInfo;
extern const OwlEnum E_TiXY;
extern const OwlEnum E_TaInfo;
extern const OwlEnum E_tdOperation;
extern const OwlEnum E_tdCibleRelative;
extern const OwlEnum E_FiltreType;

/*  — recherche* : type de recherche pour TableCherche. EN derives de
 * la convention OWL search* (canon muet sur les formes EN, invariant 3). */
static const char* const E_rechercheTable_values[] = {
    "rechercheIdentique", "rechercheCommencePar", "rechercheContient",
    "rechercheD\xc3\xa9" "faut", NULL
};
static const OwlEnum E_rechercheTable = {
    .name="rechercheTable", .nameAscii="rechercheTable", .nameEN="tableSearchMode",
    .category="UI",
    .descFR="Type de recherche pour TableCherche : identique, commence par, "
            "contient ou defaut (identique).",
    .descEN="Search mode for TableSearch: identical, starts with, contains or "
            "default (identical).",
    .values=E_rechercheTable_values, .combinable=false, .force_small_int=true
};

#define C_RCHT(nm, val, dfr, den, en) {nm, "UI", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef RECHERCHE_TABLE_CONSTS[] = {
    C_RCHT("rechercheIdentique", OWL_RCHT_IDENTIQUE,
        "TableCherche : la cellule doit etre egale a la valeur cherchee (defaut).",
        "TableSearch: the cell must equal the searched value (default).",
        "searchIdentical"),
    C_RCHT("rechercheCommencePar", OWL_RCHT_COMMENCE_PAR,
        "TableCherche : la cellule doit commencer par la valeur cherchee.",
        "TableSearch: the cell must start with the searched value.",
        "searchStartsWith"),
    C_RCHT("rechercheContient", OWL_RCHT_CONTIENT,
        "TableCherche : la cellule doit contenir la valeur cherchee.",
        "TableSearch: the cell must contain the searched value.",
        "searchContains"),
    C_RCHT("rechercheD\xc3\xa9" "faut", OWL_RCHT_DEFAUT,
        "TableCherche : recherche par defaut (equivalente a rechercheIdentique).",
        "TableSearch: default search (equivalent to searchIdentical).",
        "searchDefault"),
};

static const OwlParam p_table_cherche[] = {
    {"colonne", "tout", "Colonne de Table cible (forme TABLE.COL_X)"},
    {"valeur",  "tout", "Valeur cherchee"},
    {"type",    "entier", "Optionnel : rechercheIdentique (defaut)/CommencePar/Contient/Defaut",
     .allowedBase=&E_rechercheTable},
    {"ligne",   "entier", "Optionnel : ligne de depart 1-based"},
    {0}
};

static const OwlParam p_table_cherche_partout[] = {
    {"table",  "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"valeur", "tout", "Valeur cherchee dans toutes les colonnes"},
    {"type",   "entier", "Optionnel : rechercheContient (defaut)/Identique/CommencePar/Defaut",
     .allowedBase=&E_rechercheTable},
    {"ligne",  "entier", "Optionnel : ligne de depart 1-based"},
    {0}
};

static const OwlParam p_table_occ[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"info",    "entier",  "Optionnel : toTotal/toAffichee/toColonne/toVisible",
     .allowedBase=&E_ToInfo},
    {0}
};
static const OwlParam p_table_mod[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"ligne",   "entier",  "Numero de ligne 1-based"},
    {"valeurs", "tout", "Une valeur par colonne (variadic)"},
    {0}
};
static const OwlParam p_table_sel[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"ligne",   "entier",  "Numero de ligne 1-based"},
    {0}
};
static const OwlParam p_table_select_full[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"rang",    "entier",  "Optionnel : rang de selection (defaut 1)"},
    {"info",    "entier",  "Optionnel : tsLigne (defaut)/tsColonne/tsLigneCellule/tsColonneCellule",
     .allowedBase=&E_TsInfo},
    {0}
};
static const OwlParam p_table_sort[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"colonnes","chaîne", "Liste de '+/-NomColonne' (variadic). '-' = decroissant."},
    {0}
};
static const OwlParam p_table_filter[] = {
    {"colonne", "tout", "La colonne a filtrer (forme TABLE.COL)"},
    {"type",       "entier",  "Constante filtre*", .allowedBase=&E_FiltreType},
    {"valeur",     "tout", "Valeur du filtre (vide = retire le filtre)"},
    {0}
};
static const OwlParam p_table_disable_filter[] = {
    {"cible", "tout", "Le Champ Table (retire tous les filtres) ou une colonne (TABLE.COL)"},
    {0}
};
static const OwlParam p_table_saisie_loupe[] = {
    {"colonne", "tout", "La colonne a passer en saisie loupe (forme TABLE.COL)"},
    {"type",    "entier", "Optionnel : operateur filtre* (applique un filtre) ou "
                          "recherche* / rechercheDefaut (mode recherche, defaut)"},
    {"valeur",  "tout", "Optionnel : valeur initiale de la zone de saisie"},
    {0}
};
static const OwlParam p_table_filtered_col[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {0}
};
static const OwlParam p_table_pos[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"indice",  "entier",  "Optionnel : ligne a afficher en premier"},
    {"option",  "entier",  "Optionnel : taPremier (defaut) ou taVisible",
     .allowedBase=&E_TaInfo},
    {0}
};
static const OwlParam p_table_xy[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"type",    "entier",  "tiNumLigne / tiNumColonne / tiNomColonne",
     .allowedBase=&E_TiXY},
    {"X",       "entier",  "Coord X pixel (relatif au champ)"},
    {"Y",       "entier",  "Coord Y pixel (relatif au champ)"},
    {0}
};
static const OwlSignature sig_table_add[]    = {{-1, p_table_add,    "", ""}, {0}};
static const OwlSignature sig_table_rm[]     = {{2,  p_table_rm,     "", ""}, {0}};
static const OwlSignature sig_table_simple[] = {{1,  p_table_simple, "", ""}, {0}};
static const OwlSignature sig_table_occ[]    = {
    {1, p_table_occ, "entier", "Nombre total de lignes"},
    {2, p_table_occ, "entier", "Nombre selon info"},
    {0}
};
static const OwlSignature sig_table_cherche[] = {
    {2, p_table_cherche, "entier", "Indice 1-based trouve ou -1"},
    {3, p_table_cherche, "entier", "+ Type de recherche"},
    {4, p_table_cherche, "entier", "+ Ligne de depart"},
    {0}
};
static const OwlSignature sig_table_cherche_partout[] = {
    {2, p_table_cherche_partout, "entier", "Indice 1-based trouve ou -1"},
    {3, p_table_cherche_partout, "entier", "+ Type de recherche"},
    {4, p_table_cherche_partout, "entier", "+ Ligne de depart"},
    {0}
};
static const OwlParam p_table_swap[] = {
    {"table", "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"indiceSource",      "entier", "Numero de ligne a echanger (1-based)"},
    {"indiceDestination", "entier", "Numero de ligne avec laquelle echanger (1-based)"},
    {0}
};
static const OwlParam p_table_deplace[] = {
    {"table",  "Champ Table", "Le Champ Table cible (référence ou nom)"},
    {"source", "entier", "Numero de ligne a deplacer (1-based, ou ligneSelectionnee)"},
    /* acceptsLiterals=true : destination accepte un indice entier libre
     * OU une cible relative tdXxx — le LSP propose les 2. */
    {"destination", "entier", "Indice 1-based destination ou cible relative (tdSuivant, tdPremier, ...)",
     &E_tdCibleRelative, NULL, .acceptsLiterals=true},
    {"operation",   "entier", "Optionnel : tdDeplace (defaut), tdEchange, tdAfficheDestination (combinables)",
     NULL, &E_tdOperation},
    {0}
};
static const OwlSignature sig_table_mod[]    = {{-1, p_table_mod,    "", ""}, {0}};
static const OwlSignature sig_table_swap[]   = {{3,  p_table_swap,   "", ""}, {0}};
static const OwlSignature sig_table_deplace[] = {
    {3, p_table_deplace, "entier", "Nouvel indice de la ligne deplacee"},
    {4, p_table_deplace, "entier", "+ Operation"},
    {0}
};
static const OwlSignature sig_table_sel[]    = {{2,  p_table_sel,    "", ""}, {0}};
static const OwlSignature sig_table_selN[]   = {
    {1, p_table_select_full, "entier", "Indice selectionne ou -1"},
    {2, p_table_select_full, "entier", "+ Rang"},
    {3, p_table_select_full, "entier", "+ Information a renvoyer"},
    {0}
};
static const OwlSignature sig_table_selOcc[] = {{1,  p_table_simple, "entier", "Nombre de lignes selectionnees"}, {0}};
static const OwlSignature sig_table_sort[]   = {{-1, p_table_sort,   "", ""}, {0}};
static const OwlSignature sig_table_filter[] = {{3,  p_table_filter, "", ""}, {0}};
static const OwlSignature sig_table_disable_filter[] = {{1, p_table_disable_filter, "", ""}, {0}};
static const OwlSignature sig_table_saisie_loupe[] = {
    {1, p_table_saisie_loupe, "", ""},
    {2, p_table_saisie_loupe, "", ""},
    {3, p_table_saisie_loupe, "", ""},
    {0}
};
static const OwlSignature sig_table_filtered_col[] = {
    {1, p_table_filtered_col, "cha\xc3\xae" "ne",
     "Colonnes filtrees (\"<col>;<type>;<valeur>\" par RC), \"\" si aucune"},
    {0}
};
static const OwlSignature sig_table_pos[]    = {
    {1, p_table_pos, "entier", "1ere ligne visible (getter)"},
    {2, p_table_pos, "entier", "Positionne a l'indice"},
    {3, p_table_pos, "entier", "+ Option"},
    {0}
};
static const OwlSignature sig_table_xy[]     = {{4,  p_table_xy,     "entier|chaîne", "Numero ligne/colonne ou nom"}, {0}};

/*  — export Excel / copie presse-papier. */
static const OwlParam p_table_vers_excel[] = {
    {"table",  "Champ Table", "Le Champ Table source (référence ou nom)"},
    {"chemin", "chaîne",      "Chemin du fichier .xlsx à créer"},
    {0}
};
static const OwlSignature sig_table_vers_excel[] = {
    {2, p_table_vers_excel, "booléen", "Vrai si le classeur a été écrit"}, {0}
};
static const OwlParam p_table_vers_pp[] = {
    {"table", "Champ Table", "Le Champ Table source (référence ou nom)"},
    {0}
};
static const OwlSignature sig_table_vers_pp[] = {
    {1, p_table_vers_pp, "booléen", "Vrai si le texte a été copié"}, {0}
};
static const OwlMethodBinding MB_Table_VersExcel[] = {
    {"ChampTable", "VersExcel", 2}, {"ChampTable", "ToExcel", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Table_VersPressePapier[] = {
    {"ChampTable", "VersPressePapier", 1}, {"ChampTable", "ToClipboard", 1},
    {NULL, NULL, 0}
};

/* ====================================================================
 *  Champ Liste — V1 mono-colonne texte
 * ==================================================================== */

/* Accepte un Champ Liste OU un Champ Combo (la majorite des fonctions
 * ListeXXX operent sur les deux indifferemment via owl_ui_listcombo_*). */

/*  — items store headless pour Liste/Combo.
 *  — generalisation aux kinds SELECTEUR / INTERRUPTEUR / ZONE_REPETEE
 *            (meme bug : 0 retourne en mode headless apres N ajouts).
 *
 * Quand le widget Qt n'existe pas (mode headless ou pre-Ouvre), les natives
 * <Widget>Ajoute/Supprime/Occurrence/... doivent rester observables : un user
 * (ou un test) qui empile 3 items et lit Occurrence attend 3. Sans store,
 * owl_ui_<widget>_count retourne 0 (no-op no-widget) — c'est le bug d'origine.
 *
 * Le store est un ObjArray de Value, partage par tous les widgets concernes :
 *   - LISTE/COMBO/SELECTEUR/INTERRUPTEUR : chaque cellule = ObjString libelle.
 *   - ZONE_REPETEE : chaque cellule = NIL_VAL (placeholder ; la rep est
 *     materialisee Qt-side et on ne sait pas representer un sub-tree de
 *     widgets en headless ; seul le count importe pour Occurrence/Vide V1).
 *
 * Stockage : headless_store deja existant (ObjUiControl::headless_store ou
 * ObjUiControlInstance::headless_store) sous OWL_UI_LIST_ITEMS_KEY (defini
 * dans ui_decl.h pour partage avec vm.c). Le headless_store est trace par
 * le GC (cf. gc.c case OBJ_UI_CONTROL / OBJ_UI_CONTROL_INSTANCE), donc
 * l'ObjArray est rooted automatiquement.
 *
 * V1 : on stocke uniquement le texte affiche (Liste/Combo/Sel/Int). Les
 * valeurs memorisees / images / icones sont hors-scope du fallback (cf.
 * ticket  §"Hors-scope").
 *
 * Les entry points 'count' (lecture pure du store) sont definis dans
 * ui_decl.c — toujours compile, meme dans owl-cli sans GUI — pour eviter
 * de tirer stdlib_ui.c (GUI-only) comme dependance hard de vm.c. Leur nom
 * (owl_ui_headless_widget_items_count_*) est generique kind-agnostic. */

/* Materialise (si absent) le store d'items pour un ObjUiControlInstance.
 * Toujours retourne un ObjArray non-NULL. Kind-agnostic : la clef du store
 * est la meme pour tous les widgets a items. */

/* Variante ObjUiControl (raccourci global non-instancie). */

/* Lecture seule : retourne le store si materialise, NULL sinon. Pas de
 * side-effect. Utilise par les getters Occurrence/Vide (vm.c). */


/*  — store de lignes headless Table. Implementations des
 * helpers forward-declares avant les natives Table. Calque ZR :
 * une rep ObjInstance "nue" (structName=NULL) par ligne, champ indexe par
 * NOM de colonne, Value typee. */

/* Resout l'uici Table depuis l'arg receveur. NULL si pas une Table ou si la
 * reference n'est pas resolvable (l'erreur de type a deja ete posee par
 * table_arg_or_error cote caller). */

/* Construit une rep ObjInstance pour une ligne : pour chaque colonne declaree
 * (dans l'ordre), pose le champ <COL_Nom> = values[i] (Value d'origine typee),
 * ou chaine vide typee si la valeur manque (coherent ZR/). */





/* Reecrit la ligne Qt `row` (1-based) a partir de la rep du store, une chaine
 * par colonne declaree. Sert la reflexion widget d'un reordonnancement (swap)
 * ou le modele Qt n'a pas d'operation atomique dediee. */

/* Résout un nom de colonne (déjà accents strippés, casse-insensible) en index
 * 1-based dans la déclaration de la Table ; 0 si inconnu. Le nom déclaré est
 * normalisé de la même façon. */

/*  /  — pousse au widget une couleur de mise en forme deja
 * stockee cote store. Le store reste la source de verite (la relecture ne passe
 * jamais par Qt) : pur effet de bord d'affichage, silencieux des que la Table
 * n'a pas de widget monte.
 *
 * Trois receveurs possibles, distingues ici :
 *  - rep de ligne (store d'items) -> niveau ligne/cellule, indice retrouve par
 *    IDENTITE DE POINTEUR (une rep ne connait pas son rang, et un rang memorise
 *    deviendrait faux au premier TableSupprimeLigne / TableInsereLigne) ;
 *  - holder de colonne ($colfmt, ) -> niveau colonne, peint toute la
 *    colonne (lignes presentes + futures via persistance cote view). */

/*  — miroir GUI du set runtime `COL_X..MasqueSaisie` : pousse le masque
 * canon au widget (property col_mask_<col0> + delegate masquant). Le store OWL-side
 * reste la source de vérité du get ; ceci n'est que l'application à l'édition. */

/* owl_ui_headless_widget_items_count_uic / _uici : entry points exposes a vm.c,
 * definis dans ui_decl.c (toujours compile, contrairement a stdlib_ui.c).
 * Les helpers locaux ci-dessus (widget_items_store_peek_*) servent au code
 * GUI (fn_<widget>_* de ce fichier) ; ui_decl.c re-deroule la meme lecture
 * (acces direct au headless_store) cote CLI-friendly. */

/* Resout un argument widget-a-items en target dispatchable : soit widget Qt
 * (mode UI reelle), soit ObjArray items store (mode headless). Exactement
 * un des deux out est non-NULL en cas de succes.
 *
 * Kind-agnostic : k1 obligatoire, k2 optionnel (-1 si pas applicable). Liste
 * acceptee = {k1, k2}. Le humanKind sert au message d'erreur via le helper
 * resolve_widget_kinded.
 *
 * - create=true : materialise le store si necessaire (cas Ajoute/Insere).
 * - create=false : ne materialise pas (cas Occurrence/Cherche pure lecture).
 *   En lecture sans store materialise, items_out = NULL → caller retourne 0.
 *
 * Retourne true si la resolution kind+receiver est OK, false sinon (l'erreur
 * a deja ete emise par resolve_widget_kinded).
 *
 *  : generalise resolve_list_target (Liste/Combo only) en helper
 * partage Liste/Combo/Selecteur/Interrupteur/ZoneRepetee. */

/* Wrapper Liste/Combo (callers historiques ). */

/* Helper : convertit un Value en chaine UTF-8 transitoire. Retourne le
 * pointeur du ObjString cree (a garder en vie pendant l'appel Qt). */










/* Resout un indice canon : si == ligneSelectionnee (sentinel), retourne
 * la ligne courante, sinon retourne idx tel quel. */



/* ListeSelectOccurrence(liste [, rang]) :
 *   - 1 arg  : nombre d'elements selectionnes
 *   - 2 args : indice 1-based de la rang-eme ligne selectionnee (0 si invalide) */

/* ListeDeplaceLigne(liste, source, destination [, operation]) :
 * Source et destination supportent les indices entier 1-based, la constante
 * ligneSelectionnee, et (pour destination) les cibles relatives tdSuivant/etc. */


/* ListeSelect(liste [, indice]) :
 *   - 1 arg  : retourne l'indice 1-based de l'element selectionne (0 si rien).
 *   - 2 args : positionne la selection sur l'indice. */

static const OwlParam p_liste_simple[] = {
    {"liste", "tout", "Le Champ Liste cible (référence ou nom)"}, {0}
};
static const OwlParam p_liste_add[] = {
    {"liste",            "tout", "Le Champ Liste cible (référence ou nom)"},
    {"element",          "tout", "Optionnel : element a ajouter (converti en chaine)"},
    {"valeurMemorisee",  "tout", "Optionnel : valeur cachee associee (clef interne). Defaut : index 1-based."},
    {"cheminImage",      "chaine", "Optionnel : chemin de l'image (Liste Image)"},
    {0}
};
static const OwlParam p_liste_insere[] = {
    {"liste",            "tout", "Le Champ Liste cible (référence ou nom)"},
    {"indice",           "entier", "Indice 1-based de l'insertion"},
    {"element",          "tout", "Optionnel : element a inserer"},
    {"valeurMemorisee",  "tout", "Optionnel : valeur cachee. Defaut : index 1-based."},
    {"cheminImage",      "chaine", "Optionnel : chemin de l'image (Liste Image)"},
    {0}
};
static const OwlParam p_liste_rm[] = {
    {"liste",  "tout", "Le Champ Liste cible (référence ou nom)"},
    {"indice", "entier",  "Indice 1-based a supprimer"},
    {0}
};
static const OwlParam p_liste_modifie[] = {
    {"liste",            "tout", "Le Champ Liste cible (référence ou nom)"},
    {"indice",           "entier", "Indice 1-based a modifier"},
    {"element",          "tout", "Nouvelle valeur affichee"},
    {"valeurMemorisee",  "tout", "Optionnel : nouvelle valeur cachee"},
    {"cheminImage",      "chaine", "Optionnel : nouveau chemin d'image (Liste Image)"},
    {0}
};
static const OwlParam p_liste_cherche[] = {
    {"liste",  "tout", "Le Champ Liste cible (référence ou nom)"},
    {"valeur", "tout", "Valeur recherchee (match exact)"},
    {0}
};
static const OwlParam p_liste_trie[] = {
    {"liste", "tout", "Le Champ Liste cible (référence ou nom)"},
    {"ordre", "entier|chaîne", "Optionnel : entier (>=0=asc, <0=desc) ou chaine '+'/'-'"},
    {0}
};
static const OwlParam p_liste_select[] = {
    {"liste",  "tout", "Le Champ Liste cible (référence ou nom)"},
    {"indice", "entier",  "Optionnel : positionne la selection a cet indice"},
    {0}
};

static const OwlSignature sig_liste_simple[]  = {{1,  p_liste_simple,  "", ""}, {0}};
static const OwlSignature sig_liste_add[]     = {
    {1, p_liste_add, "", "Ligne vide"},
    {2, p_liste_add, "", "+ Element"},
    {3, p_liste_add, "", "+ Valeur memorisee"},
    {4, p_liste_add, "", "+ Chemin image"},
    {0}
};
static const OwlSignature sig_liste_insere[]  = {
    {2, p_liste_insere, "", "Indice + ligne vide"},
    {3, p_liste_insere, "", "+ Element"},
    {4, p_liste_insere, "", "+ Valeur memorisee"},
    {5, p_liste_insere, "", "+ Chemin image"},
    {0}
};
static const OwlSignature sig_liste_rm[]      = {{2,  p_liste_rm,      "", ""}, {0}};
static const OwlSignature sig_liste_modifie[] = {
    {3, p_liste_modifie, "", "Indice + element"},
    {4, p_liste_modifie, "", "+ Valeur memorisee"},
    {5, p_liste_modifie, "", "+ Chemin image"},
    {0}
};
static const OwlSignature sig_liste_cherche[] = {{2,  p_liste_cherche, "entier", "Indice 1-based ou -1"}, {0}};
static const OwlSignature sig_liste_occ[]     = {{1,  p_liste_simple,  "entier", "Nombre d'elements"}, {0}};
static const OwlSignature sig_liste_trie[]    = {
    {1, p_liste_trie, "", "Tri ascendant"},
    {2, p_liste_trie, "", "+ Ordre"},
    {0}
};
static const OwlSignature sig_liste_select[]  = {
    {1, p_liste_select, "entier", "Indice 1-based courant"},
    {2, p_liste_select, "entier", "Positionne la selection"},
    {0}
};

static const OwlParam p_liste_sel_idx[] = {
    {"liste",  "tout", "Le Champ Liste cible (référence ou nom)"},
    {"indice", "entier",  "Indice 1-based (ou ligneSelectionnee)"},
    {0}
};
static const OwlParam p_liste_sel_occ[] = {
    {"liste",  "tout", "Le Champ Liste cible (référence ou nom)"},
    {"rang",   "entier",  "Optionnel : rang 1-based dans les selections"},
    {0}
};
static const OwlParam p_liste_deplace[] = {
    {"liste",       "tout", "Le Champ Liste cible (référence ou nom)"},
    {"source",      "entier",  "Indice 1-based source (ou ligneSelectionnee)"},
    /* acceptsLiterals=true : destination accepte un indice entier libre
     * OU une cible relative tdXxx — le LSP propose les 2. */
    {"destination", "entier",  "Indice 1-based destination ou cible relative (tdSuivant, tdPremier, ...)",
     &E_tdCibleRelative, NULL, .acceptsLiterals=true},
    {"operation",   "entier",  "Optionnel : tdDeplace (defaut), tdEchange, tdAfficheDestination (combinables)",
     NULL, &E_tdOperation},
    {0}
};
static const OwlParam p_liste_info_xy[] = {
    {"liste", "tout", "Le Champ Liste cible (référence ou nom)"},
    {"type",  "entier",  "Type d'info (tiNumLigne)", .allowedBase=&E_TiXY},
    {"X",     "entier",  "Coord X pixel relatif au champ"},
    {"Y",     "entier",  "Coord Y pixel relatif au champ"},
    {0}
};

static const OwlSignature sig_liste_sel_idx[] = {{2,  p_liste_sel_idx, "", ""}, {0}};
static const OwlSignature sig_liste_sel_occ[] = {
    {1, p_liste_sel_occ, "entier", "Nombre de lignes selectionnees"},
    {2, p_liste_sel_occ, "entier", "Indice 1-based de la rang-eme selection"},
    {0}
};
static const OwlSignature sig_liste_deplace[] = {
    {3, p_liste_deplace, "entier", "Source + destination"},
    {4, p_liste_deplace, "entier", "+ Operation"},
    {0}
};
static const OwlSignature sig_liste_info_xy[] = {{4,  p_liste_info_xy, "entier", "Indice de ligne au point ou -1"}, {0}};

/* ====================================================================
 *  Champ Combo — natives Combo-specifiques (ComboFerme / ComboOuvre)
 * ==================================================================== */




static const OwlParam p_combo_simple[] = {
    {"combo", "tout", "Le Champ Combo cible (référence ou nom)"},
    {0}
};
static const OwlSignature sig_combo_simple[] = {{1, p_combo_simple, "", ""}, {0}};

static const OwlMethodBinding MB_Combo_Ouvre[] = {
    {"ChampCombo", "Ouvre", 1}, {"ChampCombo", "Open", 1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Combo_Ferme[] = {
    {"ChampCombo", "Ferme", 1}, {"ChampCombo", "Close", 1}, {NULL, NULL, 0}
};

/* Method bindings : LISTE_X..Ajoute(...) / .Add(...) etc.
 * arity = receveur + n params. Le receveur est args[0] cote fn_liste_*. */
/* MB pour ListeXXX : double-binding ChampListe + ChampCombo, le dispatch
 * runtime via owl_ui_listcombo_* fait le bon backend selon le receiver. */
static const OwlMethodBinding MB_Liste_Ajoute[] = {
    {"ChampListe", "Ajoute", -1}, {"ChampListe", "Add", -1},
    {"ChampCombo", "Ajoute", -1}, {"ChampCombo", "Add", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Insere[] = {
    {"ChampListe", "Ins\xc3\xa8re", -1}, {"ChampListe", "Insere", -1},
    {"ChampListe", "Insert", -1},
    {"ChampCombo", "Ins\xc3\xa8re", -1}, {"ChampCombo", "Insere", -1},
    {"ChampCombo", "Insert", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Supprime[] = {
    {"ChampListe", "Supprime", 2}, {"ChampListe", "Delete", 2},
    {"ChampCombo", "Supprime", 2}, {"ChampCombo", "Delete", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_SupprimeTout[] = {
    {"ChampListe", "SupprimeTout", 1}, {"ChampListe", "DeleteAll", 1},
    {"ChampCombo", "SupprimeTout", 1}, {"ChampCombo", "DeleteAll", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Modifie[] = {
    {"ChampListe", "Modifie", -1}, {"ChampListe", "Modify", -1},
    {"ChampCombo", "Modifie", -1}, {"ChampCombo", "Modify", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Affiche[] = {
    {"ChampListe", "Affiche", 1}, {"ChampListe", "Display", 1},
    {"ChampCombo", "Affiche", 1}, {"ChampCombo", "Display", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Cherche[] = {
    {"ChampListe", "Cherche", 2}, {"ChampListe", "Seek", 2},
    {"ChampCombo", "Cherche", 2}, {"ChampCombo", "Seek", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Occurrence[] = {
    {"ChampListe", "Occurrence", 1}, {"ChampListe", "Count", 1},
    {"ChampCombo", "Occurrence", 1}, {"ChampCombo", "Count", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Trie[] = {
    {"ChampListe", "Trie", -1}, {"ChampListe", "Sort", -1},
    {"ChampCombo", "Trie", -1}, {"ChampCombo", "Sort", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_Select[] = {
    {"ChampListe", "Select", -1}, {"ChampListe", "Selected", -1},
    {"ChampCombo", "Select", -1}, {"ChampCombo", "Selected", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_SelectPlus[] = {
    {"ChampListe", "SelectPlus", 2}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_SelectMoins[] = {
    {"ChampListe", "SelectMoins", 2}, {"ChampListe", "SelectMinus", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_SelectOccurrence[] = {
    {"ChampListe", "SelectOccurrence", -1}, {"ChampListe", "SelectCount", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_DeplaceLigne[] = {
    {"ChampListe", "D\xc3\xa9placeLigne", -1}, {"ChampListe", "DeplaceLigne", -1},
    {"ChampListe", "MoveLine", -1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Liste_InfoXY[] = {
    {"ChampListe", "InfoXY", 4}, {NULL, NULL, 0}
};

/* ====================================================================
 *  Champ Selecteur — natives Selecteur*
 * ==================================================================== */

/*  cleanup : selecteur_arg_or_error retire (code mort post-refactor).
 * Les natives fn_selecteur_* utilisent directement resolve_widget_items_target. */





static const OwlParam p_sel_simple[] = {
    {"selecteur", "tout", "Le Champ Selecteur cible"}, {0}
};
static const OwlParam p_sel_add[] = {
    {"selecteur",  "tout",   "Le Champ Selecteur cible"},
    {"libelle",    "chaine", "Libelle de l'option a ajouter"},
    {"sousLibelle","chaine", "Optionnel : sous-libelle"},
    {"image",      "chaine", "Optionnel : chemin de l'icone (modes Vista)"},
    {0}
};
static const OwlParam p_sel_insere[] = {
    {"selecteur",  "tout",   "Le Champ Selecteur cible"},
    {"indice",     "entier", "Indice 1-based d'insertion"},
    {"libelle",    "chaine", "Libelle de l'option a ajouter"},
    {"sousLibelle","chaine", "Optionnel : sous-libelle"},
    {"image",      "chaine", "Optionnel : chemin de l'icone (modes Vista)"},
    {0}
};
static const OwlParam p_sel_rm[] = {
    {"selecteur", "tout",   "Le Champ Selecteur cible"},
    {"indice",    "entier", "Indice 1-based a supprimer"},
    {0}
};

static const OwlSignature sig_sel_add[]    = {
    {2, p_sel_add, "entier", "Libelle, indice 1-based ajoute"},
    {3, p_sel_add, "entier", "+ Sous-libelle"},
    {4, p_sel_add, "entier", "+ Image"},
    {0}
};
static const OwlSignature sig_sel_insere[] = {
    {3, p_sel_insere, "entier", "Indice + libelle, indice 1-based insere"},
    {4, p_sel_insere, "entier", "+ Sous-libelle"},
    {5, p_sel_insere, "entier", "+ Image"},
    {0}
};
static const OwlSignature sig_sel_rm[]     = {{2,  p_sel_rm,     "", ""}, {0}};
static const OwlSignature sig_sel_occ[]    = {{1,  p_sel_simple, "entier", "Nombre d'options"}, {0}};

static const OwlMethodBinding MB_Sel_Ajoute[] = {
    {"ChampSelecteur", "Ajoute", -1}, {"ChampSelecteur", "Add", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Sel_Insere[] = {
    {"ChampSelecteur", "Ins\xc3\xa8re", -1}, {"ChampSelecteur", "Insere", -1},
    {"ChampSelecteur", "Insert", -1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Sel_Supprime[] = {
    {"ChampSelecteur", "Supprime", 2}, {"ChampSelecteur", "Delete", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Sel_Occurrence[] = {
    {"ChampSelecteur", "Occurrence", 1}, {"ChampSelecteur", "Count", 1},
    {NULL, NULL, 0}
};

/* ====================================================================
 *  Champ Interrupteur — natives Interrupteur*
 * ==================================================================== */

/*  cleanup : interrupteur_arg_or_error retire (code mort post-refactor).
 * Les natives fn_interrupteur_* utilisent directement resolve_widget_items_target. */





static const OwlParam p_int_simple[] = {
    {"interrupteur", "tout", "Le Champ Interrupteur cible"}, {0}
};
static const OwlParam p_int_add[] = {
    {"interrupteur",  "tout",   "Le Champ Interrupteur cible"},
    {"libelle",       "chaine", "Libelle de l'option"},
    {"sousLibelle",   "chaine", "Optionnel : sous-libelle"},
    {"image",         "chaine", "Optionnel : chemin de l'icone"},
    {0}
};
static const OwlParam p_int_insere[] = {
    {"interrupteur",  "tout",   "Le Champ Interrupteur cible"},
    {"indice",        "entier", "Indice 1-based"},
    {"libelle",       "chaine", "Libelle"},
    {"sousLibelle",   "chaine", "Optionnel"},
    {"image",         "chaine", "Optionnel"},
    {0}
};
static const OwlParam p_int_rm[] = {
    {"interrupteur",  "tout",   "Le Champ Interrupteur cible"},
    {"indice",        "entier", "Indice 1-based"},
    {0}
};

static const OwlSignature sig_int_add[]    = {
    {2, p_int_add, "entier", "Libelle, indice 1-based ajoute"},
    {3, p_int_add, "entier", "+ Sous-libelle"},
    {4, p_int_add, "entier", "+ Image"},
    {0}
};
static const OwlSignature sig_int_insere[] = {
    {3, p_int_insere, "entier", "Indice + libelle, indice 1-based insere"},
    {4, p_int_insere, "entier", "+ Sous-libelle"},
    {5, p_int_insere, "entier", "+ Image"},
    {0}
};
static const OwlSignature sig_int_rm[]     = {{2,  p_int_rm,     "", ""}, {0}};
static const OwlSignature sig_int_occ[]    = {{1,  p_int_simple, "entier", "Nombre d'options"}, {0}};

static const OwlMethodBinding MB_Int_Ajoute[] = {
    {"ChampInterrupteur", "Ajoute", -1}, {"ChampInterrupteur", "Add", -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Int_Insere[] = {
    {"ChampInterrupteur", "Ins\xc3\xa8re", -1}, {"ChampInterrupteur", "Insere", -1},
    {"ChampInterrupteur", "Insert", -1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Int_Supprime[] = {
    {"ChampInterrupteur", "Supprime", 2}, {"ChampInterrupteur", "Delete", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Int_Occurrence[] = {
    {"ChampInterrupteur", "Occurrence", 1}, {"ChampInterrupteur", "Count", 1},
    {NULL, NULL, 0}
};


/* Appele par open_window apres la creation du widget ZR. Scanne decl pour
 * trouver les sous-decls dont parent_name == zr_name et les memorise. */

/* Verifie si un nom est celui d'une ZR dans decl (utilise au filtre
 * d'open_window pour skip les enfants qui appartiennent au template). */

/* ZR arg helper. */

/* Construit une nouvelle repetition : cree un Conteneur, instancie chaque
 * sous-decl du template via build_control comme enfant, ajoute la rep au
 * ZR widget. Retourne l'indice 1-based. */
/* Construit le QWidget rep + ses sub-widgets sans encore l'inserer dans
 * la ZR. Le caller decide (append vs insert at idx). Retourne le widget
 * ou NULL si echec. */

/* Track une rep dans rt->reps[] a l'indice idx_1based (1-based, decalage
 * a droite si insertion mid-list). idx_1based = 0 ou > count+1 → append. */


/* Trigger surAffichageLigne(indice) sur la rep nouvellement creee. La
 * procedure utilisateur recoit l'indice 1-based. */

/*  — modele de rep headless ZR.
 *
 * En GUI une rep est un sous-arbre de QWidgets clones du template ; en
 * headless on ne monte aucun widget. Pour rendre `ZR[i].champ` (get ET set)
 * fonctionnel hors fenetre Qt (FinInitialisation, owl test), chaque rep
 * headless est un ObjInstance "nu" (structName=NULL) dont les champs sont
 * indexes par NOM de sous-controle (LIB_Num, SAI_Valeur, ...). Pre-seede
 * a "" pour que la lecture d'une cellule jamais ecrite rende une chaine
 * vide typee (coherent ), pas Nul. Stocke dans le meme ObjArray que
 * le compteur headless (OWL_UI_LIST_ITEMS_KEY) : ObjInstance par ligne au
 * lieu du NIL_VAL placeholder. Le count (donc Occurrence) reste inchange.
 *
 * Les noms de template viennent du decl fenetre owner : sous-controles dont
 * parent_name == nom de la ZR. */

/*  — hook OP_STRUCT_SET : coerce la valeur ecrite dans une cellule
 * de rep ZR selon le contrat de valeur de son sous-controle. Seules les reps
 * taggees `$zrsrc` (donc ZR headless, jamais une rep Table ni un struct user)
 * sont concernees. Le sous-controle nomme `field` est retrouve dans le decl
 * fenetre via la back-ref uici ; une Saisie coerce sa cellule en chaine
 * (parite stricte SAI..Valeur ). Tout autre kind (Libelle, etc.) :
 * valeur inchangee (le Libelle est deja chaine par seed). */

/*  — propriete par defaut d'un kind de control (lookup CONTROL_KINDS).
 * Saisie -> "Valeur", Libelle/Bouton -> "Libelle". NULL si pas de prop par
 * defaut scalaire. */

/*  — hook OP_STRUCT_GET (et SET) : materialise PARESSEUSEMENT la
 * cellule `field` d'une rep ZR headless en ObjUiControl (widget==NULL), dont
 * le headless_store devient le store de props par cellule. Garde stricte :
 * rep taggee `$zrsrc` ET uici source de kind ZoneRepetee ET `field` matchant
 * un sous-controle de la ZR. Idempotent (renvoie l'ObjUiControl existant si
 * deja materialise). Sinon : seed la prop par defaut avec le scalaire courant
 * (preserve la valeur  sous ..Valeur), remplace le champ dans la rep.
 * Retourne NIL_VAL si pas une cellule ZR -> le caller retombe sur table_get. */


/*  (ZR) +  (Table) — hook d'indexation widget[i] sur instance
 * ( / ). Resout le proxy de rep de ligne :
 *   - ZR en GUI (uici->widget) : ObjUiControl(CONTAINER) sur le QWidget rep,
 *     comme l'ancienne branche ZR de selecteur_get_subitem (parite stricte).
 *   - ZR headless : l'ObjInstance rep du store d'items.
 *   - Table : toujours l'ObjInstance rep du store d'items (le store est peuple
 *     par TableAjouteLigne/ModifieLigne dans les deux modes ; le widget Qt
 *     reste source de verite pour l'affichage, mais la lecture cellule par
 *     programme passe par la rep headless, champ indexe par nom de colonne).
 * Retourne NIL_VAL si kind non gere, idx hors-bornes, ou rep absente. */

/* Insere une repetition vide a l'indice idx_1based. Pour V1 simple : on
 * construit normalement (append en fin) puis on permute si idx != count.
 * Plus efficace serait insertRep direct cote Qt, mais on perdrait la
 * symetrie de zr_build_one_rep. */





static const OwlParam p_zr_simple[] = {
    {"zr", "tout", "Le Champ ZoneRepetee cible"}, {0}
};
static const OwlParam p_zr_insere[] = {
    {"zr",     "tout",   "Le Champ ZoneRepetee cible"},
    {"indice", "entier", "Indice 1-based"},
    {0}
};
static const OwlParam p_zr_rm[] = {
    {"zr",     "tout",   "Le Champ ZoneRepetee cible"},
    {"indice", "entier", "Indice 1-based de la repetition a supprimer"},
    {0}
};
static const OwlSignature sig_zr_simple[] = {{1, p_zr_simple, "entier", "Indice ou 0"}, {0}};
static const OwlSignature sig_zr_void[]   = {{1, p_zr_simple, "", ""}, {0}};
static const OwlSignature sig_zr_insere[] = {{2, p_zr_insere, "entier", "Indice 1-based ajoute"}, {0}};
static const OwlSignature sig_zr_rm[]     = {{2, p_zr_rm,     "booleen", "Vrai si succes"}, {0}};

static const OwlMethodBinding MB_Zr_AjouteLigne[] = {
    {"ChampZoneRepetee", "AjouteLigne", 1}, {"ChampZoneRepetee", "AddLine", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Zr_Insere[] = {
    {"ChampZoneRepetee", "Ins\xc3\xa8re", 2}, {"ChampZoneRepetee", "Insere", 2},
    {"ChampZoneRepetee", "Insert", 2}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Zr_Supprime[] = {
    {"ChampZoneRepetee", "Supprime", 2}, {"ChampZoneRepetee", "Delete", 2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Zr_SupprimeTout[] = {
    {"ChampZoneRepetee", "SupprimeTout", 1}, {"ChampZoneRepetee", "DeleteAll", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Zr_Occurrence[] = {
    {"ChampZoneRepetee", "Occurrence", 1}, {"ChampZoneRepetee", "Count", 1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_Zr_Affiche[] = {
    {"ChampZoneRepetee", "Affiche", 1}, {"ChampZoneRepetee", "Display", 1},
    {NULL, NULL, 0}
};

/* ====================================================================
 *  Lot 2 — Capture image fenetre / champ / ecran.
 *
 * Migration depuis stdlib_image.c (Lot 1 produisait du placeholder
 * uniforme). Le grab Qt reel necessite le backend GUI lie dans
 * owl_runtime_gui, accessible uniquement depuis stdlib_ui.c.
 *
 * Mode display normal :
 *   - Fenetre / champ : QWidget::grab() rend off-screen (pas besoin de
 *     show()), Format_RGBA8888 = byte layout identique a OwlImage.
 *   - Ecran : QScreen::grabWindow(0) capture le desktop entier.
 *
 * Mode test headless (vm.headless_mode = true) :
 *   - Fenetre / champ : si l'ObjWindowInstance n'a pas de widget Qt
 *     (cas standard sous owl test ou apres Ouvre headless), on
 *     materialise le widget ad-hoc via build_window, on grab, puis on
 *     detruit immediatement le widget pour eviter toute reference
 *     dangling cote instance / controls.
 *   - Ecran : refus explicite avec ErreurDetectee + message clair
 *     (pas de display physique a capturer en headless).
 *
 * Le 2e param ThèmeSnapshot reste accepte mais no-op au Lot 2
 * (activation effective Lot 3 / ).
 * ==================================================================== */


/* Fallback dimensions si grab Qt echoue (image vide). Utile aussi pour
 * le placeholder en cas d'OOM. Lit Largeur / Hauteur dans le decl. */


/* Resolution Value -> ObjWindowInstance* via vm.window_stack. Accepte
 * ObjWindowRef, ObjString (nom), ou ObjWindowInstance. NULL si pas
 * trouve. Repris du Lot 1. */

/* Resolution Value -> ObjUiControlInstance*. Repris du Lot 1. */

/*  — ChampsDe(parent) / ControlsOf(parent) : tableau des noms des
 * champs DIRECTS d'un parent (fenetre ou champ conteneur), dans l'ordre de
 * creation (= ordre de declaration = ordre de win->controls[]).
 *
 * Modele : tous les champs d'une fenetre vivent a plat dans win->controls[].
 * Un champ direct de la fenetre a decl->parent_name == NULL ; un champ interne
 * a un conteneur porte decl->parent_name = nom du conteneur. On ne descend donc
 * jamais dans les enfants d'un conteneur quand on liste une fenetre.
 *
 * Deux formes selon l'argument :
 *   - fenetre    : champs a parent_name == NULL.
 *   - conteneur  : champs dont parent_name == nom du conteneur.
 * Parent sans champ direct -> tableau vide (aucune erreur). Argument non
 * resoluble -> tableau vide. Marche en headless (aucun widget requis). */

/*  — recherche recursive d'une OptionMenu par nom dans l'arbre d'un
 * menu (top-level + sous-menus imbriques). Casse-insensible (invariant 1).
 * Retourne le noeud OPTION correspondant, ou NULL. */

/* TestSimuleMenu(fenetre, cheminOption) — declenche par code le surSelection()
 * de l'OptionMenu nommee dans le MenuPrincipal affecte a la fenetre, en
 * headless, via le MEME chemin de dispatch que QAction::triggered
 * (call_event_block_with_sender). L'option est resolue recursivement dans
 * l'arbre du menu (top-level + sous-menus imbriques).
 *
 * surSelection est cablee comme event de FENETRE (is_window=true : arity 1,
 * MaFenetre seule — une option de menu n'est pas un champ, ). On
 * pousse donc 1 seul slot synthetique ; MoiMeme reste resolu via le
 * sender_name (nom de l'option) pousse sur event_sender_stack.
 *
 * Defensif : soft-error (ErreurDetectee) si l'option est introuvable ou si la
 * fenetre n'a pas de menu principal affecte — jamais de no-op silencieux. */

/* Headless build ad-hoc : materialise la fenetre Qt + ses controls
 * meme en mode headless. build_window rattache automatiquement le
 * widget a top_inst->widget si NULL — on neutralise cette adoption
 * apres coup pour preserver l'invariant "headless => widget == NULL"
 * cote instances. Le caller utilise le pointeur retourne pour grab(),
 * puis appelle owl_ui_window_destroy() pour liberer. */

/* Capture commune : produit un OwlImage* RGBA depuis un buffer Qt, ou
 * retombe sur un placeholder uniforme si grab a echoue (taille du decl).
 * Le buffer brut Qt est libere ici. */


/*  — __CaptureFenetre(fen, chemin) : capture offscreen d'une fenetre
 * ouverte vers un PNG. Native interne (test/polissage visuel). Reutilise la
 * resolution fenetre + build headless ad-hoc de dCopieImageFenetre : en mode
 * test le widget Qt n'est pas monte (headless), on le materialise le temps du
 * grab puis on le detruit. Pousse Vrai si le PNG est ecrit (non vide). */

/*  — retrouve + grabbe un QMenu par nom vers un PNG (alpha preserve).
 * Definie en C++ dans ui_qt.cpp ; declaree ici en linkage C. */

/*  — indique si une OptionMenu porte une icone effective. Definie en
 * C++ dans ui_qt.cpp ; declaree ici en linkage C. */

/*  — editabilite reelle d'une cellule de Table. Definie en C++ dans
 * ui_qt.cpp ; declaree ici en linkage C. */

/*  — __DebugTableCelluleMasque(champ, ligne, colonne) : masque EFFECTIF
 * (forme canon) de l'editeur transitoire de la cellule (ligne/colonne 1-based).
 * Miroir de __DebugTableCelluleEditable : lit l'etat Qt REEL (le masque pose par
 * createEditor du delegate colonne), non observable en headless. "" si champ
 * introuvable/non monte, non-Table, indices hors bornes, ou colonne sans masque. */

/*  — __DebugTableCelluleValideSaisie(champ, ligne, colonne, texte) :
 * Vrai ssi le validateur EFFECTIF de l'editeur transitoire de la cellule
 * (ligne/colonne 1-based) juge `texte` Acceptable. Miroir de
 * __DebugTableCelluleMasque : lit l'etat Qt REEL (le validateur pose par
 * createEditor pour une colonne typee), non observable en headless. Cellule sans
 * validateur (colonne non contrainte) -> Vrai. Vrai aussi si champ introuvable/
 * non monte, non-Table, ou indices hors bornes. */

/*  — __DebugTableCelluleTexteAffiche(champ, ligne, colonne) : texte
 * EFFECTIVEMENT rendu par la cellule (ligne/colonne 1-based) HORS edition, tel
 * que le delegate colonne le formate (DisplayRole + format defaut/projet pour
 * une colonne Numerique/Monetaire). Miroir de __DebugTableCelluleMasque cote
 * affichage : lit l'etat Qt REEL, non observable en headless. "" si champ
 * introuvable/non monte, non-Table, ou indices hors bornes. */

/*  — valeurs de colonne pour les lignes non masquees (rowHidden reel
 * du QTableView). Definie en C++ dans ui_qt.cpp. */

/*  — __DebugTableColonneVisible(TAB.COL) : valeurs de la colonne pour
 * les lignes NON masquees (rowHidden Qt reel), RC-jointes, ordre du modele.
 * Test-only (OWL_NAT_INTERNAL). Chaine vide si champ introuvable / non monte. */

/*  — __DebugTableValeursDistinctes(TAB.COL) : valeurs distinctes du
 * store (ordre de 1ere apparition, RC). Backend pur (pas de widget requis).
 * Test-only (OWL_NAT_INTERNAL). Chaine vide si colonne vide OU au-dela du seuil
 * de cardinalite. */

/*  — __DebugTableFiltreType(TAB.COL) : type (constante filtre*) du
 * filtre actif sur la colonne, via le VRAI pont owl_ui_table_user_get_filter_type
 * (resolution widget->uici->$tblfilters, identique au prereplissage inline).
 * Test-only (OWL_NAT_INTERNAL). 0 si aucun filtre ou champ non monte. */

/*  — __DebugOmbreFlou(champ) : rayon de flou de l'ombre portée
 * courante du widget (QGraphicsDropShadowEffect), -1 si aucune. Test-only
 * (OWL_NAT_INTERNAL) : verifie le swap d'ombre au survol/appui sans capture
 * pixel. Renvoie -1 si le champ est introuvable ou non monte. */

/*  — __SimuleInteraction(champ, code) : pousse un evenement souris sur
 * le widget pour exercer l'event filter d'ombre. code : 0=survol (Enter),
 * 1=quitte (Leave), 2=appui (MouseButtonPress), 3=relache (MouseButtonRelease).
 * Test-only (OWL_NAT_INTERNAL). No-op si le champ est introuvable ou non monte. */



/* Enum + consts ThèmeSnapshot — migration depuis stdlib_image.c (Lot 2).
 * Valeurs no-op au Lot 2 (activation effective Lot 3 / ). */
static const char* const COPIE_THEME_VALUES[] = {
    "themeD\xc3\xa9" "faut", "themeClair", "themeSombre", NULL
};
static const OwlEnum E_ThemeSnapshot = {
    .name="th\xc3\xa8meSnapshot", .nameAscii="themeSnapshot", .nameEN="snapshotTheme",
    .category="Image",
    .descFR="Thème de rendu appliqué lors d'une capture d'image.",
    .descEN="Render theme applied during an image capture.",
    .values=COPIE_THEME_VALUES,
    .combinable=false
};
static const OwlEnum* const COPIE_ENUMS[] = { &E_ThemeSnapshot };

/* ====================================================================
 *   — Constantes clavier tk* / te*
 *
 * Modele de valeurs (ratifie coordinator) : les touches SPECIALES (fleches,
 * Echap, F1...) prennent un code en base haute facon Qt::Key (0x01000000+),
 * hors collision avec les codes ASCII 0-127. C'est volontairement les memes
 * valeurs que l'enum Qt::Key correspondant, ce qui rend le mapping
 * QKeyEvent::key() -> tk* trivial (cf. owl_ui_qt_key_to_owl dans ui_qt.cpp).
 * Les touches TEXTUELLES (lettres, chiffres) n'ont PAS de constante : on
 * utilise Asc("X") (pattern canon). tkEspace fait exception : l'espace est
 * une touche textuelle dont le code = son code ASCII 32 (= Asc(" ")), pour
 * rester coherent avec ce qu'un appui espace dispatche reellement.
 *
 * Les tk* ne sont PAS des enums-contraints (pas de OwlEnum, pas de sentinel) :
 * de simples constantes entieres positives. Formes EN = entrees distinctes
 * pointant la meme valeur (tkUp == tkHaut). Bilingue par construction.
 * ==================================================================== */
#define OWL_TK_ESCAPE     0x01000000
#define OWL_TK_TAB        0x01000001
#define OWL_TK_BACKSPACE  0x01000003
#define OWL_TK_ENTER      0x01000004  /* Qt::Key_Return (touche Entree principale) */
#define OWL_TK_DELETE     0x01000007
#define OWL_TK_HOME       0x01000010
#define OWL_TK_END        0x01000011
#define OWL_TK_LEFT       0x01000012
#define OWL_TK_UP         0x01000013
#define OWL_TK_RIGHT      0x01000014
#define OWL_TK_DOWN       0x01000015
#define OWL_TK_PAGEUP     0x01000016
#define OWL_TK_PAGEDOWN   0x01000017
#define OWL_TK_SHIFT      0x01000020
#define OWL_TK_CONTROL    0x01000021
#define OWL_TK_ALT        0x01000023
#define OWL_TK_SPACE      0x20        /* touche textuelle : code ASCII */
#define OWL_TK_F1         0x01000030
#define OWL_TK_F2         0x01000031
#define OWL_TK_F3         0x01000032
#define OWL_TK_F4         0x01000033
#define OWL_TK_F5         0x01000034
#define OWL_TK_F6         0x01000035
#define OWL_TK_F7         0x01000036
#define OWL_TK_F8         0x01000037
#define OWL_TK_F9         0x01000038
#define OWL_TK_F10        0x01000039
#define OWL_TK_F11        0x0100003a
#define OWL_TK_F12        0x0100003b
/*  — couverture clavier integrale. Memes valeurs que Qt::Key
 * (cf. qnamespace.h), comme les tk*  : mapping QKeyEvent trivial,
 * aucune collision ASCII. */
#define OWL_TK_INSERT     0x01000006
#define OWL_TK_PAUSE      0x01000008
#define OWL_TK_PRINT      0x01000009  /* Qt::Key_Print = Impr ecran */
#define OWL_TK_META       0x01000022  /* Cmd (macOS) / Super (Linux) / Windows */
#define OWL_TK_CAPSLOCK   0x01000024
#define OWL_TK_NUMLOCK    0x01000025
#define OWL_TK_SCROLLLOCK 0x01000026
#define OWL_TK_MENU       0x01000055  /* touche Menu contextuel (!= Alt) */
#define OWL_TK_F13        0x0100003c
#define OWL_TK_F14        0x0100003d
#define OWL_TK_F15        0x0100003e
#define OWL_TK_F16        0x0100003f
#define OWL_TK_F17        0x01000040
#define OWL_TK_F18        0x01000041
#define OWL_TK_F19        0x01000042
#define OWL_TK_F20        0x01000043
#define OWL_TK_F21        0x01000044
#define OWL_TK_F22        0x01000045
#define OWL_TK_F23        0x01000046
#define OWL_TK_F24        0x01000047
/* OWL_TK_MOUSE_LEFT / OWL_TK_MOUSE_RIGHT : voir owl_ui.h (codes sentinelles
 * boutons souris pour teBoutonG/teBoutonD). */

/* Macro helper : une constante clavier FR canon avec son alias EN.
 * Une seule entree registre ; l'alias EN runtime est auto-genere par
 * owl_registry (invariant bilingue n°3, .nameEN). Pas de doublon LSP. */
#define TK_PAIR(fr, en, val, dfr, den) \
    { .name = (fr), .category = "Clavier", .value = INTEGER_VAL(val), \
      .descFR = (dfr), .descEN = (den), .nameEN = (en) }
/* Constante clavier dont la forme EN == FR (re-set benin de .nameEN
 * pour expliciter l'invariant bilingue sur les noms deja symetriques). */
#define TK_SAME(nm, val, dfr, den) \
    { .name = (nm), .category = "Clavier", .value = INTEGER_VAL(val), \
      .descFR = (dfr), .descEN = (den), .nameEN = (nm) }

static const OwlConstDef KEY_CONSTS[] = {
    /* Directionnelles. */
    TK_PAIR("tkHaut",   "tkUp",    OWL_TK_UP,
            "Touche fleche Haut.", "Up arrow key."),
    TK_PAIR("tkBas",    "tkDown",  OWL_TK_DOWN,
            "Touche fleche Bas.", "Down arrow key."),
    TK_PAIR("tkGauche", "tkLeft",  OWL_TK_LEFT,
            "Touche fleche Gauche.", "Left arrow key."),
    TK_PAIR("tkDroite", "tkRight", OWL_TK_RIGHT,
            "Touche fleche Droite.", "Right arrow key."),
    /* Controle de flux. */
    TK_PAIR("tkEspace", "tkSpace", OWL_TK_SPACE,
            "Touche Espace.", "Space key."),
    TK_PAIR("tkEchap",  "tkEsc",   OWL_TK_ESCAPE,
            "Touche Echap.", "Escape key."),
    TK_PAIR("tkEntr\xc3\xa9""e", "tkEnter", OWL_TK_ENTER,
            "Touche Entree.", "Enter key."),
    /* tkTab : meme forme FR/EN. */
    TK_SAME("tkTab", OWL_TK_TAB, "Touche Tabulation.", "Tab key."),
    /* Edition. */
    TK_PAIR("tkRetourArri\xc3\xa8re", "tkBackspace", OWL_TK_BACKSPACE,
            "Touche Retour arriere.", "Backspace key."),
    TK_PAIR("tkSuppr",  "tkDelete", OWL_TK_DELETE,
            "Touche Suppr.", "Delete key."),
    TK_PAIR("tkD\xc3\xa9""but", "tkHome", OWL_TK_HOME,
            "Touche Debut.", "Home key."),
    TK_PAIR("tkFin",    "tkEnd",   OWL_TK_END,
            "Touche Fin.", "End key."),
    TK_PAIR("tkPagePr\xc3\xa9""c", "tkPageUp", OWL_TK_PAGEUP,
            "Touche Page precedente.", "Page Up key."),
    TK_PAIR("tkPageSuiv", "tkPageDown", OWL_TK_PAGEDOWN,
            "Touche Page suivante.", "Page Down key."),
    /* Modificateurs. */
    TK_PAIR("tkMaj",    "tkShift", OWL_TK_SHIFT,
            "Touche Majuscule (Shift).", "Shift key."),
    /* tkCtrl / tkAlt : meme forme FR/EN. */
    TK_SAME("tkCtrl", OWL_TK_CONTROL, "Touche Controle (Ctrl).", "Control key."),
    TK_SAME("tkAlt",  OWL_TK_ALT,     "Touche Alt.", "Alt key."),
    /* Touches de fonction F1..F12 (meme forme FR/EN). */
    TK_SAME("tkF1",  OWL_TK_F1,  "Touche de fonction F1.",  "Function key F1."),
    TK_SAME("tkF2",  OWL_TK_F2,  "Touche de fonction F2.",  "Function key F2."),
    TK_SAME("tkF3",  OWL_TK_F3,  "Touche de fonction F3.",  "Function key F3."),
    TK_SAME("tkF4",  OWL_TK_F4,  "Touche de fonction F4.",  "Function key F4."),
    TK_SAME("tkF5",  OWL_TK_F5,  "Touche de fonction F5.",  "Function key F5."),
    TK_SAME("tkF6",  OWL_TK_F6,  "Touche de fonction F6.",  "Function key F6."),
    TK_SAME("tkF7",  OWL_TK_F7,  "Touche de fonction F7.",  "Function key F7."),
    TK_SAME("tkF8",  OWL_TK_F8,  "Touche de fonction F8.",  "Function key F8."),
    TK_SAME("tkF9",  OWL_TK_F9,  "Touche de fonction F9.",  "Function key F9."),
    TK_SAME("tkF10", OWL_TK_F10, "Touche de fonction F10.", "Function key F10."),
    TK_SAME("tkF11", OWL_TK_F11, "Touche de fonction F11.", "Function key F11."),
    TK_SAME("tkF12", OWL_TK_F12, "Touche de fonction F12.", "Function key F12."),

    /*  — couverture clavier integrale (~49 concepts). */
    /* Navigation : insertion. */
    TK_PAIR("tkIns\xc3\xa9r", "tkInsert", OWL_TK_INSERT,
            "Touche Inser (insertion).", "Insert key."),
    /* Modificateur Meta : meme forme FR/EN (Cmd / Super / Windows). */
    TK_SAME("tkMeta", OWL_TK_META,
            "Touche Meta (Commande sur macOS, Super sous Linux, Windows).",
            "Meta key (Command on macOS, Super on Linux, Windows)."),
    /* Verrous. */
    TK_PAIR("tkVerrMaj", "tkCapsLock", OWL_TK_CAPSLOCK,
            "Touche Verrouillage majuscules.", "Caps Lock key."),
    TK_PAIR("tkVerrNum", "tkNumLock", OWL_TK_NUMLOCK,
            "Touche Verrouillage numerique.", "Num Lock key."),
    TK_PAIR("tkArr\xc3\xaatD\xc3\xa9""fil", "tkScrollLock", OWL_TK_SCROLLLOCK,
            "Touche Arret defilement.", "Scroll Lock key."),
    /* Systeme. */
    TK_PAIR("tkImpr\xc3\x89""cran", "tkPrintScreen", OWL_TK_PRINT,
            "Touche Impression ecran.", "Print Screen key."),
    /* tkPause : meme forme FR/EN. */
    TK_SAME("tkPause", OWL_TK_PAUSE, "Touche Pause.", "Pause key."),
    TK_PAIR("tkMenuContextuel", "tkContextMenu", OWL_TK_MENU,
            "Touche Menu contextuel.", "Context menu key."),
    /* Touches de fonction F13..F24 (meme forme FR/EN). */
    TK_SAME("tkF13", OWL_TK_F13, "Touche de fonction F13.", "Function key F13."),
    TK_SAME("tkF14", OWL_TK_F14, "Touche de fonction F14.", "Function key F14."),
    TK_SAME("tkF15", OWL_TK_F15, "Touche de fonction F15.", "Function key F15."),
    TK_SAME("tkF16", OWL_TK_F16, "Touche de fonction F16.", "Function key F16."),
    TK_SAME("tkF17", OWL_TK_F17, "Touche de fonction F17.", "Function key F17."),
    TK_SAME("tkF18", OWL_TK_F18, "Touche de fonction F18.", "Function key F18."),
    TK_SAME("tkF19", OWL_TK_F19, "Touche de fonction F19.", "Function key F19."),
    TK_SAME("tkF20", OWL_TK_F20, "Touche de fonction F20.", "Function key F20."),
    TK_SAME("tkF21", OWL_TK_F21, "Touche de fonction F21.", "Function key F21."),
    TK_SAME("tkF22", OWL_TK_F22, "Touche de fonction F22.", "Function key F22."),
    TK_SAME("tkF23", OWL_TK_F23, "Touche de fonction F23.", "Function key F23."),
    TK_SAME("tkF24", OWL_TK_F24, "Touche de fonction F24.", "Function key F24."),

    /* Constantes te* de controle (precedent canon ToucheEnfoncee).
     * Memes valeurs que les tk* modificateurs : ce sont les
     * memes touches physiques, exposees sous le nom historique te*. */
    { .name = "teShift", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_SHIFT),
      .descFR = "Touche Majuscule (Shift), pour ToucheEnfoncee.",
      .descEN = "Shift key, for KeyPressed.", .nameEN = "teShift" },
    { .name = "teControl", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_CONTROL),
      .descFR = "Touche Controle (Ctrl), pour ToucheEnfoncee.",
      .descEN = "Control key, for KeyPressed.", .nameEN = "teControl" },
    { .name = "teAlt", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_ALT),
      .descFR = "Touche Alt, pour ToucheEnfoncee.",
      .descEN = "Alt key, for KeyPressed.", .nameEN = "teAlt" },
    { .name = "teEchap", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_ESCAPE),
      .descFR = "Touche Echap, pour ToucheEnfoncee.",
      .descEN = "Escape key, for KeyPressed.", .nameEN = "teEsc" },
    /* Boutons souris. Prefixe te garde ; suffixe traduit. */
    { .name = "teBoutonG", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_MOUSE_LEFT),
      .descFR = "Bouton gauche de la souris, pour ToucheEnfoncee.",
      .descEN = "Left mouse button, for KeyPressed.", .nameEN = "teLeftButton" },
    { .name = "teBoutonD", .category = "Clavier", .value = INTEGER_VAL(OWL_TK_MOUSE_RIGHT),
      .descFR = "Bouton droit de la souris, pour ToucheEnfoncee.",
      .descEN = "Right mouse button, for KeyPressed.", .nameEN = "teRightButton" },
};
#undef TK_PAIR

/* Enum `touche` : regroupe les constantes clavier tk et te en une famille
 * explorable (owl explain touche, list enums --cat Clavier) et permet de
 * declarer allowedBase sur les params clavier (EnvoieTouche, ToucheEnfoncee).
 *
 * Les constantes tk et te RESTENT des constantes nues (categorie Clavier) :
 * l'enum ne stocke que des references de noms. `CAS tkHaut`,
 * `EnvoieTouche(tkHaut)` restent inchanges.
 *
 * force_small_int : les valeurs sont des codes Qt::Key positifs (0x01000000+)
 * portant une semantique runtime (passees telles quelles au backend clavier),
 * donc hors zone sentinel. Opt-out explicite de la regle 20.
 *
 * combinable=false : on teste/injecte une touche a la fois (pas de bitmask). */
static const char* const E_touche_values[] = {
    /* Directionnelles. */
    "tkHaut", "tkBas", "tkGauche", "tkDroite",
    /* Controle de flux. */
    "tkEspace", "tkEchap", "tkEntr\xc3\xa9""e", "tkTab",
    /* Edition / navigation. */
    "tkRetourArri\xc3\xa8re", "tkSuppr", "tkD\xc3\xa9""but", "tkFin",
    "tkPagePr\xc3\xa9""c", "tkPageSuiv", "tkIns\xc3\xa9r",
    /* Modificateurs. */
    "tkMaj", "tkCtrl", "tkAlt", "tkMeta",
    /* Verrous. */
    "tkVerrMaj", "tkVerrNum", "tkArr\xc3\xaatD\xc3\xa9""fil",
    /* Systeme. */
    "tkImpr\xc3\x89""cran", "tkPause", "tkMenuContextuel",
    /* Fonctions F1..F24. */
    "tkF1", "tkF2", "tkF3", "tkF4", "tkF5", "tkF6", "tkF7", "tkF8",
    "tkF9", "tkF10", "tkF11", "tkF12", "tkF13", "tkF14", "tkF15", "tkF16",
    "tkF17", "tkF18", "tkF19", "tkF20", "tkF21", "tkF22", "tkF23", "tkF24",
    /* te* de controle (canon ToucheEnfoncee) + boutons souris. */
    "teShift", "teControl", "teAlt", "teEchap",
    "teBoutonG", "teBoutonD",
    NULL
};
static const OwlEnum E_touche = {
    .name="touche", .nameEN="key", .category="Clavier",
    .descFR="Touches du clavier et boutons souris (constantes tk*/te*). "
            "Une touche textuelle s'exprime par Asc(\"X\").",
    .descEN="Keyboard keys and mouse buttons (tk*/te* constants). "
            "A textual key is expressed with Asc(\"X\").",
    .values=E_touche_values, .combinable=false, .force_small_int=true
};
static const OwlEnum* const UI_KEY_ENUMS[] = { &E_touche };

static const OwlConstDef COPIE_CONSTS[] = {
    { .name = "themeD\xc3\xa9" "faut", .category = "Image",
      .value = INTEGER_VAL(OWL_THEME_DEFAUT),
      .descFR = "dCopieImageFenêtre/Champ/Ecran : thème par défaut (palette OS courante).",
      .descEN = "dCopyWindowImage/FieldImage/ScreenImage: default theme (current OS palette).",
      .nameEN = "themeDefault" },
    { .name = "themeClair", .category = "Image",
      .value = INTEGER_VAL(OWL_THEME_CLAIR),
      .descFR = "dCopieImageFenêtre/Champ/Ecran : capture en thème clair.",
      .descEN = "dCopyWindowImage/FieldImage/ScreenImage: capture in light theme.",
      .nameEN = "themeLight" },
    { .name = "themeSombre", .category = "Image",
      .value = INTEGER_VAL(OWL_THEME_SOMBRE),
      .descFR = "dCopieImageFenêtre/Champ/Ecran : capture en thème sombre.",
      .descEN = "dCopyWindowImage/FieldImage/ScreenImage: capture in dark theme.",
      .nameEN = "themeDark" },
};

/* Params / signatures dCopieImage* (Lot 2). */
static const OwlParam p_copie_fen[] = {
    {"fenêtre", "tout",   "Fenêtre source (FEN_X) ou nom de fenêtre (chaîne)"},
    {"thème",   "entier", "Thème de capture (themeDefaut|themeClair|themeSombre)",
                          .allowedBase=&E_ThemeSnapshot},
    {0}
};
static const OwlParam p_copie_champ[] = {
    {"champ", "tout",   "Champ source (référence FEN_X.CTRL_Y ou directe)"},
    {"thème", "entier", "Thème de capture (themeDefaut|themeClair|themeSombre)",
                        .allowedBase=&E_ThemeSnapshot},
    {0}
};
static const OwlParam p_copie_ecran[] = {
    {"thème", "entier", "Thème de capture (themeDefaut|themeClair|themeSombre)",
                        .allowedBase=&E_ThemeSnapshot},
    {0}
};
static const OwlSignature sig_copie_fen[]   = {
    { 1, p_copie_fen, "Image", "image capturée (Valide=Faux si fenêtre non ouverte)"},
    { 2, p_copie_fen, "Image", "+ Thème de capture"},
    {0}
};
static const OwlSignature sig_copie_champ[] = {
    { 1, p_copie_champ, "Image", "image capturée (Valide=Faux si champ non monté)"},
    { 2, p_copie_champ, "Image", "+ Thème de capture"},
    {0}
};
static const OwlSignature sig_copie_ecran[] = {
    { 0, p_copie_ecran, "Image", "image capturée du desktop (Valide=Faux en headless ou si screen indisponible)"},
    { 1, p_copie_ecran, "Image", "+ Thème de capture"},
    {0}
};

/* ====================================================================
 *   — controle taille / etat fenetre par programme.
 *
 *  FenTaille / WinSize : resize, deplacement, lecture/ecriture du mode
 *  d'affichage. Raccourcis Maximise / Iconise / Restaure.
 *
 *  Source de verite = le store d'instance fenetre (lu par FEN_X..Largeur
 *  etc.), avec reflet Qt live quand un widget est monte (window_set_prop_qt
 *  pour la geometrie, owl_ui_window_set_display_mode pour l'etat). En mode
 *  headless (owl test, TestModeHeadless(Vrai)) il n'y a pas de widget : seul
 *  le store est mis a jour, ce qui suffit aux lectures.
 * ==================================================================== */

/* Modes d'affichage : valeurs observables (retour de FenTaille en lecture). */
#define OWL_FENTAILLE_NORMALE 0
#define OWL_FENTAILLE_MINI    1
#define OWL_FENTAILLE_MAXI    2


static const char* const E_FenTaille_values[] = {
    "TailleNormale", "TailleMini", "TailleMaxi", NULL
};
static const OwlEnum E_FenTaille = {
    .name="modeTaille", .nameAscii="modeTaille", .nameEN="sizeMode",
    .category="UI",
    .descFR="Mode d'affichage d'une fenetre : normal, reduit (icone) ou agrandi.",
    .descEN="Window display mode: normal, minimized (icon) or maximized.",
    .values=E_FenTaille_values, .combinable=false, .force_small_int=true
};

#define C_FENTAILLE(nm, val, dfr, den, en) {nm, "UI", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef FENTAILLE_CONSTS[] = {
    C_FENTAILLE("TailleNormale", OWL_FENTAILLE_NORMALE,
        "Affiche la fenetre a sa taille normale (ni reduite, ni agrandie).",
        "Shows the window at its normal size (neither minimized nor maximized).",
        "normalSize"),
    C_FENTAILLE("TailleMini", OWL_FENTAILLE_MINI,
        "Reduit la fenetre sous forme d'icone.",
        "Minimizes the window to an icon.",
        "minSize"),
    C_FENTAILLE("TailleMaxi", OWL_FENTAILLE_MAXI,
        "Agrandit la fenetre a sa taille maximale.",
        "Maximizes the window to its largest size.",
        "maxSize"),
};

/* Resout la fenetre cible : "" ou echec de resolution => fenetre active. */

/* Met a jour un scalaire geometrie dans le store + reflet Qt live. */

/* Memorise le mode d'affichage dans le store + applique a Qt si monte. */



/*  : TestFenetreIconeLargeur(FEN_X) — observabilite test de l'icone
 * de fenetre. Retourne la largeur (px) de la plus grande taille disponible de
 * l'icone Qt courante : 0 si aucune icone, -1 si widget non monte (headless).
 * Permet de distinguer "icone posee" d'un "no-op silencieux" sur un widget
 * reel (TestModeHeadless(Faux) + OuvreSoeur). */

/* Raccourcis d'etat : Maximise / Iconise / Restaure ([fen]). */

static const OwlParam p_fentaille_lire[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou \"\" pour la fenetre active. Omise : fenetre active."},
    {0}
};
static const OwlParam p_fentaille_mode[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou \"\" pour la fenetre active."},
    {"mode", "entier", "Mode d'affichage (TailleNormale | TailleMini | TailleMaxi).",
     .allowedBase=&E_FenTaille},
    {0}
};
static const OwlParam p_fentaille_taille[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou \"\" pour la fenetre active."},
    {"largeur", "entier", "Nouvelle largeur en pixels."},
    {"hauteur", "entier", "Nouvelle hauteur en pixels."},
    {0}
};
static const OwlParam p_fentaille_pos_taille[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou \"\" pour la fenetre active."},
    {"x", "entier", "Nouvelle abscisse du coin haut-gauche en pixels."},
    {"y", "entier", "Nouvelle ordonnee du coin haut-gauche en pixels."},
    {"largeur", "entier", "Nouvelle largeur en pixels."},
    {"hauteur", "entier", "Nouvelle hauteur en pixels."},
    {0}
};
static const OwlParam p_test_fen_icone_largeur[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou omise pour la fenetre active."},
    {0}
};
static const OwlSignature sig_test_fen_icone_largeur[] = {
    {0, p_test_fen_icone_largeur, "entier", "Largeur de l'icone de la fenetre active"},
    {1, p_test_fen_icone_largeur, "entier", "Largeur de l'icone de la fenetre"},
    {0}
};
static const OwlSignature sig_fentaille[] = {
    {0, p_fentaille_lire,       "entier", "Mode d'affichage courant de la fenetre active"},
    {1, p_fentaille_lire,       "entier", "Mode d'affichage courant de la fenetre"},
    {2, p_fentaille_mode,       "rien",   "Applique le mode d'affichage a la fenetre"},
    {3, p_fentaille_taille,     "rien",   "Redimensionne la fenetre"},
    {5, p_fentaille_pos_taille, "rien",   "Deplace et redimensionne la fenetre"},
    {0}
};

static const OwlParam p_fen_etat[] = {
    {"fenetre", "tout", "Fenetre cible (FEN_X) ou \"\" pour la fenetre active. Omise : fenetre active."},
    {0}
};
static const OwlSignature sig_fen_etat[] = {
    {0, p_fen_etat, "rien", "Sans fenetre cible : agit sur la fenetre active"},
    {1, p_fen_etat, "rien", "Agit sur la fenetre indiquee"},
    {0}
};

static OwlNativeDef DEFS[] = {
    { .canonical="Info",          .nameEN="Info",
      .category="UI", .descFR="Boite d'information modale.",
      .descEN="Modal info dialog.",
      .signatures=sig_msg },
    { .canonical="Erreur",        .nameEN="Error",
      .category="UI", .descFR="Boite d'erreur modale.",
      .descEN="Modal error dialog.",
      .signatures=sig_msg },
    { .canonical="Avertissement", .nameEN="Warning",
      .category="UI", .descFR="Boite d'avertissement modale.",
      .descEN="Modal warning dialog.",
      .signatures=sig_msg },
    { .canonical="Sablier", .nameEN="HourGlass",
      .category="UI",
      .descFR="Transforme le curseur de la souris en curseur d'attente (sablier) pour signaler qu'un traitement long est en cours, et inversement. Sans argument ou avec Vrai : active le sablier. Avec Faux : restitue le curseur precedent. Les activations s'empilent : autant de restitutions que d'activations sont necessaires pour revenir au curseur d'origine.",
      .descEN="Turns the mouse cursor into a wait cursor (hourglass) to signal a long-running task, and back. With no argument or True: shows the hourglass. With False: restores the previous cursor. Activations stack: as many restorations as activations are needed to return to the original cursor.",
      .signatures=sig_sablier },
    { .canonical="ToastAffiche", .nameEN="ToastDisplay",
      .category="UI",
      .descFR="Affiche un message ephemere (toast) non bloquant, fade-out automatique. Cadrage et couleur de fond optionnels.",
      .descEN="Show a non-blocking ephemeral message (toast) with automatic fade-out. Optional alignment and background color.",
      .signatures=sig_toast },
    { .canonical="OuiNon",        .nameEN="YesNo",
      .category="UI",
      .descFR="Dialogue Oui/Non. Retour entier (Oui=1, Non=0).",
      .descEN="Yes/No dialog. Returns integer (Yes=1, No=0).",
      .signatures=sig_choice },
    { .canonical="Confirmer",     .nameEN="Confirm",
      .category="UI",
      .descFR="Dialogue Oui/Non/Annuler. Retour entier (Non=0, Oui=1, Annuler=2).",
      .descEN="Yes/No/Cancel dialog. Returns integer (No=0, Yes=1, Cancel=2).",
      .signatures=sig_choice },
    { .canonical="fS\xc3\xa9lecteur", .canonicalAscii="fSelecteur", .nameEN="fSelect",
      .category="Fichiers",
      .descFR="Ouvre la boite de dialogue systeme de selection de fichier. Retourne le chemin complet du fichier choisi, ou \"\" si l'utilisateur annule. En mode fselMulti, les chemins choisis sont joints par un retour chariot.",
      .descEN="Opens the system file selection dialog. Returns the full path of the chosen file, or \"\" if the user cancels. In fselMulti mode, the chosen paths are joined by a carriage return.",
      .signatures=sig_fsel },
    { .canonical="fRepS\xc3\xa9lecteur", .canonicalAscii="fRepSelecteur", .nameEN="fSelectDir",
      .category="Fichiers",
      .descFR="Ouvre la boite de dialogue systeme de selection de repertoire. Retourne le chemin du repertoire choisi, ou \"\" si l'utilisateur annule.",
      .descEN="Opens the system directory selection dialog. Returns the path of the chosen directory, or \"\" if the user cancels.",
      .signatures=sig_frepsel },
    { .canonical="TestModeHeadless", .nameEN="TestHeadlessMode",
      .category="Test",
      .descFR="Active ou desactive le mode headless. En mode headless, les dialogs (Info, Erreur, Confirmer, ...) et Ouvre() court-circuitent l'affichage reel et retournent un fallback silencieux (Null ou bouton par defaut). Appel sans argument : retourne l'etat courant. Par defaut : ON sous owl test (reset au debut de chaque fichier), OFF en GUI normale.",
      .descEN="Enables or disables headless mode. When headless, dialogs (Info, Error, Confirm, ...) and Open() bypass actual display and return a silent fallback (Null or default button). Call with no argument: returns the current state. Defaults: ON under owl test (reset at the start of each file), OFF in normal GUI mode.",
      .signatures=sig_test_headless },
    { .canonical="TestSimuleSouris", .nameEN="TestSimulateMouse",
      .category="Test",
      .descFR="Declenche une action souris sur un champ UI. Forme 2-args : clic/double-clic (gauche ou droit), sans coordonnees ; en mode graphique, si le champ est affiche, un vrai clic est envoye au champ et emprunte donc le meme chemin qu'une interaction utilisateur. Forme 4-args : actions a coordonnees BoutonGauche/BoutonDroit/SurvolSouris et molette MoletteHaut/MoletteBas (mouvement +1/-1 derive de l'enum) ; ces variantes appellent directement le callback, la position et le mouvement transmis n'etant pas reproductibles par un clic. En mode headless, ou si le champ n'est pas affiche, le callback est appele directement. Si le callback cible n'est pas declare : no-op silencieux.",
      .descEN="Triggers a mouse action on a UI control. 2-arg form: click/double-click (left or right), no coords; in graphical mode, if the control is displayed, a real click is sent to it and therefore follows the same path as a user interaction. 4-arg form: coordinate-based BoutonGauche/BoutonDroit/SurvolSouris and wheel MoletteHaut/MoletteBas (movement +1/-1 derived from the enum); these variants invoke the callback directly, since the position and movement they carry cannot be reproduced by a click. In headless mode, or when the control is not displayed, the callback is invoked directly. If the target callback is missing: silent no-op.",
      .signatures=sig_test_simule_souris },
    { .canonical="D\xc3\xa9""clenche\xc3\x89v\xc3\xa9nement", .canonicalAscii="DeclencheEvenement",
      .nameEN="TriggerEvent", .category="UI",
      .descFR="Declenche par code le handler d'un evenement d'un champ, sans interaction utilisateur. La cible est le champ en objet (BTN_x) ou son nom en chaine (indirection). L'evenement est designe par une constante evt* (evtClic, evtAChaqueModification, evtToucheAppuyee...). Les parametres supplementaires sont transmis au handler selon la signature de l'evenement. Marche en headless comme en GUI. Si le handler cible n'est pas declare : no-op silencieux.",
      .descEN="Triggers a control event's handler by code, without user interaction. The target is the control as an object (BTN_x) or its name as a string (indirection). The event is given by an evt* constant (evtClick, evtEachChange, evtKeyDown...). Extra parameters are passed to the handler according to the event signature. Works in headless or GUI mode. If the target handler is missing: silent no-op.",
      .signatures=sig_declenche_evenement },
    { .canonical="TestSimuleSaisie", .nameEN="TestSimulateInput",
      .category="Test",
      .descFR="Ecrit un texte dans la propriete ..Valeur d'un champ de saisie et declenche le callback Modification du champ. Marche en headless comme en GUI.",
      .descEN="Writes a text into an input control's ..Value property and triggers its Modification callback. Works in headless or GUI mode.",
      .signatures=sig_test_simule_saisie },
    { .canonical="TestSimuleMenu", .nameEN="TestSimulateMenu",
      .category="Test",
      .descFR="Declenche par code le callback surSelection d'une option de menu, sans interaction reelle, comme un clic sur cette option. L'option est designee par son nom (ex. \"OPT_Nouveau\") et recherchee dans l'arbre du menu principal ou des menus contextuels de la fenetre. Une 3e valeur optionnelle (colonne d'un Champ Table, forme TABLE.COL_X) fixe le contexte d'un menu de loupe : elle devient MoiMeme dans le surSelection declenche. Marche en mode headless comme en GUI. Erreur detectee si l'option est introuvable ou si la fenetre n'a pas de menu affecte.",
      .descEN="Triggers a menu option's onSelection callback by code, without real interaction, like a click on that option. The option is given by its name (e.g. \"OPT_Nouveau\") and searched throughout the window's main menu or context menu trees. An optional 3rd value (a Table column, form TABLE.COL_X) sets a magnifier menu's context: it becomes MySelf inside the triggered onSelection. Works in headless or GUI mode. Error is raised if the option is not found or if the window has no assigned menu.",
      .signatures=sig_test_simule_menu },
    { .canonical="TestFenetreIconeLargeur", .nameEN="TestWindowIconWidth",
      .category="Test",
      .descFR="Retourne la largeur en pixels de la plus grande taille disponible de l'icone de la fenetre : 0 si aucune icone, -1 si la fenetre n'a pas de champ monte. Sert a verifier qu'une icone a bien ete appliquee sur un champ reel (TestModeHeadless(Faux)).",
      .descEN="Returns the width in pixels of the largest available size of the window icon: 0 if no icon, -1 if the window has no mounted control. Used to check that an icon was actually applied on a real control (TestHeadlessMode(False)).",
      .signatures=sig_test_fen_icone_largeur },
    { .canonical="ChampsDe",      .nameEN="ControlsOf",
      .category="UI",
      .descFR="Renvoie le tableau des noms des champs directs d'une fenetre ou d'un champ conteneur, dans l'ordre de creation. Les champs internes a un conteneur ne figurent pas dans la liste de la fenetre : ils apparaissent dans la liste du conteneur lui-meme. Un parent sans champ direct renvoie un tableau vide. Chaque nom obtenu s'exploite ensuite par indirection scopee ({nom, indChamp}..Propriete).",
      .descEN="Returns the array of the direct control names of a window or a container control, in creation order. Controls nested inside a container are not part of the window's list: they appear in the container's own list. A parent with no direct control returns an empty array. Each returned name can then be used through scoped indirection ({name, indControl}..Property).",
      .signatures=sig_champs_de },
    { .canonical="DonneFocus",    .nameEN="SetFocus",
      .category="UI",
      .descFR="Donne le focus clavier a un champ de saisie. Le champ peut etre une reference (SAI_X), une forme qualifiee (FEN_X.SAI_Y) ou un nom en chaine. Sans argument : le focus revient au champ en cours. Declenche surSortie sur le champ quitte puis surEntree sur le champ vise (aucun evenement si le champ est deja le champ en cours). A l'ouverture d'une fenetre, le premier champ saisissable recoit le focus automatiquement.",
      .descEN="Gives keyboard focus to an input control. The control may be a reference (SAI_X), a qualified form (FEN_X.SAI_Y) or a name string. With no argument: focus returns to the current control. Triggers onExit on the control being left then onEntry on the target (no event if it is already the current control). When a window opens, the first input control receives focus automatically.",
      .signatures=sig_donne_focus },
    { .canonical="__DebugFeuilleStyle", .canonicalAscii="__DebugFeuilleStyle",
      .nameEN="__DebugStyleSheet", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Feuille de style courante d'un champ (API interne de test).",
      .descEN="Current style sheet of a control (internal test API).",
      .signatures=sig_debug_feuille_style },
    { .canonical="__DebugFeuilleStyleMenu", .canonicalAscii="__DebugFeuilleStyleMenu",
      .nameEN="__DebugMenuStyleSheet", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Feuille de style courante de la barre de menu (API interne de test).",
      .descEN="Current style sheet of the menu bar (internal test API).",
      .signatures=sig_debug_feuille_style_menu },
    { .canonical="__DebugFeuilleStyleMenuContextuel",
      .canonicalAscii="__DebugFeuilleStyleMenuContextuel",
      .nameEN="__DebugContextMenuStyleSheet", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Feuille de style du panneau d'un menu contextuel de la fenetre courante (API interne de test).",
      .descEN="Style sheet of a context menu panel of the current window (internal test API).",
      .signatures=sig_debug_feuille_style_menu_ctx },
    { .canonical="__DebugPoliceBarreMenu", .canonicalAscii="__DebugPoliceBarreMenu",
      .nameEN="__DebugMenuBarFont", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Police effective de la barre de menu, descripteur famille|taille|gras|italique (API interne de test).",
      .descEN="Effective menu bar font, descriptor family|size|bold|italic (internal test API).",
      .signatures=sig_debug_police_barre_menu },
    { .canonical="__DebugPoliceSousMenu", .canonicalAscii="__DebugPoliceSousMenu",
      .nameEN="__DebugMenuSubmenuFont", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Police effective d'un deroulant de menu, descripteur famille|taille|gras|italique (API interne de test).",
      .descEN="Effective drop-down menu font, descriptor family|size|bold|italic (internal test API).",
      .signatures=sig_debug_police_sousmenu },
    { .canonical="__DebugPoliceChamp", .canonicalAscii="__DebugPoliceChamp",
      .nameEN="__DebugControlFont", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Police effective d'un champ, descripteur famille|taille|gras|italique (API interne de test).",
      .descEN="Effective control font, descriptor family|size|bold|italic (internal test API).",
      .signatures=sig_debug_police_champ },
    { .canonical="__DebugEncreBarreMenu", .canonicalAscii="__DebugEncreBarreMenu",
      .nameEN="__DebugMenuBarInk", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Hauteur d'encre peinte (px) du texte de la barre de menu, mesure du rendu reel (API interne de test).",
      .descEN="Painted ink height (px) of the menu bar text, measured from the real rendering (internal test API).",
      .signatures=sig_debug_encre_barre_menu },
    { .canonical="__DebugDernierMessage", .canonicalAscii="__DebugDernierMessage",
      .nameEN="__DebugLastMessage", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Dernier message compose par une boite de message (API interne de test).",
      .descEN="Last message composed by a message box (internal test API).",
      .signatures=sig_debug_dernier_message },
    { .canonical="__CaptureFen\xc3\xaatre", .canonicalAscii="__CaptureFenetre",
      .nameEN="__CaptureWindow", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Capture une fenetre ouverte vers un fichier PNG, offscreen (API interne de polissage visuel). La fenetre doit etre ouverte avant l'appel.",
      .descEN="Captures an open window to a PNG file, offscreen (internal visual-polish API). The window must be open before the call.",
      .signatures=sig_capture_fenetre },
    { .canonical="__CaptureSousMenu", .canonicalAscii="__CaptureSousMenu",
      .nameEN="__CaptureSubmenu", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Capture le panneau deroule d'un sous-menu nomme vers un fichier PNG, offscreen, en preservant l'alpha (API interne de polissage visuel).",
      .descEN="Captures the drop-down panel of a named submenu to a PNG file, offscreen, preserving alpha (internal visual-polish API).",
      .signatures=sig_capture_sousmenu },
    { .canonical="__DebugMenuIc\xc3\xb4ne", .canonicalAscii="__DebugMenuIcone",
      .nameEN="__DebugMenuIcon", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Indique si une option de menu porte une icone effectivement resolue (API interne de test).",
      .descEN="Whether a menu option carries an effectively resolved icon (internal test API).",
      .signatures=sig_debug_menu_icone },
    { .canonical="__DebugTableCelluleEditable", .canonicalAscii="__DebugTableCelluleEditable",
      .nameEN="__DebugTableCellEditable", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Indique si une cellule de Table s'ouvrirait en édition au double-clic (API interne de test).",
      .descEN="Whether a table cell would open for editing on double-click (internal test API).",
      .signatures=sig_debug_table_cellule_editable },
    { .canonical="__DebugTableCelluleMasque", .canonicalAscii="__DebugTableCelluleMasque",
      .nameEN="__DebugTableCellMask", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Masque de saisie effectif de l'éditeur d'une cellule de Table (API interne de test).",
      .descEN="Effective input mask of a table cell editor (internal test API).",
      .signatures=sig_debug_table_cellule_masque },
    { .canonical="__DebugTableCelluleValideSaisie", .canonicalAscii="__DebugTableCelluleValideSaisie",
      .nameEN="__DebugTableCellValidatesInput", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Indique si le validateur de l'éditeur d'une cellule de Table accepte un texte donné (API interne de test).",
      .descEN="Whether a table cell editor's validator accepts a given text (internal test API).",
      .signatures=sig_debug_table_cellule_valide_saisie },
    { .canonical="__DebugTableCelluleTexteAffiche", .canonicalAscii="__DebugTableCelluleTexteAffiche",
      .nameEN="__DebugTableCellDisplayedText", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Texte effectivement affiché par une cellule de Table hors édition (API interne de test).",
      .descEN="Text actually displayed by a table cell outside editing (internal test API).",
      .signatures=sig_debug_table_cellule_texte_affiche },
    { .canonical="__DebugTableFiltreIN", .canonicalAscii="__DebugTableFiltreIN",
      .nameEN="__DebugTableFilterIN", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Pose un filtre multi-valeurs sur une colonne de Table via le chemin de filtrage réel (API interne de test).",
      .descEN="Sets a multi-value filter on a table column through the real filtering path (internal test API).",
      .signatures=sig_debug_table_filtre_in },
    { .canonical="__DebugTableColonneVisible", .canonicalAscii="__DebugTableColonneVisible",
      .nameEN="__DebugTableColumnVisible", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Valeurs d'une colonne pour les lignes non masquées de la Table montée (API interne de test).",
      .descEN="Column values for the non-hidden rows of the mounted table (internal test API).",
      .signatures=sig_debug_table_colonne_visible },
    { .canonical="__DebugTableValeursDistinctes", .canonicalAscii="__DebugTableValeursDistinctes",
      .nameEN="__DebugTableDistinctValues", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Valeurs distinctes d'une colonne (ordre de 1ère apparition), vides au-delà du seuil de cardinalité (API interne de test).",
      .descEN="Distinct values of a column (first-appearance order), empty beyond the cardinality threshold (internal test API).",
      .signatures=sig_debug_table_valeurs_distinctes },
    { .canonical="__DebugTableFiltreType", .canonicalAscii="__DebugTableFiltreType",
      .nameEN="__DebugTableFilterType", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Type du filtre actif sur une colonne de Table (constante filtre*, 0 si aucun) via le pont réel (API interne de test).",
      .descEN="Active filter type on a table column (filter* constant, 0 if none) through the real bridge (internal test API).",
      .signatures=sig_debug_table_filtre_type },
    { .canonical="__DebugPositionImageBouton", .canonicalAscii="__DebugPositionImageBouton",
      .nameEN="__DebugButtonImagePosition", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Position d'image appliquee a un bouton (API interne de test).",
      .descEN="Applied image position of a button (internal test API).",
      .signatures=sig_debug_position_image_bouton },
    { .canonical="__DebugBoutonIconeSurvol", .canonicalAscii="__DebugBoutonIconeSurvol",
      .nameEN="__DebugButtonHoverIcon", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Indique si une icone de survol est armee sur un bouton (API interne de test).",
      .descEN="Whether a hover icon is armed on a button (internal test API).",
      .signatures=sig_debug_bouton_icone_survol },
    { .canonical="__DebugOmbreFlou", .canonicalAscii="__DebugOmbreFlou",
      .nameEN="__DebugShadowBlur", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Rayon de flou de l'ombre portee courante d'un champ (API interne de test).",
      .descEN="Blur radius of a control's current drop shadow (internal test API).",
      .signatures=sig_debug_ombre_flou },
    { .canonical="__SimuleInteraction", .canonicalAscii="__SimuleInteraction",
      .nameEN="__SimulateInteraction", .category="Test", .flags=OWL_NAT_INTERNAL,
      .descFR="Pousse un evenement souris (survol/quitte/appui/relache) sur un champ pour exercer l'ombre dynamique (API interne de test).",
      .descEN="Pushes a mouse event (enter/leave/press/release) on a control to exercise dynamic shadow (internal test API).",
      .signatures=sig_simule_interaction },
    { .canonical="ToucheEnfonc\xc3\xa9" "e", .canonicalAscii="ToucheEnfoncee",
      .nameEN="KeyPressed",
      .category="UI",
      .descFR="Indique si une touche du clavier est physiquement enfoncee a l'instant de l'appel. La touche est designee par une constante tk*/te* ou un code ASCII (Asc(\"X\")). Renvoie Faux hors interaction utilisateur reelle (notamment sous test automatise). Un code hors-domaine renvoie Faux sans declencher d'erreur.",
      .descEN="Tells whether a keyboard key is physically held down at call time. The key is given as a tk*/te* constant or an ASCII code (Asc(\"X\")). Returns False outside real user interaction (notably under automated tests). An out-of-range code returns False without raising an error.",
      .signatures=sig_touche_enfoncee },
    { .canonical="EnvoieTouche", .nameEN="SendKey",
      .category="UI",
      .descFR="Injecte une frappe clavier complete (appui puis relachement) vers la fenetre active ou le champ qui a le focus. La touche est designee par une constante tk* ou un code ASCII (Asc(\"X\")). Permet de piloter par programme les touches sans caractere textuel (fleches, Echap, Entree). Si le champ focuse possede un handler surToucheAppuyee, il recoit la touche ; sinon elle remonte a la fenetre. Sans fenetre ouverte : sans effet, sans erreur.",
      .descEN="Injects a complete key stroke (press then release) toward the active window or the focused control. The key is given as a tk* constant or an ASCII code (Asc(\"X\")). Lets a program drive non-textual keys (arrows, Esc, Enter). If the focused control has an onKeyDown handler, it receives the key; otherwise it bubbles up to the window. With no open window: no effect, no error.",
      .signatures=sig_envoie_touche },
    { .canonical="Ouvre",         .nameEN="Open",
      .category="UI",
      .descFR="Ouvre une fenetre modale.",
      .descEN="Open a modal window.",
      .signatures=sig_ouvre },
    { .canonical="OuvreFille",    .nameEN="OpenChild",
      .category="UI",
      .descFR="Ouvre une fenetre fille non-modale : la fenetre s'affiche en parallele et l'appel rend la main immediatement. Chaque appel cree une instance independante.",
      .descEN="Open a non-modal child window: it shows alongside the caller and the call returns immediately. Each call creates an independent instance.",
      .signatures=sig_ouvre },
    { .canonical="OuvreSoeur",    .nameEN="OpenSibling",
      .category="UI",
      .descFR="Ouvre une fenetre soeur non-modale : comme OuvreFille mais sans relation hierarchique avec l'appelant. Ouvrir la meme fenetre plusieurs fois cree autant d'instances independantes.",
      .descEN="Open a non-modal sibling window: like OpenChild but without a hierarchical link to the caller. Opening the same window several times creates that many independent instances.",
      .signatures=sig_ouvre },
    { .canonical="Ferme",         .nameEN="Close",
      .category="UI",
      .descFR="Ferme la fenetre courante.",
      .descEN="Close the current window.",
      .signatures=sig_close },
    { .canonical="DemandeMiseAJourIHM", .canonicalAscii="DemandeMiseAJourIHM",
      .nameEN="RequestUpdateUI",
      .category="UI",
      .descFR="Demande l'execution de l'evenement DemandeMiseAJour de la fenetre courante, pour rafraichir l'affichage apres un changement de donnees. L'evenement est execute immediatement, avant le retour de l'appel. Sans fenetre ouverte, ou si la fenetre ne declare pas cet evenement : sans effet et sans erreur.",
      .descEN="Requests execution of the current window's RequestRefresh event, to refresh the display after a data change. The event runs immediately, before the call returns. With no open window, or when the window declares no such event: no effect, no error.",
      .signatures=sig_demande_maj_ihm },
    { .canonical="FinProgramme",  .nameEN="EndProgram",
      .category="UI",
      .descFR="Termine l'application (avec message d'erreur optionnel).",
      .descEN="End the application (with optional error message).",
      .signatures=sig_fin },
    { .canonical="FenTaille",     .nameEN="WinSize",
      .category="UI",
      .descFR="Redimensionne, deplace, ou lit/fixe le mode d'affichage d'une fenetre. Sans dimensions : retourne le mode courant (TailleNormale | TailleMini | TailleMaxi). Fenetre omise ou \"\" : agit sur la fenetre active.",
      .descEN="Resizes, moves, or reads/sets the display mode of a window. Without dimensions: returns the current mode (normalSize | minSize | maxSize). Window omitted or \"\": acts on the active window.",
      .signatures=sig_fentaille },
    { .canonical="Maximise",      .nameEN="Maximize",
      .category="UI",
      .descFR="Agrandit la fenetre a sa taille maximale. Fenetre omise : fenetre active.",
      .descEN="Maximizes the window to its largest size. Window omitted: active window.",
      .signatures=sig_fen_etat },
    { .canonical="Iconise",       .nameEN="Iconize",
      .category="UI",
      .descFR="Reduit la fenetre sous forme d'icone. Fenetre omise : fenetre active.",
      .descEN="Minimizes the window to an icon. Window omitted: active window.",
      .signatures=sig_fen_etat },
    { .canonical="Restaure",      .nameEN="Restore",
      .category="UI",
      .descFR="Restaure la fenetre a sa taille normale depuis l'etat reduit ou agrandi. Fenetre omise : fenetre active.",
      .descEN="Restores the window to its normal size from the minimized or maximized state. Window omitted: active window.",
      .signatures=sig_fen_etat },

    /* Champ Table V1 — toutes les natives suivent le canon. */
    { .canonical="TableAjouteLigne", .nameEN="TableAddLine",
      .category="UI",
      .descFR="Ajoute une ligne en fin de table avec une valeur par colonne.",
      .descEN="Append a row to the table with one value per column.",
      .signatures=sig_table_add },
    { .canonical="TableSupprime", .nameEN="TableDelete",
      .category="UI",
      .descFR="Supprime la ligne specifiee (1-based).",
      .descEN="Remove the specified row (1-based).",
      .signatures=sig_table_rm },
    { .canonical="TableSupprimeTout", .nameEN="TableDeleteAll",
      .category="UI",
      .descFR="Vide la table de toutes ses lignes (conserve les colonnes).",
      .descEN="Empty all rows from the table (keeps columns).",
      .signatures=sig_table_simple },
    { .canonical="TableOccurrence", .nameEN="TableCount",
      .category="UI",
      .descFR="Nombre selon l'info (toTotal/toAffichee/toColonne/toVisible).",
      .descEN="Count by info type (toTotal/toShown/toColumn/toVisible).",
      .signatures=sig_table_occ },
    { .canonical="TableCherche", .nameEN="TableSearch",
      .category="UI",
      .descFR="Cherche une valeur dans une colonne (TABLE.COL) : indice 1-based "
              "de la premiere ligne correspondante, -1 sinon.",
      .descEN="Search a value in a column (TABLE.COL): 1-based index of the "
              "first matching row, -1 otherwise.",
      .signatures=sig_table_cherche },
    { .canonical="TableCherchePartout", .nameEN="TableSearchEverywhere",
      .category="UI",
      .descFR="Cherche une valeur dans TOUTES les colonnes d'une Table : indice "
              "1-based de la premiere ligne correspondante, -1 sinon "
              "(type par defaut : contient).",
      .descEN="Search a value across ALL columns of a Table: 1-based index of "
              "the first matching row, -1 otherwise (default mode: contains).",
      .signatures=sig_table_cherche_partout },
    { .canonical="TableModifieLigne", .nameEN="TableModifyLine",
      .category="UI",
      .descFR="Modifie les valeurs d'une ligne existante (1-based).",
      .descEN="Modify cell values of an existing row (1-based).",
      .signatures=sig_table_mod },
    { .canonical="TableEchangeLigne", .nameEN="TableSwapLine",
      .category="UI",
      .descFR="Échange deux lignes de la table identifiées par leur numéro (1-based).",
      .descEN="Swap two table rows identified by their number (1-based).",
      .signatures=sig_table_swap },
    { .canonical="TableD\xc3\xa9placeLigne", .canonicalAscii="TableDeplaceLigne",
      .nameEN="TableMoveLine",
      .category="UI",
      .descFR="Deplace une ligne avec decalage de l'intervalle. Destination = "
              "indice 1-based ou cible relative (tdSuivant, tdPremier, ...). "
              "Operation = tdDeplace (defaut), tdEchange, tdAfficheDestination "
              "(combinables). Retourne le nouvel indice de la ligne.",
      .descEN="Move a row, shifting the in-between rows. Destination = 1-based "
              "index or relative target (tdSuivant, tdPremier, ...). Operation = "
              "tdDeplace (default), tdEchange, tdAfficheDestination (combinable). "
              "Returns the row's new index.",
      .signatures=sig_table_deplace },
    { .canonical="TableSelectPlus", .nameEN="TableSelectPlus",
      .category="UI",
      .descFR="Selectionne la ligne specifiee. En multi, ajoute a la selection.",
      .descEN="Select the row. In multi-select, adds to selection.",
      .signatures=sig_table_sel },
    { .canonical="TableSelectMoins", .nameEN="TableSelectMinus",
      .category="UI",
      .descFR="De-selectionne la ligne specifiee.",
      .descEN="Deselect the row.",
      .signatures=sig_table_sel },
    { .canonical="TableSelect", .nameEN="TableSelected",
      .category="UI",
      .descFR="Indice de l'element selectionne, -1 si rien.",
      .descEN="Selected element index, -1 if none.",
      .signatures=sig_table_selN },
    { .canonical="TableSelectOccurrence", .nameEN="TableSelectCount",
      .category="UI",
      .descFR="Nombre d'elements selectionnes (multi-select).",
      .descEN="Number of selected elements (multi-select).",
      .signatures=sig_table_selOcc },
    { .canonical="TableTrie", .nameEN="TableSort",
      .category="UI",
      .descFR="Trie la table : '+NomColonne' croissant, '-NomColonne' decroissant.",
      .descEN="Sort table : '+ColumnName' ascending, '-ColumnName' descending.",
      .signatures=sig_table_sort },
    { .canonical="TableActiveFiltre", .nameEN="TableEnableFilter",
      .category="UI",
      .descFR="Active un filtre utilisateur sur une colonne (TABLE.COL, filtre*). "
              "Valeur vide retire le filtre.",
      .descEN="Enable a user filter on a column (TABLE.COL, filtre*). "
              "Empty value removes the filter.",
      .signatures=sig_table_filter },
    { .canonical="TableSaisieLoupe", .nameEN="TableInputSearch",
      .category="UI",
      .descFR="Passe une colonne (TABLE.COL) en saisie loupe, comme un clic sur "
              "l'icone loupe. Avec un operateur filtre* + valeur, applique le "
              "filtre ; avec un type recherche* (defaut), passe en mode "
              "recherche. Annulee par TableDesactiveFiltre.",
      .descEN="Switch a column (TABLE.COL) to search-icon input, as if the user "
              "clicked the magnifier. With a filtre* operator + value, applies "
              "the filter; with a recherche* type (default), enters search mode. "
              "Cancelled by TableDisableFilter.",
      .signatures=sig_table_saisie_loupe },
    { .canonical="TableD\xc3\xa9sactiveFiltre", .canonicalAscii="TableDesactiveFiltre",
      .nameEN="TableDisableFilter",
      .category="UI",
      .descFR="Retire les filtres d'une Table : tous (TABLE) ou celui d'une "
              "colonne (TABLE.COL).",
      .descEN="Remove Table filters: all of them (TABLE) or a single column's "
              "one (TABLE.COL).",
      .signatures=sig_table_disable_filter },
    { .canonical="TableColonnesFiltr\xc3\xa9""es", .canonicalAscii="TableColonnesFiltrees",
      .nameEN="TableFilteredColumn",
      .category="UI",
      .descFR="Rend les colonnes ayant un filtre actif : une entree "
              "\"<colonne>;<type>;<valeur>\" par RC, \"\" si aucune.",
      .descEN="Return columns with an active filter: one "
              "\"<column>;<type>;<value>\" entry per CR, \"\" if none.",
      .signatures=sig_table_filtered_col },
    { .canonical="TablePosition", .nameEN="TablePosition",
      .category="UI",
      .descFR="Affiche la table a partir d'une ligne, ou retourne la 1ere ligne visible.",
      .descEN="Show table from row, or return first visible row.",
      .signatures=sig_table_pos },
    { .canonical="TableInfoXY", .nameEN="TableInfoXY",
      .category="UI",
      .descFR="Info au point pixel (X,Y) : tiNumLigne/tiNumColonne/tiNomColonne.",
      .descEN="Info at pixel point (X,Y) : tiNumLine/tiNumColumn/tiNameColumn.",
      .signatures=sig_table_xy },
    { .canonical="TableVersExcel", .nameEN="TableToExcel",
      .category="UI",
      .descFR="Exporte le contenu de la table (entetes + donnees) vers un "
              "classeur Excel (.xlsx) relisable. Retourne Vrai si le fichier a "
              "ete ecrit, Faux + ErreurDetectee sur chemin invalide.",
      .descEN="Export the table content (headers + data) to a readable Excel "
              "workbook (.xlsx). Returns True if the file was written, False + "
              "ErrorDetected on an invalid path.",
      .signatures=sig_table_vers_excel,
      .methodBindings=MB_Table_VersExcel },
    { .canonical="TableVersPressePapier", .nameEN="TableToClipboard",
      .category="UI",
      .descFR="Copie le contenu de la table dans le presse-papier au format "
              "texte : entetes en premiere ligne, colonnes separees par une "
              "tabulation, lignes par un saut de ligne. Retourne Vrai si copie.",
      .descEN="Copy the table content to the clipboard as text: headers on the "
              "first line, columns separated by a tab, rows by a line feed. "
              "Returns True if copied.",
      .signatures=sig_table_vers_pp,
      .methodBindings=MB_Table_VersPressePapier },

    /* Champ Liste V1 — natives canon doc Collections (ListeXXX). */
    { .canonical="ListeAjoute", .nameEN="ListAdd",
      .category="UI",
      .descFR="Ajoute un element en fin de liste. Si Triee, repositionne automatiquement.",
      .descEN="Append an item to the list. If Sorted, auto-repositions.",
      .signatures=sig_liste_add,
      .methodBindings=MB_Liste_Ajoute },
    { .canonical="ListeIns\xc3\xa8re", .canonicalAscii="ListeInsere",
      .nameEN="ListInsert",
      .category="UI",
      .descFR="Insere un element a l'indice specifie (1-based).",
      .descEN="Insert an item at the specified 1-based index.",
      .signatures=sig_liste_insere,
      .methodBindings=MB_Liste_Insere },
    { .canonical="ListeSupprime", .nameEN="ListDelete",
      .category="UI",
      .descFR="Supprime l'element a l'indice specifie (1-based).",
      .descEN="Remove the item at the 1-based index.",
      .signatures=sig_liste_rm,
      .methodBindings=MB_Liste_Supprime },
    { .canonical="ListeSupprimeTout", .nameEN="ListDeleteAll",
      .category="UI",
      .descFR="Vide la liste de tous ses elements.",
      .descEN="Empty the list of all items.",
      .signatures=sig_liste_simple,
      .methodBindings=MB_Liste_SupprimeTout },
    { .canonical="ListeModifie", .nameEN="ListModify",
      .category="UI",
      .descFR="Modifie le texte d'un element existant (1-based).",
      .descEN="Modify the text of an existing item (1-based).",
      .signatures=sig_liste_modifie,
      .methodBindings=MB_Liste_Modifie },
    { .canonical="ListeAffiche", .nameEN="ListDisplay",
      .category="UI",
      .descFR="Force le rafraichissement de l'affichage de la liste.",
      .descEN="Force a redraw of the list display.",
      .signatures=sig_liste_simple,
      .methodBindings=MB_Liste_Affiche },
    { .canonical="ListeCherche", .nameEN="ListSeek",
      .category="UI",
      .descFR="Recherche un element par valeur. Retourne l'indice 1-based ou -1.",
      .descEN="Search for an item by value. Returns 1-based index or -1.",
      .signatures=sig_liste_cherche,
      .methodBindings=MB_Liste_Cherche },
    { .canonical="ListeOccurrence", .nameEN="ListCount",
      .category="UI",
      .descFR="Retourne le nombre d'elements dans la liste.",
      .descEN="Return the number of items in the list.",
      .signatures=sig_liste_occ,
      .methodBindings=MB_Liste_Occurrence },
    { .canonical="ListeTrie", .nameEN="ListSort",
      .category="UI",
      .descFR="Trie la liste alphabetiquement (asc par defaut).",
      .descEN="Sort the list alphabetically (asc by default).",
      .signatures=sig_liste_trie,
      .methodBindings=MB_Liste_Trie },
    { .canonical="ListeSelect", .nameEN="ListSelected",
      .category="UI",
      .descFR="Sans 2eme arg : indice 1-based de l'element selectionne (0 si rien). Avec : positionne la selection.",
      .descEN="Without 2nd arg: 1-based index of selected item (0 if none). With: positions selection.",
      .signatures=sig_liste_select,
      .methodBindings=MB_Liste_Select },
    { .canonical="ListeSelectPlus", .nameEN="ListSelectPlus",
      .category="UI",
      .descFR="Ajoute la ligne a la selection (mode multiselection). Accepte ligneSelectionnee.",
      .descEN="Add the row to the selection (multi-selection mode). Accepts ligneSelectionnee.",
      .signatures=sig_liste_sel_idx,
      .methodBindings=MB_Liste_SelectPlus },
    { .canonical="ListeSelectMoins", .nameEN="ListSelectMinus",
      .category="UI",
      .descFR="Retire la ligne de la selection (mode multiselection).",
      .descEN="Remove the row from the selection (multi-selection mode).",
      .signatures=sig_liste_sel_idx,
      .methodBindings=MB_Liste_SelectMoins },
    { .canonical="ListeSelectOccurrence", .nameEN="ListSelectCount",
      .category="UI",
      .descFR="Sans 2eme arg : nombre de lignes selectionnees. Avec rang : indice 1-based de la rang-eme ligne selectionnee.",
      .descEN="Without 2nd arg: number of selected rows. With rang: 1-based index of the rang-th selected row.",
      .signatures=sig_liste_sel_occ,
      .methodBindings=MB_Liste_SelectOccurrence },
    { .canonical="ListeD\xc3\xa9placeLigne", .canonicalAscii="ListeDeplaceLigne",
      .nameEN="ListMoveLine",
      .category="UI",
      .descFR="Deplace ou echange une ligne. Destination = indice 1-based ou cible relative (tdSuivant, tdPremier, ...). Operation = tdDeplace (defaut), tdEchange, tdAfficheDestination (combinables).",
      .descEN="Move or swap a row. Destination = 1-based index or relative target (tdSuivant, tdPremier, ...). Operation = tdDeplace (default), tdEchange, tdAfficheDestination (combinable).",
      .signatures=sig_liste_deplace,
      .methodBindings=MB_Liste_DeplaceLigne },
    { .canonical="ListeInfoXY", .nameEN="ListInfoXY",
      .category="UI",
      .descFR="Indice 1-based de la ligne au point pixel (X, Y) relatif au champ. -1 si zone vide.",
      .descEN="1-based row index at pixel point (X, Y) relative to the field. -1 if empty area.",
      .signatures=sig_liste_info_xy,
      .methodBindings=MB_Liste_InfoXY },

    /* Champ Combo V1 — natives Combo-specifiques (canon). */
    { .canonical="ComboOuvre", .nameEN="ComboOpen",
      .category="UI",
      .descFR="Ouvre la zone deroulee de la combo (force showPopup).",
      .descEN="Opens the dropdown zone of the combo (forces showPopup).",
      .signatures=sig_combo_simple,
      .methodBindings=MB_Combo_Ouvre },
    { .canonical="ComboFerme", .nameEN="ComboClose",
      .category="UI",
      .descFR="Ferme la zone deroulee de la combo (force hidePopup).",
      .descEN="Closes the dropdown zone of the combo (forces hidePopup).",
      .signatures=sig_combo_simple,
      .methodBindings=MB_Combo_Ferme },

    /* Champ Selecteur — natives canon. */
    { .canonical="S\xc3\xa9lecteurAjoute", .canonicalAscii="SelecteurAjoute",
      .nameEN="RadioButtonAdd",
      .category="UI",
      .descFR="Ajoute une option a la fin du Champ Selecteur. Retourne l'indice 1-based ajoute, -1 si erreur.",
      .descEN="Append an option to the RadioGroup control. Returns the 1-based index added, -1 on error.",
      .signatures=sig_sel_add,
      .methodBindings=MB_Sel_Ajoute },
    { .canonical="S\xc3\xa9lecteurIns\xc3\xa8re", .canonicalAscii="SelecteurInsere",
      .nameEN="RadioButtonInsert",
      .category="UI",
      .descFR="Insere une option a l'indice specifie. Retourne l'indice 1-based, -1 si erreur.",
      .descEN="Insert an option at the specified index. Returns the 1-based index, -1 on error.",
      .signatures=sig_sel_insere,
      .methodBindings=MB_Sel_Insere },
    { .canonical="S\xc3\xa9lecteurSupprime", .canonicalAscii="SelecteurSupprime",
      .nameEN="RadioButtonDelete",
      .category="UI",
      .descFR="Supprime une option du Champ Selecteur (indice 1-based).",
      .descEN="Remove an option from the Champ Selecteur (1-based index).",
      .signatures=sig_sel_rm,
      .methodBindings=MB_Sel_Supprime },
    { .canonical="S\xc3\xa9lecteurOccurrence", .canonicalAscii="SelecteurOccurrence",
      .nameEN="RadioButtonCount",
      .category="UI",
      .descFR="Nombre d'options dans le Champ Selecteur.",
      .descEN="Number of options in the Champ Selecteur.",
      .signatures=sig_sel_occ,
      .methodBindings=MB_Sel_Occurrence },

    /* Champ Interrupteur — natives canon. */
    { .canonical="InterrupteurAjoute", .nameEN="CheckBoxAdd",
      .category="UI",
      .descFR="Ajoute une option au Champ Interrupteur. Retourne l'indice 1-based, -1 si erreur.",
      .descEN="Add an option to the CheckBox group. Returns 1-based index, -1 on error.",
      .signatures=sig_int_add,
      .methodBindings=MB_Int_Ajoute },
    { .canonical="InterrupteurIns\xc3\xa8re", .canonicalAscii="InterrupteurInsere",
      .nameEN="CheckBoxInsert",
      .category="UI",
      .descFR="Insere une option a l'indice specifie.",
      .descEN="Insert an option at the specified index.",
      .signatures=sig_int_insere,
      .methodBindings=MB_Int_Insere },
    { .canonical="InterrupteurSupprime", .nameEN="CheckBoxDelete",
      .category="UI",
      .descFR="Supprime une option du Champ Interrupteur.",
      .descEN="Remove an option from the CheckBox group.",
      .signatures=sig_int_rm,
      .methodBindings=MB_Int_Supprime },
    { .canonical="InterrupteurOccurrence", .nameEN="CheckBoxCount",
      .category="UI",
      .descFR="Nombre d'options dans le Champ Interrupteur.",
      .descEN="Number of options in the CheckBox group.",
      .signatures=sig_int_occ,
      .methodBindings=MB_Int_Occurrence },

    /* Champ ZoneRepetee — natives canon. */
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eAjouteLigne",
      .canonicalAscii="ZoneRepeteeAjouteLigne",
      .nameEN="LooperAddLine",
      .category="UI",
      .descFR="Ajoute une repetition vide au Champ ZoneRepetee. Retourne "
              "l'indice 1-based de la repetition ajoutee.",
      .descEN="Append an empty repetition to the Looper field. Returns the "
              "1-based index of the new repetition.",
      .signatures=sig_zr_simple,
      .methodBindings=MB_Zr_AjouteLigne },
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eIns\xc3\xa8re",
      .canonicalAscii="ZoneRepeteeInsere",
      .nameEN="LooperInsert",
      .category="UI",
      .descFR="Insere une repetition vide a l'indice specifie. Retourne "
              "l'indice 1-based de la repetition ajoutee.",
      .descEN="Insert an empty repetition at the specified index.",
      .signatures=sig_zr_insere,
      .methodBindings=MB_Zr_Insere },
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eSupprime",
      .canonicalAscii="ZoneRepeteeSupprime",
      .nameEN="LooperDelete",
      .category="UI",
      .descFR="Supprime la repetition d'indice 1-based.",
      .descEN="Remove the repetition at the given 1-based index.",
      .signatures=sig_zr_rm,
      .methodBindings=MB_Zr_Supprime },
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eSupprimeTout",
      .canonicalAscii="ZoneRepeteeSupprimeTout",
      .nameEN="LooperDeleteAll",
      .category="UI",
      .descFR="Supprime toutes les repetitions du Champ ZoneRepetee.",
      .descEN="Removes all repetitions from the Looper field.",
      .signatures=sig_zr_void,
      .methodBindings=MB_Zr_SupprimeTout },
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eOccurrence",
      .canonicalAscii="ZoneRepeteeOccurrence",
      .nameEN="LooperCount",
      .category="UI",
      .descFR="Nombre de repetitions courantes du Champ ZoneRepetee.",
      .descEN="Current repetition count of the Looper field.",
      .signatures=sig_zr_simple,
      .methodBindings=MB_Zr_Occurrence },
    { .canonical="ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "eAffiche",
      .canonicalAscii="ZoneRepeteeAffiche",
      .nameEN="LooperDisplay",
      .category="UI",
      .descFR="Force le rafraichissement de l'affichage. Generalement no-op "
              "(le moteur d'affichage batche deja les repaints).",
      .descEN="Forces a display refresh. Usually no-op (the display engine batches repaints).",
      .signatures=sig_zr_void,
      .methodBindings=MB_Zr_Affiche },
    /*  Lot 2 — capture image fenetre / champ / ecran. */
    { .canonical="dCopieImageFen\xc3\xaatre",
      .canonicalAscii="dCopieImageFenetre",
      .nameEN="dCopyWindowImage",
      .category="Image",
      .descFR="Capture le rendu d'une fenêtre ouverte sous la forme d'une Image manipulable (sauvegardable via dSauveImagePNG, transformable comme toute autre Image). Le second paramètre optionnel sélectionne le thème de capture.",
      .descEN="Capture the rendering of an open window as a manipulable Image (saveable via dSavePNGImage, transformable like any other Image). The optional second parameter selects the capture theme.",
      .signatures=sig_copie_fen },
    { .canonical="dCopieImageChamp",
      .nameEN="dCopyFieldImage",
      .category="Image",
      .descFR="Capture le rendu d'un champ sous la forme d'une Image manipulable. Le champ doit appartenir à une fenêtre ouverte. Le second paramètre optionnel sélectionne le thème de capture.",
      .descEN="Capture the rendering of a field as a manipulable Image. The field must belong to an open window. The optional second parameter selects the capture theme.",
      .signatures=sig_copie_champ },
    { .canonical="dCopieImageEcran",
      .nameEN="dCopyScreenImage",
      .category="Image",
      .descFR="Capture l'écran entier sous la forme d'une Image manipulable. Indisponible en mode test headless (pas de display physique).",
      .descEN="Capture the entire screen as a manipulable Image. Unavailable in headless test mode (no physical display).",
      .signatures=sig_copie_ecran },
};

/* Pose l'impl d'une native UI dans DEFS, par nom canonique ou ascii.
 * Appele par stdlib_ui_impls_wire() (GUI) AVANT stdlib_ui_defs_register(). */
void stdlib_ui_defs_set_impl(const char* canonical, NativeFn fn) {
    if (canonical == NULL) return;
    int n = (int)(sizeof(DEFS) / sizeof(DEFS[0]));
    for (int i = 0; i < n; i++) {
        OwlNativeDef* d = &DEFS[i];
        if ((d->canonical && strcmp(d->canonical, canonical) == 0) ||
            (d->canonicalAscii && strcmp(d->canonicalAscii, canonical) == 0)) {
            d->impl = fn;
            return;
        }
    }
}

bool stdlib_ui_defs_contains(const char* canonical) {
    if (canonical == NULL) return false;
    int n = (int)(sizeof(DEFS) / sizeof(DEFS[0]));
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = &DEFS[i];
        if ((d->canonical && strcmp(d->canonical, canonical) == 0) ||
            (d->canonicalAscii && strcmp(d->canonicalAscii, canonical) == 0)) {
            return true;
        }
    }
    return false;
}

void stdlib_ui_defs_register(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    /* Enums + constantes UI : INCONDITIONNELS (console comme GUI) pour que la
     * validation d'appartenance d'enum des parametres resolve a l'identique.
     * Migres depuis stdlib_ui_init (ou ils etaient gates OWL_HAS_GUI). */
    owl_registry_register_enums(COPIE_ENUMS,
                                (int)(sizeof(COPIE_ENUMS)/sizeof(COPIE_ENUMS[0])));
    owl_registry_register_consts(COPIE_CONSTS,
                                 (int)(sizeof(COPIE_CONSTS)/sizeof(COPIE_CONSTS[0])));
    {
        static const OwlEnum* const FSEL_ENUMS[] = { &E_fsel };
        owl_registry_register_enums(FSEL_ENUMS,
                                    (int)(sizeof(FSEL_ENUMS)/sizeof(FSEL_ENUMS[0])));
    }
    owl_registry_register_consts(FSEL_CONSTS,
                                 (int)(sizeof(FSEL_CONSTS)/sizeof(FSEL_CONSTS[0])));
    owl_registry_register_consts(KEY_CONSTS,
                                 (int)(sizeof(KEY_CONSTS)/sizeof(KEY_CONSTS[0])));
    owl_registry_register_enums(UI_KEY_ENUMS,
                                (int)(sizeof(UI_KEY_ENUMS)/sizeof(UI_KEY_ENUMS[0])));
    {
        static const OwlEnum* const FENTAILLE_ENUMS[] = { &E_FenTaille };
        owl_registry_register_enums(FENTAILLE_ENUMS,
                                    (int)(sizeof(FENTAILLE_ENUMS)/sizeof(FENTAILLE_ENUMS[0])));
    }
    owl_registry_register_consts(FENTAILLE_CONSTS,
                                 (int)(sizeof(FENTAILLE_CONSTS)/sizeof(FENTAILLE_CONSTS[0])));
    {
        static const OwlEnum* const RECHERCHE_TABLE_ENUMS[] = { &E_rechercheTable };
        owl_registry_register_enums(RECHERCHE_TABLE_ENUMS,
                                    (int)(sizeof(RECHERCHE_TABLE_ENUMS)/sizeof(RECHERCHE_TABLE_ENUMS[0])));
    }
    owl_registry_register_consts(RECHERCHE_TABLE_CONSTS,
                                 (int)(sizeof(RECHERCHE_TABLE_CONSTS)/sizeof(RECHERCHE_TABLE_CONSTS[0])));
}
