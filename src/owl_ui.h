// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_ui.h - API C pour piloter la couche GUI depuis le runtime OWL.
 *
 * Header agnostique du backend : le runtime OWL (C99) ne voit jamais Qt
 * ni aucun framework GUI. L'implementation vit dans ui_qt.cpp (backend
 * Qt) ou un futur ui_wx.cpp (backend wxWidgets).
 *
 * stdlib_ui.c (principal includer) est compile dans owl.exe avec
 * OWL_HAS_GUI defini. La variante owl-cli (sans Qt) ne l'embarque pas et
 * degrade proprement via les #ifdef. stdlib_xls.c l'include aussi, pour le
 * seul prototype owl_ui_table_export_xlsx (logique data->.xlsx, sans Qt,
 * linkee dans owl_core donc partagee owl.exe / owl-cli).
 */

#ifndef OWL_UI_H
#define OWL_UI_H

#include <stdint.h>
#include <stddef.h>   /* size_t (owl_ui_format_cell_typed) */

#ifdef __cplusplus
extern "C" {
#endif

/* Types opaques : le client C ne voit jamais le vrai type backend. */
typedef struct OwlWindow  OwlWindow;
typedef struct OwlControl OwlControl;

/* Callback invoquee par le backend lors d'un evenement (clic, etc.).
 * userdata est un pointeur opaque qui contient le contexte cote OWL.
 * Pour stdlib_ui, on stocke une chaine C (nom de procedure canon) comme
 * userdata, et on appelle vm_call_reentrant avec ce nom. */
typedef void (*OwlCallback)(OwlControl* sender, void* userdata);

/* ------- Cycle de vie ------- */

int  owl_ui_init(void);
/* Variante hors ecran : pose la plateforme offscreen si l'environnement n'en
 * impose pas deja une, puis initialise. Pour les chemins qui doivent monter de
 * vrais widgets sans jamais afficher quoi que ce soit (generation de donnees
 * pour l'editeur). Sans effet si Qt est deja demarre. */
int  owl_ui_init_offscreen(void);
int  owl_ui_run(void);
void owl_ui_quit(void);

/*  — Ouverture modale d'une fenetre. Si aucun event loop n'est
 * actif, monte la boucle Qt principale via QApplication::exec() (cas du
 * premier Ouvre top-level). Si un event loop tourne deja (cas d'un Ouvre
 * appele depuis un handler d'une fenetre parente), monte une QEventLoop
 * LOCALE branchee sur la fermeture de `w` : Ouvre reste bloquant / modal,
 * la boucle principale continue de dispatcher les events des autres
 * fenetres, et la pile OWL reste synchrone avec la realite Qt.
 *
 * Sans ce dispatch, un second appel a QApplication::exec() est refuse par
 * Qt ("The event loop is already running"), le code post-exec dépile
 * `w` de la window stack alors que la fenetre Qt reste affichee, et tous
 * les handlers ulterieurs resolvent les noms sur la mauvaise fenetre.
 *
 * Retourne le code de retour de la boucle (0 par defaut). */
int  owl_ui_run_modal_for(OwlWindow* w);

/*  — Predicat : un event loop Qt est-il deja actif ? Utilise pour
 * que la couche stdlib_ui distingue le quit "top-level" du quit "imbrique"
 * (sortir une QEventLoop locale sans tuer la boucle principale). Retourne
 * 1 si owl_ui_run / owl_ui_run_modal_for est en cours d'execution. */
int  owl_ui_event_loop_running(void);

/* Bloque le thread courant pendant `ms` millisecondes. Si pump_events != 0
 * ET qu'un QApplication est actif, l'event loop continue de traiter les
 * messages (UI reste fluide). Sinon, fallback sur QThread::msleep
 * (sommeil bloquant pur). */
void owl_ui_wait_ms(int ms, int pump_events);

/* Curseur d'attente global (Sablier). active != 0 empile un curseur d'attente
 * sur l'application entiere (QApplication::setOverrideCursor) ; active == 0
 * depile la derniere activation (restoreOverrideCursor, no-op si la pile est
 * vide). Les activations s'empilent : N activations exigent N restitutions
 * pour retrouver le curseur d'origine. */
void owl_ui_wait_cursor(int active);

/* ------- Live preview (Lot V) ------- */

/* Installe un watcher sur le fichier `path`. Callback `cb(path, userdata)`
 * est appele a chaque modification (debounced ~150ms pour eviter les
 * doublons IDE save). Backend Qt = QFileSystemWatcher. */
typedef void (*OwlFileChangedFn)(const char* path, void* userdata);
void owl_ui_install_file_watcher(const char* path,
                                   OwlFileChangedFn cb,
                                   void* userdata);

/* Detruit une fenetre + tous ses widgets enfants. Utilise par le
 * preview pour fermer la window courante avant de la rebuild. */
void owl_ui_window_destroy(OwlWindow* w);

/* ------- Fenetre ------- */

OwlWindow* owl_ui_window_create(const char* title_utf8);
void       owl_ui_window_set_size(OwlWindow* w, int width, int height);
/*  — mode d'affichage : 0 = normal (showNormal), 1 = reduit
 * (showMinimized), 2 = agrandi (showMaximized). */
void       owl_ui_window_set_display_mode(OwlWindow* w, int mode);
void       owl_ui_window_set_title(OwlWindow* w, const char* title_utf8);
void       owl_ui_window_show(OwlWindow* w);
/* Ferme la fenetre (QEvent::Close synchrone). Retourne 1 si la fermeture a
 * ete acceptee, 0 si un handler d'autorisation l'a refusee (la fenetre reste
 * alors visible et son event de fermeture n'a pas ete declenche). */
int        owl_ui_window_close(OwlWindow* w);

/*  — Grab d'une fenetre / d'un champ / de l'ecran en buffer RGBA8.
 * Buffer alloue par malloc (le caller libere via free). out_width/out_height
 * recoivent les dimensions reelles. Retourne NULL en cas d'echec (widget
 * NULL, allocation, screen indisponible). Pour window/control : utilise
 * QWidget::grab() qui rend off-screen, n'a pas besoin de show().
 * theme = OWL_THEME_DEFAUT | OWL_THEME_CLAIR | OWL_THEME_SOMBRE (sentinels.h).
 * En _CLAIR / _SOMBRE, une palette light/dark est force-poussee dans
 * QGuiApplication le temps du grab puis restauree (push/pop). _DEFAUT
 * conserve la palette OS courante. */
unsigned char* owl_ui_window_grab_rgba (OwlWindow*  w, int theme, int* out_width, int* out_height);
unsigned char* owl_ui_control_grab_rgba(OwlControl* c, int theme, int* out_width, int* out_height);
unsigned char* owl_ui_screen_grab_rgba (int theme, int* out_width, int* out_height);

/*  — Capture directe d'une fenetre vers un fichier PNG (offscreen).
 * Grab la fenetre entiere (render()) et sauve au chemin. Retourne 1 si le PNG
 * est ecrit (fichier non vide), 0 sinon. Enabler de la boucle de polissage
 * visuel : exploitable sous QT_QPA_PLATFORM=offscreen. */
int owl_ui_window_capture(OwlWindow* w, const char* path);

/* Position : -1 sur un axe = centrage sur l'ecran principal. */
void       owl_ui_window_set_position(OwlWindow* w, int x, int y);
/* Correction post-show, frame-aware : ramene la barre de titre a l'ecran si le
 * cadre deborde en haut/gauche (fenetre plus grande que l'ecran). A appeler
 * APRES show() uniquement, et seulement pour les fenetres centrees. */
void       owl_ui_window_clamp_titlebar_onscreen(OwlWindow* w);
int        owl_ui_window_get_x(OwlWindow* w);
int        owl_ui_window_get_y(OwlWindow* w);

/* Visibilite */
void       owl_ui_window_set_visible(OwlWindow* w, int visible);
int        owl_ui_window_get_visible(OwlWindow* w);

/* Etat actif/grise. Le grise applique aussi un overlay sombre via
 * stylesheet pour signaler visuellement la fenetre HS. */
void       owl_ui_window_set_disabled(OwlWindow* w, int disabled);
int        owl_ui_window_get_disabled(OwlWindow* w);

/* Couleur de fond fenetre (RVB entier, canon BGR). -1 = fond systeme. */
void       owl_ui_window_set_bg_color(OwlWindow* w, int rgb);
/*  — variante portant le canal alpha (0-255). alpha>=255 = opaque
 * (émission `rgb(...)` byte-identique à owl_ui_window_set_bg_color) ; alpha<255
 * = fond translucide (`rgba(...)`). Le setter sans alpha appelle cette variante
 * avec alpha=255. */
void       owl_ui_window_set_bg_color_alpha(OwlWindow* w, int rgb, int alpha);
int        owl_ui_window_get_bg_color(OwlWindow* w);

/* Icone fenetre (chemin fichier .ico/.png). NULL ou vide = par defaut. */
void       owl_ui_window_set_icon(OwlWindow* w, const char* path_utf8);
/* Largeur (px) de la plus grande taille disponible de l'icone courante de la
 * fenetre. 0 si aucune icone (QIcon nul), -1 si widget absent. Observabilite
 * test : distingue "icone posee" de "no-op silencieux". */
int        owl_ui_window_get_icon_width(OwlWindow* w);

/* Redimensionnable : 1 = utilisateur peut resize (defaut), 0 = taille fixee.
 * Implementation Qt : setFixedSize(w,h) si 0, sinon Min/Max(0,QWIDGETSIZE_MAX). */
void       owl_ui_window_set_resizable(OwlWindow* w, int resizable);
int        owl_ui_window_get_resizable(OwlWindow* w);

/* ------- Libelle (statique) ------- */

OwlControl* owl_ui_label_create(OwlWindow* parent,
                                const char* text_utf8,
                                int x, int y, int w, int h);

/* ------- Bouton ------- */

OwlControl* owl_ui_button_create(OwlWindow* parent,
                                 const char* text_utf8,
                                 int x, int y, int w, int h);

/* Attache un callback C au signal "clicked" du bouton. userdata sera
 * passe tel quel a la callback lors de chaque clic. Le pointeur
 * userdata doit rester valide tant que le bouton existe (le wrapper
 * ne le copie pas). */
void owl_ui_button_on_click(OwlControl* button,
                            OwlCallback cb,
                            void* userdata);

/* Events Saisie specifiques au QLineEdit :
 *   on_validation -> surValidation()          : Enter presse (returnPressed)
 *   on_change     -> surAChaqueModification() : chaque changement (textChanged) */
void owl_ui_lineedit_on_validation(OwlControl* edit, OwlCallback cb, void* userdata);
void owl_ui_lineedit_on_change    (OwlControl* edit, OwlCallback cb, void* userdata);

/* Events communs a TOUT champ, relayes par event filter (aucun signal Qt requis,
 * donc utilisables sur les backends qui n'en offrent pas : Libelle, Table,
 * ZoneRepetee, conteneurs Selecteur/Interrupteur) :
 *   on_entry      -> surEntree()      : prise de focus
 *   on_exit       -> surSortie()      : perte de focus
 *   on_clic       -> surClic()        : relachement du bouton gauche
 *   on_double_clic-> surDoubleClic()  : double-clic gauche */
void owl_ui_control_on_entry      (OwlControl* c, OwlCallback cb, void* userdata);
void owl_ui_control_on_exit       (OwlControl* c, OwlCallback cb, void* userdata);
void owl_ui_control_on_clic       (OwlControl* c, OwlCallback cb, void* userdata);
void owl_ui_control_on_double_clic(OwlControl* c, OwlCallback cb, void* userdata);

/* Event Fenetre : PROCEDURE Fermeture() — appele juste avant que la fenetre
 * ne se ferme (QEvent::Close), une fois la fermeture autorisee. Ce callback
 * observe, il ne peut pas s'opposer : le veto passe par la requete
 * d'autorisation ci-dessous. */
void owl_ui_window_on_close(OwlWindow* w, OwlCallback cb, void* userdata);

/* Event Fenetre : PROCEDURE AutorisationFermeture() — interroge AVANT toute
 * fermeture (Ferme() comme clic sur la case de fermeture). Un retour 0 annule
 * la fermeture : le QEvent::Close est refuse, la fenetre reste ouverte et le
 * callback on_close n'est pas appele. */
typedef int (*OwlCloseQueryCb)(OwlControl* sender, void* userdata);
void owl_ui_window_on_close_query(OwlWindow* w, OwlCloseQueryCb cb, void* userdata);

/* ------- Menu classique de fenetre -------
 *
 * Le runtime marche l'arbre declare (OwlMenuDecl) et appelle ces primitives
 * dans l'ordre du parcours. `parent == NULL` designe la barre elle-meme ; un
 * OwlMenu non NULL designe un deroulant deja cree (cascade).
 *
 * Cycle : menubar_reset (barre neuve habillee) -> add_submenu / add_option /
 * add_separator... La barre precedente est detruite par Qt lors du reset : la
 * bascule runtime `FEN..MenuPrincipal = MNU_Autre` repasse par le meme chemin,
 * sans action orpheline. */

typedef struct OwlMenu     OwlMenu;      /* deroulant  (QMenu)   */
typedef struct OwlMenuItem OwlMenuItem;  /* item cliquable (QAction) */

/*  (ticket C) — style résolu d'une surface texte du menu (Barre /
 * OptionMenu, base ou état survol) transmis à la couche Qt. Sentinelle -1
 * (ints) / NULL (police_nom) = « non posé » -> repli sur le défaut du thème,
 * prop par prop, au rendu QSS. Miroir plat de OwlMenuStyleBrick (ui_decl.h),
 * découplé pour ne pas tirer ui_decl.h dans la couche Qt. */
/*  — encodage alpha d'un slot couleur menu. Convention IDENTIQUE à
 * ui_decl.h (OWL_MENU_COL_PACK) : bits 0-23 = BGR, bits 24-31 = alpha, bit 32 =
 * flag « alpha explicite » (couleur semi-transparente -> rgba()). Sans le flag,
 * le slot est un BGR 24 bits opaque -> rgb(). -1 = non posé. */
#define OWL_MENU_COL_ALPHA (1LL << 32)

typedef struct {
    int64_t     couleur;          /* texte, BGR (+ alpha) ; -1 = non posé */
    int64_t     couleur_fond;     /* fond, BGR (+ alpha) ; -1 = non posé */
    const char* police_nom;       /* famille ; NULL = non posé */
    int         police_taille;    /* points ; -1 = non posé */
    int         police_gras;      /* -1 non posé, 0/1 */
    int         police_italique;  /* -1 non posé, 0/1 */
    int         police_souligne;  /* -1 non posé, 0/1 */
    int         police_barre;     /* -1 non posé, 0/1 */
} OwlMenuStyleSurface;

typedef struct {
    int                 has_style;     /* 0 = aucun style posé (défaut pur) */
    OwlMenuStyleSurface barre_base;
    OwlMenuStyleSurface barre_survol;
    OwlMenuStyleSurface option_base;
    OwlMenuStyleSurface option_survol;
    int64_t             sousmenu_couleur_fond;        /* -1 = non posé */
    int64_t             sousmenu_couleur_separateur;   /* -1 = non posé */
    /*  lot 3-ter — bordure + arrondi du panneau. Fallback par prop au
     * rendu : Epaisseur->1, CouleurBordure->K_MENU_BORDER, Arrondi->0. */
    int                 sousmenu_arrondi;             /* -1 = non posé */
    int                 sousmenu_epaisseur;           /* -1 = non posé */
    int64_t             sousmenu_couleur_bordure;      /* -1 = non posé */
} OwlMenuStyleResolved;

/* Installe une barre de menu vierge, habillee du style OWL par defaut
 * (coherent avec le fond sombre du panneau de fenetre). Detruit la barre
 * precedente et tout son arbre. `style` NULL ou has_style==0 : rendu par
 * defaut byte-identique. Sinon chaque prop posée remplace le défaut. */
void owl_ui_window_menubar_reset(OwlWindow* w, const OwlMenuStyleResolved* style);

/*  — feuille QSS courante de la barre de menu (test-only). "" si aucune
 * barre montee. Utilise par __DebugFeuilleStyleMenu pour verifier l'emission des
 * couleurs du bloc Style menu (alpha inclus) sans capture-pixel. */
const char* owl_ui_window_menubar_get_stylesheet(OwlWindow* w);

/*  — police EFFECTIVE (posée par setFont) d'un menu, descripteur
 * "famille|taille|gras|italique" (test-only). Vérifie que la police du bloc
 * Style est réellement rendue (Qt n'honore pas font-family en QSS sur les
 * items de menu). "" si barre/déroulant introuvable. */
const char* owl_ui_window_menubar_get_font_desc(OwlWindow* w);
const char* owl_ui_window_submenu_get_font_desc(OwlWindow* w,
                                                const char* submenu_name);

/*  — hauteur d'encre (glyphes peints) du texte de la barre de menu, en
 * pixels (test-only). Mesure le rendu reel via grab off-screen, la ou
 * get_font_desc ne lit que le QFont pose. -1 si aucune barre, 0 si aucune encre. */
int owl_ui_window_menubar_ink_height(OwlWindow* w);

/* Retire toute barre de menu (aucun menu principal affecte). */
void owl_ui_window_menubar_clear(OwlWindow* w);

/* Deroulant. `enabled`/`visible` : 0 ou 1. `inactif` a 1 rend l'entree non
 * interactive MAIS de couleur normale (etat « Inactif », distinct de « Grisé »
 * qui passe par `enabled`=0). Retourne NULL si la barre n'existe pas (reset non
 * appele). */
OwlMenu* owl_ui_menu_add_submenu(OwlWindow* w, OwlMenu* parent,
                                 const char* label_utf8,
                                 int enabled, int visible, int inactif);

/* Trait de separation. Sans effet au niveau de la barre elle-meme sous les
 * styles qui ne le dessinent pas — le cas normal est dans un deroulant. */
void owl_ui_menu_add_separator(OwlWindow* w, OwlMenu* parent, int visible);

/* Item terminal. `shortcut_utf8` NULL/vide = pas d'accelerateur. `checkable`
 * a 0 laisse l'item sans case a cocher (le `checked` est alors ignore). Le
 * mnemonique '&Lettre' du libelle est pris en charge nativement par Qt.
 * `cb` est invoque sur `triggered` (clic ET validation clavier). `inactif` a 1
 * rend l'option non interactive MAIS de couleur normale (etat « Inactif »,
 * distinct de « Grisé » via `enabled`=0) : l'item reste actif cote Qt (couleur)
 * et un filtre d'evenements avale clic/validation. */
OwlMenuItem* owl_ui_menu_add_option(OwlWindow* w, OwlMenu* parent,
                                    const char* label_utf8,
                                    const char* shortcut_utf8,
                                    int enabled, int checkable, int checked,
                                    int visible, int inactif,
                                    OwlCallback cb, void* userdata);

/* Icone de l'item : chemin de fichier ou buffer RGBA8 top-down. */
void owl_ui_menu_item_set_icon_path(OwlMenuItem* item, const char* path_utf8);
void owl_ui_menu_item_set_icon_rgba(OwlMenuItem* item,
                                    const unsigned char* rgba, int w, int h);

/* ------- Menu contextuel (clic droit, ) -------
 *
 * Contrairement au menu principal (ancre a la barre de la fenetre), un menu
 * contextuel est un QMenu autonome affiche a la position du curseur. Il
 * reutilise les primitives owl_ui_menu_add_submenu/option/separator avec un
 * `parent` non NULL (le QMenu racine cree ici) : la barre n'est jamais touchee.
 *
 * Cycle : install (une fois, a l'ouverture) pose la policy + le callback de
 * demande ; a chaque clic droit le callback (cote OWL) fait create -> add_* ->
 * popup. `popup` est bloquant (exec) et detruit le QMenu au retour. */

/* Pose la policy de menu contextuel sur un champ + branche le callback declenche
 * au clic droit (customContextMenuRequested). `sender` recu par cb = le champ. */
void owl_ui_install_context_menu(OwlControl* c, OwlCallback cb, void* userdata);

/* Idem pour la fenetre elle-meme (clic droit sur une zone vide du fond). */
void owl_ui_install_window_context_menu(OwlWindow* w, OwlCallback cb, void* userdata);

/* Cree un QMenu racine autonome (habille du style resolu), non affiche. A
 * peupler via owl_ui_menu_add_submenu/option/separator (parent = ce menu). */
OwlMenu* owl_ui_context_menu_create(OwlWindow* w, const OwlMenuStyleResolved* style);

/* Affiche le menu contextuel a la position du curseur (bloquant : le dispatch
 * des surSelection a lieu ici) puis detruit le QMenu et tout son arbre. */
void owl_ui_context_menu_popup(OwlMenu* root);

/*  — feuille QSS qu'un style resolu produirait sur un panneau de menu,
 * SANS creer ni afficher de QMenu (test-only). Un menu contextuel n'existe qu'a
 * l'instant du clic droit et son popup est modal : sans ce chemin, aucun test ne
 * peut mesurer son habillage. Meme fonction de composition que celle appliquee
 * par owl_ui_context_menu_create. Le buffer rendu vit jusqu'au prochain appel. */
const char* owl_ui_menu_stylesheet_preview(const OwlMenuStyleResolved* style);

/*  — enregistre un raccourci PERSISTANT d'une OptionMenu de menu
 * contextuel sur le widget cible, actif menu FERME quand ce widget a le focus.
 * widget != NULL (menu attache a un champ) : contexte WidgetWithChildrenShortcut.
 * widget == NULL (menu attache au fond) : contexte WindowShortcut sur le central
 * de `window`. `triggered` route sur le meme dispatch que le clic (cb+userdata). */
void owl_ui_install_context_shortcut(OwlControl* widget, OwlWindow* window,
                                     const char* shortcut_utf8,
                                     OwlCallback cb, void* userdata);

/* ------- Saisie (texte mono-ligne) ------- */

OwlControl* owl_ui_lineedit_create(OwlWindow* parent,
                                   const char* placeholder_utf8,
                                   int x, int y, int w, int h);

/* Variante mot de passe : caracteres masques. */
OwlControl* owl_ui_password_create(OwlWindow* parent,
                                   const char* placeholder_utf8,
                                   int x, int y, int w, int h);

/* Variante multi-ligne (QPlainTextEdit). owl_ui_control_set_text et
 * owl_ui_lineedit_get_text reconnaissent le widget et appellent
 * setPlainText/toPlainText au lieu de setText/text. */
OwlControl* owl_ui_multiline_create(OwlWindow* parent,
                                    const char* placeholder_utf8,
                                    int x, int y, int w, int h);

/* Recupere le texte courant. Retourne un pointeur statique invalide
 * apres le prochain appel (le client doit copier s'il a besoin). */
const char* owl_ui_lineedit_get_text(OwlControl* edit);

/* Libelle integre du Saisie. Materialise (ou defait) un composite
 * [QLabel | QLineEdit] autour du champ : le QLineEdit reste le controle logique
 * (get/set/focus/event filter inchanges), le wrapper devient son representant de
 * layout (ajoute au layout parent a sa place). Le libelle est rendu si le texte
 * est non vide ET la largeur > 0 ; sinon le champ est rendu seul (retro-compat).
 * Les deux props sont memorisees independamment : changer l'une recompose. */
void owl_ui_lineedit_set_label_text(OwlControl* edit, const char* text_utf8);
void owl_ui_lineedit_set_label_width(OwlControl* edit, int width);

/* Recupere le texte d'un widget generique (label, bouton, etc.). Comme
 * owl_ui_lineedit_get_text, le pointeur retourne devient invalide au
 * prochain appel. Utilise QWidget::property("text") en interne. */
const char* owl_ui_control_get_text(OwlControl* w);

/* Lecture proprietes communes (geometry/visibilite/etat).
 * get_x / get_y sont relatifs a la zone client de la FENETRE, pas au parent
 * immediat. Appeler owl_ui_control_sync_layout avant une lecture
 * de geometrie faite hors boucle d'evenements. */
int  owl_ui_control_get_visible(OwlControl* w);
int  owl_ui_control_get_enabled(OwlControl* w);
int  owl_ui_control_get_width(OwlControl* w);
int  owl_ui_control_get_height(OwlControl* w);
int  owl_ui_control_get_x(OwlControl* w);
int  owl_ui_control_get_y(OwlControl* w);

/* Applique les layouts en attente sur la fenetre qui porte ce controle, pour
 * que sa geometrie soit celle du rendu final et non une taille provisoire. */
void owl_ui_control_sync_layout(OwlControl* w);

/*  — override de taille d'icone (..ImageTaille). 0 = auto. */
void owl_ui_control_set_icon_size_override(OwlControl* w, int size_px);
int  owl_ui_control_get_icon_size_override(OwlControl* w);

/* Lecture proprietes fenetre.
 * get_width / get_height : appeler owl_ui_window_sync_layout avant une lecture
 * faite hors boucle d'evenements (meme raison que pour un controle). */
const char* owl_ui_window_get_title(OwlWindow* w);
int  owl_ui_window_get_width(OwlWindow* w);
int  owl_ui_window_get_height(OwlWindow* w);

/* Applique les layouts en attente sur la fenetre, pour que sa taille soit
 * celle du rendu final et non une taille provisoire. */
void owl_ui_window_sync_layout(OwlWindow* w);

/*  — 1 si le champ est placé par une disposition automatique (parent
 * muni d'un QLayout : pile ou grille), 0 en disposition libre / sans widget.
 * Distingue les deux régimes d'écriture de géométrie (taille = contrainte
 * persistante en auto ; position refusée en auto). */
int  owl_ui_control_is_layout_managed(OwlControl* w);

/* Ecriture geometry. */
void owl_ui_control_set_width(OwlControl* w, int width);
void owl_ui_control_set_height(OwlControl* w, int height);
void owl_ui_control_set_x(OwlControl* w, int x);
void owl_ui_control_set_y(OwlControl* w, int y);

/* ------- Conteneur (Lot F : boite generique pour layout par nesting) -------
 *
 * Un Conteneur est un QWidget vide qui sert de parent a d'autres widgets
 * et heberge un QLayout (vertical / horizontal / grid / aucun=libre). * Voir docs/public_documentation/candidats/layouts.md.
 */

/* Cree un Conteneur enfant de `parent`. Si parent_layout != 0 : installe
 * deja un layout du mode demande (1=vert, 2=horiz, 3=grille, 0=libre).
 * Pour ajouter le Conteneur a un parent en mode layout, il faut ensuite
 * appeler owl_ui_layout_add_child sur ce Conteneur depuis le code qui
 * gere le parent. */
OwlControl* owl_ui_container_create(OwlWindow* parent,
                                    int x, int y, int w, int h);

/* Constantes layout — sentinel-zone partagee avec les UI_CONSTS du registry
 * (cf src/sentinels.h sections OWL_DISP_* et OWL_ALIGN_*). Inclusion ici
 * pour que ui_qt.cpp et stdlib_ui.c voient les memes valeurs. */
#include "sentinels.h"
/* Aliases historiques pour l'API publique du backend UI. */
#define OWL_DISPOSITION_VERTICALE    OWL_DISP_VERTICALE
#define OWL_DISPOSITION_HORIZONTALE  OWL_DISP_HORIZONTALE
#define OWL_DISPOSITION_GRILLE       OWL_DISP_GRILLE
#define OWL_DISPOSITION_LIBRE        OWL_DISP_LIBRE

/* Installe un layout sur la fenetre OU sur un conteneur. Pour la fenetre,
 * cible le central widget. Le layout est applique AVANT d'ajouter les
 * enfants (sinon les enfants deja places en X/Y absolu sont reorganises).
 * disposition : 1..4 (cf constantes ci-dessus).
 * Padding/spacing en pixels. cols/rows utilises uniquement si dispGrille ;
 * rows<=0 -> calcule depuis enfants/cols. */
/* def_align_h / def_align_v : alignement par defaut applique aux enfants
 * ajoutes via owl_ui_layout_add_child quand leur prop Alignement vaut 0
 * (= non specifie). 0 = inchange (Qt decide). Valeurs OWL_ALIGN_*. */
void owl_ui_window_set_layout(OwlWindow* parent,
                              int disposition,
                              int padding_left, int padding_top,
                              int padding_right, int padding_bottom,
                              int spacing,
                              int cols, int rows,
                              int def_align_h, int def_align_v);
void owl_ui_container_set_layout(OwlControl* container,
                                 int disposition,
                                 int padding_left, int padding_top,
                                 int padding_right, int padding_bottom,
                                 int spacing,
                                 int cols, int rows,
                                 int def_align_h, int def_align_v);

/* Ajoute un widget enfant au layout du parent (Fenetre ou Conteneur).
 * stretch : 0 = taille naturelle, >0 = facteur d'etirement (1 = "Etire = Vrai").
 * align    : 0 = defaut conteneur, sinon OWL_ALIGN_*.
 * margin_* : marges externes en px (0 = aucune).
 * col/row  : pour dispGrille uniquement (-1 = ajoute a la suite).
 * col_span/row_span : >=1 (1 = pas de fusion).
 * decl_w/decl_h : Largeur/Hauteur declarees de l'enfant (px, 0 = non figee).
 *   En box layout, figent l'axe PRINCIPAL (Hauteur en pile verticale, Largeur
 *   en pile horizontale) pour que la taille ne soit pas jetee par le layout. */
void owl_ui_layout_add_child(OwlWindow* parent_window_or_null,
                             OwlControl* parent_container_or_null,
                             OwlControl* child,
                             int stretch, int align,
                             int margin_left, int margin_top,
                             int margin_right, int margin_bottom,
                             int col, int row,
                             int col_span, int row_span,
                             int decl_w, int decl_h);

/* Ajoute un ressort terminal (addStretch) au box layout du parent (Fenetre ou
 * Conteneur) : empile les widgets en haut/gauche, l'excedent de l'axe principal
 * reste vide en fin. No-op si le parent n'a pas de box layout (grille/dispLibre).
 * A n'appeler que si aucun enfant du layout ne porte Étire (facteur de stretch),
 * sinon le ressort volerait l'excedent a l'enfant elastique. */
void owl_ui_layout_add_terminal_stretch(OwlWindow* parent_window_or_null,
                                        OwlControl* parent_container_or_null);

/* Bridge : passe un Conteneur comme "parent slot" aux _create existants.
 * Equivalent a un cast — la signature des _create accepte deja n'importe
 * quel QWidget* deguise en OwlWindow* (cf central_of qui detecte le type).
 */
OwlWindow* owl_ui_container_as_parent(OwlControl* container);

/* ------- Champ Table (V1 : QTableView + QStandardItemModel) -------
 *
 * Un OwlControl Table encapsule un QTableView + son QStandardItemModel
 * + (a venir) un QSortFilterProxyModel. La V1 expose juste la creation
 * d'un widget vide ; les colonnes / lignes / events viendront en
 * Phases 1+. */

OwlControl* owl_ui_table_create(OwlWindow* parent,
                                int x, int y, int w, int h);

/* Ajoute une colonne a la fin du modele. col_type encode le type de
 * cellule (cf OWL_COL_TYPE_* dans sentinels.h, expose au canon via les
 * constantes colTypeTexte/colTypeNumerique/...). width<=0 = laisse Qt
 * calculer une largeur par defaut. title peut etre NULL ou vide.
 * Phase 1 V1 : ne change que le header ; les delegates par type
 * arriveront en Phase 7. */
void owl_ui_table_add_column(OwlControl* table,
                             const char* title_utf8,
                             int col_type,
                             int width);

/* Phase 2 — manipulation des lignes.
 *
 * append_row : ajoute une ligne en fin de table. cells_utf8 est un
 *   tableau de n_cells chaines UTF-8 (1 par colonne, dans l'ordre de
 *   la table). Les cellules manquantes restent vides ; les cellules en
 *   trop sont ignorees. Le formatage par type (parse int/date/bool) est
 *   fait selon le col_type stocke a add_column.
 * remove_row : supprime la ligne 1-based row_1based (alignement canon).
 *   row<=0 ou >= rowCount : no-op silencieux.
 * clear     : retire toutes les lignes (mais conserve les colonnes).
 * row_count : nombre de lignes courantes. */
void owl_ui_table_append_row(OwlControl* table,
                             const char* const* cells_utf8,
                             int n_cells);
void owl_ui_table_remove_row(OwlControl* table, int row_1based);
void owl_ui_table_clear(OwlControl* table);
int  owl_ui_table_row_count(OwlControl* table);
int  owl_ui_table_column_count(OwlControl* table);

/* Masque/reaffiche une colonne (col_1based) sans la supprimer du modele :
 * column_count reste inchange, les donnees ne sont pas decalees. */
void owl_ui_table_set_column_hidden(OwlControl* table, int col_1based, int hidden);

/* Modification d'une ligne existante. */
void owl_ui_table_set_row(OwlControl* table, int row_1based,
                          const char* const* cells_utf8, int n_cells);

/*  — mise en forme conditionnelle : couleur de texte (is_background=0)
 * ou de fond (is_background=1) d'UNE cellule, ou de la ligne entiere quand
 * col_1based <= 0. bgr = couleur canon BGR, alpha 0-255. Pose la couleur sur le
 * QStandardItem (roles Foreground/Background) : c'est le seul niveau assez fin
 * pour une cellule, une feuille QSS ne descend pas sous la colonne. */
void owl_ui_table_set_cell_color(OwlControl* table, int row_1based,
                                 int col_1based, int is_background,
                                 int bgr, int alpha);

/*  — mise en forme par COLONNE : couleur de texte (is_background=0) ou
 * de fond (is_background=1) appliquée à toute la colonne col_1based, lignes
 * ajoutées après l'affectation comprises. Niveau le plus bas (cellule > ligne >
 * colonne) : ne réécrit pas une couleur cellule/ligne déjà posée. */
void owl_ui_table_set_column_color(OwlControl* table, int col_1based,
                                   int is_background, int bgr, int alpha);

/*  — masque de saisie par COLONNE : stocke la forme canon sur le view et
 * installe le delegate d'edition masquant (createEditor pose le masque via la
 * conversion Saisie reutilisee). owl_ui_table_cell_editor_mask relit (test-only)
 * le masque effectif de l'editeur transitoire d'une cellule (row/col 1-based),
 * reconverti Qt->canon ; chaine vide si cellule introuvable ou colonne sans masque. */
void owl_ui_table_set_column_mask(OwlControl* table, int col_1based,
                                  const char* wl_mask);
const char* owl_ui_table_cell_editor_mask(OwlControl* table, int row_1based,
                                          int col_1based);

/*  +  — colonne pilotee par FORMAT (framework type-driven, cf.
 * le dispatch de createEditor dans ui_qt.cpp) : stocke le format effectif (fmt,
 * ex. "JJ/MM/AAAA", "HH:MI", "HH:MI:SS") et le masque de frappe pre-derive
 * (qtmask) sur le view (col_typeformat_/col_typemask_). Le createEditor
 * dispatche sur col_type_<col0> et, selon le type (Date/Heure/Duree), pose
 * masque + validateur de plages adequat (miroir Saisie +2031+2033).
 * owl_ui_table_cell_editor_valide_saisie interroge (test-only) le validateur
 * effectif de l'editeur transitoire d'une cellule sur un texte : rend 1 ssi
 * l'etat est Acceptable (0 sinon), 1 si la cellule n'a aucun validateur. */
void owl_ui_table_set_column_format(OwlControl* table, int col_1based,
                                    const char* fmt, const char* qtmask);
int owl_ui_table_cell_editor_valide_saisie(OwlControl* table, int row_1based,
                                           int col_1based, const char* text);

/*  — formatage AFFICHAGE (hors edition) d'une cellule de colonne typee
 * Numerique/Monetaire : resout le format PROJET et formate la chaine brute (repli
 * plat si aucun format resolu). Defini cote core (stdlib_ui.c), appele par le
 * delegate colonne (ui_qt.cpp). owl_ui_table_cell_texte_affiche relit (test-only)
 * le texte RENDU d'une cellule (DisplayRole formate par le delegate), HORS
 * edition ; chaine vide si cellule introuvable. */
void owl_ui_format_cell_typed(int col_type, const char* raw,
                              char* out, size_t outsz);
const char* owl_ui_table_cell_texte_affiche(OwlControl* table, int row_1based,
                                            int col_1based);

/* Selection (canon : TableSelectPlus / TableSelectMoins / TableSelect /
 * TableSelectOccurrence). select_row = ajoute a la selection (multi),
 * deselect_row = retire. get_selected_row_n(rang) : rang-th ligne
 * selectionnee (1-based). get_selected_col(rang) : colonne du rang-th
 * item selectionne. selection_count : nombre total de lignes selectionnees. */
void owl_ui_table_select_row(OwlControl* table, int row_1based);
void owl_ui_table_deselect_row(OwlControl* table, int row_1based);
int  owl_ui_table_get_selected_row_n(OwlControl* table, int rang_1based);
int  owl_ui_table_get_selected_col(OwlControl* table, int rang_1based);
int  owl_ui_table_selection_count(OwlControl* table);

/* Tri programmatique. col_1based : index 1-based ou -1 pour resoudre par
 * nom (col_name_utf8). ascending : 0/1. */
void owl_ui_table_sort_by_column(OwlControl* table, int col_1based,
                                  const char* col_name_utf8,
                                  int ascending);

/* Recompute la visibilite des lignes en ET du jeu complet des filtres actifs
 * (store $tblfilters). Une ligne est visible ssi elle matche CHAQUE filtre.
 * Les colonnes sont deja resolues en index 0-based cote natif (ordre declare) :
 *   col_indices[i] : index 0-based de colonne
 *   types[i]       : constante OWL_FILTRE_*
 *   values[i]      : valeur du filtre (UTF-8)
 * n = 0 => aucun filtre => toutes les lignes redeviennent visibles. */
void owl_ui_table_apply_filters(OwlControl* table,
                                 const int* col_indices,
                                 const int* types,
                                 const char* const* values,
                                 int n);

/*  — ponts "filtre a la souris". Le header custom Qt (icone loupe
 * sur l'en-tete) appelle ces fonctions du runtime pour partager LE MEME store
 * de filtres ($tblfilters) que TableActiveFiltre : pas de 2e chemin d'etat cote
 * Qt. col0 = index 0-based de colonne (ordre declare = index logique de section
 * du header). Definies dans stdlib_ui.c (sens ui_qt.cpp -> runtime, inverse du
 * reste de ce header). No-op hors fenetre courante / colonne inconnue.
 *   - apply : pose/remplace le filtre (op OWL_FILTRE_* + valeur) et recompute.
 *   - clear : retire le seul filtre de cette colonne et recompute.
 *   - is_filtered : 1 si la colonne porte un filtre actif (etat de l'icone). */
void owl_ui_table_user_apply_filter(OwlControl* tableWidget, int col0,
                                    int type, const char* val);
void owl_ui_table_user_clear_filter(OwlControl* tableWidget, int col0);
int  owl_ui_table_col_is_filtered(OwlControl* tableWidget, int col0);

/*  — menu de la LOUPE attache (`MenuLoupe = MNU_X` sur le Champ Table).
 * Le header custom Qt appelle ce pont au clic droit sur l'en-tete AVANT de
 * construire son menu integre : si la Table porte un MenuLoupe, ce menu custom
 * est rendu a la place (sender des options = proxy-colonne -> MoiMeme = colonne)
 * et le pont retourne 1 (l'appelant supprime l'integre). 0 = pas d'attache,
 * l'appelant garde son menu integre (non-regression). col0 = index 0-based. */
int  owl_ui_table_show_magnifier_menu(OwlControl* tableWidget, int col0);

/*  — filtre checklist « valeurs distinctes » (facon tableur). Le
 * header custom Qt (popup checklist) appelle ces ponts du runtime ; ils
 * partagent LE MEME store $tblfilters + recompute que TableActiveFiltre (un
 * seul systeme, meme chemin que le hook de test __DebugTableFiltreIN). col0 =
 * index 0-based (ordre declare).
 *   - apply_filter_in : pose/remplace un filtre IN {ensemble} (valeursRC =
 *     valeurs a retenir, jointes par RC) et recompute la visibilite.
 *   - distinct_values : ecrit dans out (borne par cap) les valeurs distinctes
 *     de la colonne (ordre de 1ere apparition, RC) pour peupler la checklist ;
 *     chaine vide si colonne vide OU au-dela du seuil de cardinalite (pas de
 *     checklist proposee). */
void owl_ui_table_user_apply_filter_in(OwlControl* tableWidget, int col0,
                                       const char* valeursRC);
void owl_ui_table_distinct_values(OwlControl* tableWidget, int col0,
                                  char* out, int cap);

/*  — valeur du filtre actif sur la colonne col0 (0-based, ordre
 * declare). Le header custom Qt s'en sert pour PREREMPLIR le champ inline a la
 * re-ouverture de la loupe : la valeur reapparait et la croix de reset (clear
 * button Qt) redevient disponible. Lit le meme store $tblfilters (pure lecture,
 * aucune allocation OWL). Rend 1 et copie la valeur UTF-8 dans out (borne par
 * cap) si un filtre est actif, 0 sinon (out mis a chaine vide). */
int  owl_ui_table_user_get_filter_value(OwlControl* tableWidget, int col0,
                                         char* out, int cap);

/*  — type (constante filtre*) du filtre actif sur la colonne col0, 0
 * si aucun filtre. Symetrique de get_filter_value : le header custom Qt s'en
 * sert pour decider du prereplissage de l'editeur inline. Un OWL_FILTRE_IN
 * (checklist) porte un ensemble RC-joint, pas un motif texte -> editeur vide. */
int  owl_ui_table_user_get_filter_type(OwlControl* tableWidget, int col0);

/*  — mode de gestion de la loupe (propriete ModeLoupe) resolu pour la
 * Table portant tableWidget. Le header custom lit ce mode pour brancher la
 * frappe inline : filtreContient (OWL_FILTRE_CONTIENT) = masque via le store de
 * filtres partage ; rechercheCommencePar (OWL_RCHT_COMMENCE_PAR, defaut) = trie
 * + positionne sans masquer. Rend le defaut rechercheCommencePar hors fenetre
 * courante ou si la propriete n'a jamais ete ecrite. */
int  owl_ui_table_user_mode_loupe(OwlControl* tableWidget);

/*  — repeint l'en-tete horizontal (icone loupe/entonnoir) apres une
 * ecriture runtime TAB..ModeLoupe. No-op hors QTableView. */
void owl_ui_table_header_refresh(OwlControl* table);

/*  — ouvre par programme la saisie loupe de la colonne col0 (0-based,
 * ordre declare), comme un clic utilisateur sur l'icone loupe : transforme le
 * titre de colonne en champ de saisie inline (showInlineEditor). forcedOp >= 0
 * = un operateur filtre* qui prime sur ModeLoupe (la frappe masque selon cet
 * operateur) ; -1 = la frappe suit ModeLoupe. initialValue (UTF-8, peut etre
 * NULL/"") preremplit le champ. L'application du filtre au store est faite cote
 * runtime avant l'appel (le prefill relit ce store) ; ce pont ne fait que le
 * volet visuel. No-op hors QTableView. */
void owl_ui_table_open_search(OwlControl* table, int col0, int forcedOp,
                              const char* initialValue);

/* TablePosition canon : scroll a la ligne idx_1based.
 * make_visible_only=1 = mode taVisible (rend visible au plus pres, ne change
 * pas si deja visible). */
void owl_ui_table_set_position(OwlControl* table, int row_1based,
                                int make_visible_only);
int  owl_ui_table_get_first_visible_row(OwlControl* table);

/* TableInfoXY canon : hit-test pixel -> (row, col) 1-based, 0 si hors
 * champ. Lookup nom de colonne par index 1-based (chaine vide si invalid). */
void        owl_ui_table_hit_test(OwlControl* table, int x, int y,
                                   int* out_row_1based, int* out_col_1based);
const char* owl_ui_table_get_column_name(OwlControl* table, int col_1based);

/* Phase 8 — evenements lifecycle. Mapping vers les blocs PROCEDURE canon :
 *   on_click          -> surClic / onClick
 *   on_double_click   -> surDoubleClic / onDoubleClick
 *   on_selection      -> surChangementSelection / onSelectionChange
 *   on_row_enter      -> surEntreeLigne / onRowEnter
 *   on_row_exit       -> surSortieLigne / onRowExit
 *
 * surEntreeLigne et surSortieLigne sont declenches en cascade quand la
 * ligne courante change (sortie de l'ancienne, puis entree dans la
 * nouvelle), permettant de valider/sauver avant de changer de ligne. */
void owl_ui_table_on_click          (OwlControl* table, OwlCallback cb, void* userdata);
void owl_ui_table_on_double_click   (OwlControl* table, OwlCallback cb, void* userdata);
void owl_ui_table_on_selection      (OwlControl* table, OwlCallback cb, void* userdata);
void owl_ui_table_on_row_enter      (OwlControl* table, OwlCallback cb, void* userdata);
void owl_ui_table_on_row_exit       (OwlControl* table, OwlCallback cb, void* userdata);

/* Export CSV — utilise par le context menu. Pas expose en native canon
 * (CSV n'existe pas dans le canon ; user a sa propre lib Excel). */
int  owl_ui_table_export_csv(OwlControl* table, const char* path_utf8);

/* Installe le context menu standard (Copier cellule/ligne/colonne, Tout copier,
 * Exporter vers Excel, Exporter en CSV). Branche automatiquement le right-click. */
void owl_ui_table_install_default_context_menu(OwlControl* table);

/* Export « table -> .xlsx » — logique pure-données (aucun widget Qt), donc
 * testable headless. Le menu contextuel de table extrait entêtes + lignes
 * visibles puis appelle ceci avec le chemin choisi par le sélecteur de fichier.
 * headers[ncols], cells row-major nrows*ncols (une entrée NULL = cellule vide).
 * Ligne 1 = entêtes, lignes 2.. = données. Retourne 1 si écrit, 0 sinon. */
int owl_ui_table_export_xlsx(const char* path_utf8,
                             const char* const* headers, int ncols,
                             const char* const* cells, int nrows);

/* Rasterise une icone du catalogue Iconify embarqué (nom kebab-case, ex
 * "content-copy") vers un buffer RGBA (caller free via free()). N'émet aucune
 * erreur native (lookup catalogue silencieux). Retourne 1 si trouvée + rendue,
 * 0 sinon (nom inconnu ou rasterizer absent).
 *
 * color_bgr (format OWL 0x00BBGGRR) teinte les icones MONOCHROMES (celles
 * portant currentColor, couleur_modifiable=true). Les icones aux couleurs
 * bakées (couleur_modifiable=false, ex. file-type-excel) ignorent color_bgr
 * et gardent leurs couleurs natives. */
int owl_ui_icone_catalog_rgba(const char* name, int size, unsigned int color_bgr,
                              unsigned char** out_rgba, int* out_w, int* out_h);

/* ------- Generique controles ------- */

void owl_ui_control_set_text(OwlControl* c, const char* text_utf8);
void owl_ui_control_set_visible(OwlControl* c, int visible);
void owl_ui_control_set_enabled(OwlControl* c, int enabled);
/*  — donne le focus clavier au widget (DonneFocus/SetFocus, focus
 * initial automatique). No-op si c == NULL. */
void owl_ui_control_set_focus(OwlControl* c);

/*  — annule les setFocus differes (QTimer 0ms, ) encore en vol
 * cibles sur la fenetre `w` ou un de ses descendants. A appeler au teardown de
 * la fenetre (Ferme) AVANT toute invalidation des bindings : un focus differe
 * dont la fenetre se ferme ne doit jamais tirer (sinon FocusOut/FocusIn ->
 * surSortie/surEntree sur un binding OWL perime = use-after-free). No-op si
 * w == NULL ou aucun timer en vol. */
void owl_ui_cancel_focus_timers(OwlWindow* w);

/*  — detache la fenetre `w` du systeme de focus avant son teardown :
 *   1. purge les entrees focus/close du dispatcher pour w + ses champs (sinon
 *      une cle QObject* dangling subsiste apres deleteLater, et le binding OWL
 *      associe est reutilise par la fenetre suivante apres bindings_restore) ;
 *   2. retire le focus clavier Qt s'il est pose sur un widget de w.
 * Sans (1)+(2), un champ d'une fenetre fermee garde le focus Qt ; quand la
 * fenetre suivante prend le focus, le FocusOut residuel du champ fantome
 * dispatche surSortie sur un binding perime/reutilise -> contamination
 * cross-instance. Le dispatcher est purge AVANT le clearFocus pour que ce
 * dernier ne declenche aucun event sur l'instance en cours de fermeture. */
void owl_ui_purge_window_focus(OwlWindow* w);

/*  — synchronisation de l'etat logique du focus OWL (focused_index)
 * avec le focus clavier Qt REEL. Le backend appelle le callback enregistre a
 * chaque QApplication::focusChanged, avec le widget qui vient de prendre le
 * focus (`now`, NULL si plus aucun widget focuse). La couche OWL retrouve
 * l'instance fenetre + l'index du controle et met focused_index a jour SANS
 * rejouer surEntree/surSortie (ces events restent geres par l'eventFilter
 * FocusIn). Enregistre une seule fois a l'init UI ; no-op en headless (le
 * signal Qt n'existe pas). */
typedef void (*OwlFocusSyncFn)(OwlControl* now);
void owl_ui_set_focus_sync_callback(OwlFocusSyncFn fn);

/*  — true si `widget` EST `ctrl` ou un de ses descendants Qt. Permet a
 * la couche OWL de rattacher un focus pose sur un sous-widget (ex. le QLineEdit
 * interne d'un Combo editable) au controle OWL proprietaire. */
int owl_ui_control_owns_widget(OwlControl* ctrl, OwlControl* widget);
/*  — simulation d'entree reelle pour les tests UI (mode non-headless).
 * owl_ui_post_text_input : poste de vrais QKeyEvent (un par caractere) au champ
 *   de saisie -> textChanged tire a chaque frappe (surAChaqueModification).
 * owl_ui_post_mouse_click : poste press+release souris (button 0=gauche,
 *   1=droit ; x/y locaux, -1/-1 = centre) -> vrai clic qui vole le focus.
 *   dbl non nul : sequence complete de double-clic (press, release, dblclick,
 *   release), seule facon d'obtenir un MouseButtonDblClick sur un event poste.
 * owl_ui_pump_events : pompe la boucle (applique les events postes + setFocus
 *   differe + timers) sans bloquer, jusqu'a max_ms (<=0 => defaut court).
 * Le caller poste puis pompe pour rendre le pipeline deterministe avant lecture. */
void owl_ui_post_text_input(OwlControl* edit, const char* utf8);
void owl_ui_post_mouse_click(OwlControl* c, int button, int x, int y, int dbl);
void owl_ui_pump_events(int max_ms);

/*  — clavier (events surToucheAppuyee/surToucheRelachee + natives
 * ToucheEnfoncee/EnvoieTouche).
 *
 * Callback typee 1 entier : code touche (valeur OWL = tk* pour les touches
 * speciales, code ASCII pour les touches textuelles, cf. owl_ui_qt_key_to_owl).
 *
 * owl_ui_install_key_filter : installe le filtre clavier sur un widget (champ
 *   ou fenetre) ; press -> on_down, release -> on_up. Le filtre maintient aussi
 *   un set global des touches enfoncees (consomme par owl_ui_key_is_down).
 * owl_ui_install_window_key_filter : meme chose sur la fenetre racine, avec
 *   propagation : un event clavier d'un champ enfant SANS handler remonte au
 *   handler fenetre (modele bubble-up).
 * owl_ui_key_is_down : etat instantane d'une touche (ToucheEnfoncee). En
 *   l'absence de filtre installe / hors interaction reelle : 0 (Faux).
 *   Les codes OWL_TK_MOUSE_* sont routes vers l'etat des boutons souris
 *   (QGuiApplication::mouseButtons) au lieu de l'etat clavier.
 * owl_ui_post_key : poste un cycle press+release d'une touche au widget focuse
 *   de la fenetre (EnvoieTouche en mode reel). Le caller pompe ensuite. */
/* Codes sentinelles boutons souris (teBoutonG/teBoutonD). Hors
 * plage Qt::Key : ne collisionnent jamais avec un code touche clavier. */
#define OWL_TK_MOUSE_LEFT   0x02000000
#define OWL_TK_MOUSE_RIGHT  0x02000001

typedef void (*OwlEventCb1i)(OwlControl* sender, void* userdata, int a);

/*  — style typé résolu d'un Champ Onglet transmis à la couche Qt.
 * Miroir plat de OwlOngletStyleBrick (ui_decl.h), découplé pour ne pas tirer
 * ui_decl.h dans la couche Qt. CadreExtérieur = fond + bordure du pane ;
 * VoletActif/VoletInactif = en-têtes des languettes (base + surcharge état
 * survol). Encodage couleur
 * IDENTIQUE au style menu (OWL_MENU_COL_*). Sentinelle -1 / NULL = « non posé »
 * -> repli au défaut du thème, prop par prop, au rendu QSS. */
typedef struct {
    int64_t     couleur;          /* texte, BGR (+ alpha) ; -1 = non posé */
    int64_t     couleur_fond;     /* fond, BGR (+ alpha) ; -1 = non posé */
    const char* police_nom;       /* famille ; NULL = non posé */
    int         police_taille;    /* points ; -1 = non posé */
    int         police_gras;      /* -1 non posé, 0/1 */
    int         police_italique;  /* -1 non posé, 0/1 */
    int         police_souligne;  /* -1 non posé, 0/1 */
    int         police_barre;     /* -1 non posé, 0/1 */
    int         arrondi;          /* arrondi des coins de la languette, px ; -1 = non posé */
} OwlOngletStyleSurface;

/*  — sous-élément CadreExtérieur résolu (la boite du pane,
 * QTabWidget::pane) : fond + bordure. Miroir plat de OwlOngletCadreExt
 * (ui_decl.h). Sentinelle -1 = « non posé » -> repli au défaut du thème. */
typedef struct {
    int64_t couleur_fond;    /* fond du pane, BGR (+ alpha) ; -1 = non posé */
    int64_t cadre_couleur;   /* couleur de la bordure, BGR (+ alpha) ; -1 = non posé */
    int     cadre_epaisseur; /* épaisseur de la bordure, px ; -1 = non posé */
    int     cadre_arrondi;   /* arrondi des coins, px ; -1 = non posé */
} OwlOngletCadreExtResolved;

typedef struct {
    int                   has_style;              /* 0 = aucun style posé */
    OwlOngletCadreExtResolved cadre_ext;          /* fond + bordure du pane */
    OwlOngletStyleSurface actif_base;
    OwlOngletStyleSurface actif_survol;
    OwlOngletStyleSurface inactif_base;
    OwlOngletStyleSurface inactif_survol;
} OwlOngletStyleResolved;

/* ------- Champ Onglet (QTabWidget) + Volets (pages) -------
 *
 * Un Champ Onglet est un QTabWidget ; chaque Volet est une page (QWidget avec
 * son propre layout, cf owl_ui_container_set_layout) ajoutee par addTab. La page
 * retient sa barre + son index (setProperty) pour les setters Volet. Position :
 * OWL_POS_* (haut/bas/gauche/droite) -> QTabWidget::North/South/West/East. */
OwlControl* owl_ui_onglet_create(OwlWindow* parent, int x, int y, int w, int h);
void        owl_ui_onglet_set_position(OwlControl* onglet, int pos);
/*  — applique le style typé résolu sur le QTabWidget (feuille QSS :
 * pane + languettes actives/inactives + survol). `style` NULL ou has_style==0 :
 * no-op (rendu par défaut du thème inchangé). */
void        owl_ui_onglet_apply_style(OwlControl* onglet,
                                      const OwlOngletStyleResolved* style);
/*  — reglages Detail. Largeur des languettes (OWL_LARGVOL_* ;
 * largeurEgale = uniforme, proportionnelle = natif) et alignement horizontal du
 * texte des languettes (OWL_ALIGN_* ; centre = natif). No-op pour les valeurs
 * "defaut" natives. A poser avant l'ajout des volets pour la largeur egale. */
void        owl_ui_onglet_set_tab_width_mode(OwlControl* onglet, int mode);
void        owl_ui_onglet_set_label_alignment(OwlControl* onglet, int align);
OwlControl* owl_ui_onglet_volet_create(void);
void        owl_ui_onglet_add_volet(OwlControl* onglet, OwlControl* page,
                                    const char* libelle_utf8);
/* setCurrentIndex avec signaux bloques (0-based) : anti-double-fire de
 * surChangementVolet sur set programmatique de ..Valeur. */
void        owl_ui_onglet_set_current(OwlControl* onglet, int index0);
/* Index courant (0-based) du QTabWidget en mode GUI ; 0 si absent/incoherent. */
int         owl_ui_onglet_get_current(OwlControl* onglet);
/* Cable QTabWidget::currentChanged(int) — clic utilisateur uniquement (les set
 * programmatiques bloquent les signaux). Le callback recoit l'index 0-based. */
void        owl_ui_onglet_on_change(OwlControl* onglet, OwlEventCb1i cb,
                                    void* userdata);
/* Setters Volet : `page` = widget de la page (uici->widget du Volet). */
void        owl_ui_volet_set_libelle(OwlControl* page, const char* text_utf8);
void        owl_ui_volet_set_visible(OwlControl* page, int visible);

void owl_ui_install_key_filter(OwlControl* c,
                               OwlEventCb1i on_down, void* down_data,
                               OwlEventCb1i on_up,   void* up_data);
void owl_ui_install_window_key_filter(OwlWindow* w,
                                      OwlEventCb1i on_down, void* down_data,
                                      OwlEventCb1i on_up,   void* up_data);
int  owl_ui_key_is_down(int owl_code);
void owl_ui_post_key(OwlWindow* w, int owl_code);
void owl_ui_control_set_tooltip(OwlControl* c, const char* tooltip_utf8);
const char* owl_ui_control_get_tooltip(OwlControl* c);

/* Specifiques bouton */
void owl_ui_button_set_shortcut(OwlControl* btn, const char* shortcut_utf8);
const char* owl_ui_button_get_shortcut(OwlControl* btn);

/*  — raccourci "donne le focus" pour les champs interactifs non-bouton. */
void owl_ui_set_focus_shortcut(OwlControl* c, const char* shortcut_utf8);
const char* owl_ui_get_focus_shortcut(OwlControl* c);
void owl_ui_button_set_icon(OwlControl* btn, const char* path_utf8);
const char* owl_ui_button_get_icon(OwlControl* btn);

/* Position de l'icone par rapport au texte (feuille Style..Image..Position).
 * La valeur est un E_PositionImageBouton (posBtnImage*). Installe un QProxyStyle
 * custom sur le bouton au premier appel. */
void owl_ui_button_set_position_image(OwlControl* btn, int position);
int  owl_ui_button_get_position_image(OwlControl* btn);

/* TypeBouton : encapsule la semantique canon.
 *   0 TBNormal       — rien
 *   1 TBValider      — setDefault(true), Enter natif
 *   2 TBInterruption — V2
 *   3 TBAbandon      — shortcut Esc
 *   4 TBAide         — shortcut F1
 * Stocke la valeur en dynamic property pour permettre le read-back.
 * Reset le default+shortcut precedents avant d'appliquer (idempotent). */
void owl_ui_button_set_type_bouton(OwlControl* btn, int type_bouton);
int  owl_ui_button_get_type_bouton(OwlControl* btn);

/* Specifiques libelle */
void owl_ui_label_set_word_wrap(OwlControl* lbl, int wrap);
int  owl_ui_label_get_word_wrap(OwlControl* lbl);

/* Bucket E — texte riche : pose le contenu en mode rich text si rich != 0
 * (HTML interprete via Qt::RichText), sinon en mode plain text Qt::PlainText.
 * Le contenu doit etre du HTML deja transpile (cf owl_text_riche_to_html). */
void owl_ui_label_set_text_rich(OwlControl* lbl, const char* html_utf8);
void owl_ui_lineedit_set_text_rich(OwlControl* edit, const char* html_utf8);

/* Cadrage (alignement) horizontal/vertical. Valeurs OWL :
 *   horizontal : 0=chGauche, 1=chCentre, 2=chDroite
 *   vertical   : 0=cvHaut,   1=cvCentre, 2=cvBas
 * Le widget doit avoir un attribut alignment (QLabel/QLineEdit oui,
 * QPushButton non — silently no-op). */
void owl_ui_control_set_h_align(OwlControl* w, int h);
int  owl_ui_control_get_h_align(OwlControl* w);
void owl_ui_control_set_v_align(OwlControl* w, int v);
int  owl_ui_control_get_v_align(OwlControl* w);

/* Etat unifie : combinaison de Visible + Grise (et ReadOnly pour Saisie).
 *   0 Actif              — Visible=true,  Enabled=true,  (ReadOnly=false)
 *   1 AffichageSeulement — Visible=true,  Enabled=true,  ReadOnly=true (Saisie),
 *                          sinon Enabled=false comme Inactif (V1.5).
 *   2 Grise              — Visible=true,  Enabled=false
 *   3 Invisible          — Visible=false
 * Read-back deduit l'etat depuis Visible/Enabled/ReadOnly. */
void owl_ui_control_set_etat(OwlControl* w, int etat);
int  owl_ui_control_get_etat(OwlControl* w);

/* Cadre compose etendu (canon). Spec C-compat passee au
 * backend Qt6. Tous les champs en INT ; -1 = "Indefini" / non override.
 *
 * Scalaires globaux : couleur, epaisseur, line_style, arrondi, edges.
 *   Si trait_X_* override est defini (>= 0), il prend le pas sur le scalaire
 *   pour ce bord. Idem coin_X_* override l'arrondi global.
 *
 * Mapping Qt6 :
 *   - border-X-color / border-X-style / border-X-width : par bord
 *   - border-X-Y-radius : Wpx Hpx par coin
 *   - sentinels Indefini -> skip emission CSS pour cette prop. */
typedef struct {
    /* Scalaires (defauts canon : invisible 0, noir, continu, droits, tous bords) */
    int color, thickness, line_style, corner_radius, edges;
    /*  — alpha (0..255) de `color` (bordure). Instance Couleur translucide
     * portee jusqu'au border-color rgba(...). Toujours renseigne par
     * cadre_instance_to_spec (255 = opaque). Ininfluent tant que thickness<=0 :
     * la border-color n'est emise que pour un liseré visible. */
    int color_alpha;
    /* Per-bord overrides : si line_type != 0 OU thick != 0, prend le pas.
     *  — alpha par bord (0..255, 255 = opaque), renseigné par read_trait
     * depuis une instance Couleur translucide. Application per-bord = chantier
     * séparé (l'ingestion est faite ici pour ne plus lire l'entier nu). */
    int trait_haut_color, trait_haut_thick, trait_haut_type, trait_haut_alpha;
    int trait_bas_color,  trait_bas_thick,  trait_bas_type,  trait_bas_alpha;
    int trait_gauche_color, trait_gauche_thick, trait_gauche_type, trait_gauche_alpha;
    int trait_droite_color, trait_droite_thick, trait_droite_type, trait_droite_alpha;
    /* Per-coin overrides : si w > 0 OU h > 0, prend le pas */
    int coin_hg_w, coin_hg_h;   /* CoinHautGauche */
    int coin_hd_w, coin_hd_h;   /* CoinHautDroite */
    int coin_bg_w, coin_bg_h;   /* CoinBasGauche */
    int coin_bd_w, coin_bd_h;   /* CoinBasDroite */
} OwlCadreSpec;

/*  — applique le styleConteneur (sous-element CadreExtérieur) sur le
 * QWidget conteneur : fond + bordure + retraits, en QSS scope `#objectName`
 * pour ne PAS cascader sur les enfants. bg BGR canon (-1 = pas de fond) ;
 * border NULL/thickness<=0 = pas de bordure ; retraits <0 = unset. */
void owl_ui_container_set_style(OwlControl* container, int bg, int bgAlpha,
                                const OwlCadreSpec* border,
                                int padL, int padT, int padR, int padB);

/*  — applique le CadreExtérieur d'un styleConteneur sur le panneau
 * central de la Fenêtre (QWidget#owlCentral), même mécanique QSS scope +
 * WA_StyledBackground que le Conteneur. Le Style prime sur CouleurFond plat
 * (appliqué plus tard).  — gradType/gradAngle/gradStart/gradEnd :
 * un dégradé actif (linéaire/radial) peint le fond à la place du fond uni ;
 * gradType = dégradéAucun -> fond uni bg comme avant. */
void owl_ui_window_set_panel_style(OwlWindow* w, int bg, int bgAlpha,
                                   const OwlCadreSpec* border,
                                   int padL, int padT, int padR, int padB,
                                   int gradType, int gradAngle,
                                   int gradStart, int gradStartAlpha,
                                   int gradEnd, int gradEndAlpha);

/* Apply un Cadre compose sur n'importe quel QWidget (QLabel/QPushButton/
 * QLineEdit/QFrame/QTableView). Genere QSS avec selector pour ne pas
 * cascader aux enfants. */
void owl_ui_widget_set_cadre(OwlControl* w, const OwlCadreSpec* spec);

/*  — sous-elements de style profond du Saisie (styleSaisie refondu).
 * ensure_wrapper materialise (always-wrap) le conteneur CadreExtérieur autour du
 * champ et le retourne ; get_label retourne le QLabel du libelle integre (ou
 * NULL) ; wrapper_set_style pose le fond + la bordure du CadreExtérieur en QSS
 * `#objectName`-scope (sans cascader sur les enfants du wrapper). */
OwlControl* owl_ui_saisie_ensure_wrapper(OwlControl* edit);
OwlControl* owl_ui_saisie_get_label(OwlControl* edit);
void owl_ui_saisie_wrapper_set_style(OwlControl* wrapperCtrl, int fg, int bg,
                                     int bgAlpha, const OwlCadreSpec* border);

/* Image Libelle : QPixmap charge depuis fichier. Path stocke en dynamic
 * property pour read-back. */
void        owl_ui_label_set_image(OwlControl* lbl, const char* path_utf8);
const char* owl_ui_label_get_image(OwlControl* lbl);

/* Image Libelle / Bouton / Fenetre depuis buffer RGBA8 (top-down,
 * row-major, 4 octets/pixel). Used pour afficher un ObjImage OWL en
 * memoire (cf stdlib_image_get_rgba). cadrage : 0=centre (taille native,
 * centre), 1=etire (deforme pour remplir), 2=homothetique (scale +
 * ratio + centre + bandes). cadrage ignore par button/window (icones).*/
void owl_ui_label_set_image_rgba(OwlControl* lbl,
                                 const unsigned char* rgba,
                                 int w, int h, int cadrage);
void owl_ui_button_set_icon_rgba(OwlControl* btn,
                                 const unsigned char* rgba, int w, int h);
/*  /  — surcharge d'icone par etat (mode : 0 = survol via
 * event filter, 1 = grisé/QIcon::Disabled). A appeler apres
 * owl_ui_button_set_icon_rgba. */
void owl_ui_button_set_icon_rgba_state(OwlControl* btn,
                                       const unsigned char* rgba,
                                       int w, int h, int mode);
/*  — getter test-only : 1 si une icone survol est armee (event filter
 * installe + icone survol stockee) sur le bouton. */
int owl_ui_button_has_hover_icon(OwlControl* btn);
struct OwlWindow;
void owl_ui_window_set_icon_rgba(struct OwlWindow* win,
                                 const unsigned char* rgba, int w, int h);

/*  — QTimer parenté à la fenêtre `parent` (mort avec elle). Tire
 * pendant la boucle d'événements. `onTick(owlInstance)` ré-entre dans la VM
 * pour appeler la procédure du timer. `repeat == 0` => one-shot. */
void owl_ui_qt_timer_start(struct OwlWindow* parent, void* owlInstance,
                           int intervalMs, int repeat,
                           void (*onTick)(void*));
void owl_ui_qt_timer_stop(void* owlInstance);
/* Propage un ..Intervalle / ..Répète modifié à chaud au QTimer vivant.
 * setInterval/setSingleShot prennent effet au cycle suivant (sémantique
 * « pris en compte au tic suivant »). No-op si aucun QTimer associé. */
void owl_ui_qt_timer_update(void* owlInstance, int intervalMs, int repeat);

/* Cadrage Libelle : applique a une image deja posee via setPixmap.
 * Stocke comme dynamic property pour read-back. Effet visible au prochain
 * setPixmap ou si le pixmap source est deja stocke. */
void owl_ui_label_set_cadrage(OwlControl* lbl, int cadrage);
int  owl_ui_label_get_cadrage(OwlControl* lbl);

/* ============================================================
 * Champ Image V2 — widget dedie avec events typés + Zoom.
 *
 * Backend : OwlImageLabel (subclass QLabel) qui override les events
 * souris et roulette. Pas de QScrollArea pour V2 minimum (ajout V2.5).
 * ============================================================ */

/* Callback signatures pour les events du Champ Image. */
typedef void (*OwlEventCb0)(OwlControl* sender, void* userdata);
typedef void (*OwlEventCb2i)(OwlControl* sender, void* userdata, int a, int b);
typedef void (*OwlEventCb3i)(OwlControl* sender, void* userdata, int a, int b, int c);

/*  — event fenetre surChangementTaille / onResize : installe un
 * filtre sur le QResizeEvent de la fenetre. Le callback recoit (largeur,
 * hauteur) en pixels. Ne tire que sur changement effectif de taille (les
 * resize redondants de meme dimension, ex. exposition initiale, sont
 * filtres). */
void owl_ui_install_window_resize_filter(OwlWindow* w,
                                         OwlEventCb2i cb, void* userdata);

OwlControl* owl_ui_image_field_create(OwlWindow* parent,
                                       int x, int y, int w, int h);

/* Reuse owl_ui_label_set_image* / set_cadrage : OwlImageLabel herite de
 * QLabel donc ces setters s'appliquent identiquement. */

/* Prop Zoom : pourcentage 1..1000, 100 = taille native, 200 = double,
 * 50 = moitie. Re-applique au prochain setPixmap. */
void owl_ui_image_field_set_zoom(OwlControl* w, int percent);

/* V2 events Champ Image — chaque on_X register un callback typé. */
void owl_ui_image_field_on_clic              (OwlControl* w, OwlEventCb0 cb,  void* d);
void owl_ui_image_field_on_double_clic       (OwlControl* w, OwlEventCb0 cb,  void* d);
void owl_ui_image_field_on_entree_survol     (OwlControl* w, OwlEventCb0 cb,  void* d);
void owl_ui_image_field_on_sortie_survol     (OwlControl* w, OwlEventCb0 cb,  void* d);
void owl_ui_image_field_on_survol            (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_bouton_gauche     (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_relache_btn_gauche(OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_bouton_droit      (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_relache_btn_droit (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_double_clic_droit (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_molette           (OwlControl* w, OwlEventCb3i cb, void* d);
void owl_ui_image_field_on_menu_contextuel   (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_deplacement       (OwlControl* w, OwlEventCb2i cb, void* d);
void owl_ui_image_field_on_changement_taille (OwlControl* w, OwlEventCb2i cb, void* d);

/* ============================================================
 * Champ Liste — V1 mono-colonne texte simple.
 *
 * Backend : QListWidget. Items sont des chaines UTF-8. Index canon
 * = 1-based. La fonction _create installe un mode de selection
 * SingleSelection par defaut ; basculer via set_multiselect.
 * ============================================================ */

OwlControl* owl_ui_list_create(OwlWindow* parent, int x, int y, int w, int h);

/* Manipulation des items. Index 1-based.
 * stored_utf8 : valeur memorisee optionnelle (peut etre NULL = pas de
 *   stocked, fallback sur l'index 1-based pour ..ValeurMemorisee).
 * image_path_utf8 : V2 (Liste Image), accepte mais ignore en V1. */
void owl_ui_list_add_item   (OwlControl* lst, const char* text_utf8,
                              const char* stored_utf8,
                              const char* image_path_utf8);
void owl_ui_list_insert_item(OwlControl* lst, int index_1based,
                              const char* text_utf8,
                              const char* stored_utf8,
                              const char* image_path_utf8);
void owl_ui_list_remove_item(OwlControl* lst, int index_1based);
void owl_ui_list_clear      (OwlControl* lst);
void owl_ui_list_modify_item(OwlControl* lst, int index_1based,
                              const char* text_utf8,
                              const char* stored_utf8,
                              const char* image_path_utf8);
int  owl_ui_list_count      (OwlControl* lst);

/* Lecture de la valeur memorisee (UserRole) pour l'index 1-based.
 * Retourne NULL si index hors bornes ou pas de valeur stockee. */
const char* owl_ui_list_get_stored(OwlControl* lst, int index_1based);

/* Pointeur retourne valide jusqu'au prochain appel a get_item (buffer
 * statique). Caller doit copier s'il a besoin de stocker. NULL si
 * index hors bornes. */
const char* owl_ui_list_get_item(OwlControl* lst, int index_1based);

/* Recherche text -> index 1-based ou -1 si non trouve. Match exact
 * case-sensitive. */
int  owl_ui_list_find(OwlControl* lst, const char* text_utf8);

/* Tri programmatique. ascending : 0/1. */
void owl_ui_list_sort(OwlControl* lst, int ascending);

/* Selection (mono ou multi). Lecture : index 1-based courant (premiere
 * selection en multisel) ; 0 = aucune. */
int  owl_ui_list_get_selection(OwlControl* lst);
void owl_ui_list_set_selection(OwlControl* lst, int index_1based);
void owl_ui_list_set_multiselect(OwlControl* lst, int multi);
int  owl_ui_list_get_multiselect(OwlControl* lst);

/* Tri automatique a l'ajout (canon Triée). */
void owl_ui_list_set_sorted(OwlControl* lst, int sorted);
int  owl_ui_list_get_sorted(OwlControl* lst);

/* Contenu complet en ecriture, lignes separees par '\n'. set_content
 * remplace tout le contenu courant. */
void owl_ui_list_set_content(OwlControl* lst, const char* multiline_utf8);

/* Refresh — provoque un repaint complet. V1 mostly no-op (Qt batch deja),
 * sera utile en V2 pour Liste fichier (re-sync depuis HFSQL). */
void owl_ui_list_refresh(OwlControl* lst);

/*  — style typé du Champ Liste (styleListe). Applique en UNE feuille
 * QSS sur le QListWidget, decomposee par sous-element :
 *   - CadreExtérieur     -> fond + bordure de la liste (QListWidget-scope).
 *   - LibelléLignes      -> couleur + police du texte des lignes (::item).
 *   - LigneSélectionnée  -> couleur + fond du texte de la ligne selectionnee
 *     (::item:selected).
 * Sentinelles -1 = feuille non posee (skip emission). font_* : -1/0 non pose. */
typedef struct {
    /* CadreExtérieur (cadre + fond de la liste) */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* LibelléLignes (texte des lignes, ::item) */
    int row_fg;
    int row_fg_alpha;                 /* 0-255, 255 = opaque.  */
    int row_bg;                       /* CouleurFond des lignes, -1 = non pose */
    int row_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const char* row_font_family;      /* NULL/"" = herite */
    int row_font_size;                /* <=0 = herite */
    int row_bold;                     /* -1 non pose, 0/1 */
    int row_italic;                   /* -1 non pose, 0/1 */
    /* LigneSélectionnée (::item:selected) */
    int sel_fg;
    int sel_fg_alpha;                 /* 0-255, 255 = opaque.  */
    int sel_bg;                       /* CouleurFond de la ligne selectionnee, -1 = non pose */
    int sel_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const char* sel_font_family;      /* NULL/"" = herite */
    int sel_font_size;                /* <=0 = herite */
    int sel_bold;                     /* -1 non pose, 0/1 */
    int sel_italic;                   /* -1 non pose, 0/1 */
} OwlListeStyleSpec;

void owl_ui_liste_set_style(OwlControl* lst, const OwlListeStyleSpec* s);

/*  — style typé du Champ Combo (styleCombo). Applique en UNE feuille
 * QSS sur le QComboBox, decomposee par sous-element :
 *   - CadreExtérieur     -> fond + bordure du champ combo (QComboBox-scope).
 *   - ZoneSaisie         -> couleur + police + cadre du champ affiche (QComboBox).
 *   - LibelléLignes      -> couleur + police des items du popup
 *     (QComboBox QAbstractItemView::item).
 *   - LigneSélectionnée  -> couleur + fond de l'item surligné
 *     (QComboBox QAbstractItemView::item:selected).
 *   - Libellé            -> couleur + police du libellé integre (label_fg/font).
 * Sentinelles -1 = feuille non posee (skip emission). font_* : -1/0 non pose. */
typedef struct {
    /* CadreExtérieur (cadre + fond du champ combo) */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* ZoneSaisie (le champ combo lui-meme) */
    int field_fg;
    int field_fg_alpha;               /*  */
    int field_bg;                     /* -1 = non pose */
    int field_bg_alpha;               /*  */
    const char* field_font_family;    /* NULL/"" = herite */
    int field_font_size;              /* <=0 = herite */
    int field_bold;                   /* -1 non pose, 0/1 */
    int field_italic;                 /* -1 non pose, 0/1 */
    /*  — Cadre de la ZoneSaisie (bordure + arrondi du champ combo
     * lui-meme). NULL = feuille non posee. Miroir de styleSaisie..ZoneSaisie..
     * Cadre qui habille le QLineEdit ; ici la cible est le QComboBox. Prime sur
     * ext_border sur le QComboBox (le CadreExtérieur garde le wrapper). */
    const OwlCadreSpec* field_border;
    /* LibelléLignes (texte des items du popup, ::item) */
    int row_fg;
    int row_fg_alpha;                 /*  */
    int row_bg;
    int row_bg_alpha;                 /*  */
    const char* row_font_family;
    int row_font_size;
    int row_bold;
    int row_italic;
    /* LigneSélectionnée (::item:selected) */
    int sel_fg;
    int sel_fg_alpha;                 /*  */
    int sel_bg;
    int sel_bg_alpha;                 /*  */
    const char* sel_font_family;
    int sel_font_size;
    int sel_bold;
    int sel_italic;
    /* Libellé integre (le QLabel du wrapper) */
    int label_fg;
    int label_fg_alpha;               /*  */
    int label_bg;
    int label_bg_alpha;               /*  */
    const char* label_font_family;
    int label_font_size;
    int label_bold;
    int label_italic;
} OwlComboStyleSpec;

void owl_ui_combo_set_style(OwlControl* cmb, const OwlComboStyleSpec* s);

/*  — style typé du Champ Table (styleTable) + override par colonne
 * (styleColonne). Brique texte reutilisable (Couleur + CouleurFond + Police),
 * calquee sur le fragment text_body des styles Liste/Combo :
 *   fg / bg : -1 = feuille non posee (skip emission).
 *   font_family : NULL/"" = herite. font_size : <=0 = herite.
 *   bold / italic : -1 non pose, 0/1. */
typedef struct {
    int fg;
    int fg_alpha;   /* 0-255, 255 = opaque (defaut).  */
    int bg;
    int bg_alpha;   /* 0-255, 255 = opaque (defaut).  */
    const char* font_family;
    int font_size;
    int bold;
    int italic;
} OwlTextBrick;

/* Descripteur complet du styleTable. Chaque brique porte les sentinelles -1 =
 * non posee. Les feuilles absentes ne modifient pas le rendu par defaut. */
typedef struct {
    /* CadreExtérieur -> fond + bordure du QTableView. */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* TitreColonne -> QHeaderView::section (texte des en-tetes). */
    OwlTextBrick titre;
    /* LibelléLignes -> QTableView::item (texte des cellules). */
    OwlTextBrick lignes;
    /* LignesPaires / LignesImpaires -> seul CouleurFond compte (bg). */
    int paires_bg;
    int paires_bg_alpha;      /*  */
    int impaires_bg;
    int impaires_bg_alpha;    /*  */
    /* LigneSélectionnée -> ::item:selected. */
    OwlTextBrick selection;
    /* LigneSurvolée -> ::item:hover. */
    OwlTextBrick survol;
    /* Quadrillage -> gridline-color (-1 = defaut theme). */
    int quadrillage;
    int quadrillage_alpha;    /* 0-255, 255 = opaque.  */
} OwlTableStyleSpec;

/* Applique le styleTable global (QSS sur QTableView + QHeaderView). */
void owl_ui_table_set_style(OwlControl* table, const OwlTableStyleSpec* s);

/* Descripteur d'override de colonne (styleColonne). Sous-ensemble du styleTable
 * SANS CadreExtérieur ni Quadrillage. Les feuilles statiques (titre/lignes/
 * paires/impaires) sont posees par role sur le modele ; les etats dynamiques
 * (selection/survol) via un QStyledItemDelegate installe sur la colonne. */
typedef struct {
    OwlTextBrick titre;
    OwlTextBrick lignes;
    int paires_bg;
    int paires_bg_alpha;      /*  */
    int impaires_bg;
    int impaires_bg_alpha;    /*  */
    OwlTextBrick selection;
    OwlTextBrick survol;
} OwlColumnStyleSpec;

/* Applique un override de style sur la colonne 0-based `col`. `table_fallback`
 * (peut etre NULL) fournit les feuilles heritees du styleTable pour composer
 * les etats du delegate quand une feuille n'est pas posee sur la colonne. */
void owl_ui_table_set_column_style(OwlControl* table, int col_0based,
                                   const OwlColumnStyleSpec* s,
                                   const OwlTableStyleSpec* table_fallback);

/* Etat initial de la table (mappe le radio EtatInitial) :
 *   0 = EnSaisie (editable), 1 = Grisé (disabled),
 *   2 = Sélection (non editable, selectionnable),
 *   3 = NiSaisieNiSélection (non editable, non selectionnable). */
void owl_ui_table_set_etat_initial(OwlControl* table, int etat);

/* Hauteur d'une ligne en pixels (>0). */
void owl_ui_table_set_row_height(OwlControl* table, int height_px);

/* Hauteur de la bande de titre des colonnes, en pixels : 0 = en-tête masqué
 * (la place revient à la zone de données), > 0 = hauteur imposée, < 0 = retour
 * à la hauteur naturelle. */
void owl_ui_table_set_title_height(OwlControl* table, int px);

/* Hauteur rendue de la bande de titre, en pixels. 0 si l'en-tête est masqué. */
int  owl_ui_table_get_title_height(OwlControl* table);

/* Ancre une colonne (1-based) : elle s'étire pour occuper l'espace horizontal
 * restant (désactive le stretch de la dernière colonne par défaut). Index hors
 * bornes = no-op. */
void owl_ui_table_set_stretch_column(OwlControl* table, int col_1based);

/* Marque une colonne (1-based) comme non éditable (Etat = AffichageSeulement).
 * Effet : les cases à cocher des colonnes booléennes de cette colonne restent
 * affichées mais ne basculent plus au clic. editable != 0 = éditable (défaut). */
void owl_ui_table_set_column_editable(OwlControl* table, int col_1based,
                                      int editable);

/*  — libelle integre a gauche du champ combo (composite {QLabel|combo},
 * calque du wrapper Saisie). Retourne le wrapper comme OwlControl* (ou NULL). */
void owl_ui_combo_set_label_text (OwlControl* cmb, const char* text_utf8);
void owl_ui_combo_set_label_width(OwlControl* cmb, int width);
OwlControl* owl_ui_combo_ensure_wrapper(OwlControl* cmb);
OwlControl* owl_ui_combo_get_label(OwlControl* cmb);

/* Events Liste — signatures 0-arg (la valeur courante se lit via Valeur). */
void owl_ui_list_on_selection(OwlControl* lst, OwlCallback cb, void* userdata);
void owl_ui_list_on_entry    (OwlControl* lst, OwlCallback cb, void* userdata);
void owl_ui_list_on_exit     (OwlControl* lst, OwlCallback cb, void* userdata);

/* Multiselection (mode ExtendedSelection) — gestion par indice 1-based. */
void owl_ui_list_select_plus    (OwlControl* lst, int index_1based);
void owl_ui_list_select_moins   (OwlControl* lst, int index_1based);
int  owl_ui_list_select_count   (OwlControl* lst);
/* Retourne l'indice 1-based de la rang_1based-eme ligne selectionnee
 * (par ordre d'apparition dans la liste). 0 si rang invalide. */
int  owl_ui_list_select_at      (OwlControl* lst, int rang_1based);

/* DeplaceLigne : deplace ou echange src vers dest. operation = bitmask
 *   bit 0 (TD_ECHANGE) : echange au lieu de deplacer
 *   bit 1 (TD_AFFICHE) : assure que la ligne modifiee est visible
 * Retourne le nouvel indice 1-based de la ligne deplacee, ou 0 si invalide. */
int  owl_ui_list_move_row(OwlControl* lst, int src_1based, int dest_1based,
                           int op_bitmask);

/* Hit-test : pixel local (x,y) -> indice 1-based de la ligne, -1 si vide. */
int  owl_ui_list_hit_test(OwlControl* lst, int x, int y);

/* Zoom (canon Liste : pourcentage 1..1000, 100 = taille native). Applique
 * un facteur sur la taille de police de la liste. */
void owl_ui_list_set_zoom(OwlControl* lst, int percent);
int  owl_ui_list_get_zoom(OwlControl* lst);

/* OptionTri : bitmask compose des constantes ccXxx (ccSansAccent, ccSansCasse,
 * ccRespecteNumerique, etc., cf doc 3024011). Modifie le comparateur
 * applique a sortItems via une re-implementation custom. */
void owl_ui_list_set_sort_option(OwlControl* lst, int cc_bitmask);
int  owl_ui_list_get_sort_option(OwlControl* lst);

/* ============================================================
 * Champ Combo — V1
 *
 * Backend Qt6 : QComboBox. La majorite des operations ListeXXX dispatchent
 * automatiquement entre Liste et Combo via owl_ui_listcombo_* (helpers
 * generiques). Les natives Combo-specifiques (Ouvre/Ferme dropdown,
 * EnSaisie) ont leur propre API.
 * ============================================================ */

OwlControl* owl_ui_combo_create(OwlWindow* parent, int x, int y, int w, int h);

/* Specifiques Combo */
void owl_ui_combo_set_editable(OwlControl* cmb, int editable);
int  owl_ui_combo_get_editable(OwlControl* cmb);
void owl_ui_combo_set_max_visible(OwlControl* cmb, int max);
int  owl_ui_combo_get_max_visible(OwlControl* cmb);
void owl_ui_combo_open (OwlControl* cmb);  /* force ouverture du dropdown */
void owl_ui_combo_close(OwlControl* cmb);  /* force fermeture */

/* Events Combo : surEntree / surSortie / surSelection / surAChaqueModification
 * + Combo-specifiques surOuverture / surFermeture. */
void owl_ui_combo_on_selection (OwlControl* cmb, OwlCallback cb, void* userdata);
void owl_ui_combo_on_entry     (OwlControl* cmb, OwlCallback cb, void* userdata);
void owl_ui_combo_on_exit      (OwlControl* cmb, OwlCallback cb, void* userdata);
void owl_ui_combo_on_change    (OwlControl* cmb, OwlCallback cb, void* userdata);
void owl_ui_combo_on_open      (OwlControl* cmb, OwlCallback cb, void* userdata);
void owl_ui_combo_on_close     (OwlControl* cmb, OwlCallback cb, void* userdata);

/* ============================================================
 * Champ Selecteur — V1 (radio button group exclusif).
 *
 * Backend Qt6 : QFrame custom contenant QButtonGroup exclusif et N
 * QRadioButton (avec optionnellement QLabel sous-libelle + QIcon).
 * 4 modes d'affichage : standard, sous-libelles, Vista (image), Vista+sous-lib.
 * ============================================================ */

OwlControl* owl_ui_selecteur_create(OwlWindow* parent, int x, int y, int w, int h);

/* Mode d'affichage (cf OWL_SELMODE_*). */
void owl_ui_selecteur_set_display_mode(OwlControl* sel, int mode);
int  owl_ui_selecteur_get_display_mode(OwlControl* sel);

/* Nombre de colonnes (1 = vertical par defaut). >1 = grille. */
void owl_ui_selecteur_set_column_count(OwlControl* sel, int cols);
int  owl_ui_selecteur_get_column_count(OwlControl* sel);

/* Hauteur en pixels de chaque ligne d'option (0 = hauteur naturelle). */
void owl_ui_selecteur_set_option_height(OwlControl* sel, int px);
int  owl_ui_selecteur_get_option_height(OwlControl* sel);

/* Manipulation des options. Index 1-based. */
void owl_ui_selecteur_add_option   (OwlControl* sel,
                                     const char* label_utf8,
                                     const char* sub_label_utf8,
                                     const char* stored_utf8,
                                     const char* image_path_utf8);
void owl_ui_selecteur_insert_option(OwlControl* sel, int index_1based,
                                     const char* label_utf8,
                                     const char* sub_label_utf8,
                                     const char* stored_utf8,
                                     const char* image_path_utf8);
void owl_ui_selecteur_remove_option(OwlControl* sel, int index_1based);
void owl_ui_selecteur_clear        (OwlControl* sel);
int  owl_ui_selecteur_count        (OwlControl* sel);

/* Acces aux fields d'une option. */
const char* owl_ui_selecteur_get_option_label    (OwlControl* sel, int index_1based);
const char* owl_ui_selecteur_get_option_sub_label(OwlControl* sel, int index_1based);
const char* owl_ui_selecteur_get_option_stored   (OwlControl* sel, int index_1based);
void        owl_ui_selecteur_set_option_label    (OwlControl* sel, int index_1based,
                                                   const char* label_utf8);
void        owl_ui_selecteur_set_option_sub_label(OwlControl* sel, int index_1based,
                                                   const char* sub_label_utf8);
void        owl_ui_selecteur_set_option_stored   (OwlControl* sel, int index_1based,
                                                   const char* stored_utf8);

/* Selection courante. */
int  owl_ui_selecteur_get_selection_index   (OwlControl* sel);
void owl_ui_selecteur_set_selection_index   (OwlControl* sel, int index_1based);

/* Events. */
void owl_ui_selecteur_on_change(OwlControl* sel, OwlCallback cb, void* userdata);
void owl_ui_selecteur_on_entry (OwlControl* sel, OwlCallback cb, void* userdata);
void owl_ui_selecteur_on_exit  (OwlControl* sel, OwlCallback cb, void* userdata);

/*  — style typé du Champ Sélecteur (styleSélecteur). Applique en UNE
 * feuille QSS sur le QFrame conteneur, decomposee par sous-element (pendant du
 * styleInterrupteur, adapte radio) :
 *   - CadreExtérieur -> fond + bordure du conteneur (#objectName-scope, ne
 *     cascade pas aux options).
 *   - TexteOptions   -> couleur + police du texte des options (QRadioButton).
 *   - CadrePuce      -> fond + bordure de la boite du rond (QRadioButton::indicator).
 *   - Puce           -> couleur du rond selectionne (approche : fond de
 *     ::indicator:checked, Qt ne stylise pas le glyphe seul).
 * Sentinelles -1 = feuille non posee (skip emission). font_* : -1/0 non pose. */
typedef struct {
    /* CadreExtérieur (conteneur) */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* TexteOptions (texte des options) */
    int txt_fg;
    int txt_fg_alpha;                 /* 0-255, 255 = opaque.  */
    int txt_bg;                       /* CouleurFond du texte des options, -1 = non pose */
    int txt_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const char* txt_font_family;      /* NULL/"" = herite */
    int txt_font_size;                /* <=0 = herite */
    int txt_bold;                     /* -1 non pose, 0/1 */
    int txt_italic;                   /* -1 non pose, 0/1 */
    /* CadrePuce (boite du rond, ::indicator) */
    int puce_bg;
    int puce_bg_alpha;                /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* puce_border;  /* NULL = pas de bordure */
    /* Puce (rond selectionne, ::indicator:checked) */
    int puce_mark;
    int puce_mark_alpha;              /* 0-255, 255 = opaque.  */
} OwlSelecteurStyleSpec;

void owl_ui_selecteur_set_style(OwlControl* sel, const OwlSelecteurStyleSpec* s);

/* ============================================================
 * Champ Interrupteur — V1 (checkbox group multi-select).
 *
 * Backend Qt6 : QFrame custom + N QCheckBox indépendantes (pas de
 * QButtonGroup — chaque option est cochée/décochée indépendamment).
 * Memes 4 modes que Selecteur. ..Valeur = bitmask entier (option N -> bit N-1).
 * ============================================================ */

OwlControl* owl_ui_interrupteur_create(OwlWindow* parent, int x, int y, int w, int h);

void owl_ui_interrupteur_set_display_mode(OwlControl* it, int mode);
int  owl_ui_interrupteur_get_display_mode(OwlControl* it);
void owl_ui_interrupteur_set_column_count(OwlControl* it, int cols);
int  owl_ui_interrupteur_get_column_count(OwlControl* it);

/* Hauteur en pixels de chaque ligne d'option. 0 = hauteur naturelle (auto). */
void owl_ui_interrupteur_set_option_height(OwlControl* it, int px);
int  owl_ui_interrupteur_get_option_height(OwlControl* it);
void owl_ui_interrupteur_set_three_states(OwlControl* it, int enabled);
int  owl_ui_interrupteur_get_three_states(OwlControl* it);

/* Manipulation des options. Index 1-based. Pas de stored : la valeur c'est
 * le bit N-1 du bitmask de selection. */
void owl_ui_interrupteur_add_option   (OwlControl* it,
                                        const char* label,
                                        const char* sub_label,
                                        const char* image_path);
void owl_ui_interrupteur_insert_option(OwlControl* it, int idx,
                                        const char* label,
                                        const char* sub_label,
                                        const char* image_path);
void owl_ui_interrupteur_remove_option(OwlControl* it, int idx);
void owl_ui_interrupteur_clear        (OwlControl* it);
int  owl_ui_interrupteur_count        (OwlControl* it);

/* Acces aux fields d'une option. */
const char* owl_ui_interrupteur_get_option_label    (OwlControl* it, int idx);
const char* owl_ui_interrupteur_get_option_sub_label(OwlControl* it, int idx);
void        owl_ui_interrupteur_set_option_label    (OwlControl* it, int idx, const char* s);
void        owl_ui_interrupteur_set_option_sub_label(OwlControl* it, int idx, const char* s);

/* Etat coche/decoche/indetermine. -1 = indetermine, 0 = decoche, 1 = coche. */
int  owl_ui_interrupteur_get_option_state(OwlControl* it, int idx);
void owl_ui_interrupteur_set_option_state(OwlControl* it, int idx, int state);

/* Mode tri-etat PAR OPTION (l'option gere-t-elle l'etat indetermine -1). */
int  owl_ui_interrupteur_get_option_three_states(OwlControl* it, int idx);
void owl_ui_interrupteur_set_option_three_states(OwlControl* it, int idx, int enabled);

/* Bitmask global (somme des 2^(i-1) pour les options i cochees). */
int64_t owl_ui_interrupteur_get_bitmask(OwlControl* it);
void    owl_ui_interrupteur_set_bitmask(OwlControl* it, int64_t mask);

/* Events. */
void owl_ui_interrupteur_on_change(OwlControl* it, OwlCallback cb, void* userdata);
void owl_ui_interrupteur_on_entry (OwlControl* it, OwlCallback cb, void* userdata);
void owl_ui_interrupteur_on_exit  (OwlControl* it, OwlCallback cb, void* userdata);

/*  — style typé du Champ Interrupteur (styleInterrupteur). Applique en
 * UNE feuille QSS sur le QFrame conteneur, decomposee par sous-element :
 *   - CadreExtérieur -> fond + bordure du conteneur (#objectName-scope, ne
 *     cascade pas aux cases).
 *   - TexteOptions   -> couleur + police du texte des cases (selecteur QCheckBox).
 *   - CadreCoche     -> fond + bordure de la boite de coche (QCheckBox::indicator).
 *   - Coche          -> couleur de la marque cochée (approche : fond de
 *     ::indicator:checked, Qt ne stylise pas le glyphe seul).
 * Sentinelles -1 = feuille non posee (skip emission). font_* : -1/0 non pose. */
typedef struct {
    /* CadreExtérieur (conteneur) */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* TexteOptions (texte des cases) */
    int txt_fg;
    int txt_fg_alpha;                 /* 0-255, 255 = opaque.  */
    int txt_bg;                       /* CouleurFond du texte des cases, -1 = non pose */
    int txt_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const char* txt_font_family;      /* NULL/"" = herite */
    int txt_font_size;                /* <=0 = herite */
    int txt_bold;                     /* -1 non pose, 0/1 */
    int txt_italic;                   /* -1 non pose, 0/1 */
    /* CadreCoche (boite de la coche, ::indicator) */
    int coche_bg;
    int coche_bg_alpha;               /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* coche_border; /* NULL = pas de bordure */
    /* Coche (marque cochée, ::indicator:checked) */
    int coche_mark;
    int coche_mark_alpha;             /* 0-255, 255 = opaque.  */
} OwlInterrupteurStyleSpec;

void owl_ui_interrupteur_set_style(OwlControl* it, const OwlInterrupteurStyleSpec* s);

/* ============================================================
 * Champ Jauge — V1 (barre de progression output-only).
 *
 * Backend Qt6 : QProgressBar (sous-classe OwlJauge pour le rendu symetrique).
 * ..Valeur = entier borne a [BorneMin, BorneMax] (QProgressBar clampe
 * nativement au set). Variantes lineaire horizontale (defaut) / verticale +
 * symetrique (progression depuis le centre vers les bords).
 * ============================================================ */

OwlControl* owl_ui_jauge_create(OwlWindow* parent, int x, int y, int w, int h);

/* Plage [min, max]. La Valeur courante est re-clampee par Qt. */
void owl_ui_jauge_set_range(OwlControl* g, int min_val, int max_val);
int  owl_ui_jauge_get_min(OwlControl* g);
int  owl_ui_jauge_get_max(OwlControl* g);

/* Valeur courante (clampee a [min, max] par le backend). */
void owl_ui_jauge_set_value(OwlControl* g, int value);
int  owl_ui_jauge_get_value(OwlControl* g);

/* Orientation : 1 = verticale (remplissage bas->haut), 0 = horizontale. */
void owl_ui_jauge_set_vertical(OwlControl* g, int vertical);
int  owl_ui_jauge_get_vertical(OwlControl* g);

/* Cadrage horizontal du texte de progression (sentinel OWL_CH_*). Le rendu
 * peint le texte lui-meme -> alignement dedie, distinct du set_h_align
 * generique (no-op sur un QProgressBar). Defaut = chCentre. */
void owl_ui_jauge_set_h_align(OwlControl* g, int h);
int  owl_ui_jauge_get_h_align(OwlControl* g);

/* Mode symetrique : 1 = progression depuis le centre, 0 = min->max classique. */
void owl_ui_jauge_set_symmetric(OwlControl* g, int symmetric);
int  owl_ui_jauge_get_symmetric(OwlControl* g);

/* Couleur de la portion remplie (BGR int canon, -1 = defaut theme). */
void owl_ui_jauge_set_progress_color(OwlControl* g, int bgr);
int  owl_ui_jauge_get_progress_color(OwlControl* g);

/* Texte affiche a la place du pourcentage (NULL/"" = pourcentage auto). */
void owl_ui_jauge_set_text(OwlControl* g, const char* text_utf8);
const char* owl_ui_jauge_get_text(OwlControl* g);

/*  — style typé du Champ Jauge (styleJauge). Rendu HYBRIDE :
 *   - CadreExtérieur (piste + bordure) -> QSS `QProgressBar {}` + palette Base
 *     (le mode symetrique peint la piste via QPalette::Base, hors QSS).
 *   - Remplissage (barre)              -> QSS `QProgressBar::chunk {}` +
 *     m_progressColor (le mode symetrique peint le chunk custom).
 *   - Libellé (couleur texte)          -> palette Text (le paintEvent dessine
 *     le texte au pen QPalette::Text, un QSS `color:` ne suffit pas).
 * Sentinelles -1 = feuille non posee. */
typedef struct {
    int ext_bg;                       /* CadreExtérieur..CouleurFond, -1 = non pose */
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* CadreExtérieur..Cadre, NULL = pas de bordure */
    int fill;                         /* Remplissage..CouleurFond, -1 = non pose */
    int fill_alpha;                   /* 0-255, 255 = opaque.  */
    int label_fg;                     /* Libellé..Couleur, -1 = non pose */
    int label_fg_alpha;               /*  — alpha du texte %, 255 = opaque */
    /* Libellé..Police ( lot4) — pointe la QFont du texte %. Meme
     * convention de sentinelle que OwlTextBrick : famille NULL / taille 0 =
     * non posee, drapeaux -1 = non pose (0/1 sinon). */
    const char* label_font_family;    /* Libellé..Police..Nom, NULL = non pose */
    int label_font_size;              /* Libellé..Police..Taille, 0 = non pose */
    int label_bold;                   /* Libellé..Police..Gras, -1 = non pose */
    int label_italic;                 /* Libellé..Police..Italique, -1 = non pose */
    int label_underline;              /* Libellé..Police..Souligné, -1 = non pose */
    int label_strike;                 /* Libellé..Police..Barré, -1 = non pose */
} OwlJaugeStyleSpec;

void owl_ui_jauge_set_style(OwlControl* g, const OwlJaugeStyleSpec* s);

/* ============================================================
 * Champ ZoneRepetee (Looper) — V1 par programmation.
 *
 * Backend Qt6 : QFrame top-level + QScrollArea + QWidget central +
 * QVBoxLayout (Sens=zrVertical, unique valeur).
 * Chaque repetition = un QWidget (typiquement un OwlContainer construit
 * via build_control sur une copie de OwlControlDecl* template).
 *
 * Le clonage du template + l'instanciation des sub-widgets est faite cote
 * stdlib_ui.c (qui a access a build_control). Ce backend expose une API
 * neutre qui ne sait pas ce qu'est un template — juste des QWidget
 * enfants. Le mapping (rep_index, sub_name) -> OwlControl* pour
 * l'indexation ZR[i].sub..prop est tenu cote stdlib_ui.c aussi, dans
 * une table associee au OwlControl* ZR via dynamic property.
 * ============================================================ */

OwlControl* owl_ui_zr_create(OwlWindow* parent, int x, int y, int w, int h);

/* Multi-selection : 0 = mono (defaut), 1 = multi (Ctrl/Shift+clic). */
void owl_ui_zr_set_multi_select(OwlControl* zr, int multi);
int  owl_ui_zr_get_multi_select(OwlControl* zr);

/* Manipulation des repetitions. rep_widget doit etre un QWidget enfant
 * deja cree (typiquement un OwlContainer). add_rep l'insere a la fin et
 * retourne l'indice 1-based. insert_rep avant l'indice donne (1-based). */
int  owl_ui_zr_add_rep   (OwlControl* zr, OwlControl* rep_widget);
int  owl_ui_zr_insert_rep(OwlControl* zr, int idx_1based, OwlControl* rep_widget);
void owl_ui_zr_remove_rep(OwlControl* zr, int idx_1based);
void owl_ui_zr_clear     (OwlControl* zr);
int  owl_ui_zr_count     (OwlControl* zr);

/* Recupere le widget d'une repetition. NULL si idx hors bornes. */
OwlControl* owl_ui_zr_get_rep(OwlControl* zr, int idx_1based);

/* Bridge : permet d'utiliser un widget repetition (Conteneur) comme parent
 * dans les owl_ui_*_create (le central widget de la rep accepte les enfants
 * du template). */
OwlWindow* owl_ui_zr_rep_as_parent(OwlControl* rep_widget);

/* Selection courante (ligne 1-based ; 0 = aucune). */
int  owl_ui_zr_get_selected(OwlControl* zr);
void owl_ui_zr_set_selected(OwlControl* zr, int idx_1based);

/* Events. row_select carry l'indice 1-based de la nouvelle selection. */
void owl_ui_zr_on_row_select(OwlControl* zr, OwlEventCb2i cb, void* userdata);

/*  — style typé du Champ Zone répétée (styleZoneRépétée). Plus simple
 * que la Table : les reps sont de vrais QWidgets stylés directement (pas de
 * delegate). Sentinelles -1 = feuille non posee (skip). */
typedef struct {
    /* CadreExtérieur -> fond + bordure du widget entier (QFrame OwlZoneRepetee). */
    int ext_bg;
    int ext_bg_alpha;                 /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* ext_border;   /* NULL = pas de bordure */
    /* CadreInterne -> bordure de CHAQUE repetition. */
    int inner_bg;
    int inner_bg_alpha;               /* 0-255, 255 = opaque.  */
    const OwlCadreSpec* inner_border; /* NULL = pas de bordure */
    /* Fond de rep selon parite (seul CouleurFond compte). */
    int impaires_bg;
    int impaires_bg_alpha;   /*  — 255 = opaque */
    int paires_bg;
    int paires_bg_alpha;     /*  */
    /* Etats dynamiques : fond (+ texte) de la rep selectionnee / survolee. */
    int selection_bg;
    int selection_bg_alpha;  /*  */
    int selection_fg;
    int selection_fg_alpha;  /*  */
    int survol_bg;
    int survol_bg_alpha;     /*  */
    int survol_fg;
    int survol_fg_alpha;     /*  */
    /* Séparateur -> couleur du trait entre reps (-1 = pas de trait). */
    int separateur;
    int separateur_alpha;    /*  */
} OwlZrStyleSpec;

/* Applique le styleZoneRépétée : QSS du conteneur (CadreExtérieur) + memorise
 * la spec pour re-styler chaque rep (fond parite/selection/survol, CadreInterne,
 * Séparateur), y compris les reps ajoutees ensuite via AjouteLigne. */
void owl_ui_zr_set_style(OwlControl* zr, const OwlZrStyleSpec* s);

/* Helpers transverses pour identification + walk des QWidget enfants.
 * Utilises par l'indexation ZR[i].sub..prop : chaque sous-widget d'une
 * repetition est tagge avec son nom OWL via setObjectName, le lookup
 * direct sur le rep retrouve le sub par nom. */
void  owl_ui_control_set_object_name(OwlControl* w, const char* name);
const char* owl_ui_control_get_object_name(OwlControl* w);
/* Tag le kind OwlControlKind sur le widget Qt via dynamic property pour
 * permettre la reconstruction d'un ObjUiControl a partir du widget seul. */
void owl_ui_control_set_widget_kind(OwlControl* w, int kind);
int  owl_ui_control_get_widget_kind(OwlControl* w);
/* Recherche un QWidget enfant direct par objectName. NULL si aucun match. */
OwlControl* owl_ui_control_find_child(OwlControl* parent, const char* name);

/* Plancher irreductible d'un champ : la taille en dessous de laquelle un
 * layout ne le descend jamais. C'est exactement la mesure qu'un layout Qt
 * interroge sur son item (QWidgetItem::minimumSize) — on ne recalcule rien,
 * on demande au moteur. Rend 0/0 sur widget nul. */
void owl_ui_control_min_floor(OwlControl* w, int* out_w, int* out_h);

/* Espacement entre deux lignes du layout interne d'un champ, -1 s'il n'en a
 * pas. Les familles a options (Selecteur, Interrupteur) empilent leurs lignes
 * dans une grille : leur hauteur naturelle depend de cet espacement, que
 * l'editeur doit composer sans le graver. */
int owl_ui_control_layout_spacing(OwlControl* w);

/* Taille en points de la police effective d'un champ, -1 sur widget nul
 *. C'est la police a laquelle son plancher a ete mesure ; l'editeur
 * la publie pour deriver la croissance d'un plancher qui suit sa police. */
int owl_ui_control_font_point_size(OwlControl* w);

/* Plancher du meme champ apres avoir gonfle sa police a `point_size`.
 * Compare au plancher par defaut, il dit par la mesure si le plancher SUIT sa
 * police (Libelle, Onglet) ou lui est insensible (Table, Liste, ZoneRepetee).
 * Rend 0/0 sur widget nul ; la police est restauree apres mesure. */
void owl_ui_control_min_floor_at_font(OwlControl* w, int point_size,
                                      int* out_w, int* out_h);

/* ============================================================
 * Helpers generiques Liste OU Combo — dispatchent via dynamic_cast.
 * Permettent aux natives ListeXXX d'operer sur les deux widgets sans
 * dupliquer le code cote OWL.
 * ============================================================ */

void        owl_ui_listcombo_add_item   (OwlControl* w, const char* text,
                                          const char* stored, const char* image);
void        owl_ui_listcombo_insert_item(OwlControl* w, int index_1based,
                                          const char* text,
                                          const char* stored, const char* image);
void        owl_ui_listcombo_remove_item(OwlControl* w, int index_1based);
void        owl_ui_listcombo_clear      (OwlControl* w);
void        owl_ui_listcombo_modify_item(OwlControl* w, int index_1based,
                                          const char* text,
                                          const char* stored, const char* image);
int         owl_ui_listcombo_count      (OwlControl* w);
const char* owl_ui_listcombo_get_item   (OwlControl* w, int index_1based);
const char* owl_ui_listcombo_get_stored (OwlControl* w, int index_1based);
int         owl_ui_listcombo_find       (OwlControl* w, const char* text);
void        owl_ui_listcombo_sort       (OwlControl* w, int ascending);
int         owl_ui_listcombo_get_selection(OwlControl* w);
void        owl_ui_listcombo_set_selection(OwlControl* w, int index_1based);
void        owl_ui_listcombo_set_sorted (OwlControl* w, int sorted);
int         owl_ui_listcombo_get_sorted (OwlControl* w);
void        owl_ui_listcombo_set_content(OwlControl* w, const char* multiline);
void        owl_ui_listcombo_refresh    (OwlControl* w);

/* Opacite (commun) : 0..100 en canon, mappe sur QGraphicsOpacityEffect 0.0-1.0. */
void owl_ui_control_set_opacite(OwlControl* w, int percent);
int  owl_ui_control_get_opacite(OwlControl* w);

/* Ombre portée : mappe sur QGraphicsDropShadowEffect. color = RVB
 * packé BGR, offsetX/Y en px, blur = rayon de flou (<=0 retire l'ombre), spread
 * = étendue approximée via le flou (pas de spread natif dans Qt). alpha
 * = opacité 0-255 de la couleur d'ombre (255 = opaque, byte-identique à avant),
 * appliquée au QColor de l'effet — distinct de l'atténuation du flou. */
void owl_ui_control_set_shadow(OwlControl* w, int color, int offsetX,
                               int offsetY, int blur, int spread, int alpha);

/* Ombre dynamique d'état : mémorise la spec d'ombre d'un état
 * (0=base, 1=survol, 2=appuyé) sur le widget et arme un event filter qui swappe
 * le QGraphicsDropShadowEffect au survol/appui (Qt n'a pas de box-shadow QSS).
 * present=0 => l'état n'a pas de surcharge d'ombre (fallback base). alpha
 * = opacité 0-255 de la couleur d'ombre. */
void owl_ui_control_set_shadow_state(OwlControl* w, int stateIdx, int color,
                                     int offsetX, int offsetY, int blur,
                                     int spread, int alpha, int present);

/* Test-only : rayon de flou de l'ombre courante, -1 si aucune. */
int owl_ui_control_shadow_blur(OwlControl* w);

/* Test-only : pousse un événement d'interaction (0=Enter, 1=Leave,
 * 2=MouseButtonPress, 3=MouseButtonRelease) pour exercer l'event filter d'ombre. */
void owl_ui_control_push_interaction(OwlControl* w, int kind);

/* Dégradé de fond : peint le fond du widget via un dégradé QSS
 * (qlineargradient/qradialgradient). type = OWL_GRAD_* (aucun/lineaire/radial ;
 * aucun ou inconnu = retire le dégradé, le fond uni reprend). angle en degrés
 * (0 = horizontal G->D, 90 = vertical H->B ; ignoré en radial). startColor/
 * endColor = arrêts RVB packés BGR. Le dégradé actif prime sur la CouleurFond. */
void owl_ui_control_set_bg_gradient(OwlControl* w, int type, int angle,
                                    int startColor, int startAlpha,
                                    int endColor, int endAlpha);

/* ------- Lot A : props canon Libelle (doc 1013002) ------- */

/* AffichageActif — active/desactive le rafraichissement (QWidget::setUpdatesEnabled).
 * En Qt6 les repaints sont deja batches via l'event loop, mais cet hook reste utile
 * pour les boucles intensives qui modifient un widget des milliers de fois. */
void owl_ui_control_set_display_active(OwlControl* w, int active);

/* Altitude — z-order intra-fenetre. Mappe sur raise()/stackUnder() en Qt :
 * altitudes superieures sont au-dessus. Stockage dynamic-property pour read-back. */
void owl_ui_control_set_altitude(OwlControl* w, int z);

/* CurseurSouris — forme du curseur de survol. cur_id est l'index 0..13 declare
 * cote ui_catalog.c (E_Curseurs). Mappe sur Qt::CursorShape. -1 = reset (curFleche). */
void owl_ui_control_set_cursor_shape(OwlControl* w, int cur_id);

/* Ellipse — gestion du texte trop long. mode : 0=ellipseInactive, 1=ellipseFin,
 * 2=ellipseChemin. Pour QLabel : on bascule un mode wordwrap+elide custom paint
 * via dynamic property + QPaintEvent override (V1.5 minimal : truncation simple). */
void owl_ui_control_set_ellipsis_mode(OwlControl* w, int mode);

/* Bornes de redimensionnement (QWidget::setMinimumSize/setMaximumSize).
 * Une dimension <= 0 sur min = pas de min ; sur max = pas de max
 * (QWIDGETSIZE_MAX). Operations independantes pour set width-only ou height-only. */
void owl_ui_control_set_min_height(OwlControl* w, int h);
void owl_ui_control_set_max_height(OwlControl* w, int h);
void owl_ui_control_set_min_width (OwlControl* w, int w_px);
void owl_ui_control_set_max_width (OwlControl* w, int w_px);

/* Message — texte d'aide affiche dans la barre de statut au survol
 * (QWidget::setStatusTip). */
void        owl_ui_control_set_status_tip(OwlControl* w, const char* text_utf8);

/* SensEcriture — direction d'ecriture forcee. dir : 0=sensDefaut (LayoutDirectionAuto),
 * 1=sensGaucheDroite (LeftToRight), 2=sensDroiteGauche (RightToLeft). */
void owl_ui_control_set_text_direction(OwlControl* w, int dir);

/*  — style complet d'un etat visuel (Style[survol]/Style[focus]).
 * Fragment QSS (couleur, fond, police, bordure) concatene dans la feuille
 * UNIQUE du widget (contrainte  : jamais un setStyleSheet additionnel).
 * stateIdx : 0 = survol (:hover), 1 = focus (:focus), 2 = disabled (grisé,
 * :disabled), 3 = read-only (lectureSeule, :read-only). Champs a -1/0 = non
 * definis (omis du fragment). border NULL = pas de bordure d'etat. Rappeler
 * avec un fragment vide (tout unset, border NULL) reset l'etat. */
typedef struct {
    int fg;         /* couleur texte (RVB BGR), -1 = unset */
    int fgAlpha;    /*  — alpha du texte (0-255), 255 = opaque (défaut) */
    int bg;         /* couleur fond, -1 = unset */
    int bgAlpha;    /*  — alpha du fond (0-255), 255 = opaque (défaut) */
    int bold;       /* -1 unset, 0 = normal, 1 = gras */
    int italic;     /* -1 unset, 0 = normal, 1 = italique */
    int font_size;  /* taille en points, <= 0 = unset */
    int pad_l;      /* retrait gauche px, <= 0 = unset */
    int pad_r;      /* retrait droit px, <= 0 = unset */
    int pad_t;      /* retrait haut px, <= 0 = unset */
    int pad_b;      /* retrait bas px, <= 0 = unset */
} OwlStateTextStyle;
void owl_ui_control_set_state_style(OwlControl* w, int stateIdx,
                                    const OwlStateTextStyle* t,
                                    const OwlCadreSpec* border);

/*  — surcharge d'etat du CadreExtérieur (fond + bordure + dégradé) sur
 * un widget mono-element (Bouton, Libellé, Image, Saisie). Fragment QSS distinct
 * des états texte (owl_state_body_*) : concatené dans le même bloc de pseudo-
 * classe (:hover/:focus/:pressed/:disabled) par apply_color_stylesheet. Couleurs
 * au format compact `rgb(r,g,b)` (symétrique du dégradé ).
 * stateIdx : 0 = survol (:hover), 1 = focus (:focus), 2 = grisé (:disabled),
 * 4 = appuyé (:pressed).
 *   bg          : CouleurFond de l'état, -1 = non posée.
 *   bgAlpha     : alpha de bg (0-255, 255 = opaque). .
 *   borderColor : Cadre..Couleur, <= 0 = non posée (0 indistinguable du seed).
 *   borderColorAlpha : alpha de borderColor (0-255, 255 = opaque). .
 *   borderThick : Cadre..Epaisseur px, > 0 = bordure pleine (sinon border-color seul).
 *   borderStyle : Cadre..Trait (OWL_TC_*).
 *   borderRadius: Cadre..Arrondi px, > 0 émet border-radius.
 *   gradType..gradEnd : Dégradé de l'état (Type dégradéAucun -> pas de dégradé).
 *   gradStartAlpha/gradEndAlpha : alpha par arrêt (0 = arrêt transparent, non
 *     défini ; 255 = opaque).  : parité avec le dégradé de base.
 * Un état sans aucune prop posée reset son fragment (idempotent). */
void owl_ui_control_set_frame_state(OwlControl* w, int stateIdx,
                                    int bg, int bgAlpha,
                                    int borderColor, int borderColorAlpha,
                                    int borderThick,
                                    int borderStyle, int borderRadius,
                                    int gradType, int gradAngle,
                                    int gradStart, int gradStartAlpha,
                                    int gradEnd, int gradEndAlpha);

/*  — feuille de style QSS courante du widget (test-only, expose via
 * __DebugFeuilleStyle). Buffer statique partage, valide jusqu'au prochain
 * appel getter. */
const char* owl_ui_control_get_stylesheet(OwlControl* w);

/* Couleurs (texte / fond). RGB packed 0xRRGGBB. -1 = reset au defaut. */
void owl_ui_control_set_text_color(OwlControl* w, int rgb);
/*  — variante portant le canal alpha du TEXTE (0-255). alpha>=255 =
 * opaque (émission `rgb(...)` byte-identique) ; alpha<255 = texte translucide
 * (`rgba(...)`). owl_ui_control_set_text_color appelle cette variante avec 255. */
void owl_ui_control_set_text_color_alpha(OwlControl* w, int rgb, int alpha);
int  owl_ui_control_get_text_color(OwlControl* w);
void owl_ui_control_set_bg_color(OwlControl* w, int rgb);
/*  — variante portant le canal alpha (0-255). alpha>=255 = opaque
 * (émission `rgb(...)` byte-identique) ; alpha<255 = fond translucide
 * (`rgba(...)`). owl_ui_control_set_bg_color appelle cette variante avec 255. */
void owl_ui_control_set_bg_color_alpha(OwlControl* w, int rgb, int alpha);
int  owl_ui_control_get_bg_color(OwlControl* w);

/* Police / typo. Operations sur QFont, communes a tous les widgets.
 * Empty/zero/negative input = no-op pour les setters. */
void        owl_ui_control_set_font_name(OwlControl* w, const char* name_utf8);
const char* owl_ui_control_get_font_name(OwlControl* w);

/* Resout une sentinelle de famille (iPoliceXxx, iSwiss, iRoman, iCourier) vers
 * la fonte embarquee equivalente (ou "" pour iDefaut, NULL si pas une
 * sentinelle). Defini dans ui_catalog.c (toujours compile, hors GUI). */
const char* owl_ui_ipol_embedded_family(long long v);
void        owl_ui_control_set_font_size(OwlControl* w, int point_size);
int         owl_ui_control_get_font_size(OwlControl* w);
void        owl_ui_control_set_font_bold(OwlControl* w, int bold);
int         owl_ui_control_get_font_bold(OwlControl* w);
void        owl_ui_control_set_font_italic(OwlControl* w, int italic);
int         owl_ui_control_get_font_italic(OwlControl* w);
void        owl_ui_control_set_font_underline(OwlControl* w, int underline);
int         owl_ui_control_get_font_underline(OwlControl* w);
void        owl_ui_control_set_font_strikeout(OwlControl* w, int strikeout);
int         owl_ui_control_get_font_strikeout(OwlControl* w);

/* Lot B — extensions Police (canon).
 * Etirement horizontal des caracteres (QFont::stretch). Reglage unique :
 * la valeur de stretch passee a setStretch fait foi (100 = nominal, 75 =
 * condense, 125 = etendu, 150 = elargi). Le getter relit la valeur courante. */
void owl_ui_control_set_font_stretch(OwlControl* w, int stretch);
int  owl_ui_control_get_font_stretch(OwlControl* w);

/* PoliceTailleMin — option "Reduire la police" (canon).
 * Stocke la valeur en dynamic property + appelle apply_font_min_size_fit
 * qui ajuste la taille de police pour faire tenir le texte dans la zone.
 * V1 : fit initial seulement (au create). V2 : event filter sur resize/setText.
 * Le getter relit la valeur stockee (0 si jamais posee). */
void owl_ui_control_set_font_min_size(OwlControl* w, int size);
int  owl_ui_control_get_font_min_size(OwlControl* w);

/* Lot C — marges interieures (canon).
 * Espace en pixels entre le bord du widget et le texte.
 * Mappe sur QWidget::contentsMargins (left/top/right/bottom). Setters atomiques :
 * lit la marge actuelle, modifie le composant cible, ecrit back. */
void owl_ui_control_set_margin_left  (OwlControl* w, int px);
void owl_ui_control_set_margin_right (OwlControl* w, int px);
void owl_ui_control_set_margin_top   (OwlControl* w, int px);
void owl_ui_control_set_margin_bottom(OwlControl* w, int px);

/* Specifiques saisie (LineEdit / Password) */
void owl_ui_lineedit_set_max_length(OwlControl* edit, int max_length);
int  owl_ui_lineedit_get_max_length(OwlControl* edit);
void owl_ui_lineedit_set_placeholder(OwlControl* edit, const char* text_utf8);
const char* owl_ui_lineedit_get_placeholder(OwlControl* edit);

/* TypeSaisie : 1 = typSaisieMotDePasse (echo mode Password), 0 = typSaisieTexte. */
void owl_ui_lineedit_set_password_mode(OwlControl* edit, int password);
int  owl_ui_lineedit_get_password_mode(OwlControl* edit);

/* TypeSaisie type-aware : le type (sentinel OWL_TYPESAISIE_*) est memorise sur
 * le widget a la creation. set applique aussi le validateur numerique/monetaire
 * quand il y a lieu ; get relit le type memorise (round-trip fidele). */
void owl_ui_control_set_input_type(OwlControl* edit, int type);
int  owl_ui_control_get_input_type(OwlControl* edit);

/* Curseur / FinCurseur / TexteSelectionne. Positions canon = 1-based.
 * Curseur = debut selection (ou position cursor si pas de selection).
 * FinCurseur = fin selection (idem).
 * Pour creer une selection : ecrire Curseur puis FinCurseur (ou inverse). */
int  owl_ui_lineedit_get_cursor(OwlControl* edit);
void owl_ui_lineedit_set_cursor(OwlControl* edit, int pos_1based);
int  owl_ui_lineedit_get_end_cursor(OwlControl* edit);
void owl_ui_lineedit_set_end_cursor(OwlControl* edit, int pos_1based);
const char* owl_ui_lineedit_get_selected_text(OwlControl* edit);
void        owl_ui_lineedit_set_selected_text(OwlControl* edit, const char* text_utf8);

/* Modifie : drapeau read-only natif Qt (devient true des que l'utilisateur
 * touche au champ apres init/setText programmatique). */
int  owl_ui_lineedit_get_modified(OwlControl* edit);
void owl_ui_lineedit_set_modified(OwlControl* edit, int modified);

/* MasqueSaisie : masque canon converti en QLineEdit input mask.
 * Supporte le masque texte (TypeSaisie=typSaisieTexte). Caracteres :
 *   9 = chiffre, A = lettre, C = alphanum. Tout autre = literal.
 * V2 : extension pour date/heure/num/monetaire. */
void        owl_ui_lineedit_set_mask(OwlControl* edit, const char* wl_mask);
const char* owl_ui_lineedit_get_mask(OwlControl* edit);

/*  — validateur de plages date/heure derive du format effectif, pose
 * EN PLUS du masque de frappe. Borne chaque jeton a la frappe (jour 01-31, mois
 * 01-12, heure 00-23, minute/seconde 00-59). Format a jetons nommes ou vide ->
 * retire le validateur (saisie libre). */
void        owl_ui_lineedit_set_date_time_validator(OwlControl* edit,
                                                    const char* fmt);

/*  — validateur de plages DURÉE (typSaisieDurée). Bornes durée : HH
 * n'est PAS plafonné à 23 (0-99), MM/SS 0-59, JJ 0-99. Format variable (J seul,
 * +N) -> retire le validateur (saisie libre). */
void        owl_ui_lineedit_set_duree_validator(OwlControl* edit,
                                                const char* fmt);

/*  — validateur config-aware pour un champ num a FormatNombre effectif.
 * Remplace le QDoubleValidator locale-C : toute touche decimale tapee (. ou ,)
 * est remappee vers le separateur decimal configure (decSep), un seul decimal
 * est autorise, le reste (dont le separateur de milliers, qui ne se tape pas)
 * est neutralise a la frappe. decSep vide -> point plat. */
void        owl_ui_lineedit_set_num_format_validator(OwlControl* edit,
                                                     const char* decSep);

/* Bornes : stockage dynamic-property pour R/W + application aux bornes du
 * validateur d'un champ numerique/monetaire. Le message d'erreur sur Sortie
 * (validation de valeur au blur) reste une sur-couche future. */
int  owl_ui_lineedit_get_min_value(OwlControl* edit);
void owl_ui_lineedit_set_min_value(OwlControl* edit, int v);
int  owl_ui_lineedit_get_max_value(OwlControl* edit);
void owl_ui_lineedit_set_max_value(OwlControl* edit, int v);

/* ------- Dialogues ------- */

void owl_ui_message_info(const char* title_utf8, const char* message_utf8);
void owl_ui_message_error(const char* title_utf8, const char* message_utf8);
void owl_ui_message_warning(const char* title_utf8, const char* message_utf8);

/* Dialogues a choix multiples. La valeur retournee suit la convention canon :
 *   yesno   : 0 = Non, 1 = Oui (fermeture par X = 0)
 *   confirm : 0 = Non, 1 = Oui, 2 = Annuler
 *
 * default_button : bouton presélectionné. Pour yesno, accepte 0 ou 1.
 *   Pour confirm, accepte 0/1/2. */
int owl_ui_message_yesno(const char* title_utf8,
                          const char* message_utf8,
                          int default_button);
int owl_ui_message_confirm(const char* title_utf8,
                            const char* message_utf8,
                            int default_button);

/* ------- Selecteurs fichier / repertoire (QFileDialog) -------
 *
 * fSelecteur / fRepSelecteur. La chaine retournee est allouee par malloc
 * (le caller C la copie en ObjString puis free). Retourne NULL si l'utilisateur
 * annule. Pour la multiselection, les chemins sont joints par "\r\n" (RC canon
 * canon) dans la chaine retournee.
 *
 *   rep_initial_utf8 : repertoire d'ouverture (vide = dernier visite par l'OS).
 *   fichier_defaut_utf8 : nom de fichier preselectionne (peut etre vide).
 *   titre_utf8 : titre de la boite (vide = titre systeme par defaut).
 *   filtre_qt_utf8 : filtre au format Qt ("Desc (*.ext);;...") ou vide.
 *   mode : bitmask OWL_FSEL_* (OUVRE/CREE/EXISTE/MULTI). */
char* owl_ui_file_dialog(const char* rep_initial_utf8,
                         const char* fichier_defaut_utf8,
                         const char* titre_utf8,
                         const char* filtre_qt_utf8,
                         int mode);

/* fRepSelecteur. rep_defaut_utf8 = repertoire ouvert au depart, titre_utf8 =
 * titre de la boite, racine_utf8 = repertoire racine de l'arborescence (peut
 * etre NULL/vide). Retourne le chemin du repertoire (malloc) ou NULL si
 * annule. Le parametre Commentaires de canon n'a pas d'equivalent QFileDialog :
 * gere cote stdlib_ui (ignore par ce backend). */
char* owl_ui_dir_dialog(const char* rep_defaut_utf8,
                        const char* titre_utf8,
                        const char* racine_utf8);

/* ------- Toast (notification ephemere, non bloquante) -------
 *
 * Affiche un widget frameless top-most pendant `duration_ms`, fade-out
 * automatique. Appels successifs : file sequentielle (le 2e attend la
 * fin du 1er). Le rendu utilise la fenetre Qt active comme reference de
 * cadrage ; si aucune, l'ecran principal.
 *
 *   cv  : sentinel CadrageVertical (cvHaut/cvCentre/cvBas).
 *   ch  : sentinel CadrageHorizontal (chGauche/chCentre/chDroite).
 *   rgb : couleur de fond RGB (0xRRGGBB). -1 = laisse le rendu choisir
 *         une couleur lisible (theme par defaut).
 */
void owl_ui_toast_show(const char* msg, int duration_ms, int cv, int ch, int rgb);

/* Pompe l'event loop Qt jusqu'a ce que tous les toasts actifs soient
 * termines (fade-out compris), ou jusqu'a expiration de max_ms (cap dur
 * 10s applique si max_ms <= 0 ou > 10000). No-op si Qt jamais initialise.
 * Appele par main.c a la sortie d'un script pour les cas "script court
 * avec ToastAffiche" sans Fenetre / event loop active. */
void owl_ui_toast_wait_all(int max_ms);

#ifdef __cplusplus
}
#endif

#endif /* OWL_UI_H */
