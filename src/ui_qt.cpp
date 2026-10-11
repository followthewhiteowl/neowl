// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_qt.cpp - Implementation Qt du wrapper C owl_ui.h.
 *
 * Compile avec g++ MSYS2 mingw64. Lie en static avec mingw-w64-x86_64-qt6-static.
 * Ce fichier est compile dans owl.exe (cible avec OWL_HAS_GUI), pas dans owl-cli.
 */

#include "owl_ui.h"
#include "ui_position.h"
extern "C" {
#include "sentinels.h"
#include "owl_image.h"
}

/*  — generateur C-level du Champ CodeBarres (dans stdlib_codebarres.c),
 * appele par la regen au resize. Chemin 100% C : PAS de VM/GC (le timer Qt
 * n'est pas dans un contexte VM), pas de wrapper ObjImage -> l'OwlImage* produit
 * est libere via owl_image_free apres readback. */
extern "C" OwlImage* owl_codebarre_generate_owlimage(
    int64_t cbType, int64_t ecc, int64_t options,
    const char* contenu, int contenuLen,
    int largeurMax, int hauteurMax,
    bool hasBg, int64_t bgColor, bool hasFg, int64_t fgColor,
    const char** errOut);

/*  — superpose une image centrale (logo) sur un QR au niveau C pur
 * (aucune VM/GC). Utilise par la regen au resize pour recompositer le logo
 * stocke sur le widget, comme le chemin instance. Defini dans
 * stdlib_codebarres.c. */
extern "C" void owl_codebarre_composite_center_image(
    OwlImage* barcode, int64_t cbType, int64_t ecc,
    OwlImage* centre, double proportion);

/* Detecte le contexte `owl test` (campagne de tests). Defini dans testing.c.
 * Sert a forcer la plateforme Qt offscreen quand un test active la vraie UI
 * (TestModeHeadless(Faux)) : aucune fenetre ne doit popper pendant une
 * campagne, et le mode reste utilisable sans display (CI). */
extern "C" bool testing_is_unitaire(void);

/*  — jalons du chemin menu (gates OWL_MENU_TRACE, impl platform.c).
 * Trace la localisation d'un segfault intermittent au setup MenuPrincipal +
 * MenuContextuel combines. */
extern "C" void owl_menu_trace(const char* fmt, ...);

/*  — traceur cycle de vie QObject (gate OWL_QOBJ_TRACE, impl platform.c).
 * Complementaire du menu_trace : au lieu de jalonner les etapes, il logge la
 * CREATION et la DESTRUCTION de chaque QObject notable du chemin menu, pour
 * identifier lequel meurt trop tot (UAF au show natif). */
extern "C" void owl_qobj_trace(const char* fmt, ...);
extern "C" int  owl_qobj_trace_enabled(void);

/*  — fabrique le style applicatif (OwlMenuProxyStyle enrobant Fusion).
 * Defini apres la classe OwlMenuProxyStyle, appele des l'init de QApplication. */
class QStyle;
static QStyle* owl_app_style();

#include <cstdio>
#include <string>
#include <cstdlib>
#include <cmath>

#include <QtPlugin>
/* En Qt static, les plugins doivent etre importes explicitement sinon
 * QApplication echoue avec "no Qt platform plugin could be initialized".
 * En Qt dynamic, les plugins sont charges depuis plugins/platforms a
 * cote de l'exe, donc Q_IMPORT_PLUGIN n'est pas necessaire (et casserait
 * le link car les symboles statiques n'existent pas). */
#if defined(QT_STATIC)
#  if defined(_WIN32)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QModernWindowsStylePlugin)
#  elif defined(__APPLE__)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin)
#  elif defined(OWL_MUSL_STATIC)
     /* Profil musl-static universel : offscreen only (FEATURE_xcb=OFF, ).
      * Le plugin xcb n'est pas compile -> pas d'import. Le plugin offscreen
      * importe ci-dessous fournit le rendu sans serveur graphique. */
#  else
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)
#  endif
/*  : plugin de rendu hors-ecran (raster en memoire, sans serveur
 * graphique). Permet la capture d'une fenetre en image sans display, via
 * QT_QPA_PLATFORM=offscreen force par le chemin snapshot. Non auto-linke
 * (QT_DEFAULT_PLUGIN=0) -> import explicite + link CMake requis. */
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)
#endif

#include <QApplication>
#include <QStyleFactory>
#include <QProxyStyle>
#include <QStyleOptionButton>
#include <QStyleOptionProgressBar>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QTableView>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QListView>
#include <QComboBox>
#include <QProgressBar>
#include <QTabWidget>
#include <QTabBar>
#include <QAbstractButton>
#include <QStyleOptionTab>
#include <QFontMetrics>
#include <QButtonGroup>
#include <QRadioButton>
#include <QRegularExpression>
#include <QDoubleValidator>
#include <QValidator>
#include <algorithm>
#include <vector>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QCoreApplication>

extern "C" {
#include "sentinels.h"
}
#include <QWidget>
#include <QMessageBox>
#include <QString>
#include <QByteArray>
#include <QKeySequence>
#include <QShortcut>
/* Fonction Qt exportee par QtGui (Q_GUI_EXPORT) mais absente des headers
 * publics : active/desactive globalement la generation des raccourcis mnemonic
 * ('&Lettre'). Declaree ici pour l'appeler a l'init. */
Q_GUI_EXPORT void qt_set_sequence_auto_mnemonic(bool enable);
#include <QVariant>
#include <QScreen>
#include <QGuiApplication>
#include <QStyleHints>
#include <QPalette>
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>
#include <QFrame>
#include <QEvent>
#include <QFocusEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QHash>
#include <QSet>
#include <QObject>
#include <QTimer>
#include <QScrollBar>
#include <QPointer>
#include <QVector>
#include <QEventLoop>
#include <QThread>
#include <QFile>
#include <QTextStream>
#include <QFont>
#include <QFontDatabase>
#include <QClipboard>
#include <QMenu>
#include <QMenuBar>
#include <QCursor>
#include <QAction>
#include <QWidgetAction>
#include <QSignalBlocker>
#include <QFileDialog>

#include <cstdlib>
#include <cstring>

/* ------- QApplication singleton ------- */

static int   g_argc = 1;
static char* g_argv[] = { (char*)"owl", nullptr };
static QApplication* g_app = nullptr;

/* Buffer de retour pour owl_ui_lineedit_get_text (lifetime jusqu'au
 * prochain appel). Suffisant pour le POC. */
static QByteArray g_lineedit_buf;

/* Buffer commun pour tous les getters retournant const char* (tooltip,
 * shortcut, icon path, placeholder, title...). Lifetime jusqu'au
 * prochain appel d'un getter quelconque. */
static QByteArray g_ctrl_text_buf;

/* ------- Cycle de vie ------- */

/* Polices embarquees (assets/fonts.qrc, compilees dans le binaire via qrc).
 * Sans serveur graphique (rendu hors-ecran), aucune fonte systeme n'est
 * garantie : il faut enregistrer les fontes embarquees AVANT tout rendu de
 * widget, sinon les glyphes manquent (texte rendu en carres "tofu"). On pose
 * aussi une famille par defaut sur l'application pour que les widgets sans
 * police explicite heritent d'une fonte aux glyphes complets. */
static const char* const OWL_UI_EMBED_FONTS[] = {
    ":/fonts/LiberationSans-Regular.ttf",
    ":/fonts/LiberationSans-Bold.ttf",
    ":/fonts/LiberationSans-Italic.ttf",
    ":/fonts/LiberationSans-BoldItalic.ttf",
    /* Serif + monospace : necessaires pour resoudre iPoliceTimesNewRoman /
     * iRoman -> "Liberation Serif" et iPoliceCourierNew / iCourier ->
     * "Liberation Mono" (sinon Qt ne trouve pas la famille -> fallback). */
    ":/fonts/LiberationSerif-Regular.ttf",
    ":/fonts/LiberationSerif-Bold.ttf",
    ":/fonts/LiberationSerif-Italic.ttf",
    ":/fonts/LiberationSerif-BoldItalic.ttf",
    ":/fonts/LiberationMono-Regular.ttf",
    ":/fonts/LiberationMono-Bold.ttf",
    ":/fonts/LiberationMono-Italic.ttf",
    ":/fonts/LiberationMono-BoldItalic.ttf",
    ":/fonts/DejaVuSans-Regular.ttf",
    ":/fonts/DejaVuSans-Bold.ttf",
    nullptr
};

static void owl_ui_register_fonts(void) {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; OWL_UI_EMBED_FONTS[i] != nullptr; i++) {
        QFile f(QString::fromUtf8(OWL_UI_EMBED_FONTS[i]));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QFontDatabase::addApplicationFontFromData(f.readAll());
    }
    if (g_app) {
        QFont def("Liberation Sans", 10);
        QApplication::setFont(def);
    }
}

/*  — callback de synchro focused_index (cf. owl_ui.h). NULL tant que
 * la couche OWL ne l'a pas enregistre. Le connect QApplication::focusChanged
 * est pose une seule fois (g_focus_sync_wired) a la creation de g_app. */
static OwlFocusSyncFn g_focus_sync_cb    = nullptr;
static bool           g_focus_sync_wired = false;

extern "C" void owl_ui_set_focus_sync_callback(OwlFocusSyncFn fn) {
    g_focus_sync_cb = fn;
}

/*  — palettes OWL light/dark (definies plus bas, cf. ~L547).
 * Forward-declare ici pour poser la palette applicative par defaut a
 * l'init, avant le premier show. */
static QPalette owl_palette_light();
static QPalette owl_palette_dark();

/*  — a l'execution normale (run/--single), une fenetre OWL en
 * theme DEFAUT herite de la palette Qt/OS. Sur OS sombre, Qt garde
 * WindowText noir -> texte noir sur fond sombre, illisible. On detecte
 * le colorScheme de l'OS et on pose la palette OWL correspondante comme
 * palette applicative par defaut. Un style OWL explicite (Couleur/
 * CouleurFond via stylesheet du widget) prime toujours sur la palette
 * applicative. Les grabs a theme explicite (OwlPaletteScope) continuent
 * d'override temporairement via push/pop : intacts.
 *
 * Qt 6.10 vendore -> QStyleHints::colorScheme() (6.5+) dispo. Dark ->
 * palette sombre ; Light/Unknown -> palette claire (defaut sur, cross-
 * platform). */
static void owl_ui_apply_os_default_palette() {
    if (!g_app) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    QApplication::setPalette(scheme == Qt::ColorScheme::Dark
                             ? owl_palette_dark()
                             : owl_palette_light());
    /* Reaction a chaud : l'OS bascule clair<->sombre pendant que l'app
     * tourne -> on re-pose la palette correspondante. Connect unique
     * (styleHints est un singleton, la lambda est idempotente). */
    static bool wired = false;
    if (!wired) {
        QObject::connect(QGuiApplication::styleHints(),
                         &QStyleHints::colorSchemeChanged,
                         g_app, [](Qt::ColorScheme s) {
            QApplication::setPalette(s == Qt::ColorScheme::Dark
                                     ? owl_palette_dark()
                                     : owl_palette_light());
        });
        wired = true;
    }
#else
    /* Qt < 6.5 : colorScheme() indispo. Defaut clair (sur, lisible). */
    QApplication::setPalette(owl_palette_light());
#endif
}

extern "C" int owl_ui_init_offscreen(void) {
    if (g_app == nullptr && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    return owl_ui_init();
}

extern "C" int owl_ui_init(void) {
    if (g_app == nullptr) {
        /*  — sous `owl test`, un test qui active la vraie UI
         * (TestModeHeadless(Faux)) monte des widgets Qt reels. On force la
         * plateforme offscreen AVANT de creer QApplication pour qu'aucune
         * fenetre ne s'affiche pendant la campagne et que le mode tourne sans
         * display (CI). `seulement si pas deja defini` : un override explicite
         * de l'environnement (ex. snapshot, debug visuel) garde la priorite. */
        if (testing_is_unitaire() &&
            !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
#if defined(OWL_MUSL_STATIC)
        /* Profil musl-static universel : offscreen only (xcb non compile,
         * ). Force le defaut offscreen sans ecraser une preference user
         * -> les scripts tournent sans display (conteneur/serveur). */
        setenv("QT_QPA_PLATFORM", "offscreen", 0);
#elif defined(__linux__) && !defined(__ANDROID__)
        /* Sur Linux on n'embarque que le plugin xcb (qtwayland est un module
         * Qt separe non vendore). Sous WSLg ou Wayland desktop, Qt tenterait
         * "wayland" en premier et logguerait un warning avant de fallback.
         * On force le defaut xcb sans ecraser une preference user. */
        setenv("QT_QPA_PLATFORM", "xcb", 0);
#endif
        g_app = new QApplication(g_argc, g_argv);

        /*  — force l'auto-mnemonic sur toutes les plateformes. Un '&'
         * devant une lettre d'un libelle ('&Ouvrir') doit souligner la lettre ET
         * l'activer via Alt+lettre. Sans ce forcage, certaines plateformes (macOS,
         * offscreen) laissent qt_sequence_no_mnemonics a true : setText ne genere
         * alors AUCUN raccourci mnemonic (la lettre reste soulignee mais Alt+lettre
         * ne declenche rien) et QKeySequence::mnemonic() renvoie vide. On veut le
         * comportement mnemonic actif partout. La fonction est exportee par QtGui
         * mais non declaree dans un header public -> forward decl locale. */
        qt_set_sequence_auto_mnemonic(true);

        /*  — style Fusion force. Le style natif Windows applique le
         * fond de la palette (role Window) mais ignore la couleur de texte
         * (WindowText/Text) et force le noir systeme -> texte noir illisible
         * sur fond sombre. Fusion respecte integralement la palette. A poser
         * AVANT owl_ui_apply_os_default_palette() pour que la palette
         * s'applique sur le bon style. Look portable, coherent cross-platform.
         * Un style explicite Couleur/CouleurFond (QSS widget) prime toujours.
         *
         *  — le style applicatif est un OwlMenuProxyStyle qui ENROBE
         * Fusion : il ne surcharge QUE la coche de menu (PE_IndicatorMenuCheckMark)
         * et delegue tout le reste a Fusion (rendu inchange hors menus). Poser le
         * style custom au niveau APPLICATION est le seul montage supporte par Qt
         * pour combiner style custom + feuilles QSS (setStyleSheet). L'ancien
         * montage posait le proxy par-widget via QWidget::setStyle sur chaque
         * barre/QMenu QUI PORTE AUSSI une feuille -> combinaison explicitement
         * non supportee (cf. doc QWidget::setStyle) : inheritStyle recreait un
         * QStyleSheetStyle enrobant le proxy partage a la creation de la fenetre
         * popup du menu -> UAF (vtable corrompue) au show natif. */
        g_app->setStyle(owl_app_style());

        owl_ui_register_fonts();

        /*  — palette applicative par defaut qui suit le theme OS.
         * PAS en mode test unitaire : les captures pixel doivent rester
         * reproductibles quel que soit le theme du poste CI. En test, la
         * palette par defaut reste celle de Qt (pass-through) ; les grabs
         * a theme explicite (themeClair/themeSombre) posent leur palette
         * via OwlPaletteScope. Hors test, on suit l'OS pour la lisibilite. */
        if (!testing_is_unitaire()) {
            owl_ui_apply_os_default_palette();
        }

        /*  — un seul connect global qui suit TOUS les changements de
         * focus clavier Qt (clic, Tab, setFocus programmatique). On relaie le
         * widget `now` a la couche OWL qui met focused_index a jour. `old` est
         * ignore : la couche OWL n'a besoin que de la cible courante. Le sync
         * ne fait QUE l'etat logique ; surEntree/surSortie restent geres par
         * l'eventFilter FocusIn de OwlEventDispatcher (pas de double emission). */
        if (!g_focus_sync_wired) {
            QObject::connect(g_app, &QApplication::focusChanged,
                             g_app, [](QWidget* /*old*/, QWidget* now) {
                if (g_focus_sync_cb) {
                    g_focus_sync_cb(reinterpret_cast<OwlControl*>(now));
                }
            });
            g_focus_sync_wired = true;
        }
    }
    return 0;
}

/*  — pile des QEventLoop actifs. Index 0 = boucle principale
 * (QApplication::exec). Index >0 = boucles locales imbriquees (Ouvre
 * appele depuis un handler). On expose owl_ui_event_loop_running() pour
 * que la couche OWL (Ferme) decide si owl_ui_quit doit quitter la boucle
 * principale ou laisser la boucle locale se terminer toute seule via le
 * close event de la fenetre cible. */
static int g_loop_depth = 0;

extern "C" int owl_ui_event_loop_running(void) {
    return g_loop_depth > 0 ? 1 : 0;
}

/*  — purge des entrees dispatcher (focus_map/close_map) pour une
 * fenetre fermee + ses champs. Defini apres OwlEventDispatcher (plus bas) ;
 * forward-declare ici car owl_ui_run_modal_for l'appelle. No-op si aucun
 * dispatcher n'a encore ete cree. */
static void purge_dispatcher_for(QObject* root);

/*  — installe l'eventFilter du dispatcher sur un objet. Defini apres
 * OwlEventDispatcher (plus bas) ; forward-declare ici car owl_ui_lineedit_create
 * (plus haut) l'appelle pour cabler le fallback Entree -> bouton par defaut. */
static void install_owl_event_filter(QObject* obj);

/*  — annule les setFocus differes en vol cibles sur `root`/descendants
 * (defini avec owl_ui_control_set_focus, plus bas). Forward-declare ici car
 * owl_ui_window_close l'appelle au teardown. */
static void cancel_focus_timers_for(QWidget* root);

/*  — registre des connexions Qt posees par OWL qui reentrent dans la
 * VM (lambdas capturant un OwlCallback + userdata = index de binding).
 *
 * A la sortie d'une modale imbriquee, ces connexions-la doivent tomber : leur
 * index de binding est invalide des que bindings_restore a joue cote OWL. La
 * version precedente les coupait en balayant findChildren<QWidget*> + un
 * disconnect wildcard par widget, ce qui emportait AUSSI tout ce que Qt
 * connecte pour son propre compte sur ces widgets — dont destroyed(), y
 * compris les hooks destroyed() qu'OWL utilise pour nettoyer ses propres maps
 * (g_combo_label_wrappers, g_owl_table_hover_row).
 *
 * Mesure sur une fenetre Champ Combo + Champ Saisie : 11 connexions destroyed()
 * REELLEMENT vivantes rompues par fermeture (Qt n'emet son avertissement
 * « wildcard call disconnects from destroyed signal » que dans ce cas). Ces
 * avertissements ne partent sur stderr que si QT_FORCE_STDERR_LOGGING=1 est
 * pose : sous Windows, hors console attachee, Qt6 les route vers
 * OutputDebugString et ils sont invisibles dans un pipe.
 *
 * `owner` est le champ (ou la fenetre) auquel la connexion appartient — pas
 * forcement l'emetteur : les events Table sont emis par le QItemSelectionModel
 * du QTableView, qui n'est pas un widget. QPointer parce que le widget peut
 * mourir avant la fenetre. */
struct OwlOwnedConn {
    QPointer<QWidget>       owner;
    QMetaObject::Connection conn;
};
static QVector<OwlOwnedConn> g_owl_conns;

static void owl_track_conn(QWidget* owner, const QMetaObject::Connection& conn) {
    if (!owner) return;
    /* Compaction opportuniste : sans modale imbriquee, rien ne vient jamais
     * relire ce registre. Un Selecteur qui reconstruit ses options en pose une
     * par case a chaque passe. */
    if (g_owl_conns.size() >= 256 && (g_owl_conns.size() % 256) == 0) {
        for (int i = g_owl_conns.size() - 1; i >= 0; i--)
            if (!g_owl_conns[i].owner) g_owl_conns.remove(i);
    }
    g_owl_conns.append(OwlOwnedConn{QPointer<QWidget>(owner), conn});
}

/* Rompt les connexions OWL de `root` et de ses descendants, et purge au passage
 * les entrees dont le porteur est deja mort. Retourne le nombre rompu. */
static int owl_disconnect_owl_conns_for(QWidget* root) {
    if (!root) return 0;
    int broken = 0;
    for (int i = g_owl_conns.size() - 1; i >= 0; i--) {
        QWidget* owner = g_owl_conns[i].owner.data();
        if (!owner) { g_owl_conns.remove(i); continue; }
        if (owner != root && !root->isAncestorOf(owner)) continue;
        QObject::disconnect(g_owl_conns[i].conn);
        g_owl_conns.remove(i);
        broken++;
    }
    /* Trace opt-in (OWL_TRACE_UI, meme regime que trace_ui_connect) : c'est le
     * seul oracle de la raison d'etre de cette purge. Un compte a zero sur une
     * fenetre qui portait des gestionnaires signifie que des lambdas OWL
     * survivent a la fermeture avec un index de binding perime. */
    static int trace_cached = -1;
    if (trace_cached < 0) trace_cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
    if (trace_cached) {
        fprintf(stderr, "[ui-event] purge %d connexions OWL, %d en registre\n",
                broken, (int)g_owl_conns.size());
        fflush(stderr);
    }
    return broken;
}

extern "C" int owl_ui_run(void) {
    if (g_app == nullptr) return -1;
    g_loop_depth++;
    int rc = g_app->exec();
    g_loop_depth--;
    return rc;
}

extern "C" int owl_ui_run_modal_for(OwlWindow* w) {
    if (g_app == nullptr) return -1;
    /* Cas top-level : aucune boucle Qt active. On utilise la boucle
     * principale via QApplication::exec(), comportement historique. */
    if (g_loop_depth == 0) {
        return owl_ui_run();
    }
    /* Cas imbrique : un event loop tourne deja. Re-entrer dans
     * QApplication::exec() est refuse par Qt. On monte une QEventLoop
     * locale, et on la quitte sur le close event de `w` (Ferme cote OWL
     * appellera owl_ui_window_close qui poste un QEvent::Close). */
    if (!w) return -1;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QEventLoop loop;
    /* Le QEvent::Close peut etre soit declenche par un Ferme() OWL (qui
     * appelle owl_ui_window_close => mw->close()), soit par l'utilisateur
     * (clic [X]). Dans les deux cas, on veut quitter la boucle locale.
     * destroyed() couvre le cas defensif ou la fenetre serait detruite
     * sans event Close prealable.
     *
     *  — on CAPTURE la Connection pour la rompre explicitement a la
     * sortie. `loop` est stack-local (detruit a la fin de cette fonction),
     * mais `mw` SURVIT (owl_ui_window_close ne fait qu'un mw->close()/hide,
     * jamais un delete). Sans disconnect, la connexion destroyed->&loop
     * reste pendante sur mw ; quand mw est detruit plus tard (fermeture de
     * l'app, ou cycle suivant), Qt appelle quit() sur un QEventLoop mort =>
     * use-after-free => l'app se ferme/crash. Le bug est cumulatif : chaque
     * Ouvre imbrique laissait une connexion pendante de plus. */
    QMetaObject::Connection destroyedConn =
        QObject::connect(mw, &QObject::destroyed, &loop, &QEventLoop::quit);
    /* Filtre d'event sur la fenetre pour capter QEvent::Close. On passe
     * par un QObject inline (lambda dans QObject::installEventFilter
     * directement n'existe pas en Qt6). Solution simple : ad-hoc filter. */
    class CloseQuitter : public QObject {
    public:
        QEventLoop* target;
        CloseQuitter(QEventLoop* t) : target(t) {}
        bool eventFilter(QObject* obj, QEvent* ev) override {
            if (ev->type() == QEvent::Close) {
                /* Differe le quit pour laisser le handler natif Qt (et le
                 * dispatcher OwlEventDispatcher qui invoque le code Fermeture
                 * OWL) tourner d'abord. Sans QueuedConnection, on quitterait
                 * la boucle locale AVANT que le QEvent::Close finisse son
                 * traitement. */
                QMetaObject::invokeMethod(target, "quit", Qt::QueuedConnection);
            }
            return QObject::eventFilter(obj, ev);
        }
    };
    CloseQuitter filter(&loop);
    mw->installEventFilter(&filter);
    /*  — la connexion destroyed() ci-dessus protege le cas ou `mw`
     * serait detruite pendant la boucle. Le bloc de sortie dereference `mw`
     * quatre fois : sans garde, ce cas defensif taperait dans de la memoire
     * liberee. QPointer se met a nul tout seul a la destruction (meme idiome
     * qu'ailleurs dans ce fichier : ToastManager, OwlZoneRepetee::showEvent).
     * Durcissement : aucun appelant de owl_ui_window_destroy ne vise
     * aujourd'hui une fenetre modale en cours. */
    QPointer<QMainWindow> alive(mw);
    g_loop_depth++;
    int rc = loop.exec();
    g_loop_depth--;
    if (!alive) return rc;
    /* Cleanup filter (la fenetre peut survivre a la boucle si l'app garde
     * une ref). */
    mw->removeEventFilter(&filter);
    /*  — rompt la connexion destroyed->&loop AVANT que `loop` sorte
     * du scope. Indispensable : `mw` survit a cette fonction, garder la
     * connexion vivante revient a pointer vers un QEventLoop detruit. */
    QObject::disconnect(destroyedConn);

    /*  (bonus) — la fenetre imbriquee est fermee et ne sera pas
     * reutilisee (chaque Ouvre rebuild une instance fraiche). On purge ses
     * entrees focus_map/close_map du dispatcher (sinon QObject* cles
     * dangling apres deleteLater) et on rompt les connexions Qt de la
     * fenetre + ses champs (les lambdas returnPressed/textChanged captent
     * des callbacks C avec userdata = index de binding, devenu invalide
     * apres bindings_restore cote OWL). Fuite cumulative documentee ~1415.
     *
     *  — la coupe se fait desormais sur le registre des connexions
     * qu'OWL a posees (cf. owl_track_conn), plus par balayage wildcard du
     * sous-arbre : celui-ci emportait aussi les liens internes de Qt.
     * `mw` survit bien a cette fonction — verifie, la fenetre repond encore
     * 400 ms apres ce bloc — donc la detruire ici n'est PAS une option. */
    purge_dispatcher_for(mw);
    owl_disconnect_owl_conns_for(mw);
    return rc;
}

extern "C" void owl_ui_quit(void) {
    if (!g_app) return;
    /*  — ne tuer la boucle principale QUE si on est au top-level.
     * En imbrique, la QEventLoop locale doit sortir via le close event de
     * SA fenetre, pas via un quit global qui ferait s'effondrer aussi la
     * boucle parent (et toutes les fenetres ouvertes derriere). */
    if (g_loop_depth <= 1) {
        g_app->quit();
    }
    /* En imbrique (g_loop_depth > 1), no-op : la sortie de la boucle locale
     * est pilotee par le QEvent::Close de la fenetre concernee (cf.
     * owl_ui_run_modal_for). */
}

/* Pause UI-friendly. Si un QApplication tourne et que l'appelant le souhaite,
 * on monte un QEventLoop local + QTimer::singleShot pour rendre la main au
 * dispatch Qt pendant l'attente — les widgets se repaintent, les events
 * souris/clavier sont traités. Sinon, sommeil bloquant pur via QThread. */
extern "C" void owl_ui_wait_ms(int ms, int pump_events) {
    if (ms <= 0) return;
    if (pump_events && g_app != nullptr && QCoreApplication::eventDispatcher() != nullptr) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    } else {
        QThread::msleep(static_cast<unsigned long>(ms));
    }
}

extern "C" void owl_ui_wait_cursor(int active) {
    if (!g_app) return;
    if (active) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    } else {
        /* restoreOverrideCursor est un no-op si la pile est vide : appeler
         * Sablier(Faux) sans activation prealable est donc sans effet. */
        QGuiApplication::restoreOverrideCursor();
    }
}

/* ------- Live preview (Lot V) ------- */

#include <QFileSystemWatcher>

static QFileSystemWatcher* g_fileWatcher = nullptr;
static OwlFileChangedFn    g_fileChangedCb = nullptr;
static void*               g_fileChangedUd = nullptr;
static QString             g_watchedPath;

extern "C" void owl_ui_install_file_watcher(const char* path,
                                              OwlFileChangedFn cb,
                                              void* userdata) {
    if (!g_app) owl_ui_init();
    if (!g_fileWatcher) {
        g_fileWatcher = new QFileSystemWatcher();
        QObject::connect(g_fileWatcher, &QFileSystemWatcher::fileChanged,
                         g_app, [](const QString& p) {
            /* Debounce : certains editeurs save en 2 etapes (delete + write
             * atomique) qui declenche 2 events rapproches. On laisse 100ms
             * de marge avant le reload. Aussi : QFileSystemWatcher peut
             * perdre le watch si le fichier est replace par un nouveau
             * inode — on re-add. */
            QTimer::singleShot(100, g_app, [p]() {
                if (!QFile::exists(p)) return;
                /* Re-add au cas ou le watcher a perdu le fichier. */
                if (!g_fileWatcher->files().contains(p)) {
                    g_fileWatcher->addPath(p);
                }
                if (g_fileChangedCb) {
                    QByteArray utf8 = p.toUtf8();
                    g_fileChangedCb(utf8.constData(), g_fileChangedUd);
                }
            });
        });
    }
    g_watchedPath = QString::fromUtf8(path);
    g_fileWatcher->removePaths(g_fileWatcher->files());
    g_fileWatcher->addPath(g_watchedPath);
    g_fileChangedCb = cb;
    g_fileChangedUd = userdata;
}

extern "C" void owl_ui_window_destroy(OwlWindow* w) {
    owl_menu_trace("owl_ui_window_destroy ENTER w=%p", (void*)w);
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    /*  — purge les entrees g_focus_timers de cette fenetre avant le
     * delete : les QTimer parentes aux widgets sont detruits par Qt avec leur
     * parent, mais la map garderait des pointeurs dangling. */
    cancel_focus_timers_for(mw);
    mw->close();
    mw->deleteLater();
    owl_menu_trace("owl_ui_window_destroy EXIT");
}

/* ------- Timer -------
 *
 * Un QTimer parenté à la fenêtre courante (QMainWindow). Le parentage Qt
 * garantit que le timer meurt avec sa fenêtre (fenêtre fermée => timer
 * détruit => plus de tick). Le QTimer tire pendant la boucle d'événements
 * principale ET pendant les boucles locales (Temporise via owl_ui_wait_ms).
 *
 * Mapping instance OWL (opaque void*) -> QTimer*, pour pouvoir arrêter le
 * bon timer depuis owl_ui_qt_timer_stop. Le callback C `onTick` (impl dans
 * stdlib_ui.c) ré-entre dans la VM pour appeler la procédure du timer. */
static QHash<void*, QTimer*> g_owl_timers;

extern "C" void owl_ui_qt_timer_start(OwlWindow* parent, void* owlInstance,
                                      int intervalMs, int repeat,
                                      void (*onTick)(void*)) {
    if (!owlInstance || !onTick) return;
    /* Stoppe un éventuel timer déjà associé (re-Démarre). */
    QTimer* existing = g_owl_timers.value(owlInstance, nullptr);
    if (existing) { existing->stop(); existing->deleteLater(); g_owl_timers.remove(owlInstance); }

    QObject* parentObj = parent ? reinterpret_cast<QMainWindow*>(parent) : nullptr;
    QTimer* timer = new QTimer(parentObj);
    timer->setInterval(intervalMs < 0 ? 0 : intervalMs);
    timer->setSingleShot(repeat == 0);
    QObject::connect(timer, &QTimer::timeout, [owlInstance, onTick]() {
        onTick(owlInstance);
        /* one-shot : Qt l'a déjà stoppé (singleShot). On nettoie le mapping
         * pour que ..Actif puisse repasser à Faux côté stdlib_ui.c. On lit
         * l'état singleShot LIVE (et non capturé à l'armement) : ..Répète peut
         * avoir basculé à chaud via owl_ui_qt_timer_update. */
        QTimer* t = g_owl_timers.value(owlInstance, nullptr);
        if (t && t->isSingleShot()) {
            t->deleteLater();
            g_owl_timers.remove(owlInstance);
        }
    });
    g_owl_timers.insert(owlInstance, timer);
    timer->start();
}

/* ..Intervalle / ..Répète modifié à chaud sur un timer actif. setInterval
 * sur un QTimer en marche s'applique au cycle suivant (exactement « pris en
 * compte au tic suivant »). setSingleShot bascule répétitif <-> one-shot sans
 * re-création du timer. No-op si aucun QTimer associé (timer pas armé). */
extern "C" void owl_ui_qt_timer_update(void* owlInstance, int intervalMs, int repeat) {
    if (!owlInstance) return;
    QTimer* timer = g_owl_timers.value(owlInstance, nullptr);
    if (!timer) return;
    timer->setInterval(intervalMs < 0 ? 0 : intervalMs);
    timer->setSingleShot(repeat == 0);
}

extern "C" void owl_ui_qt_timer_stop(void* owlInstance) {
    if (!owlInstance) return;
    QTimer* timer = g_owl_timers.value(owlInstance, nullptr);
    if (timer) {
        timer->stop();
        timer->deleteLater();
        g_owl_timers.remove(owlInstance);
    }
}

/* ------- Fenetre ------- */

extern "C" OwlWindow* owl_ui_window_create(const char* title_utf8) {
    QMainWindow* w = new QMainWindow();
    w->setWindowTitle(QString::fromUtf8(title_utf8 ? title_utf8 : ""));
    QWidget* central = new QWidget(w);
    w->setCentralWidget(central);
    return reinterpret_cast<OwlWindow*>(w);
}

extern "C" void owl_ui_window_set_size(OwlWindow* w, int width, int height) {
    if (!w) return;
    reinterpret_cast<QMainWindow*>(w)->resize(width, height);
}

extern "C" void owl_ui_window_set_title(OwlWindow* w, const char* title_utf8) {
    if (!w) return;
    reinterpret_cast<QMainWindow*>(w)->setWindowTitle(QString::fromUtf8(title_utf8 ? title_utf8 : ""));
}

/*  — phase courante du cycle build_window, consultee au moment ou un
 * QObject surveille meurt. Simple pointeur vers un litteral statique (pas
 * d'alloc, ecriture atomique de fait). Les jalons OWL_MENU_TRACE couvrent
 * deja SETUP ; on distingue ici BEFORE_SHOW / IN_SHOW / AFTER_SHOW autour du
 * show() natif, la fenetre soupconnee du UAF. */
static const char* g_owl_qobj_phase = "SETUP";

static void owl_qobj_set_phase(const char* p) {
    g_owl_qobj_phase = p;
    owl_qobj_trace("PHASE -> %s", p);
}

/* Enregistre un QObject a surveiller : logge sa creation (type + adresse +
 * parent) et branche son signal destroyed() pour logger sa mort avec la phase
 * courante. Le type est capture a la creation (a la destruction, ~QObject a
 * deja rembobine le vtable derive -> className() ne renverrait plus que
 * "QObject"). No-op total quand OWL_QOBJ_TRACE est absent (aucun connect pose,
 * zero surcout hors debug). */
static void owl_qobj_watch(QObject* o, const char* type) {
    if (!o || !owl_qobj_trace_enabled()) return;
    QObject* parent = o->parent();
    owl_qobj_trace("CREATE type=%s addr=%p parent=%p(%s) phase=%s",
                   type, (void*)o, (void*)parent,
                   parent ? parent->metaObject()->className() : "-",
                   g_owl_qobj_phase);
    std::string t(type);
    void* addr = static_cast<void*>(o);
    QObject::connect(o, &QObject::destroyed, [t, addr](QObject*) {
        owl_qobj_trace("DESTROY type=%s addr=%p phase=%s",
                       t.c_str(), addr, g_owl_qobj_phase);
    });
}

extern "C" void owl_ui_window_show(OwlWindow* w) {
    owl_menu_trace("owl_ui_window_show ENTER w=%p", (void*)w);
    owl_qobj_set_phase("BEFORE_SHOW");
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    owl_qobj_set_phase("IN_SHOW");
    mw->show();
    owl_qobj_set_phase("AFTER_SHOW");
    owl_menu_trace("  window_show AFTER show()");
    /*  — sur Windows, un show() ne rend pas la fenetre active : le
     * keyboard grab n'est pose qu'une fois la fenetre activee. Sans ca, le
     * focus initial sur un champ de saisie est cosmetique (curseur visible)
     * mais les frappes ne remontent pas (textChanged muet). raise + activate
     * forcent l'activation pour que le focus initial differe capte vraiment
     * le clavier. */
    mw->raise();
    mw->activateWindow();
    owl_menu_trace("owl_ui_window_show EXIT");
}

extern "C" int owl_ui_window_close(OwlWindow* w) {
    owl_menu_trace("owl_ui_window_close ENTER w=%p", (void*)w);
    if (!w) return 0;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    /*  — annule les setFocus differes encore en vol sur cette fenetre
     * AVANT le hide. Sans ca, un DonneFocus(X) suivi d'un Ferme() immediat
     * laisse le QTimer 0ms tirer pendant un pump ulterieur : son setFocus
     * emet focusChanged -> surSortie/surEntree sur un binding OWL deja demonte
     * (instance fermee) -> use-after-free / contamination cross-instance. */
    cancel_focus_timers_for(mw);
    /* QWidget::close rend false quand le QCloseEvent a ete refuse — c'est le
     * canal par lequel un veto AutorisationFermeture remonte a l'appelant OWL. */
    return mw->close() ? 1 : 0;
}

/* ================================================================
 *  — Grab Qt en buffer RGBA8 (caller-free).
 *
 * Helper commun : QPixmap -> QImage RGBA8888 -> malloc copy. Le format
 * Qt Format_RGBA8888 a exactement le meme byte layout que le stockage
 * interne OwlImage (R, G, B, A par pixel). Aucun byte-swap necessaire.
 * Le caller doit free() le buffer.
 * ================================================================ */
static unsigned char* qpixmap_to_rgba_buffer(const QPixmap& pix,
                                              int* out_w, int* out_h) {
    if (pix.isNull()) return nullptr;
    QImage img = pix.toImage();
    if (img.isNull()) return nullptr;
    /* Conversion vers Format_RGBA8888 = bytes RGBA strict, identique au
     * stockage OwlImage. convertToFormat() retourne une copie si le
     * format differe deja, sinon la meme image. */
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) return nullptr;
    int w = rgba.width();
    int h = rgba.height();
    if (w <= 0 || h <= 0) return nullptr;
    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) return nullptr;
    /* QImage peut etre row-padded. On copie ligne par ligne en tassant. */
    for (int y = 0; y < h; y++) {
        memcpy(buf + (size_t)y * w * 4, rgba.constScanLine(y), (size_t)w * 4);
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return buf;
}

/*  — Lot 3 : palettes light/dark hardcodees pour le push/pop
 * applique autour des grabs themeClair/themeSombre. Couleurs canon Fusion :
 * Window/WindowText/Base/Text/Button/ButtonText. Suffisant pour rendre
 * un fond fenetre + libelle differencie entre les deux themes. Pas de
 * dependance sur QStyleHints::colorScheme() (option A) : on veut un
 * resultat reproductible quelle que soit la palette OS courante. */
static QPalette owl_palette_light() {
    QPalette p;
    p.setColor(QPalette::Window,      QColor(0xF0, 0xF0, 0xF0));
    p.setColor(QPalette::WindowText,  QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Base,        QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Text,        QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Button,      QColor(0xE0, 0xE0, 0xE0));
    p.setColor(QPalette::ButtonText,  QColor(0x00, 0x00, 0x00));
    /*  — sans Highlight/HighlightedText explicites, la selection de
     * tout QTableView/QListView suit le defaut du style/OS (souvent sombre
     * sur sombre = invisible). Orange charte OWL (#FFA500, OrangeClair) +
     * texte noir : bande de selection nettement contrastee, lisible sur un
     * fond clair. Texte force en noir (pas WindowText) car il repose sur la
     * bande orange -> noir-sur-orange > blanc-sur-orange. */
    p.setColor(QPalette::Highlight,       QColor(0xFF, 0xA5, 0x00));
    p.setColor(QPalette::HighlightedText, QColor(0x00, 0x00, 0x00));
    return p;
}
static QPalette owl_palette_dark() {
    QPalette p;
    p.setColor(QPalette::Window,      QColor(0x2D, 0x2D, 0x2D));
    p.setColor(QPalette::WindowText,  QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Base,        QColor(0x1E, 0x1E, 0x1E));
    p.setColor(QPalette::Text,        QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Button,      QColor(0x3C, 0x3C, 0x3C));
    p.setColor(QPalette::ButtonText,  QColor(0xFF, 0xFF, 0xFF));
    /*  — meme selection orange charte qu'en clair. En sombre, le
     * defaut Highlight de Fusion vire au bleu terne peu visible et
     * HighlightedText tend vers le blanc : on impose l'orange + texte noir,
     * qui reste le meilleur contraste sur la bande de selection quel que
     * soit le fond de la fenetre. */
    p.setColor(QPalette::Highlight,       QColor(0xFF, 0xA5, 0x00));
    p.setColor(QPalette::HighlightedText, QColor(0x00, 0x00, 0x00));
    return p;
}

/* Push/pop palette globale autour d'un grab. Pattern Qt anti-leak :
 * snapshot palette courante avant override, restauration apres. theme
 * = OWL_THEME_DEFAUT laisse pass-through (pas de push, save vide). */
struct OwlPaletteScope {
    QPalette saved;
    bool pushed;
    OwlPaletteScope(int theme) : pushed(false) {
        if (theme == OWL_THEME_CLAIR || theme == OWL_THEME_SOMBRE) {
            saved = QGuiApplication::palette();
            QGuiApplication::setPalette(theme == OWL_THEME_CLAIR
                                        ? owl_palette_light()
                                        : owl_palette_dark());
            pushed = true;
            /* Applique le repaint sous nouvelle palette avant le grab. */
            if (g_app) QApplication::processEvents();
        }
    }
    ~OwlPaletteScope() {
        if (pushed) {
            QGuiApplication::setPalette(saved);
            if (g_app) QApplication::processEvents();
        }
    }
};

/*  — le grab d'un champ composite doit viser le wrapper (boite), pas
 * le widget logique. La resolution vit plus bas (definie L~5218). */
static QWidget* owl_widget_layout_rep(QWidget* inner);

extern "C" unsigned char* owl_ui_window_grab_rgba(OwlWindow* w,
                                                   int theme,
                                                   int* out_width,
                                                   int* out_height) {
    if (!w) return nullptr;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    /* Garantit une taille non nulle avant le grab (sinon QPixmap vide). */
    if (mw->width() <= 0 || mw->height() <= 0) {
        QSize hint = mw->sizeHint();
        mw->resize(hint.width()  > 0 ? hint.width()  : 640,
                   hint.height() > 0 ? hint.height() : 480);
    }
    OwlPaletteScope scope(theme);
    /* processEvents() applique layout/paint en attente pour eviter un
     * rendu blanc (le widget est freshly built, layout pas encore propage). */
    if (g_app) QApplication::processEvents();
    QPixmap pix = mw->grab();
    if (g_app) QApplication::processEvents();
    return qpixmap_to_rgba_buffer(pix, out_width, out_height);
}

extern "C" unsigned char* owl_ui_control_grab_rgba(OwlControl* c,
                                                    int theme,
                                                    int* out_width,
                                                    int* out_height) {
    if (!c) return nullptr;
    QWidget* wid = reinterpret_cast<QWidget*>(c);
    /*  — pour un champ composite (libelle integre : [QLabel|QLineEdit],
     * [QLabel|QComboBox]), la boite du champ est le wrapper, pas le widget
     * logique interne. La capture doit rendre la boite complete (libelle
     * inclus), en accord avec la geometrie ( route deja ..Largeur / X
     * vers ce meme wrapper). Champ simple : pas de wrapper, widget inchange. */
    if (QWidget* rep = owl_widget_layout_rep(wid)) wid = rep;
    if (wid->width() <= 0 || wid->height() <= 0) {
        QSize hint = wid->sizeHint();
        wid->resize(hint.width()  > 0 ? hint.width()  : 100,
                    hint.height() > 0 ? hint.height() : 30);
    }
    OwlPaletteScope scope(theme);
    if (g_app) QApplication::processEvents();
    QPixmap pix = wid->grab();
    if (g_app) QApplication::processEvents();
    return qpixmap_to_rgba_buffer(pix, out_width, out_height);
}

extern "C" unsigned char* owl_ui_screen_grab_rgba(int theme,
                                                   int* out_width,
                                                   int* out_height) {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return nullptr;
    /* grabWindow(0) = root window (le desktop entier). En headless / sans
     * display physique, QGuiApplication::primaryScreen() peut retourner
     * NULL (cas couvert ci-dessus) ; sinon Qt offscreen platform renvoie
     * un ecran virtuel et grabWindow produit un pixmap vide / noir. La
     * decision "headless => echec" est cote stdlib_ui.c (verifie
     * vm.headless_mode avant d'appeler ce helper).
     *
     * Lot 3 : le push/pop palette ne tinte pas un grab du desktop reel
     * (l'override n'agit que sur les widgets Qt de l'app), mais on garde
     * le scope pour coherence d'interface et future evolution. */
    OwlPaletteScope scope(theme);
    QPixmap pix = screen->grabWindow(0);
    return qpixmap_to_rgba_buffer(pix, out_width, out_height);
}

/*  — __CaptureFenetre : grab de la fenetre entiere vers un PNG.
 * QWidget::grab() rend off-screen (render()), donc marche sous
 * QT_QPA_PLATFORM=offscreen sans display physique. La fenetre doit etre
 * batie (widgets montes) ; le processEvents() applique layout/paint en
 * attente pour eviter un rendu blanc. Retourne 1 si le PNG est ecrit,
 * 0 sinon (widget/path NULL, pixmap vide, echec d'ecriture disque). */
extern "C" int owl_ui_window_capture(OwlWindow* w, const char* path) {
    if (!w || !path) return 0;
    QWidget* wid = reinterpret_cast<QWidget*>(w);
    if (wid->width() <= 0 || wid->height() <= 0) {
        QSize hint = wid->sizeHint();
        wid->resize(hint.width()  > 0 ? hint.width()  : 640,
                    hint.height() > 0 ? hint.height() : 480);
    }
    if (g_app) QApplication::processEvents();
    QPixmap pix = wid->grab();
    if (g_app) QApplication::processEvents();
    if (pix.isNull()) return 0;
    return pix.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

/*  — retrouve un QMenu par nom (titre sans mnemonique '&', insensible
 * a la casse) en descendant recursivement l'arbre d'un QMenu. */
static QMenu* owl_menu_find_submenu(QMenu* menu, const QString& target) {
    if (!menu) return nullptr;
    const QList<QAction*> acts = menu->actions();
    for (QAction* act : acts) {
        QMenu* sub = act->menu();
        if (!sub) continue;
        QString title = sub->title();
        title.remove(QLatin1Char('&'));
        if (title.compare(target, Qt::CaseInsensitive) == 0) return sub;
        QMenu* deeper = owl_menu_find_submenu(sub, target);
        if (deeper) return deeper;
    }
    return nullptr;
}

/*  — __CaptureSousMenu : capture offscreen d'un sous-menu deroule vers
 * un PNG, en preservant l'alpha. Le sous-menu nomme (par son libelle, mnemonique
 * '&' ignore) est retrouve dans l'arbre du menu principal, son layout est force
 * hors-ecran, puis rendu dans un pixmap ARGB pre-rempli transparent. Effet :
 * les coins hors-arrondi rendus transparents par WA_TranslucentBackground
 * ressortent transparents dans le PNG (fond de fenetre non peint), alors qu'un
 * panneau opaque y montrerait sa couleur de fond. Retourne 1 si le PNG est
 * ecrit, 0 sinon (menu introuvable, taille nulle, echec disque). */
extern "C" int owl_ui_menu_capture_submenu(OwlWindow* w,
                                           const char* submenu_name,
                                           const char* path) {
    if (!w || !submenu_name || !path) return 0;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    if (!bar) return 0;
    QString target = QString::fromUtf8(submenu_name);
    target.remove(QLatin1Char('&'));

    QMenu* found = nullptr;
    const QList<QAction*> tops = bar->actions();
    for (QAction* act : tops) {
        QMenu* top = act->menu();
        if (!top) continue;
        QString title = top->title();
        title.remove(QLatin1Char('&'));
        if (title.compare(target, Qt::CaseInsensitive) == 0) { found = top; break; }
        found = owl_menu_find_submenu(top, target);
        if (found) break;
    }
    if (!found) return 0;

    /* Force un layout correct hors-ecran sans declencher le popup reel :
     * WA_DontShowOnScreen + show() polit et dimensionne le QMenu comme s'il
     * etait affiche, mais sans fenetre visible. */
    found->setAttribute(Qt::WA_DontShowOnScreen, true);
    found->ensurePolished();
    found->show();
    found->adjustSize();
    if (g_app) QApplication::processEvents();

    QSize sz = found->size();
    if (sz.width() <= 0 || sz.height() <= 0) sz = found->sizeHint();
    if (sz.width() <= 0 || sz.height() <= 0) { found->hide(); return 0; }

    /* Le prefill simule le backing de la FENETRE popup reelle : opaque =>
     * couleur de fond de la palette (ce qui deborde dans les coins hors-arrondi,
     * = le bug) ; translucide (WA_TranslucentBackground) => transparent (coins
     * propres, = le fix). On rend ensuite SANS DrawWindowBackground : seul le
     * panneau QSS (clippe au border-radius) est peint par-dessus le backing,
     * reproduisant fidelement ce que l'OS composite a l'ecran. */
    QPixmap pix(sz);
    if (found->testAttribute(Qt::WA_TranslucentBackground)) {
        pix.fill(Qt::transparent);
    } else {
        pix.fill(found->palette().color(QPalette::Window));
    }
    found->render(&pix, QPoint(), QRegion(), QWidget::DrawChildren);
    if (g_app) QApplication::processEvents();
    found->hide();

    if (pix.isNull()) return 0;
    return pix.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

/*  — read-back test-only : indique si l'option de menu designee par son
 * libelle (mnemonique '&' ignore, casse insensible), dans le sous-menu nomme,
 * porte une icone effective (QAction dont le QIcon est non nul). Permet de
 * verifier automatiquement la resolution de OptionMenu..Image (chemin fichier
 * OU reference catalogue "icones.X") sans oracle pixel. Retourne 1 si l'icone
 * est posee, 0 sinon (sous-menu/option introuvable ou icone absente). */
extern "C" int owl_ui_menu_option_has_icon(OwlWindow* w,
                                           const char* submenu_name,
                                           const char* option_label) {
    if (!w || !submenu_name || !option_label) return 0;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    if (!bar) return 0;
    QString target = QString::fromUtf8(submenu_name);
    target.remove(QLatin1Char('&'));

    QMenu* found = nullptr;
    const QList<QAction*> tops = bar->actions();
    for (QAction* act : tops) {
        QMenu* top = act->menu();
        if (!top) continue;
        QString title = top->title();
        title.remove(QLatin1Char('&'));
        if (title.compare(target, Qt::CaseInsensitive) == 0) { found = top; break; }
        found = owl_menu_find_submenu(top, target);
        if (found) break;
    }
    if (!found) return 0;

    QString wanted = QString::fromUtf8(option_label);
    wanted.remove(QLatin1Char('&'));
    const QList<QAction*> opts = found->actions();
    for (QAction* a : opts) {
        if (a->menu() || a->isSeparator()) continue;
        QString txt = a->text();
        txt.remove(QLatin1Char('&'));
        if (txt.compare(wanted, Qt::CaseInsensitive) == 0)
            return a->icon().isNull() ? 0 : 1;
    }
    return 0;
}

extern "C" void owl_ui_window_set_position(OwlWindow* w, int x, int y) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    /* -1 sur un axe = centrage sur l'ecran principal. */
    if (x < 0 || y < 0) {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geo = screen->availableGeometry();
            int fw = mw->width()  > 0 ? mw->width()  : 640;
            int fh = mw->height() > 0 ? mw->height() : 480;
            owl_ui_center_clamp(geo.x(), geo.y(), geo.width(), geo.height(),
                                fw, fh, &x, &y);
        } else {
            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
    }
    mw->move(x, y);
}

/* Correction POST-show : le clamp pre-show (owl_ui_center_clamp) positionne la
 * zone CLIENT, mais avant show() Qt ne connait pas les marges du cadre (barre
 * de titre). Une fenetre plus haute que l'ecran peut donc se retrouver avec sa
 * barre de titre a y<0 (hors ecran). frameGeometry() n'inclut le chrome
 * qu'APRES show() : on ramene alors le coin haut-gauche du CADRE dans la zone
 * disponible. On ne descend jamais la fenetre (dx/dy >= 0), on ne fait que
 * degager la barre de titre vers le bas/droite. Non testable en offscreen
 * (frameGeometry == geometry sans decorations) : gate visuel dev. */
extern "C" void owl_ui_window_clamp_titlebar_onscreen(OwlWindow* w) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QScreen* screen = mw->screen() ? mw->screen() : QGuiApplication::primaryScreen();
    if (!screen) return;
    QRect avail = screen->availableGeometry();
    QRect frame = mw->frameGeometry();
    int dx = (frame.left() < avail.left()) ? (avail.left() - frame.left()) : 0;
    int dy = (frame.top()  < avail.top())  ? (avail.top()  - frame.top())  : 0;
    owl_menu_trace("clamp_titlebar avail=(%d,%d %dx%d) frame=(%d,%d %dx%d) dx=%d dy=%d",
                   avail.x(), avail.y(), avail.width(), avail.height(),
                   frame.x(), frame.y(), frame.width(), frame.height(), dx, dy);
    if (dx || dy) mw->move(mw->x() + dx, mw->y() + dy);
}

extern "C" int owl_ui_window_get_x(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->x();
}

extern "C" int owl_ui_window_get_y(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->y();
}

extern "C" void owl_ui_window_set_visible(OwlWindow* w, int visible) {
    if (!w) return;
    reinterpret_cast<QMainWindow*>(w)->setVisible(visible != 0);
}

extern "C" int owl_ui_window_get_visible(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->isVisible() ? 1 : 0;
}

/* Stylesheet applique au centralWidget quand la fenetre est grisee :
 * fond gris fonce + opacite reduite des descendants pour signaler
 * visuellement le HS. setEnabled(false) propage le grisage natif Qt
 * sur tous les enfants ; le stylesheet ajoute un contraste fond. */
static const char* k_disabled_bg_stylesheet =
    "QWidget#owlCentral { background-color: rgb(72, 72, 78); }";

extern "C" void owl_ui_window_set_disabled(OwlWindow* w, int disabled) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    mw->setEnabled(disabled == 0);
    QWidget* central = mw->centralWidget();
    if (central) {
        if (central->objectName().isEmpty()) {
            central->setObjectName(QStringLiteral("owlCentral"));
        }
        central->setStyleSheet(disabled ? k_disabled_bg_stylesheet : QString());
    }
}

extern "C" int owl_ui_window_get_disabled(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->isEnabled() ? 0 : 1;
}

/*  — couleur de fond QSS avec canal alpha. Émet `rgb(r,g,b)`
 * STRICTEMENT inchangé (byte-identique à l'existant) quand alpha >= 255 (opaque,
 * cas dominant) ; `rgba(r,g,b,a/255.0)` uniquement si alpha < 255 (fond
 * translucide, glassmorphisme). v = couleur BGR canon (R + V*256 + B*65536).
 * Point d'émission unique partagé par tous les chemins de fond (fenêtre,
 * conteneur, contrôle, arrêts de dégradé) : la non-régression opaque est garantie
 * par la branche alpha>=255. */
static QString owl_bg_css(int v, int alpha) {
    int r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
    if (alpha >= 255)
        return QString("rgb(%1,%2,%3)").arg(r).arg(g).arg(b);
    return QString("rgba(%1,%2,%3,%4)")
        .arg(r).arg(g).arg(b).arg(alpha / 255.0, 0, 'g', 4);
}

/*  — couleur de TEXTE alpha-aware, miroir strict de owl_bg_css : `rgb(...)`
 * byte-identique à l'ancien émetteur opaque quand alpha >= 255, `rgba(...)` sinon
 * (texte translucide, parité glassmorphisme du fond). Point d'émission unique de
 * la couleur de texte de style. Format sans espace (symétrique du fond) — les
 * fragments d'état gardent leur propre format espacé (voir owl_ui_control_set_state_style). */
static QString owl_fg_css(int v, int alpha) {
    return owl_bg_css(v, alpha);
}

/* Couleur de fond fenetre : appliquee au central widget via stylesheet
 * cible (QWidget#owlCentral) pour ne pas teinter les enfants. OWL stocke
 * en BGR (canon : R + V*256 + B*65536). -1 = retire le fond explicite. */
extern "C" void owl_ui_window_set_bg_color_alpha(OwlWindow* w, int rgb, int alpha) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QWidget* central = mw->centralWidget();
    if (!central) return;
    if (central->objectName().isEmpty()) {
        central->setObjectName(QStringLiteral("owlCentral"));
    }
    central->setProperty("owl_bg", rgb < 0 ? QVariant() : QVariant(rgb));
    if (rgb < 0) {
        central->setStyleSheet(QString());
    } else {
        central->setStyleSheet(
            "QWidget#owlCentral { background-color: " + owl_bg_css(rgb, alpha) + "; }");
    }
}

extern "C" void owl_ui_window_set_bg_color(OwlWindow* w, int rgb) {
    owl_ui_window_set_bg_color_alpha(w, rgb, 255);
}

extern "C" int owl_ui_window_get_bg_color(OwlWindow* w) {
    if (!w) return -1;
    QWidget* central = reinterpret_cast<QMainWindow*>(w)->centralWidget();
    if (!central) return -1;
    QVariant v = central->property("owl_bg");
    return v.isValid() ? v.toInt() : -1;
}

extern "C" void owl_ui_window_set_icon(OwlWindow* w, const char* path_utf8) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    if (!path_utf8 || !*path_utf8) {
        mw->setWindowIcon(QIcon());
        return;
    }
    mw->setWindowIcon(QIcon(QString::fromUtf8(path_utf8)));
}

/* Helper : RGBA8 top-down → QImage sans copie pixel-par-pixel. La QImage
 * partage le buffer source ; on appelle .copy() pour la rendre autonome. */
static QImage rgba_to_qimage(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return QImage();
    QImage shared(rgba, w, h, w * 4, QImage::Format_RGBA8888);
    return shared.copy();
}

extern "C" void owl_ui_window_set_icon_rgba(OwlWindow* w,
                                            const unsigned char* rgba,
                                            int width, int height) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QImage img = rgba_to_qimage(rgba, width, height);
    if (img.isNull()) { mw->setWindowIcon(QIcon()); return; }
    mw->setWindowIcon(QIcon(QPixmap::fromImage(img)));
}

extern "C" int owl_ui_window_get_icon_width(OwlWindow* w) {
    if (!w) return -1;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QIcon icon = mw->windowIcon();
    if (icon.isNull()) return 0;
    int best = 0;
    const QList<QSize> sizes = icon.availableSizes();
    for (const QSize& s : sizes) {
        if (s.width() > best) best = s.width();
    }
    /* Icone posee depuis un fichier .ico/.png ou pixmap : availableSizes
     * peut etre vide pour un theme dynamique -> renvoie 1 (icone presente). */
    if (best == 0) best = icon.isNull() ? 0 : 1;
    return best;
}

extern "C" void owl_ui_window_set_resizable(OwlWindow* w, int resizable) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    if (resizable) {
        mw->setMinimumSize(0, 0);
        mw->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    } else {
        /* Fige aux dimensions courantes. Si la fenetre n'a pas encore ete
         * dimensionnee, fallback sur sizeHint pour eviter un fix a 0x0. */
        int fw = mw->width()  > 0 ? mw->width()  : mw->sizeHint().width();
        int fh = mw->height() > 0 ? mw->height() : mw->sizeHint().height();
        mw->setFixedSize(fw, fh);
    }
}

extern "C" int owl_ui_window_get_resizable(OwlWindow* w) {
    if (!w) return 1;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    return (mw->minimumSize() == mw->maximumSize()) ? 0 : 1;
}

/* ------- Menu classique de fenetre -------
 *
 * Habillage par defaut CABLE EN DUR. Le panneau d'une fenetre OWL est sombre
 * (k_disabled_bg_stylesheet / owl_ui_window_set_bg_color : rgb(72,72,78)) ;
 * une barre laissee au style natif clair jurerait. La palette ci-dessous se
 * derive de ce fond : barre legerement plus sombre que le panneau, deroulants
 * un cran en dessous, texte clair, accent bleu-gris desature sur l'item
 * survole. La palette Qt est posee EN PLUS du QSS : les primitives dessinees
 * par le style (fleche de cascade, coche) lisent la palette, pas la feuille. */

static const int  K_MENU_BAR_BG[3]     = { 60,  60,  66  };
static const int  K_MENU_POPUP_BG[3]   = { 54,  54,  60  };
static const int  K_MENU_BORDER[3]     = { 94,  94,  102 };
static const int  K_MENU_TEXT[3]       = { 234, 234, 238 };
static const int  K_MENU_TEXT_OFF[3]   = { 138, 138, 146 };
static const int  K_MENU_ACCENT[3]     = { 92,  110, 152 };
static const int  K_MENU_SEP[3]        = { 88,  88,  96  };

static QString owl_menu_rgb(const int c[3]) {
    return QString("rgb(%1,%2,%3)").arg(c[0]).arg(c[1]).arg(c[2]);
}

/*  — BGR canon (0x00BBGGRR) -> CSS rgb() SANS espaces (format
 * identique a owl_menu_rgb : garantit un rendu par defaut byte-identique).
 *  — quand le slot porte le flag OWL_MENU_COL_ALPHA (couleur
 * semi-transparente via Couleur(r,v,b,a) avec a<255), emet rgba() avec l'octet
 * alpha (bits 24-31). Sinon rgb() sur les 24 bits BGR (retro-compatible). */
static QString owl_menu_bgr_css(int64_t v) {
    int r = (int)(v & 0xFF), g = (int)((v >> 8) & 0xFF), b = (int)((v >> 16) & 0xFF);
    if (v & OWL_MENU_COL_ALPHA) {
        int a = (int)((v >> 24) & 0xFF);
        return QString("rgba(%1,%2,%3,%4)").arg(r).arg(g).arg(b).arg(a);
    }
    return QString("rgb(%1,%2,%3)").arg(r).arg(g).arg(b);
}

/*  — couleur posee (>=0, BGR) sinon defaut du theme. */
static QString owl_menu_pick(int64_t slot, const int def[3]) {
    return slot >= 0 ? owl_menu_bgr_css(slot) : owl_menu_rgb(def);
}

/*  — fragment QSS des attributs de police POSES d'une surface. Vide
 * si aucun attribut posé (le rendu par defaut reste inchange). */
static QString owl_menu_font_css(const OwlMenuStyleSurface* s) {
    if (!s) return QString();
    QString out;
    if (s->police_nom && s->police_nom[0]) {
        out += QString("font-family: '%1'; ").arg(QString::fromUtf8(s->police_nom));
    }
    if (s->police_taille >= 0) out += QString("font-size: %1pt; ").arg(s->police_taille);
    if (s->police_gras >= 0)   out += QString("font-weight: %1; ").arg(s->police_gras ? "bold" : "normal");
    if (s->police_italique >= 0) out += QString("font-style: %1; ").arg(s->police_italique ? "italic" : "normal");
    if (s->police_souligne >= 0 || s->police_barre >= 0) {
        QStringList deco;
        if (s->police_souligne == 1) deco << "underline";
        if (s->police_barre == 1)    deco << "line-through";
        if (deco.isEmpty()) deco << "none";
        out += QString("text-decoration: %1; ").arg(deco.join(' '));
    }
    return out;
}

/*  — pose la police d'une surface de menu directement sur le widget via
 * setFont. Qt n'honore PAS `font-family`/`font-size`/`font-weight` en QSS sur
 * QMenuBar::item / QMenu::item de façon fiable (limitation connue : la police
 * d'un menu se règle par QWidget::setFont, pas par la feuille de l'item). Le
 * fragment QSS reste émis (owl_menu_font_css) pour les couleurs et cohérence,
 * mais c'est ce setFont qui rend réellement la police. police_nom est déjà
 * résolu en famille embarquée côté compilateur (menu_style_store_value), donc
 * on l'utilise tel quel. No-op si aucun attribut de police posé -> défaut du
 * widget inchangé (non-régression des menus sans police). */
static void owl_menu_apply_font(QWidget* target, const OwlMenuStyleSurface* s) {
    if (!target || !s) return;
    const bool any = (s->police_nom && s->police_nom[0]) || s->police_taille >= 0
                     || s->police_gras >= 0 || s->police_italique >= 0
                     || s->police_souligne >= 0 || s->police_barre >= 0;
    if (!any) return;
    QFont f = target->font();
    if (s->police_nom && s->police_nom[0]) f.setFamily(QString::fromUtf8(s->police_nom));
    if (s->police_taille >= 0)   f.setPointSize(s->police_taille);
    if (s->police_gras >= 0)     f.setBold(s->police_gras != 0);
    if (s->police_italique >= 0) f.setItalic(s->police_italique != 0);
    if (s->police_souligne >= 0) f.setUnderline(s->police_souligne == 1);
    if (s->police_barre >= 0)    f.setStrikeOut(s->police_barre == 1);
    target->setFont(f);
}

/*  — la police des OPTIONS (surface OptionMenu) doit se poser sur chaque
 * QMenu déroulant, or ceux-ci sont créés paresseusement (owl_ui_menu_add_submenu)
 * SANS accès au style résolu. On mémorise donc la police d'option résolue sur le
 * QMainWindow (mêmes propriétés dynamiques que owl_menu_arrondi), lue à chaque
 * création de déroulant. NULL/reset efface la mémoire (menu suivant sans police
 * d'option -> déroulants au défaut). */
static const char* K_OPT_FAM   = "owl_optfont_fam";
static const char* K_OPT_SIZE  = "owl_optfont_size";
static const char* K_OPT_BOLD  = "owl_optfont_bold";
static const char* K_OPT_ITAL  = "owl_optfont_ital";
static const char* K_OPT_UNDL  = "owl_optfont_undl";
static const char* K_OPT_STRK  = "owl_optfont_strk";

static void owl_menu_store_option_font(QWidget* holder,
                                       const OwlMenuStyleSurface* s) {
    if (!holder) return;
    holder->setProperty(K_OPT_FAM, (s && s->police_nom && s->police_nom[0])
                                       ? QString::fromUtf8(s->police_nom)
                                       : QString());
    holder->setProperty(K_OPT_SIZE, s ? s->police_taille   : -1);
    holder->setProperty(K_OPT_BOLD, s ? s->police_gras     : -1);
    holder->setProperty(K_OPT_ITAL, s ? s->police_italique : -1);
    holder->setProperty(K_OPT_UNDL, s ? s->police_souligne : -1);
    holder->setProperty(K_OPT_STRK, s ? s->police_barre    : -1);
}

/*  — applique sur `menu` la police d'option mémorisée sur `holder`
 * (QMainWindow). Reconstruit une surface plate puis délègue à owl_menu_apply_font
 * (no-op si rien posé). Le famille borrow pointe sur un QByteArray local vivant
 * le temps de l'appel : suffisant, setFont copie la famille. */
static void owl_menu_apply_stored_option_font(QWidget* holder, QWidget* menu) {
    if (!holder || !menu) return;
    const QByteArray fam = holder->property(K_OPT_FAM).toString().toUtf8();
    OwlMenuStyleSurface s;
    s.couleur = -1; s.couleur_fond = -1;
    s.police_nom      = fam.isEmpty() ? nullptr : fam.constData();
    s.police_taille   = holder->property(K_OPT_SIZE).toInt();
    s.police_gras     = holder->property(K_OPT_BOLD).toInt();
    s.police_italique = holder->property(K_OPT_ITAL).toInt();
    s.police_souligne = holder->property(K_OPT_UNDL).toInt();
    s.police_barre    = holder->property(K_OPT_STRK).toInt();
    /* property() absente -> toInt() rend 0, pas -1 : sans mémoire, on ne veut
     * RIEN poser. On neutralise les slots à -1 quand la propriété n'existe pas. */
    if (!holder->property(K_OPT_SIZE).isValid()) s.police_taille   = -1;
    if (!holder->property(K_OPT_BOLD).isValid()) s.police_gras     = -1;
    if (!holder->property(K_OPT_ITAL).isValid()) s.police_italique = -1;
    if (!holder->property(K_OPT_UNDL).isValid()) s.police_souligne = -1;
    if (!holder->property(K_OPT_STRK).isValid()) s.police_barre    = -1;
    owl_menu_apply_font(menu, &s);
}

/*  — feuille de style du menu, paramétrée par le style résolu. `style`
 * NULL ou has_style==0 : chaque couleur retombe sur sa constante K_MENU_*,
 * aucun fragment police. Sinon chaque prop posée remplace le défaut, prop par
 * prop. Rendu voulu : plat, carré, surbrillance pleine largeur — un seul look
 * cohérent pour le défaut ET le stylé (pas de branche "rounded" par défaut). */
static QString owl_menu_stylesheet(const OwlMenuStyleResolved* style) {
    const OwlMenuStyleSurface* barreBase = style ? &style->barre_base   : nullptr;
    const OwlMenuStyleSurface* barreHov  = style ? &style->barre_survol : nullptr;
    const OwlMenuStyleSurface* optBase   = style ? &style->option_base  : nullptr;
    const OwlMenuStyleSurface* optHov    = style ? &style->option_survol: nullptr;
    const int64_t barreFondSlot  = barreBase ? barreBase->couleur_fond : -1;
    const int64_t barreTxtSlot   = barreBase ? barreBase->couleur      : -1;
    const int64_t barreHovFond   = barreHov  ? barreHov->couleur_fond  : -1;
    const int64_t barreHovTxt    = barreHov  ? barreHov->couleur       : -1;
    const int64_t popFondSlot    = style ? style->sousmenu_couleur_fond       : -1;
    const int64_t sepSlot        = style ? style->sousmenu_couleur_separateur : -1;
    /* Bordure + arrondi du panneau (fallback par prop : Epaisseur->1,
     * CouleurBordure->K_MENU_BORDER, Arrondi->0). Epaisseur/Arrondi restent des
     * tailles en pixels (int), seules les couleurs portent l'alpha (int64). */
    const int     bordThickSlot  = style ? style->sousmenu_epaisseur       : -1;
    const int64_t bordColorSlot  = style ? style->sousmenu_couleur_bordure : -1;
    const int     arrondiSlot    = style ? style->sousmenu_arrondi         : -1;
    const int64_t optTxtSlot     = optBase ? optBase->couleur      : -1;
    const int64_t optFondSlot    = optBase ? optBase->couleur_fond : -1;
    const int64_t optHovFond     = optHov  ? optHov->couleur_fond  : -1;
    const int64_t optHovTxt      = optHov  ? optHov->couleur       : -1;

    const QString barBg   = owl_menu_pick(barreFondSlot, K_MENU_BAR_BG);
    const QString popBg   = owl_menu_pick(popFondSlot,   K_MENU_POPUP_BG);
    /* Liseré du panneau : epaisseur (>=0 posé, sinon 1), couleur (posée sinon
     * K_MENU_BORDER), arrondi (>=0 posé sinon 0). */
    const int     bordThick = bordThickSlot >= 0 ? bordThickSlot : 1;
    const QString bordColor = owl_menu_pick(bordColorSlot, K_MENU_BORDER);
    const int     arrondi   = arrondiSlot   >= 0 ? arrondiSlot   : 0;
    /* Gouttiere nulle : le panneau n'a AUCUN retrait interne (padding 0px). Les
     * options remplissent le panneau bord a bord ; l'arrondi est reporte sur les
     * options elles-memes (border-radius sur QMenu::item, meme rayon que le
     * panneau) pour que la 1ere/derniere option epouse les coins arrondis. */
    const int     gutter    = 0;
    /* Arrondi reporte sur les options : l'option est peinte A L'INTERIEUR de la
     * bordure du panneau (epaisseur bordThick px), donc son arc doit suivre le
     * rayon INTERIEUR du panneau = arrondi - bordThick (concentrique avec le bord
     * interne de la bordure), pas le rayon exterieur. Sinon l'arc de l'option est
     * trop grand et laisse un lisere de fond-panneau entre les deux arcs.
     * Applique au fond de base ET au survol (:selected). */
    const int     itemRadiusPx = arrondi - bordThick > 0 ? arrondi - bordThick : 0;
    const QString itemRadius = arrondi > 0
        ? QString("border-radius: ") + QString::number(itemRadiusPx) + "px; "
        : QString();
    const QString text    = owl_menu_pick(barreTxtSlot,  K_MENU_TEXT);
    const QString textOff = owl_menu_rgb(K_MENU_TEXT_OFF);
    const QString sep     = owl_menu_pick(sepSlot,       K_MENU_SEP);
    /* Item de barre survolé : fond accent + texte blanc par défaut. */
    const QString barHovBg  = owl_menu_pick(barreHovFond, K_MENU_ACCENT);
    const QString barHovTx  = barreHovTxt >= 0 ? owl_menu_bgr_css(barreHovTxt) : QString("#FFFFFF");
    /* Rangée de menu (panneau) : texte + fond de base + survol. */
    const QString itemTx    = owl_menu_pick(optTxtSlot, K_MENU_TEXT);
    const QString itemBg    = optFondSlot >= 0 ? owl_menu_bgr_css(optFondSlot) : QString("transparent");
    const QString itemHovBg = owl_menu_pick(optHovFond, K_MENU_ACCENT);
    const QString itemHovTx = optHovTxt >= 0 ? owl_menu_bgr_css(optHovTxt) : QString("#FFFFFF");
    /* Fragments police des OPTIONS (vides si rien posé -> défaut inchangé).
     * Rien pour la BARRE : Qt n'honore pas la police sur QMenuBar::item (ni
     * font-family, ni font-size/weight de façon fiable) -> c'était du code mort
     * qui perpétuait la confusion. La police de barre est posée exclusivement par
     * owl_menu_apply_font (setFont sur le QMenuBar), mesurable via l'encre peinte
     * (__DebugEncreBarreMenu). */
    const QString optFont    = owl_menu_font_css(optBase);

    return
        "QMenuBar { background-color: " + barBg + "; color: " + text + "; "
            "padding: 0px; border: 0px; } "
        "QMenuBar::item { background: transparent; color: " + text + "; "
            "padding: 4px 10px; margin: 0px; } "
        "QMenuBar::item:selected { background-color: " + barHovBg + "; color: " + barHovTx + "; } "
        "QMenuBar::item:pressed { background-color: " + barHovBg + "; color: " + barHovTx + "; } "
        "QMenuBar::item:disabled { color: " + textOff + "; } "
        "QMenu { background-color: " + popBg + "; color: " + text + "; "
            "border: " + QString::number(bordThick) + "px solid " + bordColor + "; "
            "border-radius: " + QString::number(arrondi) + "px; "
            "padding: " + QString::number(gutter) + "px; } "
        "QMenu::item { background: " + itemBg + "; color: " + itemTx + "; " + optFont + itemRadius +
            "padding: 6px 28px 6px 24px; margin: 0px; } "
        "QMenu::item:selected { background-color: " + itemHovBg + "; color: " + itemHovTx + "; " + itemRadius + "} "
        "QMenu::item:disabled { color: " + textOff + "; background: transparent; } "
        /* Respiration gauche de l'icone. Sous-controle QMenu::icon : origine par
         * defaut = Origin_Padding + AlignLeft (cf. qstylesheetstyle defaultOrigin/
         * defaultPosition pour PseudoElement_MenuIcon), donc l'icone se colle au
         * bord interne de l'item -- le padding-left de QMenu::item ne pousse QUE le
         * texte, pas l'icone. `left` (mode position relatif) translate l'icone vers
         * la droite sans toucher la colonne de texte (positionnee sur maxIconWidth,
         * independant), preservant l'alignement des options sans icone. */
        "QMenu::icon { left: 10px; } "
        /* Colonne de la coche cochable : largeur/hauteur deterministes pour que la
         * coche vectorielle (OwlMenuProxyStyle::PE_IndicatorMenuCheckMark) soit
         * dimensionnee et centree proprement, et que le texte reserve sa gouttiere.
         * Aucune propriete « dessinable » (pas de fond/bordure/image) -> une option
         * DECOCHEE n'affiche aucune case (rendu classique : rien quand off). */
        "QMenu::indicator { width: 18px; height: 18px; margin-left: 6px; } "
        /* Séparateur QSS net : height + background explicites + `border: none`
         * pour REMPLACER le séparateur natif (frame sunken/etched que Qt dessine
         * sinon EN PLUS -> bande grise autour du trait). margin 0 partout :
         * la marge verticale exposait le fond de panneau dans la gouttière
         * (bande grise autour du trait car fond d'item != fond de panneau). */
        "QMenu::separator { height: 1px; background-color: " + sep + "; "
            "border: none; margin: 0px; } "
        "QMenu::right-arrow { margin-right: 8px; }";
}

static QColor owl_menu_color(const int c[3]) {
    return QColor(c[0], c[1], c[2]);
}

/* Style proxy applique aux barres et panneaux de menu. Le style effectif d'un
 * menu stylise est QStyleSheetStyle (setStyleSheet est pose sur la barre) ;
 * pour les primitives non couvertes par la feuille il delegue au style de base
 * du widget -> ce proxy. On y reprend la coche cochable :
 *   QCommonStyle::PE_IndicatorMenuCheckMark dessine une marque cappee a 7px, en
 *   escalier de segments 1px SANS antialiasing et decalee de +1px -> rendu
 *   pixelise/minuscule/pas centre. On la remplace par une coche vectorielle
 *   nette, antialiasee et centree, dimensionnee sur la colonne (QMenu::indicator).
 * QStyleSheetStyle n'appelle ce primitive que pour un item COCHE et SANS icone
 * (branche icon.isNull()) : coexistence Coché+Image gere en amont (menu_build_node
 * pose l'icone quand decoche, la laisse tomber quand coche pour rendre la coche). */
class OwlMenuProxyStyle : public QProxyStyle {
public:
    /* base==nullptr : QProxyStyle enrobe le style applicatif (usage historique
     * par-widget, retire ). base!=nullptr : enrobe un style explicite
     * dont il prend la propriete (usage style applicatif : enrobe Fusion). */
    explicit OwlMenuProxyStyle(QStyle* base = nullptr) : QProxyStyle(base) {}

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override {
        if (pe == PE_IndicatorMenuCheckMark && opt) {
            drawCrispCheck(opt, p);
            return;
        }
        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }

private:
    static void drawCrispCheck(const QStyleOption* opt, QPainter* p) {
        const QRect r = opt->rect;
        const qreal side = qMin(r.width(), r.height());
        if (side <= 0) return;
        /* Boite carree centree dans la colonne, avec une marge proportionnelle
         * pour aerer la coche. */
        const qreal margin = side * 0.20;
        const qreal box    = side - 2.0 * margin;
        const qreal bx = r.x() + (r.width()  - side) / 2.0 + margin;
        const qreal by = r.y() + (r.height() - side) / 2.0 + margin;
        /* Coche en trois points : descente courte a gauche puis remontee longue
         * a droite (proportions classiques d'un « check »). */
        QPainterPath path;
        path.moveTo(bx + box * 0.06, by + box * 0.55);
        path.lineTo(bx + box * 0.40, by + box * 0.90);
        path.lineTo(bx + box * 0.96, by + box * 0.14);

        QColor col;
        if (!(opt->state & State_Enabled))
            col = opt->palette.color(QPalette::Disabled, QPalette::Text);
        else if (opt->state & State_Selected)
            col = opt->palette.highlightedText().color();
        else
            col = opt->palette.text().color();

        QPen pen(col);
        pen.setWidthF(qMax<qreal>(1.5, side * 0.14));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
        p->restore();
    }
};

/*  lot B — QProxyStyle custom pilotant le placement icone/texte d'un
 * QPushButton selon la property "owl_position_image" (valeur E_PositionImageBouton).
 * La source de verite est la property ; le proxy la consulte a chaque repaint.
 *
 * Strategie par valeur :
 *   - Gauche  : natif Qt (icone a gauche du texte) — rien a faire.
 *   - Droite  : layoutDirection RTL sur l'option (icone a droite du texte).
 *   - Haut/Bas: on repeint le label nous-memes (icone empilee sur le texte).
 *   - Seule   : on efface le texte de l'option avant de dessiner le label.
 *   - TexteSeul: on efface l'icone de l'option avant de dessiner le label.
 * Le proxy ne modifie jamais l'etat du widget (texte/icone reels preserves
 * pour le read-back BTN..Image et ..Libellé).
 *
 *  — ce proxy est desormais un maillon du STYLE APPLICATIF (enrobe le
 * proxy menu, cf. owl_app_style), plus jamais pose par-widget via
 * QWidget::setStyle. Combiner un style par-widget avec une feuille QSS sur le
 * meme bouton (Style..CadreExtérieur/CouleurFond passe par setStyleSheet) etait
 * le meme montage non supporte qui provoquait l'UAF des menus. Au
 * niveau application, tous les boutons heritent du proxy : la property par
 * defaut (GAUCHE) rend le chemin natif, les boutons sans position d'image sont
 * inchanges. */
class OwlButtonProxyStyle : public QProxyStyle {
public:
    explicit OwlButtonProxyStyle(QStyle* base) : QProxyStyle(base) {}

    void drawControl(ControlElement element, const QStyleOption* opt,
                     QPainter* p, const QWidget* widget) const override {
        if (element == CE_PushButtonLabel) {
            const auto* bopt = qstyleoption_cast<const QStyleOptionButton*>(opt);
            int pos = OWL_POSBTNIMG_GAUCHE;
            if (widget) {
                QVariant v = widget->property("owl_position_image");
                if (v.isValid()) pos = v.toInt();
            }
            if (bopt) {
                QStyleOptionButton local = *bopt;
                switch (pos) {
                    case OWL_POSBTNIMG_DROITE:
                        local.direction = Qt::RightToLeft;
                        QProxyStyle::drawControl(element, &local, p, widget);
                        return;
                    case OWL_POSBTNIMG_SEULE:
                        local.text.clear();
                        QProxyStyle::drawControl(element, &local, p, widget);
                        return;
                    case OWL_POSBTNIMG_TEXTE_SEUL:
                        local.icon = QIcon();
                        QProxyStyle::drawControl(element, &local, p, widget);
                        return;
                    case OWL_POSBTNIMG_HAUT:
                    case OWL_POSBTNIMG_BAS:
                        drawStackedLabel(local, p, pos == OWL_POSBTNIMG_BAS);
                        return;
                    default: /* OWL_POSBTNIMG_GAUCHE : natif */
                        break;
                }
            }
        }
        QProxyStyle::drawControl(element, opt, p, widget);
    }

private:
    /* Empilement vertical icone + texte (icone au-dessus par defaut, en-dessous
     * si iconBelow). Rendu manuel : Qt n'expose pas d'option verticale native
     * sur QPushButton. */
    void drawStackedLabel(const QStyleOptionButton& opt, QPainter* p,
                          bool iconBelow) const {
        QRect r = opt.rect;
        QSize isz = opt.iconSize.isValid() ? opt.iconSize : QSize(16, 16);
        bool hasIcon = !opt.icon.isNull();
        bool hasText = !opt.text.isEmpty();
        QFontMetrics fm(opt.fontMetrics);
        int textH = hasText ? fm.height() : 0;
        int gap = (hasIcon && hasText) ? 4 : 0;
        int totalH = (hasIcon ? isz.height() : 0) + gap + textH;
        int y = r.top() + (r.height() - totalH) / 2;

        int iconY = iconBelow ? (y + textH + gap) : y;
        int textY = iconBelow ? y : (y + (hasIcon ? isz.height() + gap : 0));

        if (hasIcon) {
            QPixmap pm = opt.icon.pixmap(isz,
                (opt.state & State_Enabled) ? QIcon::Normal : QIcon::Disabled);
            int ix = r.left() + (r.width() - isz.width()) / 2;
            p->drawPixmap(ix, iconY, pm);
        }
        if (hasText) {
            QRect tr(r.left(), textY, r.width(), textH);
            p->setPen(opt.palette.buttonText().color());
            p->drawText(tr, Qt::AlignHCenter | Qt::AlignVCenter, opt.text);
        }
    }
};

/*  /  — style applicatif : chaine de proxies enrobant Fusion,
 * posee une seule fois via QApplication::setStyle (qui en prend la propriete).
 * OwlButtonProxyStyle (placement image bouton) enrobe OwlMenuProxyStyle (coche
 * de menu) qui enrobe Fusion. Tous les widgets heritent de ce style par defaut
 * (plus aucun setStyle par-widget nulle part), donc coche menu ET placement
 * image bouton restent actifs partout SANS le montage non supporte
 * style-par-widget + feuille QSS qui provoquait l'UAF au show. */
static QStyle* owl_app_style() {
    return new OwlButtonProxyStyle(
               new OwlMenuProxyStyle(QStyleFactory::create("Fusion")));
}

/* Filtre « Inactif » pour widgets sans lecture-seule native (Bouton/Libellé…).
 * L'etat Inactif veut un rendu de COULEUR NORMALE (donc widget garde
 * enabled=true, contrairement a Grisé qui passe disabled et grise), mais
 * NON-interactif : ce filtre avale les evenements souris/clavier/molette pour
 * neutraliser toute interaction sans toucher a la palette. Stateless -> une
 * instance partagee, posee/retiree par owl_ui_control_set_etat. */
class OwlInactiveEventFilter : public QObject {
public:
    bool eventFilter(QObject* /*obj*/, QEvent* ev) override {
        switch (ev->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::ContextMenu:
                return true; /* avale -> non-interactif */
            default:
                return false;
        }
    }
};

static OwlInactiveEventFilter* owl_inactive_filter() {
    static OwlInactiveEventFilter* f = [] {
        auto* p = new OwlInactiveEventFilter();
        owl_qobj_watch(p, "OwlInactiveEventFilter(singleton)");
        return p;
    }();
    return f;
}

/* Filtre « Inactif » pour les options de menu. Cote menu stylisé, le rendu de
 * l'item appartient a QStyleSheetStyle (setStyleSheet posé sur la barre) : on ne
 * peut pas repeindre le texte d'un item disabled en couleur normale via le proxy.
 * L'option Inactif reste donc enabled (couleur normale garantie) et ce filtre,
 * posé sur chaque QMenu, avale le clic ET la validation clavier quand l'action
 * survolee/active porte le marqueur `owl_inactif` -> non-cliquable sans grisé.
 * Limite connue : un item enabled se surligne quand meme au survol (le highlight
 * n'est pas neutralisable sans reprendre entierement CE_MenuItem). */
class OwlMenuInactiveFilter : public QObject {
public:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        QMenu* menu = qobject_cast<QMenu*>(obj);
        if (!menu) return false;
        switch (ev->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick: {
                QMouseEvent* me = static_cast<QMouseEvent*>(ev);
                QAction* a = menu->actionAt(me->pos());
                return a && a->property("owl_inactif").toBool();
            }
            case QEvent::KeyPress: {
                QKeyEvent* ke = static_cast<QKeyEvent*>(ev);
                int k = ke->key();
                if (k != Qt::Key_Return && k != Qt::Key_Enter
                    && k != Qt::Key_Space)
                    return false;
                QAction* a = menu->activeAction();
                return a && a->property("owl_inactif").toBool();
            }
            default:
                return false;
        }
    }
};

static OwlMenuInactiveFilter* owl_menu_inactive_filter() {
    static OwlMenuInactiveFilter* f = [] {
        auto* p = new OwlMenuInactiveFilter();
        owl_qobj_watch(p, "OwlMenuInactiveFilter(singleton)");
        return p;
    }();
    return f;
}

static void owl_menu_apply_default_look(QWidget* target) {
    /*  — plus de target->setStyle(...) ici : le proxy (coche custom) est
     * desormais un maillon du style applicatif (owl_app_style), herite par tous
     * les menus. Poser un style par-widget en plus d'une feuille QSS est un
     * montage non supporte par Qt qui crashait au show natif (UAF). */
    if (qobject_cast<QMenu*>(target))
        target->installEventFilter(owl_menu_inactive_filter());
    QPalette pal = target->palette();
    pal.setColor(QPalette::Window,          owl_menu_color(K_MENU_POPUP_BG));
    pal.setColor(QPalette::Base,            owl_menu_color(K_MENU_POPUP_BG));
    pal.setColor(QPalette::Button,          owl_menu_color(K_MENU_BAR_BG));
    pal.setColor(QPalette::WindowText,      owl_menu_color(K_MENU_TEXT));
    pal.setColor(QPalette::Text,            owl_menu_color(K_MENU_TEXT));
    pal.setColor(QPalette::ButtonText,      owl_menu_color(K_MENU_TEXT));
    pal.setColor(QPalette::Highlight,       owl_menu_color(K_MENU_ACCENT));
    pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, owl_menu_color(K_MENU_TEXT_OFF));
    pal.setColor(QPalette::Disabled, QPalette::Text,       owl_menu_color(K_MENU_TEXT_OFF));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, owl_menu_color(K_MENU_TEXT_OFF));
    target->setPalette(pal);
}

extern "C" void owl_ui_window_menubar_reset(OwlWindow* w,
                                            const OwlMenuStyleResolved* style) {
    owl_menu_trace("owl_ui_window_menubar_reset ENTER w=%p style=%p", (void*)w,
                   (const void*)style);
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QWidget* central0 = mw->centralWidget();
    owl_menu_trace("  menubar_reset central=%p", (void*)central0);
    QMenuBar* bar = new QMenuBar(mw);
    bar->setObjectName(QStringLiteral("owlMenuBar"));
    owl_qobj_watch(bar, "QMenuBar");
    owl_menu_apply_default_look(bar);
    /*  — style==NULL ou has_style==0 : rendu par defaut byte-identique. */
    bar->setStyleSheet(owl_menu_stylesheet(
        (style && style->has_style) ? style : nullptr));
    /*  — memorise l'arrondi resolu du panneau sur la fenetre : les QMenu
     * sont crees plus tard (owl_ui_menu_add_submenu) sans acces au style. Arrondi>0
     * => coins transparents requis (WA_TranslucentBackground), sinon un QMenu reste
     * une fenetre popup opaque rectangulaire et son fond deborde carre derriere le
     * lisere arrondi (piege Qt : la QSS border-radius ne clippe pas la fenetre). */
    {
        int arrondi = (style && style->has_style && style->sousmenu_arrondi > 0)
                          ? style->sousmenu_arrondi : 0;
        mw->setProperty("owl_menu_arrondi", arrondi);
    }
    /*  — police de la barre posée par setFont (QSS non honoré sur
     * QMenuBar::item), police des options mémorisée pour les déroulants créés
     * plus tard (owl_ui_menu_add_submenu). */
    if (style && style->has_style) {
        owl_menu_apply_font(bar, &style->barre_base);
        owl_menu_store_option_font(mw, &style->option_base);
    } else {
        owl_menu_store_option_font(mw, nullptr);
    }
    /* Qt prend la propriete de la nouvelle barre ET detruit l'ancienne avec
     * tout son arbre de QMenu/QAction : rien a liberer a la main cote OWL. */
    owl_menu_trace("  menubar_reset BEFORE setMenuBar bar=%p", (void*)bar);
    mw->setMenuBar(bar);
    owl_menu_trace("owl_ui_window_menubar_reset EXIT");
}

extern "C" void owl_ui_window_menubar_clear(OwlWindow* w) {
    if (!w) return;
    reinterpret_cast<QMainWindow*>(w)->setMenuBar(nullptr);
}

/*  — feuille QSS courante de la barre de menu (test-only, cf.
 * owl_ui_control_get_stylesheet). Reutilise le meme buffer statique. */
extern "C" const char* owl_ui_window_menubar_get_stylesheet(OwlWindow* w) {
    if (!w) return "";
    QMenuBar* bar = reinterpret_cast<QMainWindow*>(w)->menuBar();
    if (!bar) return "";
    g_ctrl_text_buf = bar->styleSheet().toUtf8();
    return g_ctrl_text_buf.constData();
}

/*  — descripteur "famille|taille|gras|italique" de la police EFFECTIVE
 * d'un widget de menu (test-only). Vérifie que setFont a bien posé la police
 * sans capture-pixel. */
static const char* owl_menu_font_desc(QWidget* wdg) {
    if (!wdg) { g_ctrl_text_buf = QByteArray(); return ""; }
    const QFont f = wdg->font();
    QString d = f.family() + "|" + QString::number(f.pointSize()) + "|"
              + (f.bold() ? "1" : "0") + "|" + (f.italic() ? "1" : "0");
    g_ctrl_text_buf = d.toUtf8();
    return g_ctrl_text_buf.constData();
}

/*  — police effective de la barre de menu (test-only). "" si aucune
 * barre montée. */
extern "C" const char* owl_ui_window_menubar_get_font_desc(OwlWindow* w) {
    if (!w) return "";
    QMenuBar* bar = reinterpret_cast<QMainWindow*>(w)->menuBar();
    return bar ? owl_menu_font_desc(bar) : "";
}

/*  — police effective du déroulant désigné par le libellé de son
 * sous-menu (test-only). "" si barre/déroulant introuvable. */
extern "C" const char* owl_ui_window_submenu_get_font_desc(OwlWindow* w,
                                                           const char* submenu_name) {
    if (!w || !submenu_name) return "";
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    if (!bar) return "";
    QString target = QString::fromUtf8(submenu_name);
    target.remove(QLatin1Char('&'));
    QMenu* found = nullptr;
    const QList<QAction*> tops = bar->actions();
    for (QAction* act : tops) {
        QMenu* top = act->menu();
        if (!top) continue;
        QString title = top->title();
        title.remove(QLatin1Char('&'));
        if (title.compare(target, Qt::CaseInsensitive) == 0) { found = top; break; }
        found = owl_menu_find_submenu(top, target);
        if (found) break;
    }
    return found ? owl_menu_font_desc(found) : "";
}

/*  — hauteur d'ENCRE (glyphes reellement peints) du texte de la barre de
 * menu, en pixels (test-only). Contrairement a owl_ui_window_menubar_get_font_desc
 * qui lit le QFont pose (descripteur), on GRAB la barre off-screen (render(),
 * marche sous QT_QPA_PLATFORM=offscreen) et on mesure l'extension verticale des
 * pixels differant du fond. Cible la classe de regression que le descripteur
 * rate : si un fragment QSS (QMenuBar::item font) neutralisait le setFont, le
 * QFont dirait toujours "15pt" mais l'encre peinte retomberait au defaut. Une
 * police plus grande peint une encre plus haute -> mesure monotone avec la taille
 * effectivement rendue. Retourne -1 si pas de barre, 0 si aucune encre. */
extern "C" int owl_ui_window_menubar_ink_height(OwlWindow* w) {
    if (!w) return -1;
    QMenuBar* bar = reinterpret_cast<QMainWindow*>(w)->menuBar();
    if (!bar) return -1;
    if (g_app) QApplication::processEvents();
    QPixmap pix = bar->grab();
    if (g_app) QApplication::processEvents();
    if (pix.isNull()) return -1;
    const QImage img = pix.toImage();
    if (img.width() <= 0 || img.height() <= 0) return -1;
    /* Fond = pixel du coin haut-gauche (padding barre 0px, mais l'item a un
     * padding 4px : le coin est du fond de barre, jamais un glyphe). Seuil somme
     * RVB > 60 : ignore l'antialiasing proche-fond, capte le coeur des glyphes. */
    const QRgb bg = img.pixel(0, 0);
    int top = -1, bottom = -1;
    for (int y = 0; y < img.height(); ++y) {
        bool rowHasInk = false;
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            const int diff = qAbs(qRed(p)   - qRed(bg))
                           + qAbs(qGreen(p) - qGreen(bg))
                           + qAbs(qBlue(p)  - qBlue(bg));
            if (diff > 60) { rowHasInk = true; break; }
        }
        if (rowHasInk) { if (top < 0) top = y; bottom = y; }
    }
    return top < 0 ? 0 : bottom - top + 1;
}

/*  — QMenu translucide pour panneau arrondi. WA_TranslucentBackground
 * DOIT etre pose dans le CONSTRUCTEUR, avant tout addAction/setStyleSheet/popup :
 * des que la fenetre native du popup est creee (premier polish/show) elle l'est
 * en RGB opaque sans canal alpha, et l'attribut pose apres coup n'a plus d'effet
 * -> le fond deborde carre opaque dans les coins hors border-radius (bug de timing
 * visible seulement sur le popup reel, pas sur une capture offscreen). Frameless
 * + NoDropShadow coupent l'ombre native rectangulaire qui gacherait l'arrondi. */
class OwlRoundedMenu : public QMenu {
public:
    explicit OwlRoundedMenu(QWidget* parent = nullptr) : QMenu(parent) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setWindowFlags(windowFlags()
                       | Qt::FramelessWindowHint
                       | Qt::NoDropShadowWindowHint);
    }
};

extern "C" OwlMenu* owl_ui_menu_add_submenu(OwlWindow* w, OwlMenu* parent,
                                            const char* label_utf8,
                                            int enabled, int visible,
                                            int inactif) {
    if (!w) return nullptr;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    /*  — parent non NULL (sous-menu, ou racine d'un menu contextuel) :
     * on n'a pas besoin de la barre. Seul le top-level du menu principal
     * (parent==NULL) exige la barre. */
    if (!parent && !bar) return nullptr;
    const QString label = QString::fromUtf8(label_utf8 ? label_utf8 : "");
    /*  — panneau arrondi : sous-classe OwlRoundedMenu qui pose la
     * translucence dans son ctor AVANT addMenu (seul moment ou l'attribut prend
     * effet, cf. commentaire de la classe). Les surcharges addMenu(QMenu*)
     * adoptent un menu deja construit. Panneau carre (defaut) : addMenu(label)
     * plain -> fenetre popup opaque, ombre native conservee, rendu inchange. */
    QMenu* menu;
    if (mw->property("owl_menu_arrondi").toInt() > 0) {
        /*  — parente a la BARRE (sous-menu top-level) ou au MENU PARENT
         * (sous-menu imbrique), jamais a `mw` : la feuille de style est posee sur
         * la barre (owl_ui_window_menubar_reset) et ne cascade qu'a ses enfants
         * QObject. Un OwlRoundedMenu enfant de `mw` echappe a cette cascade et
         * retombe carre/non style (comme le cas carre `bar->addMenu(label)`, qui
         * marche justement parce qu'il parente a la barre). La translucence reste
         * posee dans le ctor : la parente QObject n'affecte que la cascade QSS,
         * pas la nature top-level/native de la fenetre popup. */
        QWidget* qparent = parent
            ? static_cast<QWidget*>(reinterpret_cast<QMenu*>(parent))
            : static_cast<QWidget*>(bar);
        OwlRoundedMenu* m = new OwlRoundedMenu(qparent);
        m->setTitle(label);
        parent ? reinterpret_cast<QMenu*>(parent)->addMenu(m) : bar->addMenu(m);
        menu = m;
        owl_qobj_watch(m, "OwlRoundedMenu");
    } else {
        menu = parent ? reinterpret_cast<QMenu*>(parent)->addMenu(label)
                      : bar->addMenu(label);
        owl_qobj_watch(menu, "QMenu(submenu)");
    }
    if (!menu) return nullptr;
    /* menuAction : QAction cree par addMenu, porte par la barre/menu parent —
     * suspect potentiel du UAF (l'action survit-elle au menu ?). */
    owl_qobj_watch(menu->menuAction(), "QAction(menuAction)");
    /* Le deroulant est un popup top-level : la feuille de la barre ne
     * l'atteint que par la chaine de parente QObject. addMenu la garantit,
     * mais on repose la palette pour les primitives dessinees par le style. */
    owl_menu_apply_default_look(menu);
    /*  — police des options (surface OptionMenu) posée par setFont sur
     * chaque déroulant : QSS non honoré sur QMenu::item, et la police de la barre
     * ne doit pas cascader ici. Mémorisée sur mw à la construction de la barre /
     * du menu contextuel. */
    owl_menu_apply_stored_option_font(mw, menu);
    /* Inactif : entree de couleur normale (enabled) mais non-interactive. Le
     * filtre du menu parent avale clic/validation sur l'action porteuse du
     * marqueur ; le deroulant reste ouvrable au survol (limite connue). */
    menu->menuAction()->setEnabled(inactif ? true : (enabled != 0));
    menu->menuAction()->setVisible(visible != 0);
    menu->menuAction()->setProperty("owl_inactif", inactif ? true : QVariant());
    return reinterpret_cast<OwlMenu*>(menu);
}

extern "C" void owl_ui_menu_add_separator(OwlWindow* w, OwlMenu* parent,
                                          int visible) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    if (!parent && !bar) return;
    QAction* sep = parent ? reinterpret_cast<QMenu*>(parent)->addSeparator()
                          : bar->addSeparator();
    if (sep) sep->setVisible(visible != 0);
}

extern "C" OwlMenuItem* owl_ui_menu_add_option(OwlWindow* w, OwlMenu* parent,
                                               const char* label_utf8,
                                               const char* shortcut_utf8,
                                               int enabled, int checkable,
                                               int checked, int visible,
                                               int inactif,
                                               OwlCallback cb, void* userdata) {
    if (!w) return nullptr;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QMenuBar* bar = mw->menuBar();
    if (!parent && !bar) return nullptr;
    const QString label = QString::fromUtf8(label_utf8 ? label_utf8 : "");
    QAction* act = parent ? reinterpret_cast<QMenu*>(parent)->addAction(label)
                          : bar->addAction(label);
    if (!act) return nullptr;
    owl_qobj_watch(act, "QAction(option)");
    if (shortcut_utf8 && *shortcut_utf8) {
        act->setShortcut(QKeySequence(QString::fromUtf8(shortcut_utf8)));
    }
    if (checkable) {
        act->setCheckable(true);
        act->setChecked(checked != 0);
    }
    /* Inactif : option de couleur normale (enabled) mais non-cliquable. On garde
     * enabled=true (le rendu QSS grise les items disabled) et on marque l'action
     * pour que le filtre du menu avale clic/validation. Grisé passe par enabled=0. */
    act->setEnabled(inactif ? true : (enabled != 0));
    act->setVisible(visible != 0);
    if (inactif) act->setProperty("owl_inactif", true);
    if (cb) {
        /* Capture par valeur : rien a liberer quand Qt detruit l'action lors
         * d'une bascule de menu. `triggered` couvre le clic ET la validation
         * clavier (mnemonique, accelerateur, Entree sur l'item survole). */
        OwlCallback lcb = cb;
        void*       lud = userdata;
        QObject::connect(act, &QAction::triggered, act, [lcb, lud]() {
            lcb(nullptr, lud);
        });
    }
    return reinterpret_cast<OwlMenuItem*>(act);
}

extern "C" void owl_ui_menu_item_set_icon_path(OwlMenuItem* item,
                                               const char* path_utf8) {
    if (!item) return;
    QAction* act = reinterpret_cast<QAction*>(item);
    if (!path_utf8 || !*path_utf8) { act->setIcon(QIcon()); return; }
    act->setIcon(QIcon(QString::fromUtf8(path_utf8)));
}

extern "C" void owl_ui_menu_item_set_icon_rgba(OwlMenuItem* item,
                                               const unsigned char* rgba,
                                               int w, int h) {
    if (!item) return;
    QAction* act = reinterpret_cast<QAction*>(item);
    QImage img = rgba_to_qimage(rgba, w, h);
    if (img.isNull()) { act->setIcon(QIcon()); return; }
    act->setIcon(QIcon(QPixmap::fromImage(img)));
}

/* ------- Menu contextuel ------- */

static QWidget* central_of(OwlWindow* w);  /* defini plus bas */

/* Handler d'une demande de menu contextuel : porte le cb OWL + son userdata +
 * le widget source (sender). Alloue une fois a l'installation, capture par le
 * lambda de connexion ; jamais libere (meme regime que OwlTableHandler). */
struct OwlCtxMenuHandler { OwlCallback cb; void* userdata; OwlControl* sender; };

extern "C" void owl_ui_install_context_menu(OwlControl* c, OwlCallback cb,
                                            void* userdata) {
    if (!c) return;
    QWidget* wdg = reinterpret_cast<QWidget*>(c);
    wdg->setContextMenuPolicy(Qt::CustomContextMenu);
    OwlCtxMenuHandler* h = new OwlCtxMenuHandler{cb, userdata, c};
    owl_qobj_trace("CREATE type=OwlCtxMenuHandler(control) addr=%p host=%p phase=%s",
                   (void*)h, (void*)wdg, g_owl_qobj_phase);
    owl_track_conn(wdg,
        QObject::connect(wdg, &QWidget::customContextMenuRequested, wdg,
                         [h](const QPoint&) {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

extern "C" void owl_ui_install_window_context_menu(OwlWindow* w, OwlCallback cb,
                                                   void* userdata) {
    owl_menu_trace("owl_ui_install_window_context_menu ENTER w=%p", (void*)w);
    if (!w) return;
    QWidget* central = central_of(w);
    owl_menu_trace("  install_window_ctx central_of=%p", (void*)central);
    if (!central) central = reinterpret_cast<QWidget*>(w);
    OwlCtxMenuHandler* h = new OwlCtxMenuHandler{cb, userdata,
                                                 reinterpret_cast<OwlControl*>(central)};
    owl_qobj_trace("CREATE type=OwlCtxMenuHandler(window) addr=%p host=%p phase=%s",
                   (void*)h, (void*)central, g_owl_qobj_phase);
    central->setContextMenuPolicy(Qt::CustomContextMenu);
    owl_track_conn(central,
        QObject::connect(central, &QWidget::customContextMenuRequested, central,
                         [h](const QPoint&) {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
    owl_menu_trace("owl_ui_install_window_context_menu EXIT");
}

extern "C" OwlMenu* owl_ui_context_menu_create(OwlWindow* w,
                                               const OwlMenuStyleResolved* style) {
    if (!w) return nullptr;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    int arrondi = (style && style->has_style && style->sousmenu_arrondi > 0)
                      ? style->sousmenu_arrondi : 0;
    /* La propriete est relue par owl_ui_menu_add_submenu pour choisir le panneau
     * arrondi ; posee ici, elle vaut le temps de la construction du contextuel.
     * La barre principale (si presente) est deja construite -> pas d'interference. */
    mw->setProperty("owl_menu_arrondi", arrondi);
    QMenu* root = (arrondi > 0) ? static_cast<QMenu*>(new OwlRoundedMenu(mw))
                                : new QMenu(mw);
    root->setObjectName(QStringLiteral("owlContextMenu"));
    owl_qobj_watch(root, arrondi > 0 ? "OwlRoundedMenu(ctx-root)"
                                     : "QMenu(ctx-root)");
    owl_menu_apply_default_look(root);
    root->setStyleSheet(owl_menu_stylesheet(
        (style && style->has_style) ? style : nullptr));
    /*  — police des options sur le panneau racine (setFont, QSS non
     * honoré) + mémorisation pour les sous-menus créés ensuite via
     * owl_ui_menu_add_submenu. */
    if (style && style->has_style) {
        owl_menu_apply_font(root, &style->option_base);
        owl_menu_store_option_font(mw, &style->option_base);
    } else {
        owl_menu_store_option_font(mw, nullptr);
    }
    return reinterpret_cast<OwlMenu*>(root);
}

/*  — cf. owl_ui.h : la feuille qu'owl_ui_context_menu_create poserait,
 * calculee sans widget. Rendue via un buffer statique (meme regime que
 * owl_ui_window_menubar_get_stylesheet). */
extern "C" const char* owl_ui_menu_stylesheet_preview(
        const OwlMenuStyleResolved* style) {
    static QByteArray buf;
    buf = owl_menu_stylesheet((style && style->has_style) ? style : nullptr)
              .toUtf8();
    return buf.constData();
}

extern "C" void owl_ui_context_menu_popup(OwlMenu* root) {
    if (!root) return;
    QMenu* m = reinterpret_cast<QMenu*>(root);
    /* Bloquant : le dispatch des surSelection (triggered) a lieu dans cette
     * boucle modale imbriquee, comme pour un item de barre. */
    m->exec(QCursor::pos());
    m->deleteLater();
}

/*  — raccourci PERSISTANT d'une OptionMenu de menu contextuel.
 *
 * Le popup (owl_ui_context_menu_popup) recree l'arbre a chaque clic droit puis
 * le detruit : le setShortcut pose sur ses QAction n'est donc actif que pendant
 * l'affichage du menu. Menu ferme, fenetre focus -> le raccourci ne firait rien.
 * On installe ici, a l'attache (une fois), une QAction porteuse du SEUL
 * raccourci sur le widget cible (jamais ajoutee a un menu visuel, addAction ne
 * fait que l'enregistrer pour la resolution des accelerateurs) :
 *   - menu attache a un CHAMP (widget != NULL) : Qt::WidgetWithChildrenShortcut
 *     -> actif quand ce champ (ou un enfant) a le focus. Un menu partage entre
 *     plusieurs champs recoit une QAction par champ (un appel par attache).
 *   - menu attache au FOND (widget == NULL) : Qt::WindowShortcut sur le widget
 *     central -> actif tant que la fenetre est active.
 * `triggered` route vers le meme dispatch que le clic (cb + userdata), donc la
 * surSelection s'execute par le chemin habituel. */
extern "C" void owl_ui_install_context_shortcut(OwlControl* widget,
                                                OwlWindow* window,
                                                const char* shortcut_utf8,
                                                OwlCallback cb, void* userdata) {
    if (!shortcut_utf8 || !*shortcut_utf8 || !cb) return;
    QWidget* target = nullptr;
    Qt::ShortcutContext ctx;
    if (widget) {
        target = reinterpret_cast<QWidget*>(widget);
        ctx = Qt::WidgetWithChildrenShortcut;
    } else {
        if (!window) return;
        target = central_of(window);
        if (!target) target = reinterpret_cast<QWidget*>(window);
        ctx = Qt::WindowShortcut;
    }
    if (!target) return;
    QAction* act = new QAction(target);
    act->setShortcut(QKeySequence(QString::fromUtf8(shortcut_utf8)));
    act->setShortcutContext(ctx);
    OwlCallback lcb = cb;
    void*       lud = userdata;
    QObject::connect(act, &QAction::triggered, act, [lcb, lud]() {
        lcb(nullptr, lud);
    });
    target->addAction(act);
}

/* Lot F : accepte soit un QMainWindow* (cas standard fenetre) soit un QWidget*
 * generique (cas Conteneur deguise en OwlWindow* pour rester compatible avec
 * l'API existante des _create). qobject_cast renvoie NULL si pas QMainWindow,
 * dans ce cas on utilise le widget tel quel. */
static QWidget* central_of(OwlWindow* w) {
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    QMainWindow* mw = qobject_cast<QMainWindow*>(qw);
    return mw ? mw->centralWidget() : qw;
}

/* ------- Lot F : Conteneur + Layouts ------- */

extern "C" OwlControl* owl_ui_container_create(OwlWindow* parent,
                                                int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QWidget* box = new QWidget(central_of(parent));
    /*  — marque le QWidget comme un Conteneur OWL. Un conteneur nu a une
     * sizePolicy Preferred et un sizeHint dependant de son layout interne : dans
     * un box layout parent il retombe sur ce hint (etroit) au lieu de prendre la
     * place disponible comme les autres champs. Le predicat owl_widget_is_container
     * permet a owl_ui_layout_add_child de le traiter comme un widget etirable sur
     * l'axe transverse par defaut (parite avec le WYSIWYG du designer). */
    box->setProperty("owl_is_container", true);
    if (w > 0 && h > 0) box->setGeometry(x, y, w, h);
    box->show();
    return reinterpret_cast<OwlControl*>(box);
}

extern "C" OwlWindow* owl_ui_container_as_parent(OwlControl* container) {
    /* Cast a travers — central_of() detecte que ce n'est pas un QMainWindow
     * et retourne le QWidget tel quel. */
    return reinterpret_cast<OwlWindow*>(container);
}

static const char* cadre_line_style_to_css(int line_style);   /* defini plus bas */

/*  — applique le styleConteneur (sous-element CadreExtérieur) sur le
 * QWidget conteneur. Le Conteneur est un QWidget nu qui HEBERGE des enfants :
 * un setStyleSheet non scope cascaderait fond/bordure sur tous les enfants. On
 * scope donc par `#objectName { ... }` (patron owl_ui_zr_set_style / wrapper
 * Saisie). Un QWidget nu ne peint NI fond NI bordure depuis une stylesheet
 * (meme scopee #id) tant que Qt::WA_StyledBackground n'est pas actif — a la
 * difference d'un QFrame (ZoneRepetee) qui peint nativement. On active donc
 * l'attribut des qu'un style non vide est pose. L'attribut n'affecte que le
 * widget qui le porte, pas ses enfants, donc pas de cascade.
 *
 * bg  : CouleurFond BGR canon (-1 = pas de fond).
 * border : Cadre (NULL/thickness<=0 = pas de bordure).
 * padL/T/R/B : Retraits en px (<0 = unset). Le padding QSS reserve la boite
 * interieure ; en presence d'un layout on repercute aussi les Retraits +
 * l'epaisseur du cadre en contentsMargins pour que les enfants s'inscrivent
 * dans le contour (sinon un enfant Expanding recouvrirait cadre + retraits). */
/*  — construit build_gradient_qss (défini plus bas) pour le fond de la
 * Fenêtre / du conteneur : un dégradé actif remplace le fond uni. */
static QString build_gradient_qss(int type, int angle,
                                  int startColor, int startAlpha,
                                  int endColor, int endAlpha);

/*  — corps QSS de bordure partagé par les émetteurs Style (Conteneur,
 * Saisie, Table/ZoneRépétée). Défini plus bas (près de cadre_css_line) ;
 * forward-déclaré ici car les trois émetteurs le précèdent. `colorFn` formate
 * la couleur selon la surface pour préserver la byte-identité de chacune. */
static QString cadre_border_body(const OwlCadreSpec* s,
                                 QString (*colorFn)(int, int),
                                 bool* perSideOut);

/* Impl commune : peint un QWidget conteneur (fond + bordure + retraits) via un
 * QSS scopé `#objectName`. gradientQss non vide -> `background: <gradient>`
 * remplace `background-color` (, symétrie avec le fond des widgets). */
static void container_set_style_impl(QWidget* q, int bg, int bgAlpha,
                                     const OwlCadreSpec* border,
                                     int padL, int padT, int padR, int padB,
                                     const QString& gradientQss) {
    if (!q) return;
    if (q->objectName().isEmpty())
        q->setObjectName(QString("owlContainer_%1").arg((quintptr)q));

    QString body;
    if (!gradientQss.isEmpty())
        body += "background: " + gradientQss + "; ";
    else if (bg >= 0)
        body += "background-color: " + owl_bg_css(bg, bgAlpha) + "; ";
    /* Marges de layout par côté : en global tous égaux à l'épaisseur (rendu
     * inchangé) ; en per-side, l'épaisseur effective de chaque bord. */
    int bTop = 0, bRight = 0, bBottom = 0, bLeft = 0;
    if (border) {
        bool perSide = false;
        /*  — border-color alpha-aware (owl_bg_css : rgb(...) byte-identique
         * si opaque, rgba(...) sinon). Une bordure translucide se mélange au fond.
         *  — délègue au corps partagé : per-side si un côté porte un trait
         * propre, sinon shorthand global byte-identique. */
        QString bb = cadre_border_body(border, owl_bg_css, &perSide);
        if (!bb.isEmpty()) {
            body += bb;
            if (!perSide) {
                bTop = bRight = bBottom = bLeft = border->thickness;
                /*  — bordure translucide + fond opaque : par défaut Qt (comme
                 * CSS background-clip: border-box) peint le fond SOUS le liseré, si
                 * bien qu'une bordure alpha se mélange au fond du conteneur, pas à ce
                 * qu'il y a derrière. On clippe le fond à la boîte intérieure
                 * (padding-box) pour que le liseré translucide révèle l'arrière-plan
                 * (fenêtre). Émis UNIQUEMENT si fond ET bordure translucide : bordure
                 * opaque -> aucune émission (rendu byte-identique). */
                if (bg >= 0 && border->color_alpha < 255)
                    body += "background-clip: padding; ";
            } else {
                /*  — épaisseur/opacité effectives par côté (trait propre
                 * sinon scalaire global). */
                auto effThick = [&](int c, int t) {
                    if (t != 0 || c != 0) return t > 0 ? t : 0;
                    return border->thickness > 0 ? border->thickness : 0;
                };
                auto effAlpha = [&](int c, int t, int a) {
                    if (t != 0 || c != 0) return a;
                    return border->color_alpha;
                };
                bTop    = effThick(border->trait_haut_color,   border->trait_haut_thick);
                bRight  = effThick(border->trait_droite_color, border->trait_droite_thick);
                bBottom = effThick(border->trait_bas_color,    border->trait_bas_thick);
                bLeft   = effThick(border->trait_gauche_color, border->trait_gauche_thick);
                bool anyTrans =
                    effAlpha(border->trait_haut_color,   border->trait_haut_thick,   border->trait_haut_alpha)   < 255 ||
                    effAlpha(border->trait_droite_color, border->trait_droite_thick, border->trait_droite_alpha) < 255 ||
                    effAlpha(border->trait_bas_color,    border->trait_bas_thick,    border->trait_bas_alpha)    < 255 ||
                    effAlpha(border->trait_gauche_color, border->trait_gauche_thick, border->trait_gauche_alpha) < 255;
                if (bg >= 0 && anyTrans)
                    body += "background-clip: padding; ";
            }
        }
    }
    /*  — l'arrondi est indépendant de l'épaisseur : un fond arrondi
     * sans liseré (Epaisseur=0, Arrondi>0) est un motif design courant. Émis
     * hors du gate thickness (aligné sur owl_ui_control_set_frame_state). */
    if (border && border->corner_radius > 0)
        body += QString("border-radius: %1px; ").arg(border->corner_radius);

    int pl = padL > 0 ? padL : 0;
    int pt = padT > 0 ? padT : 0;
    int pr = padR > 0 ? padR : 0;
    int pb = padB > 0 ? padB : 0;
    if (pl || pt || pr || pb)
        body += QString("padding: %1px %2px %3px %4px; ")
                    .arg(pt).arg(pr).arg(pb).arg(pl);

    if (body.isEmpty()) {
        q->setStyleSheet(QString());
        q->setAttribute(Qt::WA_StyledBackground, false);
    } else {
        q->setAttribute(Qt::WA_StyledBackground, true);
        q->setStyleSheet("#" + q->objectName() + " { " + body + "}");
    }

    /* Repercute cadre + retraits en marges de layout (le styled content box de
     * QSS n'inset pas les enfants d'un QWidget conteneur). En dispLibre (pas de
     * layout, X/Y absolu) rien a faire : les enfants sont positionnes a la main.
     *
     *  — un cote dont le Style ne dit rien (retrait < 0) retombe sur la
     * marge interieure declaree sur le bloc (Padding*), memorisee par
     * install_layout. Sans ce repli, habiller une Fenetre ou un Conteneur
     * remettait sa marge a zero : la propriete etait acceptee et sans effet.
     *
     * Le seuil est `> 0`, pas `>= 0` : un retrait absent du Style arrive ici a 0
     * et non a -1 (le slot du styleConteneur existe toujours, a son defaut). Un
     * retrait nul est donc indiscernable d'un retrait absent — mesure faite —
     * et ne peut pas servir d'override. */
    if (QLayout* ly = q->layout()) {
        int mL = (padL > 0) ? padL : ly->property("owl_bloc_pad_l").toInt();
        int mT = (padT > 0) ? padT : ly->property("owl_bloc_pad_t").toInt();
        int mR = (padR > 0) ? padR : ly->property("owl_bloc_pad_r").toInt();
        int mB = (padB > 0) ? padB : ly->property("owl_bloc_pad_b").toInt();
        ly->setContentsMargins(mL + bLeft, mT + bTop, mR + bRight, mB + bBottom);
    }
}

extern "C" void owl_ui_container_set_style(OwlControl* container,
                                           int bg, int bgAlpha,
                                           const OwlCadreSpec* border,
                                           int padL, int padT,
                                           int padR, int padB) {
    if (!container) return;
    container_set_style_impl(reinterpret_cast<QWidget*>(container),
                             bg, bgAlpha, border, padL, padT, padR, padB, QString());
}

/* /1027 — habille le panneau central de la Fenêtre via la même
 * mécanique que le Conteneur. On force l'objectName "owlCentral" (partagé avec
 * le fond plat / l'overlay grisé) pour rester cohérent avec les autres feuilles
 * ciblant ce panneau.  : un dégradé actif (gradType linéaire/radial)
 * peint le fond via `background: qlineargradient/qradialgradient` au lieu du
 * fond uni. Bordure + retraits inchangés. Pas d'ombre sur la Fenêtre. */
extern "C" void owl_ui_window_set_panel_style(OwlWindow* w, int bg, int bgAlpha,
                                              const OwlCadreSpec* border,
                                              int padL, int padT,
                                              int padR, int padB,
                                              int gradType, int gradAngle,
                                              int gradStart, int gradStartAlpha,
                                              int gradEnd, int gradEndAlpha) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    QWidget* central = mw->centralWidget();
    if (!central) return;
    if (central->objectName().isEmpty())
        central->setObjectName(QStringLiteral("owlCentral"));
    QString grad = build_gradient_qss(gradType, gradAngle,
                                      gradStart, gradStartAlpha,
                                      gradEnd, gradEndAlpha);
    container_set_style_impl(central, bg, bgAlpha, border,
                             padL, padT, padR, padB, grad);
}

/* Helper interne : applique un layout d'un mode donne sur un QWidget cible.
 * Si un layout existe deja, on le supprime (utile en cas de reconfiguration).
 * disposition : 1=vert, 2=horiz, 3=grille, 4=libre (no layout). */
static QLayout* install_layout(QWidget* target, int disposition,
                                int pl, int pt, int pr, int pb,
                                int spacing) {
    if (!target) return nullptr;
    QLayout* old = target->layout();
    if (old) {
        /* deleteLater() pour eviter le crash si appele depuis un slot. */
        old->deleteLater();
        target->setLayout(nullptr);
    }
    QLayout* lay = nullptr;
    switch (disposition) {
        case OWL_DISPOSITION_VERTICALE:    lay = new QVBoxLayout(target); break;
        case OWL_DISPOSITION_HORIZONTALE:  lay = new QHBoxLayout(target); break;
        case OWL_DISPOSITION_GRILLE:       lay = new QGridLayout(target); break;
        case OWL_DISPOSITION_LIBRE:
        default:                           return nullptr; /* X/Y absolu */
    }
    lay->setContentsMargins(pl, pt, pr, pb);
    lay->setSpacing(spacing);
    /*  — la marge interieure du bloc est memorisee sur le layout : une
     * passe de style ulterieure (container_set_style_impl) la relit pour les
     * cotes que le Style ne surcharge pas, au lieu de les remettre a zero. */
    lay->setProperty("owl_bloc_pad_l", pl);
    lay->setProperty("owl_bloc_pad_t", pt);
    lay->setProperty("owl_bloc_pad_r", pr);
    lay->setProperty("owl_bloc_pad_b", pb);
    return lay;
}

/* Memorise les defauts AlignH/AlignV sur le QLayout via setProperty. Lu par
 * owl_ui_layout_add_child quand un enfant n'a pas pose sa propre prop
 * Alignement. 0 = non set. */
static void stash_layout_defaults(QLayout* lay, int def_h, int def_v) {
    if (!lay) return;
    if (def_h) lay->setProperty("owl_def_align_h", def_h);
    if (def_v) lay->setProperty("owl_def_align_v", def_v);
}

/*  — stocke Colonnes/Lignes du dispGrille sur le QGridLayout via
 * setProperty. Lu par owl_ui_layout_add_child pour calculer la position
 * (row,col) en auto-append. Sans ca, tous les enfants tombaient en
 * colonne 0 -> empilement vertical au lieu d'une vraie grille. */
static void stash_grid_dims(QLayout* lay, int cols, int rows) {
    if (!lay) return;
    if (!qobject_cast<QGridLayout*>(lay)) return;
    if (cols > 0) lay->setProperty("owl_grid_cols", cols);
    if (rows > 0) lay->setProperty("owl_grid_rows", rows);
}

extern "C" void owl_ui_window_set_layout(OwlWindow* parent,
                                          int disposition,
                                          int pl, int pt, int pr, int pb,
                                          int spacing,
                                          int cols, int rows,
                                          int def_align_h, int def_align_v) {
    if (!parent) return;
    QLayout* lay = install_layout(central_of(parent), disposition, pl, pt, pr, pb, spacing);
    stash_layout_defaults(lay, def_align_h, def_align_v);
    stash_grid_dims(lay, cols, rows);
}

extern "C" void owl_ui_container_set_layout(OwlControl* container,
                                             int disposition,
                                             int pl, int pt, int pr, int pb,
                                             int spacing,
                                             int cols, int rows,
                                             int def_align_h, int def_align_v) {
    if (!container) return;
    QWidget* qw = reinterpret_cast<QWidget*>(container);
    QLayout* lay = install_layout(qw, disposition, pl, pt, pr, pb, spacing);
    stash_layout_defaults(lay, def_align_h, def_align_v);
    stash_grid_dims(lay, cols, rows);
}

/* Map OWL_ALIGN_* -> Qt::Alignment. 0 = aucun (defaut layout). */
static Qt::Alignment owl_align_to_qt(int a) {
    switch (a) {
        case OWL_ALIGN_DEBUT:  return Qt::AlignLeft  | Qt::AlignTop;
        case OWL_ALIGN_CENTRE: return Qt::AlignCenter;
        case OWL_ALIGN_FIN:    return Qt::AlignRight | Qt::AlignBottom;
        case OWL_ALIGN_ETIRE:  return Qt::Alignment();  /* defaut = stretch */
        default: return Qt::Alignment();
    }
}

/* Mapping H et V independants pour les defauts AlignH/AlignV poses sur le
 * conteneur. La meme valeur OWL_ALIGN_* est interpretee horizontalement OU
 * verticalement selon l'axe. ETIRE = pas de flag (le layout etire par
 * defaut sur l'axe concerne). */
static Qt::Alignment owl_align_h_to_qt(int a) {
    switch (a) {
        case OWL_ALIGN_DEBUT:  return Qt::AlignLeft;
        case OWL_ALIGN_CENTRE: return Qt::AlignHCenter;
        case OWL_ALIGN_FIN:    return Qt::AlignRight;
        default:               return Qt::Alignment();
    }
}
static Qt::Alignment owl_align_v_to_qt(int a) {
    switch (a) {
        case OWL_ALIGN_DEBUT:  return Qt::AlignTop;
        case OWL_ALIGN_CENTRE: return Qt::AlignVCenter;
        case OWL_ALIGN_FIN:    return Qt::AlignBottom;
        default:               return Qt::Alignment();
    }
}

/*  — un Saisie a libelle integre est insere dans le layout via son
 * wrapper composite [QLabel|QLineEdit], pas via le QLineEdit nu. */
static QWidget* owl_widget_layout_rep(QWidget* inner);
/*  — true si `w` est un Conteneur OWL (QWidget nu marque a la creation).
 * Un conteneur doit occuper l'axe transverse du box layout parent par defaut
 * (comme un champ Saisie en Expanding), au lieu de retomber sur son sizeHint. */
static bool owl_widget_is_container(QWidget* w) {
    return w && w->property("owl_is_container").toBool();
}

/*  — true si `w` est un champ a image regenerable (Champ Image /
 * Champ CodeBarres, marque a la creation). Sous Étire, il doit remplir sa
 * cellule comme un conteneur (Expanding + pas d'ancrage transverse) pour que
 * l'image suive la taille de la fenetre. */
static bool owl_widget_is_image_field(QWidget* w) {
    return w && w->property("owl_is_image_field").toBool();
}

/*  — marge externe d'un enfant de layout. Qt n'a pas de marge par item
 * sur les box/grid layouts. L'ecrire sur le contentsMargins du widget la fait
 * entrer DANS le champ : le texte se decale, le champ ne bouge pas, et une
 * taille declaree (min = max) l'avale entierement. Elle ecrase en plus les
 * Retraits, qui visent le meme reglage Qt (owl_ui_control_set_margin_*).
 * On encapsule donc le QWidgetItem : l'item reserve dans le layout la place du
 * widget PLUS sa marge, et ne remet au widget que la part interieure. Le
 * widget garde son contentsMargins pour ses seuls Retraits. */
/* L'item ENCAPSULE un QWidgetItem au lieu d'en heriter : QWidgetItem::
 * setGeometry realigne lui-meme le widget dans le rectangle recu, en
 * interrogeant sizeHint() et maximumSize() — virtuelles. Un heritage qui
 * grossit ces deux mesures compterait donc la marge une seconde fois, apres
 * qu'elle a deja ete retiree du rectangle (mesure : un champ de 43 rendu a 73
 * pour une marge de 30). Par composition, l'item interieur ne raisonne que
 * dans les dimensions du widget, et lui seul aligne. */
class OwlMarginWidgetItem : public QLayoutItem {
public:
    OwlMarginWidgetItem(QWidget* w, const QMargins& m, Qt::Alignment a)
        : QLayoutItem(a), m_item(w), m_m(m) { m_item.setAlignment(a); }

    QSize sizeHint()    const override { return grown(m_item.sizeHint()); }
    QSize minimumSize() const override { return grown(m_item.minimumSize()); }
    QSize maximumSize() const override { return grown(m_item.maximumSize()); }
    Qt::Orientations expandingDirections() const override { return m_item.expandingDirections(); }
    bool isEmpty()      const override { return m_item.isEmpty(); }
    QWidget* widget()   const override { return m_item.widget(); }
    void invalidate()         override { m_item.invalidate(); }
    QSizePolicy::ControlTypes controlTypes() const override { return m_item.controlTypes(); }
    QRect geometry()    const override { return m_item.geometry().marginsAdded(m_m); }
    void setGeometry(const QRect& r) override { m_item.setGeometry(r.marginsRemoved(m_m)); }

    bool hasHeightForWidth() const override { return m_item.hasHeightForWidth(); }
    int heightForWidth(int w) const override {
        return m_item.heightForWidth(inner(w)) + m_m.top() + m_m.bottom();
    }
    int minimumHeightForWidth(int w) const override {
        return m_item.minimumHeightForWidth(inner(w)) + m_m.top() + m_m.bottom();
    }

private:
    int inner(int w) const {
        int v = w - m_m.left() - m_m.right();
        return v > 0 ? v : 0;
    }
    /* Le plafond Qt ne doit pas deborder : une maximumSize illimitee plus une
     * marge repasserait sous le plafond et figerait le widget. */
    QSize grown(const QSize& s) const {
        int w = s.width()  + m_m.left() + m_m.right();
        int h = s.height() + m_m.top()  + m_m.bottom();
        if (w > QWIDGETSIZE_MAX) w = QWIDGETSIZE_MAX;
        if (h > QWIDGETSIZE_MAX) h = QWIDGETSIZE_MAX;
        return QSize(w, h);
    }
    QWidgetItem m_item;
    QMargins    m_m;
};

extern "C" void owl_ui_layout_add_child(OwlWindow* parent_window_or_null,
                                         OwlControl* parent_container_or_null,
                                         OwlControl* child,
                                         int stretch, int align,
                                         int ml, int mt, int mr, int mb,
                                         int col, int row,
                                         int col_span, int row_span,
                                         int decl_w, int decl_h) {
    if (!child) return;
    QWidget* parent = nullptr;
    if (parent_container_or_null) {
        parent = reinterpret_cast<QWidget*>(parent_container_or_null);
    } else if (parent_window_or_null) {
        parent = central_of(parent_window_or_null);
    }
    if (!parent) return;
    /* Si le controle est un Saisie a libelle integre, c'est son wrapper
     * composite qui entre dans le layout, pas le QLineEdit logique. */
    QWidget* rep = owl_widget_layout_rep(reinterpret_cast<QWidget*>(child));
    QWidget* layout_child = rep ? rep : reinterpret_cast<QWidget*>(child);
    QLayout* lay = parent->layout();
    if (!lay) {
        /* Pas de layout = mode libre : on laisse le QWidget en X/Y absolu,
         * mais on doit quand meme reparenter pour etre coherent. */
        QWidget* qchild = layout_child;
        qchild->setParent(parent);
        qchild->show();
        return;
    }
    QWidget* qchild = layout_child;
    /* Reparente avant addWidget (sinon Qt warning). */
    qchild->setParent(parent);

    /* alignÉtire mappe sur une alignment Qt vide : le layout offre bien toute la
     * dimension transverse a l'item, mais un widget dont la sizePolicy est Fixed
     * sur cet axe refuse de grandir, et Qt le centre alors dans la place offerte.
     * Il faut donc poser Expanding sur l'axe transverse.
     *  posait deja cet override, mais reserve au widget mono stylé
     * (Bouton/Libellé porteur d'un CadreExtérieur en QSS directe).  : il
     * vaut pour tout enfant. Un QLineEdit est Expanding en largeur et Fixed en
     * hauteur, si bien que le remplissage transverse marchait en pile verticale
     * (transverse = largeur) et pas en pile horizontale (mesure : hauteur 23,
     * centree, dans une fenetre de 300). L'etirement dependait de l'orientation
     * de la pile, que rien n'annonce a la lecture de la description. */
    QWidget* stretchTarget = nullptr;
    if (align == OWL_ALIGN_ETIRE) {
        stretchTarget = qchild;
    }
    if (stretchTarget) {
        bool vertical = qobject_cast<QVBoxLayout*>(lay) != nullptr;
        bool horizontal = qobject_cast<QHBoxLayout*>(lay) != nullptr;
        /*  — Étire = occuper toute la dimension TRANSVERSE du layout, en
         * respectant la taille figee de l'axe PRINCIPAL. Dans une pile verticale
         * l'axe transverse est horizontal (largeur), dans une pile horizontale
         * il est vertical (hauteur). On etire donc l'axe transverse et on laisse
         * l'axe principal a sa taille declaree. */
        QSizePolicy sp = stretchTarget->sizePolicy();
        if (vertical)        sp.setHorizontalPolicy(QSizePolicy::Expanding);
        else if (horizontal) sp.setVerticalPolicy(QSizePolicy::Expanding);
        stretchTarget->setSizePolicy(sp);
    }

    /*  — un Conteneur (QWidget nu marque owl_is_container) doit se
     * comporter comme un champ plein : il remplit l'axe TRANSVERSE du box layout
     * parent par defaut (largeur en pile verticale, hauteur en pile horizontale),
     * comme le montre le designer, au lieu de retomber sur son sizeHint etroit
     * (sizePolicy Preferred + AlignLeft transverse). On ne force le remplissage
     * transverse QUE si aucune taille transverse explicite n'est declaree : une
     * Largeur/Hauteur ecrite fige la dimension (bornage  ci-dessous).
     * Avec Étire=Vrai (stretch > 0) le conteneur remplit AUSSI l'axe principal :
     * on pose Expanding sur l'axe principal, le facteur de stretch consommant
     * l'excedent (l'alignment transverse ne doit alors pas neutraliser
     * l'expansion — cf. gate plus bas). */
    bool containerFillTransverse = false;
    bool containerFillMain = false;
    bool isImageField = owl_widget_is_image_field(qchild);
    if ((owl_widget_is_container(qchild) || isImageField)
        && qobject_cast<QBoxLayout*>(lay)) {
        bool vertical   = qobject_cast<QVBoxLayout*>(lay) != nullptr;
        bool horizontal = qobject_cast<QHBoxLayout*>(lay) != nullptr;
        /* Taille transverse declaree ? (largeur en V, hauteur en H) */
        int declTransverse = vertical ? decl_w : (horizontal ? decl_h : 0);
        /*  — un champ image ne remplit le transverse que sous Étire
         * (sinon il respecte sa taille declaree / son sizeHint) ; un conteneur
         * le remplit par defaut (parite designer). */
        containerFillTransverse = (declTransverse <= 0)
                                   && (!isImageField || stretch > 0);
        /* Étire=Vrai = remplir aussi l'axe principal. */
        containerFillMain = (stretch > 0);
        QSizePolicy sp = qchild->sizePolicy();
        if (containerFillTransverse) {
            if (vertical)        sp.setHorizontalPolicy(QSizePolicy::Expanding);
            else if (horizontal) sp.setVerticalPolicy(QSizePolicy::Expanding);
        }
        if (containerFillMain) {
            if (vertical)        sp.setVerticalPolicy(QSizePolicy::Expanding);
            else if (horizontal) sp.setHorizontalPolicy(QSizePolicy::Expanding);
        }
        qchild->setSizePolicy(sp);
    }

    /*  — Étire=Vrai doit faire grandir un champ ORDINAIRE sur l'axe
     * PRINCIPAL, pas seulement un Conteneur. Un QLineEdit/QComboBox/QPushButton/
     * jauge est Fixed sur sa hauteur : dans une pile verticale le facteur de
     * stretch lui alloue bien la place, mais sa policy Fixed le recentre a sa
     * hauteur minimale (mesure : 23 px dans une fenetre de 300). On pose donc
     * Expanding sur l'axe principal (hauteur en pile V, largeur en pile H). Une
     * taille figee sur cet axe (Hauteur en V, Largeur en H) PRIME sur Étire : on
     * ne force pas Expanding dans ce cas, et le bornage min=max=decl
     * fige la taille. Les Conteneurs et champs image ont leur propre traitement
     * (containerFillMain ci-dessus) : semantique Hauteur=taille-de-depart, non
     * reprise ici. */
    if (stretch > 0 && qobject_cast<QBoxLayout*>(lay)
        && !owl_widget_is_container(qchild) && !isImageField) {
        bool vertical   = qobject_cast<QVBoxLayout*>(lay) != nullptr;
        bool horizontal = qobject_cast<QHBoxLayout*>(lay) != nullptr;
        int declMain = vertical ? decl_h : (horizontal ? decl_w : 0);
        if (declMain <= 0) {
            QSizePolicy sp = qchild->sizePolicy();
            if (vertical)        sp.setVerticalPolicy(QSizePolicy::Expanding);
            else if (horizontal) sp.setHorizontalPolicy(QSizePolicy::Expanding);
            qchild->setSizePolicy(sp);
        }
    }

    /*  (A) — la Hauteur/Largeur figee d'un enfant doit contraindre
     * l'axe PRINCIPAL du box layout (sinon le QVBoxLayout la jette et un widget
     * a policy Preferred, ex QLabel, gonfle pour absorber l'excedent). En pile
     * verticale l'axe principal est vertical (Hauteur fige), en pile horizontale
     * il est horizontal (Largeur fige). L'axe transverse reste libre : c'est lui
     * que l'Étire remplit, les deux coexistent (Hauteur figee + Étire
     * pleine largeur). On borne min ET max = valeur declaree pour figer sans
     * empecher l'etirement transverse. */
    if (qobject_cast<QBoxLayout*>(lay)) {
        bool mainVertical   = qobject_cast<QVBoxLayout*>(lay) != nullptr;
        bool mainHorizontal = qobject_cast<QHBoxLayout*>(lay) != nullptr;
        /*  — un Conteneur qui remplit l'axe PRINCIPAL (Étire=Vrai,
         * containerFillMain) doit pouvoir depasser sa Hauteur/Largeur declaree
         * pour absorber l'excedent de stretch : la taille declaree devient une
         * taille de DEPART (min), pas un plafond. Sans ca le max = decl fige la
         * dimension et neutralise le stretch/Expanding pose plus haut, si bien
         * que le layout repartit l'excedent en gaps autour du conteneur au lieu
         * de l'agrandir. On borne donc uniquement le min sur l'axe principal
         * quand Étire est actif ; le max reste libre (illimite). Hors Étire, le
         * comportement d'origine (min=max=decl, taille figee) est conserve. */
        if (mainVertical && decl_h > 0) {
            qchild->setMinimumHeight(decl_h);
            if (!containerFillMain) qchild->setMaximumHeight(decl_h);
        } else if (mainHorizontal && decl_w > 0) {
            qchild->setMinimumWidth(decl_w);
            if (!containerFillMain) qchild->setMaximumWidth(decl_w);
        }
        /*  — la taille TRANSVERSE declaree (Largeur en pile verticale,
         * Hauteur en pile horizontale) doit aussi etre honoree quand l'enfant
         * n'etire PAS son axe transverse. Auparavant l'alignment vide laissait
         * le layout etirer l'enfant sur toute la dimension transverse, ce qui
         * masquait le fait que sa taille declaree n'etait jamais figee. Avec le
         * defaut alignDébut (compact), sans ce bornage l'enfant retombe sur son
         * sizeHint (largeur du texte) au lieu de sa Largeur declaree.
         * On NE borne PAS quand l'axe transverse est explicitement etire
         * (alignÉtire) ou pris en charge par un stretchTarget (CadreExtérieur
         * stylé, /0882) : dans ce cas l'expansion prime. */
        bool transverseEtire = (align == OWL_ALIGN_ETIRE) || (stretchTarget != nullptr)
                               || containerFillTransverse;
        if (!transverseEtire) {
            if (mainVertical && decl_w > 0) {
                qchild->setMinimumWidth(decl_w);
                qchild->setMaximumWidth(decl_w);
            } else if (mainHorizontal && decl_h > 0) {
                qchild->setMinimumHeight(decl_h);
                qchild->setMaximumHeight(decl_h);
            }
        }
    }

    /* Marge externe : reservee AUTOUR du widget par un item dedie (cf.
     * OwlMarginWidgetItem), jamais prise sur la boite du widget. */
    QMargins childMargins(ml > 0 ? ml : 0, mt > 0 ? mt : 0,
                          mr > 0 ? mr : 0, mb > 0 ? mb : 0);
    bool hasMargins = !childMargins.isNull();

    Qt::Alignment qalign;
    if (align != 0) {
        /* L'enfant a pose sa propre prop Alignement (preset 2D). Prime sur le
         * defaut du conteneur. */
        qalign = owl_align_to_qt(align);
    } else {
        /* Fallback sur les defauts AlignH/AlignV du conteneur (poses via
         * setProperty dans owl_ui_*_set_layout). */
        QVariant vh = lay->property("owl_def_align_h");
        QVariant vv = lay->property("owl_def_align_v");
        if (vh.isValid()) qalign |= owl_align_h_to_qt(vh.toInt());
        if (vv.isValid()) qalign |= owl_align_v_to_qt(vv.toInt());
    }

    /*  — defaut d'alignement transverse = DEBUT, pas etirement.
     * Une alignment Qt vide sur addWidget laisse le layout etirer l'enfant sur
     * tout l'axe transverse (largeur en pile verticale, hauteur en pile
     * horizontale) : c'est un Étire implicite non demande. On veut au contraire
     * que l'enfant reste compact et accroche au debut de l'axe transverse (haut
     * ou gauche) quand ni sa prop Alignement ni un defaut conteneur n'ont ete
     * poses. On n'ancre QUE l'axe transverse : l'axe principal du box est
     * gouverne par le facteur d'etirement, jamais par l'alignment.
     * L'etirement transverse explicite (alignÉtire) mappe justement sur une
     * alignment Qt vide (owl_align_to_qt) : on l'exclut donc explicitement pour
     * ne pas convertir un Étire demande en accroche au debut. */
    /*  — un Conteneur qui remplit son axe transverse (ou principal via
     * Étire) ne doit PAS recevoir l'ancrage AlignLeft/AlignTop : cet alignment
     * neutraliserait l'expansion (le layout accrocherait le widget au debut a sa
     * taille naturelle au lieu de l'etirer). On laisse alors l'alignment vide sur
     * l'axe rempli pour que le layout etire. On preserve l'alignment TRANSVERSE
     * explicite d'un AlignH/AlignV pose (qalign deja calcule) si l'axe transverse
     * n'est PAS rempli. */
    bool containerFills = containerFillTransverse || containerFillMain;
    if (qalign == Qt::Alignment() && align != OWL_ALIGN_ETIRE && !containerFills) {
        if (qobject_cast<QVBoxLayout*>(lay)) {
            qalign = Qt::AlignLeft;   /* transverse horizontal -> gauche */
        } else if (qobject_cast<QHBoxLayout*>(lay)) {
            qalign = Qt::AlignTop;    /* transverse vertical -> haut */
        }
    }

    if (auto* box = qobject_cast<QBoxLayout*>(lay)) {
        if (hasMargins) {
            OwlMarginWidgetItem* item =
                new OwlMarginWidgetItem(qchild, childMargins, qalign);
            int idx = box->count();
            box->insertItem(idx, item);
            if (stretch) box->setStretch(idx, stretch);
        } else if (qalign != Qt::Alignment()) {
            box->addWidget(qchild, stretch, qalign);
        } else {
            box->addWidget(qchild, stretch);
        }
    } else if (auto* grid = qobject_cast<QGridLayout*>(lay)) {
        if (col_span < 1) col_span = 1;
        if (row_span < 1) row_span = 1;
        int actualRow, actualCol;
        if (col < 0 && row < 0) {
            /*  — auto-append base sur Colonnes/Lignes stoques par
             * owl_ui_*_set_layout. Ligne-major si Colonnes > 0, sinon
             * colonne-major si Lignes > 0. Fallback colonne unique sinon. */
            int idx = grid->count();
            int gcols = lay->property("owl_grid_cols").toInt();
            int grows = lay->property("owl_grid_rows").toInt();
            if (gcols > 0) {
                actualRow = idx / gcols;
                actualCol = idx % gcols;
            } else if (grows > 0) {
                actualCol = idx / grows;
                actualRow = idx % grows;
            } else {
                actualRow = grid->rowCount();
                actualCol = 0;
            }
        } else {
            actualRow = (row < 0) ? grid->rowCount() : row;
            actualCol = (col < 0) ? 0 : col;
        }
        if (hasMargins) {
            OwlMarginWidgetItem* item =
                new OwlMarginWidgetItem(qchild, childMargins, qalign);
            grid->addItem(item, actualRow, actualCol, row_span, col_span, qalign);
        } else if (qalign != Qt::Alignment()) {
            grid->addWidget(qchild, actualRow, actualCol, row_span, col_span, qalign);
        } else {
            grid->addWidget(qchild, actualRow, actualCol, row_span, col_span);
        }
    } else if (hasMargins) {
        lay->addItem(new OwlMarginWidgetItem(qchild, childMargins, qalign));
    } else {
        lay->addWidget(qchild);
    }
    qchild->show();
}

/*  (B) — ressort terminal d'un box layout (fenetre ou conteneur).
 * addStretch(1) final = absorbe l'excedent de l'axe principal apres le dernier
 * widget -> empilement en haut (QVBoxLayout) / a gauche (QHBoxLayout). No-op si
 * le layout n'est pas un box (grille, ou pas de layout = dispLibre). */
extern "C" void owl_ui_layout_add_terminal_stretch(OwlWindow* parent_window_or_null,
                                                     OwlControl* parent_container_or_null) {
    QWidget* parent = nullptr;
    if (parent_container_or_null) {
        parent = reinterpret_cast<QWidget*>(parent_container_or_null);
    } else if (parent_window_or_null) {
        parent = central_of(parent_window_or_null);
    }
    if (!parent) return;
    QBoxLayout* box = qobject_cast<QBoxLayout*>(parent->layout());
    if (!box) return;
    box->addStretch(1);
}

/* ------- Champ Onglet (QTabWidget) + Volets (pages) ------- */

/*  — un Champ Onglet est un QTabWidget. Chaque Volet est une page
 * (QWidget nu avec son propre layout, comme un Conteneur) ajoutee par addTab.
 * La page memorise via setProperty son QTabWidget parent + son index, pour que
 * les setters Volet (Libelle/Visible) atteignent la barre d'onglets a partir
 * du seul widget-page attache a l'ObjUiControlInstance du Volet. */

/*  (B1) — QTabBar qui rend toutes les languettes a la meme largeur :
 * celle de la plus large. Qt appelle tabSizeHint pendant le layout, apres
 * l'ajout des volets, donc le max est calcule sur des languettes reelles.
 *
 *  — la barre est desormais posee sur TOUT Onglet des sa creation, avec
 * la largeur egale DESACTIVEE par defaut : tabSizeHint delegue alors mot pour
 * mot au QTabBar natif (rendu proportionnel inchange). Motif : QTabWidget::
 * setTabBar detruit la barre en place — donc ses languettes et le style
 * d'alignement qui y vit. Installer la barre a la volee n'etait tenable qu'avant
 * le premier addTab, ce qui interdisait toute ecriture runtime de LargeurVolets
 * (et tout retour au mode proportionnel). Un simple booleen mutable rend les
 * deux sens de la bascule non destructifs. */
class OwlEqualWidthTabBar : public QTabBar {
public:
    explicit OwlEqualWidthTabBar(QWidget* parent = nullptr) : QTabBar(parent) {}
    void setEqualWidth(bool on) {
        if (m_equal == on) return;
        m_equal = on;
        invalideGeometrieLanguettes();
    }
    void setLabelAlignment(Qt::Alignment halign) {
        if (m_halign == halign) return;
        m_halign = halign;
        update();
    }
    /*  — polices des deux etats de languette, telles que la feuille de
     * style les pose. Sert uniquement au calcul de la largeur commune (cf.
     * tabSizeHint) ; la peinture reste entierement au style. */
    void setStateFonts(const QFont& actif, const QFont& inactif) {
        if (m_policesPosees && m_policeActif == actif && m_policeInactif == inactif) return;
        m_policeActif = actif;
        m_policeInactif = inactif;
        m_policesPosees = true;
        invalideGeometrieLanguettes();
    }
protected:
    /* La largeur commune est le max des largeurs de languette prises dans leur
     * etat LE PLUS LARGE, pas dans leur etat courant.
     *
     *  — QTabBar::tabSizeHint(i) mesure la languette i avec la police de
     * son etat du moment : la languette active porte celle du volet actif (grasse
     * dans le cas courant), les autres celle du volet inactif. Prendre le max de
     * ces mesures fait donc dependre la largeur commune de QUI est actif : au
     * changement de volet, c'est une autre languette qui passe en gras, le max
     * change, et toute la barre se reorganise sous le clic (mesure : 154 px de
     * languette volet 1 actif, 163 px volet 2 actif, sur trois libelles dont le
     * plus long est celui du volet 2).
     *
     * On ajoute donc a chaque mesure l'ecart d'empattement entre la police de son
     * etat courant et la plus large des deux polices d'etat : chaque languette est
     * comparee comme si elle etait active, et le max ne depend plus de l'etat.
     * Sans police d'etat posee, l'ecart est nul partout -> calcul inchange. */
    QSize tabSizeHint(int index) const override {
        QSize base = QTabBar::tabSizeHint(index);
        if (!m_equal) return base;
        int maxw = base.width();
        for (int i = 0; i < count(); ++i)
            maxw = qMax(maxw, QTabBar::tabSizeHint(i).width() + jeuPoliceEtat(i));
        base.setWidth(maxw);
        return base;
    }
    /*  — le libelle est retire de la passe de fond, jamais des calculs
     * de taille : QTabBar::tabSizeHint lit le texte de la languette, pas cette
     * option (elle ne sert qu'a la peinture et aux boutons de languette). */
    void initStyleOption(QStyleOptionTab* option, int index) const override {
        QTabBar::initStyleOption(option, index);
        if (m_stripLabel) option->text.clear();
    }
    /*  (B2), refondu par  — alignement horizontal du libelle.
     *
     * Interdit de passer par un QProxyStyle : des qu'une feuille de style touche
     * la languette avec une boite ou une bordure non native (c'est le cas d'un
     * arrondi, et du padding de compensation qu'il entraine), QStyleSheetStyle
     * peint LUI-MEME le libelle et ne redescend jamais vers le style de base.
     * Un style intercale est donc structurellement borgne dans ce cas, alors
     * qu'il fonctionne quand le style ne pose que des couleurs — d'ou un
     * alignement qui marchait a la mesure mais restait inerte des qu'un vrai
     * style etait pose.
     *
     * On peint donc en deux passes : le rendu complet de la barre libelles
     * effaces (fond, cadre de base, indicateur de decoupe), puis les libelles
     * seuls, decales du jeu de la languette — l'ecart entre sa largeur reelle
     * et sa largeur naturelle (texte + retraits, feuille de style comprise) :
     * le texte centre, decale d'une demi-marge, vient se coller au bord voulu.
     * Le libelle reste dessine par la feuille de style, donc garde sa couleur
     * et sa police.
     *
     *  — le decalage est applique au REPERE du peintre, jamais au
     * rectangle de l'option. Deplacer le rectangle imposait d'effacer l'index de
     * la languette, sans quoi la feuille de style redemande son rectangle a la
     * barre et annule le decalage ; mais sans index, le calcul du rectangle de
     * texte repart du rectangle deja retire au lieu du rectangle brut, et
     * retranche donc les retraits une seconde fois — sur un style arrondi, ou
     * ces retraits compensent les metriques natives perdues, il ne restait au
     * texte que quelques pixels de haut (tranche a l'horizontale). Une
     * translation du peintre laisse au style un jeu de rectangles strictement
     * identique au rendu normal ; la decoupe au rectangle de la languette
     * garantit qu'un libelle decale ne deborde pas sur sa voisine.
     *
     * Barres verticales exclues : le libelle y est peint tourne, et le jeu se
     * mesurerait sur la hauteur. */
    void paintEvent(QPaintEvent* e) override {
        const bool vertical = (shape() == QTabBar::RoundedWest ||
                               shape() == QTabBar::RoundedEast ||
                               shape() == QTabBar::TriangularWest ||
                               shape() == QTabBar::TriangularEast);
        if (m_halign == Qt::AlignHCenter || vertical) {
            QTabBar::paintEvent(e);
            return;
        }
        m_stripLabel = true;
        QTabBar::paintEvent(e);
        m_stripLabel = false;

        QPainter p(this);
        /* Meme reserve que QTabBar : les boutons de defilement ne doivent pas
         * etre recouverts par un libelle. */
        QRegion clip(rect());
        const auto boutons = findChildren<QAbstractButton*>(Qt::FindDirectChildrenOnly);
        for (QAbstractButton* b : boutons)
            if (b->isVisible()) clip -= b->geometry();
        p.setClipRegion(clip);

        const int selected = currentIndex();
        for (int passe = 0; passe < 2; ++passe) {
            for (int i = 0; i < count(); ++i) {
                if ((i == selected) != (passe == 1)) continue;
                if (!isTabVisible(i)) continue;
                QStyleOptionTab opt;
                initStyleOption(&opt, i);
                const int jeu = opt.rect.width() - QTabBar::tabSizeHint(i).width();
                p.save();
                if (jeu > 0) {
                    p.setClipRect(opt.rect, Qt::IntersectClip);
                    p.translate(m_halign == Qt::AlignLeft ? -jeu / 2 : jeu / 2, 0);
                }
                style()->drawControl(QStyle::CE_TabBarTabLabel, &opt, &p, this);
                p.restore();
            }
        }
    }
private:
    /* updateGeometry() ne suffit pas : QTabBar met en cache la geometrie de
     * chaque languette et ne la recalcule que sur invalidation interne. Un
     * QEvent::StyleChange est le declencheur public qui la marque sale (et vide
     * le cache des tailles de texte), donc le seul moyen de faire relire
     * tabSizeHint apres coup sans reconstruire la barre. */
    void invalideGeometrieLanguettes() {
        QEvent relayout(QEvent::StyleChange);
        QCoreApplication::sendEvent(this, &relayout);
        updateGeometry();
    }
    /* Largeur a ajouter a la mesure de la languette i pour l'amener a son etat le
     * plus large. Empattement seul : le reste de la boite (retraits, cadre,
     * arrondi) est identique aux deux etats, donc s'annule dans l'ecart. */
    int jeuPoliceEtat(int i) const {
        if (!m_policesPosees) return 0;
        const QString t = tabText(i);
        if (t.isEmpty()) return 0;
        const int wActif   = QFontMetrics(m_policeActif).size(Qt::TextShowMnemonic, t).width();
        const int wInactif = QFontMetrics(m_policeInactif).size(Qt::TextShowMnemonic, t).width();
        const int courant  = (i == currentIndex()) ? wActif : wInactif;
        return qMax(wActif, wInactif) - courant;
    }
    bool m_equal = false;
    Qt::Alignment m_halign = Qt::AlignHCenter;
    mutable bool m_stripLabel = false;
    bool m_policesPosees = false;
    QFont m_policeActif;
    QFont m_policeInactif;
};

/* Sous-classe exposant setTabBar (protege sur QTabWidget) pour installer la
 * barre OWL a la creation, avant tout addTab. */
class OwlOngletTabWidget : public QTabWidget {
public:
    explicit OwlOngletTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        setTabBar(new OwlEqualWidthTabBar(this));
    }
    OwlEqualWidthTabBar* owlBar() const {
        return static_cast<OwlEqualWidthTabBar*>(tabBar());
    }
};

/*  — recupere la barre OWL d'un Onglet. Tout Onglet est cree comme
 * OwlOngletTabWidget (owl_ui_onglet_create) : le qobject_cast valide le type
 * QTabWidget (Q_OBJECT), le static_cast descend vers la sous-classe (pas de
 * Q_OBJECT/moc dessus, comme le reste de la famille Onglet). */
static OwlEqualWidthTabBar* owl_onglet_bar(OwlControl* onglet) {
    if (!onglet) return nullptr;
    QTabWidget* base = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!base) return nullptr;
    return static_cast<OwlOngletTabWidget*>(base)->owlBar();
}

extern "C" OwlControl* owl_ui_onglet_create(OwlWindow* parent,
                                            int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QTabWidget* tabs = new OwlOngletTabWidget(central_of(parent));
    if (w > 0 && h > 0) tabs->setGeometry(x, y, w, h);
    tabs->show();
    return reinterpret_cast<OwlControl*>(tabs);
}

/*  — feuilles QSS du style de l'Onglet pre-calculees pour les 4
 * positions de barre, memorisees sur le widget par owl_ui_onglet_apply_style.
 * L'arrondi et le padding de compensation des languettes dependent en effet de
 * l'orientation : deplacer la barre apres coup doit rebasculer la feuille, sinon
 * les coins arrondis restent orientes pour l'ancienne position. Index = ordre
 * de QTabWidget::TabPosition (North, South, West, East). */
static const char* const OWL_ONGLET_QSS_PROP = "owlOngletQssParPosition";

extern "C" void owl_ui_onglet_set_position(OwlControl* onglet, int pos) {
    if (!onglet) return;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!tabs) return;
    QTabWidget::TabPosition qp = QTabWidget::North;
    switch (pos) {
        case OWL_POS_HAUT:   qp = QTabWidget::North; break;
        case OWL_POS_BAS:    qp = QTabWidget::South; break;
        case OWL_POS_GAUCHE: qp = QTabWidget::West;  break;
        case OWL_POS_DROITE: qp = QTabWidget::East;  break;
        default: break;
    }
    tabs->setTabPosition(qp);
    const QStringList sheets = tabs->property(OWL_ONGLET_QSS_PROP).toStringList();
    if (sheets.size() == 4) tabs->setStyleSheet(sheets.at((int)qp));
}

/*  — fragment QSS des props POSEES d'une surface de volet (couleurs +
 * police). Vide si rien posé -> la surface retombe sur le rendu par défaut du
 * thème (aucun selecteur émis pour elle). Réutilise l'encodage couleur du style
 * menu (owl_menu_bgr_css : BGR + alpha). */
static QString owl_onglet_surface_css(const OwlOngletStyleSurface* s) {
    QString out;
    if (!s) return out;
    if (s->couleur_fond >= 0) out += "background: " + owl_menu_bgr_css(s->couleur_fond) + "; ";
    if (s->couleur >= 0)      out += "color: " + owl_menu_bgr_css(s->couleur) + "; ";
    if (s->police_nom && s->police_nom[0])
        out += QString("font-family: '%1'; ").arg(QString::fromUtf8(s->police_nom));
    if (s->police_taille >= 0) out += QString("font-size: %1pt; ").arg(s->police_taille);
    if (s->police_gras >= 0)   out += QString("font-weight: %1; ").arg(s->police_gras ? "bold" : "normal");
    if (s->police_italique >= 0) out += QString("font-style: %1; ").arg(s->police_italique ? "italic" : "normal");
    if (s->police_souligne >= 0 || s->police_barre >= 0) {
        QStringList deco;
        if (s->police_souligne == 1) deco << "underline";
        if (s->police_barre == 1)    deco << "line-through";
        if (deco.isEmpty()) deco << "none";
        out += QString("text-decoration: %1; ").arg(deco.join(' '));
    }
    return out;
}

/*  — meme police que celle emise par owl_onglet_surface_css, sous forme
 * de QFont : la barre en a besoin pour caler toutes les languettes sur la meme
 * largeur sans dependre du volet actif. Souligne et barre sont ignores (ils ne
 * changent pas l'empattement). */
static QFont owl_onglet_surface_font(const QFont& base, const OwlOngletStyleSurface* s) {
    QFont f = base;
    if (!s) return f;
    if (s->police_nom && s->police_nom[0]) f.setFamily(QString::fromUtf8(s->police_nom));
    if (s->police_taille >= 0)   f.setPointSize(s->police_taille);
    if (s->police_gras >= 0)     f.setBold(s->police_gras != 0);
    if (s->police_italique >= 0) f.setItalic(s->police_italique != 0);
    return f;
}

/*  — fragment QSS de l'arrondi des coins d'une languette. Les coins
 * arrondis sont ceux du bord "sortant" de la barre, fonction de la position :
 * en haut, ce sont les coins hauts ; en bas, les coins bas ; à gauche/droite,
 * les deux coins du bord extérieur correspondant. -1 = non posé -> vide. */
static QString owl_onglet_tab_radius_css(int arrondi, QTabWidget::TabPosition pos) {
    if (arrondi < 0) return QString();
    const QString r = QString::number(arrondi) + "px";
    switch (pos) {
        case QTabWidget::South:
            return "border-bottom-left-radius: " + r + "; border-bottom-right-radius: " + r + "; ";
        case QTabWidget::West:
            return "border-top-left-radius: " + r + "; border-bottom-left-radius: " + r + "; ";
        case QTabWidget::East:
            return "border-top-right-radius: " + r + "; border-bottom-right-radius: " + r + "; ";
        case QTabWidget::North:
        default:
            return "border-top-left-radius: " + r + "; border-top-right-radius: " + r + "; ";
    }
}

/*  — padding des languettes restitué depuis les métriques natives.
 *
 * Dès qu'une règle QSS force Qt à dessiner lui-même la boîte d'une languette
 * (c'est le cas d'un border-radius), QStyleSheetStyle rend nuls les
 * PM_TabBarTabHSpace/VSpace du thème : la languette se réduit exactement à la
 * taille du texte (mesuré sous Fusion : 27px -> 15px de haut, texte collé aux
 * bords). On réinjecte donc un padding équivalent au retrait natif, DERIVE et
 * non codé en dur : un QTabBar sonde (jamais montré, même thème et même
 * police) donne le tabSizeHint natif d'un libellé témoin ; l'écart avec les
 * métriques de la police EST le retrait appliqué par le thème.
 *
 * Les formes verticales (gauche/droite) reçoivent un hint transposé par Qt :
 * l'axe du texte est alors la hauteur, d'où l'inversion des deux écarts. */
static QString owl_onglet_tab_native_padding_css(QTabWidget::TabPosition pos) {
    /* tabSizeHint est protégé : sous-classe locale pour y accéder. */
    struct SizeProbe : public QTabBar { using QTabBar::tabSizeHint; };
    SizeProbe probe;
    const bool vertical = (pos == QTabWidget::West || pos == QTabWidget::East);
    switch (pos) {
        case QTabWidget::South: probe.setShape(QTabBar::RoundedSouth); break;
        case QTabWidget::West:  probe.setShape(QTabBar::RoundedWest);  break;
        case QTabWidget::East:  probe.setShape(QTabBar::RoundedEast);  break;
        case QTabWidget::North:
        default:                probe.setShape(QTabBar::RoundedNorth); break;
    }
    static const QString temoin = QStringLiteral("Xg");
    probe.addTab(temoin);
    const QSize hint = probe.tabSizeHint(0);
    const QFontMetrics fm = probe.fontMetrics();
    const int textAxis  = fm.horizontalAdvance(temoin); /* le long du texte */
    const int crossAxis = fm.height();                  /* perpendiculaire   */
    int dV = hint.height() - (vertical ? textAxis : crossAxis);
    int dH = hint.width()  - (vertical ? crossAxis : textAxis);
    if (dV < 0) dV = 0;
    if (dH < 0) dH = 0;
    /* Répartition exacte : le reste impair va au bord bas/droit. */
    return QString("padding: %1px %2px %3px %4px; ")
               .arg(dV / 2).arg(dH / 2).arg(dV - dV / 2).arg(dH - dH / 2);
}

/*  — feuille de style du Champ Onglet, paramétrée par le style résolu.
 * Chaque prop non posée est OMISE -> repli sur le rendu par défaut du thème
 * (QTabBar Fusion). Un état survol ne redéclare que ses props posées : la
 * cascade QSS (:selected:hover plus spécifique que :selected) hérite du reste.
 * `pos` : position de la barre, oriente les coins arrondis des languettes. */
static QString owl_onglet_stylesheet(const OwlOngletStyleResolved* st,
                                     QTabWidget::TabPosition pos) {
    if (!st || !st->has_style) return QString();
    QString ss;
    /*  — le pane porte fond + bordure via le sous-élément CadreExtérieur.
     * Chaque prop non posée est omise (repli sur le rendu par défaut du thème).
     * QTabWidget::pane accepte background + border + border-radius (box-model).
     *
     *  — un fond POSE SEUL doit quand meme engager le modele de boite.
     * Tant que la regle du pane ne declare AUCUNE propriete de bordure, le
     * moteur de feuille de style la classe "bordure native" : il peint le fond
     * demande, puis laisse le theme redessiner le cadre du pane PAR-DESSUS, et
     * ce trace commence par remplir toute la surface de sa propre couleur ->
     * le fond demande est efface. Pire, cette couleur derive du role Bouton de
     * la palette, que la regle de transparence du champ a rendu
     * translucide : le theme la recompose en NOIR opaque (mesure : pane rendu
     * 0,0,0 pour un fond Blanc demande). Une seule declaration de bordure
     * suffit a faire prendre la main au modele de boite (mesure : border-width,
     * border ou border-radius corrigent tous les trois ; margin non). On emet
     * donc `border: none` a defaut de cadre pose — meme parade que le fond d'un
     * Bouton, dont le chrome natif ignore aussi le fond sans bordure. */
    const OwlOngletCadreExtResolved* ce = &st->cadre_ext;
    QString paneBody;
    if (ce->couleur_fond >= 0)
        paneBody += "background: " + owl_menu_bgr_css(ce->couleur_fond) + "; ";
    if (ce->cadre_epaisseur >= 0 && ce->cadre_couleur >= 0)
        paneBody += "border: " + QString::number(ce->cadre_epaisseur) + "px solid "
                  + owl_menu_bgr_css(ce->cadre_couleur) + "; ";
    else if (ce->cadre_couleur >= 0)
        paneBody += "border-color: " + owl_menu_bgr_css(ce->cadre_couleur) + "; ";
    else if (ce->cadre_epaisseur >= 0)
        paneBody += "border-width: " + QString::number(ce->cadre_epaisseur) + "px; ";
    else if (ce->couleur_fond >= 0 && ce->cadre_arrondi < 0)
        paneBody += "border: none; ";
    if (ce->cadre_arrondi >= 0)
        paneBody += "border-radius: " + QString::number(ce->cadre_arrondi) + "px; ";
    if (!paneBody.isEmpty())
        ss += "QTabWidget::pane { " + paneBody + "} ";
    /*  — l'arrondi de la forme vit sur les surfaces de base (la
     * surcharge survol ne pilote que les couleurs) ; il s'ajoute au corps QSS
     * de la languette selon la position de la barre. */
    QString actB = owl_onglet_surface_css(&st->actif_base);
    actB += owl_onglet_tab_radius_css(st->actif_base.arrondi, pos);
    QString inaB = owl_onglet_surface_css(&st->inactif_base);
    inaB += owl_onglet_tab_radius_css(st->inactif_base.arrondi, pos);
    /*  — un arrondi sur UNE seule des deux surfaces suffit à faire
     * perdre au thème les retraits natifs de TOUTE la barre : le padding de
     * compensation se pose donc sur les deux languettes, sinon l'autre reste
     * écrasée. Sans arrondi : aucun padding émis, rendu natif intact. */
    if (st->actif_base.arrondi >= 0 || st->inactif_base.arrondi >= 0) {
        const QString pad = owl_onglet_tab_native_padding_css(pos);
        actB += pad;
        inaB += pad;
    }
    const QString actH = owl_onglet_surface_css(&st->actif_survol);
    const QString inaH = owl_onglet_surface_css(&st->inactif_survol);
    /*  — la bande de languettes n'a pas de fond propre : elle laisse voir
     * ce qui est derriere le champ (mesure : fenetre RVB(0,200,0), bande lue
     * exactement 0,200,0, styles poses ou non). Sans regle explicite, ce n'est
     * vrai que tant qu'un parent peint derriere : rendu isolement (capture d'un
     * champ, canevas d'editeur), Qt considere le QTabWidget opaque et remplit la
     * bande de la couleur Window de la palette systeme (mesure : 239,239,239) ->
     * rectangles clairs parasites autour des languettes. On declare donc la
     * transparence, ce qui rend le widget non-opaque et fait aussi tomber le
     * remplissage systeme du rendu isole.
     *
     *  — mais `QTabBar { background: transparent }` bascule la barre en
     * rendu QSS complet : sans AUCUNE regle `QTabBar::tab` de surface (pane
     * stylé mais aucune couleur ni arrondi de volet posés), Qt peint alors les
     * languettes en noir plein (texte illisible). On ne declare donc la
     * transparence de la barre QUE si au moins une regle de surface de languette
     * est produite ; sinon on laisse le rendu natif Fusion peindre la bande. */
    const bool aReglesTab = !actB.isEmpty() || !inaB.isEmpty()
                         || !actH.isEmpty() || !inaH.isEmpty();
    ss += "QTabWidget { background: transparent; } ";
    if (aReglesTab)
        ss += "QTabBar { background: transparent; } ";
    if (!actB.isEmpty()) ss += "QTabBar::tab:selected { "        + actB + "} ";
    if (!inaB.isEmpty()) ss += "QTabBar::tab:!selected { "       + inaB + "} ";
    if (!actH.isEmpty()) ss += "QTabBar::tab:selected:hover { "  + actH + "} ";
    if (!inaH.isEmpty()) ss += "QTabBar::tab:!selected:hover { " + inaH + "} ";
    return ss;
}

extern "C" void owl_ui_onglet_apply_style(OwlControl* onglet,
                                          const OwlOngletStyleResolved* style) {
    if (!onglet) return;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!tabs) return;
    /*  — memorise les 4 variantes d'orientation pour qu'une ecriture
     * ulterieure de ..Position puisse rebasculer la feuille (coins arrondis et
     * padding de compensation suivent la barre). */
    QStringList sheets;
    sheets << owl_onglet_stylesheet(style, QTabWidget::North)
           << owl_onglet_stylesheet(style, QTabWidget::South)
           << owl_onglet_stylesheet(style, QTabWidget::West)
           << owl_onglet_stylesheet(style, QTabWidget::East);
    tabs->setProperty(OWL_ONGLET_QSS_PROP, sheets);
    const QString ss = sheets.at((int)tabs->tabPosition());
    if (!ss.isEmpty()) tabs->setStyleSheet(ss);
    /*  — la barre calcule sa largeur commune a partir des polices des
     * deux etats : elle doit les connaitre en meme temps que la feuille. */
    OwlEqualWidthTabBar* bar = owl_onglet_bar(onglet);
    if (bar && style)
        bar->setStateFonts(owl_onglet_surface_font(bar->font(), &style->actif_base),
                           owl_onglet_surface_font(bar->font(), &style->inactif_base));
    /*  — le liseré de jonction de la barre (PE_FrameTabBarBase) est
     * tracé sur toute sa largeur AVANT les languettes. Des que le CadreExtérieur
     * est habillé, QStyleSheetStyle annule le recouvrement de la barre
     * (PM_TabBarBaseOverlap = 0 si une regle QTabWidget::pane existe) : au lieu de
     * rester sous le pane, le liseré remonte au
     * ras du bord haut des languettes, qui le recouvrent... sauf dans leurs coins
     * des qu'un arrondi est pose. D'ou de petits arcs gris (Fusion : outline
     * eclairci, mesure 188,188,188) au-dessus de chaque coin. Un arrondi de
     * languette designe une barre entierement peinte par le style : ce liseré
     * natif n'y a plus de role, on le coupe. Sans arrondi, rendu natif intact. */
    if (bar) {
        const bool arrondi = style && (style->actif_base.arrondi >= 0 ||
                                       style->inactif_base.arrondi >= 0);
        bar->setDrawBase(!arrondi);
    }
}

/*  (B1) — mode de largeur des languettes. largeurEgale cale toutes les
 * languettes sur la plus large ; largeurProportionnelle rend la main au calcul
 * natif.  : bascule dans les deux sens, a tout moment (la barre OWL est
 * posee des la creation). */
extern "C" void owl_ui_onglet_set_tab_width_mode(OwlControl* onglet, int mode) {
    OwlEqualWidthTabBar* bar = owl_onglet_bar(onglet);
    if (!bar) return;
    bar->setEqualWidth(mode == OWL_LARGVOL_EGALE);
}

/*  (B2) — alignement horizontal du texte des languettes. alignCentre
 * (defaut) rend le libelle comme le thème natif.  : reversible a tout
 * moment. */
extern "C" void owl_ui_onglet_set_label_alignment(OwlControl* onglet, int align) {
    OwlEqualWidthTabBar* bar = owl_onglet_bar(onglet);
    if (!bar) return;
    Qt::Alignment ha;
    switch (align) {
        case OWL_ALIGN_DEBUT: ha = Qt::AlignLeft;    break;
        case OWL_ALIGN_FIN:   ha = Qt::AlignRight;   break;
        default:              ha = Qt::AlignHCenter; break; /* centre / etire */
    }
    bar->setLabelAlignment(ha);
}

/* Cree une page vide (QWidget) destinee a etre ajoutee par owl_ui_onglet_add_volet.
 * Sans parent : addTab la reparente dans le QTabWidget. */
extern "C" OwlControl* owl_ui_onglet_volet_create(void) {
    QWidget* page = new QWidget(nullptr);
    return reinterpret_cast<OwlControl*>(page);
}

extern "C" void owl_ui_onglet_add_volet(OwlControl* onglet, OwlControl* page,
                                        const char* libelle_utf8) {
    if (!onglet || !page) return;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    QWidget* pg = reinterpret_cast<QWidget*>(page);
    if (!tabs) return;
    int idx = tabs->addTab(pg, QString::fromUtf8(libelle_utf8 ? libelle_utf8 : ""));
    /* La page retient sa barre d'onglets + son index pour les setters Volet. */
    pg->setProperty("owl_tab_parent", QVariant::fromValue<void*>((void*)tabs));
    pg->setProperty("owl_tab_index", idx);
}

/* setCurrentIndex avec signaux bloques : un set programmatique de ..Valeur ne
 * doit PAS reentrer par currentChanged (qui retirerait surChangementVolet). Le
 * hook OP_STRUCT_SET (vm.c) est la source UNIQUE de l'event programmatique. Seul
 * le clic utilisateur (signaux non bloques) passe par currentChanged. */
extern "C" void owl_ui_onglet_set_current(OwlControl* onglet, int index0) {
    if (!onglet) return;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!tabs) return;
    if (index0 < 0 || index0 >= tabs->count()) return;
    bool prev = tabs->blockSignals(true);
    tabs->setCurrentIndex(index0);
    tabs->blockSignals(prev);
}

/* Lecture de l'index courant (0-based) du QTabWidget. Le mode GUI lit ..Valeur
 * via ce getter ; le +1 (contrat 1-based cote DSL) est applique par l'appelant.
 * Renvoie 0 si widget absent ou incoherent. */
extern "C" int owl_ui_onglet_get_current(OwlControl* onglet) {
    if (!onglet) return 0;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!tabs) return 0;
    int idx = tabs->currentIndex();
    return idx < 0 ? 0 : idx;
}

struct OwlOngletChangeHandler {
    OwlEventCb1i cb;
    void*        userdata;
    OwlControl*  sender;
};

extern "C" void owl_ui_onglet_on_change(OwlControl* onglet,
                                        OwlEventCb1i cb, void* userdata) {
    if (!onglet) return;
    QTabWidget* tabs = qobject_cast<QTabWidget*>(reinterpret_cast<QObject*>(onglet));
    if (!tabs) return;
    OwlOngletChangeHandler* h = new OwlOngletChangeHandler{ cb, userdata, onglet };
    owl_track_conn(tabs,
        QObject::connect(tabs, &QTabWidget::currentChanged, tabs, [h](int idx) {
            if (h->cb) h->cb(h->sender, h->userdata, idx);
        }));
}

/* Setters Volet : le widget attache a l'ObjUiControlInstance du Volet est sa
 * page. On remonte a la barre d'onglets + index memorises a l'addTab. */
static bool onglet_resolve_tab(OwlControl* page, QTabWidget** outTabs, int* outIdx) {
    if (!page) return false;
    QWidget* pg = reinterpret_cast<QWidget*>(page);
    QVariant vp = pg->property("owl_tab_parent");
    QVariant vi = pg->property("owl_tab_index");
    if (!vp.isValid() || !vi.isValid()) return false;
    QTabWidget* tabs = reinterpret_cast<QTabWidget*>(vp.value<void*>());
    if (!tabs) return false;
    /* indexOf reste fiable meme apres masquage/reordonnancement Qt. */
    int idx = tabs->indexOf(pg);
    if (idx < 0) idx = vi.toInt();
    *outTabs = tabs;
    *outIdx = idx;
    return true;
}

extern "C" void owl_ui_volet_set_libelle(OwlControl* page, const char* text_utf8) {
    QTabWidget* tabs = nullptr; int idx = -1;
    if (!onglet_resolve_tab(page, &tabs, &idx)) return;
    if (idx < 0 || idx >= tabs->count()) return;
    tabs->setTabText(idx, QString::fromUtf8(text_utf8 ? text_utf8 : ""));
}

extern "C" void owl_ui_volet_set_visible(OwlControl* page, int visible) {
    QTabWidget* tabs = nullptr; int idx = -1;
    if (!onglet_resolve_tab(page, &tabs, &idx)) return;
    if (idx < 0 || idx >= tabs->count()) return;
    tabs->setTabVisible(idx, visible ? true : false);
}

/* ------- Libelle ------- */

extern "C" OwlControl* owl_ui_label_create(OwlWindow* parent,
                                           const char* text_utf8,
                                           int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QLabel* lbl = new QLabel(QString::fromUtf8(text_utf8 ? text_utf8 : ""), central_of(parent));
    lbl->setGeometry(x, y, w, h);
    lbl->show();
    return reinterpret_cast<OwlControl*>(lbl);
}

/* ------- Bouton ------- */

struct OwlClickHandler {
    OwlCallback cb;
    void*       userdata;
    OwlControl* sender;
};

extern "C" OwlControl* owl_ui_button_create(OwlWindow* parent,
                                            const char* text_utf8,
                                            int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QPushButton* btn = new QPushButton(QString::fromUtf8(text_utf8 ? text_utf8 : ""),
                                        central_of(parent));
    btn->setGeometry(x, y, w, h);
    btn->show();
    return reinterpret_cast<OwlControl*>(btn);
}

extern "C" void owl_ui_button_on_click(OwlControl* button,
                                       OwlCallback cb,
                                       void* userdata) {
    if (!button) return;
    QPushButton* btn = reinterpret_cast<QPushButton*>(button);

    /* On stocke un OwlClickHandler heap. Lifetime = bouton (POC :
     * leak intentionnel acceptable). */
    OwlClickHandler* h = new OwlClickHandler{ cb, userdata, button };

    owl_track_conn(btn,
        QObject::connect(btn, &QPushButton::clicked, btn, [h]() {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

/* ------- Champ Table (V1 — squelette) ------- */

/*  — libelle bilingue resolu au MOMENT de l'appel. La langue courante
 * (owl_i18n_langue_courante, forme BCP 47 ex "fr-FR"/"en-US", defaut "fr-FR")
 * est lue au clic, car le menu loupe est reconstruit a la volee dans
 * showContextMenu : basculer la langue puis rouvrir le menu suffit a changer
 * les libelles. Choix binaire FR/EN sur le sous-tag primaire : "en*" -> anglais,
 * tout le reste -> francais (defaut/canon). Ce menu de base est transitionnel :
 * l'etape MenuContextuel declare le remplacera par une surcharge utilisateur. */
extern "C" const char* owl_i18n_langue_courante(void);

static QString owl_ui_label(const char* fr, const char* en) {
    const char* lang = owl_i18n_langue_courante();
    bool isEn = lang && (lang[0] == 'e' || lang[0] == 'E')
                     && (lang[1] == 'n' || lang[1] == 'N');
    return QString::fromUtf8(isEn ? en : fr);
}

/* +2122 — header horizontal custom aligne sur le canon (loupe) :
 * peint une icone LOUPE a droite du texte de chaque en-tete. Le clic gauche sur
 * la loupe transforme le titre de la colonne en champ de saisie inline (QLineEdit
 * superpose) ; la frappe applique le mode de gestion de la loupe configure par la
 * propriete ModeLoupe, lu au runtime via owl_ui_table_user_mode_loupe :
 *   - filtreContient : filtre "contient" en direct, masque les lignes hors motif
 *     (meme store $tblfilters que TableActiveFiltre, ponts owl_ui_table_user_*).
 *   - rechercheCommencePar (defaut) : trie la colonne (ascendant) et positionne
 *     la selection sur la 1re ligne « commence par » le motif, SANS masquer.
 * Le clic sur le texte du titre (hors loupe) garde le tri par defaut
 * (QHeaderView::mousePressEvent). La sortie (Entree / Echap / focus perdu) masque
 * le champ et restaure le titre ; en mode filtre le filtre pose reste actif et
 * l'icone en reflete l'etat. Le clic DROIT sur la loupe ouvre un
 * menu contextuel : Rechercher (= saisie inline), Autres filtres ▸ (choix d'un
 * operateur filtre* applique a la saisie inline), Supprimer le filtre. */
class OwlFilterHeaderView : public QHeaderView {
public:
    explicit OwlFilterHeaderView(QTableView* view)
        : QHeaderView(Qt::Horizontal, view), m_view(view) {
        /* Le champ inline doit suivre la section editee quand la colonne bouge,
         * change de taille ou defile horizontalement. */
        QObject::connect(this, &QHeaderView::sectionResized, this,
                         [this](int, int, int) { positionEditor(); });
        QObject::connect(this, &QHeaderView::sectionMoved, this,
                         [this](int, int, int) { positionEditor(); });
        QObject::connect(this, &QHeaderView::geometriesChanged, this,
                         [this]() { positionEditor(); });
        /*  (rework) — la reserve de la fleche de tri (et donc le
         * decalage de la loupe) n'existe que sur la colonne effectivement triee
         * (cf. iconRect). Quand l'utilisateur trie une autre colonne, l'ancienne
         * perd sa fleche : il faut repeindre tout l'en-tete pour que sa loupe
         * revienne au bord droit et que la nouvelle se decale. */
        QObject::connect(this, &QHeaderView::sortIndicatorChanged, this,
                         [this](int, Qt::SortOrder) {
                             viewport()->update();
                             positionEditor();
                         });
        if (m_view && m_view->horizontalScrollBar()) {
            QObject::connect(m_view->horizontalScrollBar(),
                             &QScrollBar::valueChanged, this,
                             [this](int) { positionEditor(); });
        }
    }

    /*  — ouverture programmatique de la saisie loupe (native
     * TableSaisieLoupe). Reutilise EXACTEMENT le chemin du clic gauche loupe :
     * showInlineEditor(col0, forcedOp). Le prefill via le store $tblfilters s'y
     * fait deja ; on force en plus la valeur initiale fournie (cas mode
     * Recherche ou aucun filtre persiste : setText n'emet pas textEdited, donc
     * pas de re-application, coherent avec la re-ouverture d'une colonne
     * filtree). */
    void openSearchProgrammatic(int col0, int forcedOp, const char* initialValue) {
        if (col0 < 0) return;
        showInlineEditor(col0, forcedOp);
        if (m_editor && initialValue && initialValue[0] != '\0') {
            m_editor->setText(QString::fromUtf8(initialValue));
        }
    }

protected:
    void paintSection(QPainter* painter, const QRect& rect,
                      int logicalIndex) const override {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        QRect ir = iconRect(rect, logicalIndex);
        if (!m_view || logicalIndex < 0 || ir.isEmpty()) return;
        OwlControl* tbl = reinterpret_cast<OwlControl*>(m_view);
        bool active = owl_ui_table_col_is_filtered(tbl, logicalIndex) != 0;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor col = active ? QColor(0x2d, 0x7d, 0xd2) : QColor(0x9a, 0x9a, 0x9a);
        qreal x0 = ir.left(), y0 = ir.top();
        qreal ww = ir.width(), hh = ir.height();
        /*  — l'icone reflete l'etat de filtrage : ENTONNOIR des qu'un
         * filtre est actif sur la colonne (pose au clavier en mode Filtre OU
         * force via le menu contextuel « Autres filtres », ) OU si
         * ModeLoupe == Filtre ; LOUPE sinon (mode Recherche sans filtre actif :
         * trie + positionne, ne masque rien). Le mode est resolu au niveau Table
         * (owl_ui_table_user_mode_loupe lit le store d'instance, defaut
         * rechercheCommencePar) ; l'etat filtre vient du store $tblfilters
         * (owl_ui_table_col_is_filtered, deja lu dans `active`). */
        bool showFunnel =
            active || owl_ui_table_user_mode_loupe(tbl) == OWL_FILTRE_CONTIENT;
        if (showFunnel) {
            /* Entonnoir : bord superieur large, col qui descend au centre. */
            QPainterPath p;
            p.moveTo(x0,             y0);
            p.lineTo(x0 + ww,        y0);
            p.lineTo(x0 + ww * 0.60, y0 + hh * 0.42);
            p.lineTo(x0 + ww * 0.60, y0 + hh);
            p.lineTo(x0 + ww * 0.40, y0 + hh * 0.82);
            p.lineTo(x0 + ww * 0.40, y0 + hh * 0.42);
            p.closeSubpath();
            if (active) {
                painter->fillPath(p, col);
            } else {
                QPen pen(col);
                pen.setWidthF(1.1);
                painter->setPen(pen);
                painter->drawPath(p);
            }
        } else {
            /* Loupe : cercle (lentille) en haut-gauche + manche en bas-droite. */
            qreal r  = qMin(ww, hh) * 0.30;
            qreal cx = x0 + ww * 0.42;
            qreal cy = y0 + hh * 0.42;
            QPen pen(col);
            pen.setWidthF(active ? 1.6 : 1.2);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(QPointF(cx, cy), r, r);
            /* Manche partant du bord bas-droite du cercle (45 deg) vers le coin. */
            qreal off = r * 0.7071;   /* cos/sin 45 deg */
            painter->drawLine(QPointF(cx + off, cy + off),
                              QPointF(x0 + ww * 0.96, y0 + hh * 0.96));
        }
        painter->restore();
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            int logical = logicalIndexAt(e->position().toPoint());
            if (logical >= 0) {
                int vpos = sectionViewportPosition(logical);
                int vw   = sectionSize(logical);
                QRect sectionRect(vpos, 0, vw, height());
                if (iconRect(sectionRect, logical).contains(e->position().toPoint())) {
                    showInlineEditor(logical);
                    e->accept();      /* consomme : pas de tri sur la loupe */
                    return;
                }
            }
        }
        QHeaderView::mousePressEvent(e);
    }

    /*  — clic DROIT sur la loupe d'une colonne : ouvre un menu
     * contextuel (canon). Le hit-test reprend celui du clic gauche
     * (iconRect) : hors loupe, on delegue au comportement par defaut. Couvre
     * aussi la touche Menu clavier (Qt route contextMenuEvent dans les deux
     * cas). */
    void contextMenuEvent(QContextMenuEvent* e) override {
        int logical = logicalIndexAt(e->pos());
        if (logical >= 0) {
            int vpos = sectionViewportPosition(logical);
            int vw   = sectionSize(logical);
            QRect sectionRect(vpos, 0, vw, height());
            if (iconRect(sectionRect, logical).contains(e->pos())) {
                showContextMenu(logical, e->globalPos());
                e->accept();
                return;
            }
        }
        QHeaderView::contextMenuEvent(e);
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj == m_editor) {
            if (event->type() == QEvent::KeyPress) {
                QKeyEvent* ke = static_cast<QKeyEvent*>(event);
                if (ke->key() == Qt::Key_Escape ||
                    ke->key() == Qt::Key_Return ||
                    ke->key() == Qt::Key_Enter) {
                    hideEditor();
                    return true;
                }
            } else if (event->type() == QEvent::FocusOut) {
                hideEditor();
                /* Ne pas consommer : laisser le focus partir normalement. */
            }
        }
        return QHeaderView::eventFilter(obj, event);
    }

private:
    QRect iconRect(const QRect& sectionRect, int logicalIndex) const {
        int sz = 12;
        /*  (POINT A) — laisser la place a l'indicateur de tri Qt. La
         * fleche de tri est dessinee au bord droit de la section (SE_HeaderArrow
         * ~ largeur PM_HeaderMargin*2 + hauteur/2, cf. qcommonstyle). L'icone
         * loupe/entonnoir se pose A GAUCHE de cette reserve pour ne pas la
         * chevaucher.
         *  (rework) — la reserve n'est faite QUE sur la colonne
         * effectivement triee (indicateur de tri visible ET pointant sur CETTE
         * section) : la loupe s'y decale a gauche de la fleche pour que les clics
         * sur la fleche/le texte atteignent QHeaderView (tri asc/desc). Sur toute
         * colonne non triee, pas de fleche a l'ecran donc pas de reserve : la
         * loupe se pose au bord droit. Le decalage suit l'indicateur de tri en
         * dynamique (repaint sur sortIndicatorChanged). */
        bool sortShown = isSortIndicatorShown() &&
                         sortIndicatorSection() == logicalIndex;
        int x;
        if (sortShown) {
            int margin =
                style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, this);
            int arrowReserve = margin * 2 + sectionRect.height() / 2;
            x = sectionRect.right() - arrowReserve - 3 - sz;
        } else {
            x = sectionRect.right() - 3 - sz;
        }
        /* Section trop etroite pour poser texte + icone sans chevauchement : on
         * exige une reserve de texte minimale a gauche de l'icone (sinon, sur une
         * colonne serree comme "ID", l'icone ecrase le libelle). En-dessous du
         * seuil : pas d'icone -> iconRect vide -> paintSection saute l'icone ET
         * le hit-test de mousePressEvent laisse le clic filer au tri (coherence
         * clic/rendu). Seuil = place texte mini + (deja soustrait : reserve
         * fleche + gap + icone). */
        const int minTextReserve = 30;
        if (x < sectionRect.left() + minTextReserve) return QRect();
        int y = sectionRect.top() + (sectionRect.height() - sz) / 2;
        return QRect(x, y, sz, sz);
    }

    /* Transforme le titre de la colonne col0 en champ de saisie inline.
     * forcedOp : quand >= 0, la frappe applique un filtre avec CET
     * operateur filtre* (choisi dans le menu contextuel « Autres filtres »),
     * court-circuitant ModeLoupe. Defaut -1 : la frappe suit ModeLoupe (clic
     * gauche loupe, comportement ). */
    void showInlineEditor(int col0, int forcedOp = kNoForcedFilterOp) {
        if (col0 < 0) return;
        m_forcedFilterOp = forcedOp;
        if (!m_editor) {
            m_editor = new QLineEdit(this);
            m_editor->setPlaceholderText(QString::fromUtf8("Rechercher\xe2\x80\xa6"));
            /*  — croix integree Qt : vider le champ retire le filtre
             * sans retaper. Le clic sur la croix emet textEdited("") -> onEditorText
             * -> clear_filter (mode Filtre) / re-tri neutre (mode Recherche). */
            m_editor->setClearButtonEnabled(true);
            m_editor->installEventFilter(this);
            OwlFilterHeaderView* self = this;
            QObject::connect(m_editor, &QLineEdit::textEdited, m_editor,
                [self](const QString& text) { self->onEditorText(text); });
        }
        m_editCol = col0;
        /*  (POINT B) — prereplir avec la valeur du filtre actif sur la
         * colonne (pont runtime owl_ui_table_user_get_filter_value). A la
         * re-ouverture d'une colonne deja filtree, la valeur reapparait et la
         * croix de reset (clear button) redevient visible (Qt ne l'affiche que
         * s'il y a du texte). Champ vide sinon (mode Recherche : pas de filtre
         * persistant). setText n'emet pas textEdited -> pas de re-application du
         * filtre deja pose.
         *  — exception : si le filtre actif est un OWL_FILTRE_IN
         * (checklist), sa valeur stockee est l'ensemble RC-joint des valeurs
         * cochees, pas un motif texte editable. L'afficher/editer comme du
         * texte n'a pas de sens (et ecraserait silencieusement l'IN par un
         * filtre texte decorrele). Dans ce cas l'editeur s'ouvre VIDE ; taper
         * un texte remplace proprement l'IN (dernier geste gagne, 1 filtre par
         * colonne). Les filtres texte gardent le prereplissage. */
        char fbuf[512];
        int ftype = owl_ui_table_user_get_filter_type(
            reinterpret_cast<OwlControl*>(m_view), col0);
        if (ftype != OWL_FILTRE_IN &&
            owl_ui_table_user_get_filter_value(
                reinterpret_cast<OwlControl*>(m_view), col0, fbuf,
                (int)sizeof fbuf) && fbuf[0] != '\0') {
            m_editor->setText(QString::fromUtf8(fbuf));
        } else {
            m_editor->clear();
        }
        /* show() AVANT positionEditor() : ce dernier court-circuite tant que
         * l'editeur n'est pas visible, sinon la geometrie n'est jamais posee
         * et le champ reste a sa taille/position par defaut (coin haut-gauche). */
        m_editor->show();
        positionEditor();
        m_editor->raise();
        m_editor->setFocus(Qt::MouseFocusReason);
    }

    /*  — menu contextuel de la loupe (clic droit). Reutilise la feuille
     * de style partagee des menus (owl_menu_stylesheet, ) pour un rendu
     * aligne sur les MenuContextuel du reste de l'appli (look plat OWL par
     * defaut : la Table genere ce menu elle-meme, il n'y a pas d'instance
     * MenuContextuel utilisateur d'ou tirer un style typé). Entrees backees par
     * l'existant :
     *   - Rechercher : ouvre la saisie inline (identique au clic gauche).
     *   - Autres filtres ▸ : sous-ensemble des operateurs filtre* ; le choix
     *     ouvre la saisie inline en forcant CET operateur (masque via le store).
     *   - Supprimer le filtre : retire le filtre de la colonne ; grise si aucun
     *     filtre actif. */
    void showContextMenu(int col, const QPoint& globalPos) {
        if (col < 0 || !m_view) return;
        OwlControl* tbl = reinterpret_cast<OwlControl*>(m_view);

        /*  — si un `MenuLoupe = MNU_X` est attache au Champ Table, on
         * rend CE menu utilisateur a la place du menu loupe integre. Le pont
         * runtime construit + affiche le menu (popup bloquant) et dispatche les
         * surSelection avec la colonne cliquee comme MoiMeme. Retourne 1 => menu
         * custom rendu, on n'affiche pas l'integre. 0 => pas d'attache, on garde
         * le comportement d'origine ci-dessous. */
        if (owl_ui_table_show_magnifier_menu(tbl, col)) return;

        QMenu menu(this);
        menu.setStyleSheet(owl_menu_stylesheet(nullptr));

        QAction* actRechercher =
            menu.addAction(owl_ui_label("Rechercher", "Search"));

        /*  — « Filtrer par valeurs ▸ » : sous-menu de checklist des
         * valeurs distinctes (façon tableur). Le contenu est un widget embarque
         * (QWidgetAction) avec cases a cocher live — le menu reste ouvert et le
         * filtre IN s'applique a chaque toggle. Disponible seulement si la
         * cardinalite est <= seuil (distinctes non vides) ; grise au-dela (pas
         * de checklist geante). */
        char dbuf[64];
        dbuf[0] = '\0';
        owl_ui_table_distinct_values(tbl, col, dbuf, (int)sizeof dbuf);
        QMenu* subVals =
            menu.addMenu(owl_ui_label("Filtrer par valeurs\xE2\x80\xA6",
                                      "Filter by values\xE2\x80\xA6"));
        subVals->setStyleSheet(owl_menu_stylesheet(nullptr));
        subVals->menuAction()->setEnabled(dbuf[0] != '\0');
        if (dbuf[0] != '\0') populateValuesSubmenu(subVals, col);

        QMenu* sub = menu.addMenu(owl_ui_label("Autres filtres", "More filters"));
        sub->setStyleSheet(owl_menu_stylesheet(nullptr));
        struct FilterOp { int type; const char* fr; const char* en; };
        /* Sous-ensemble raisonnable des 12 operateurs filtre* (les plus courants
         * sur du texte). Les operateurs de comparaison numerique (<, <=, >, >=)
         * restent hors de ce menu par choix UX. */
        static const FilterOp OPS[] = {
            { OWL_FILTRE_CONTIENT,        "Contient",        "Contains" },
            { OWL_FILTRE_EGAL,            "\xC3\x89gal",     "Equals" },
            { OWL_FILTRE_COMMENCE_PAR,    "Commence par",    "Starts with" },
            { OWL_FILTRE_TERMINE_PAR,     "Termine par",     "Ends with" },
            { OWL_FILTRE_DIFFERENT,       "Diff\xC3\xA9rent","Not equal" },
            { OWL_FILTRE_NE_CONTIENT_PAS, "Ne contient pas", "Does not contain" },
        };
        for (const FilterOp& op : OPS) {
            QAction* a = sub->addAction(owl_ui_label(op.fr, op.en));
            a->setData(op.type);
        }

        menu.addSeparator();
        QAction* actSupprimer =
            menu.addAction(owl_ui_label("Supprimer le filtre", "Remove filter"));
        actSupprimer->setEnabled(owl_ui_table_col_is_filtered(tbl, col) != 0);

        QAction* chosen = menu.exec(globalPos);
        if (!chosen) return;
        if (chosen == actRechercher) {
            showInlineEditor(col);
        } else if (chosen == actSupprimer) {
            owl_ui_table_user_clear_filter(tbl, col);
            viewport()->update();
        } else if (chosen->data().isValid()) {
            /* Operateur choisi (data porte la constante filtre*) : ouvre la
             * saisie inline en le forcant. */
            showInlineEditor(col, chosen->data().toInt());
        }
    }

    /*  — remplit le sous-menu « Filtrer par valeurs ▸ » avec un widget
     * embarque (QWidgetAction) : boutons Tout / Aucun en tete, puis les cases a
     * cocher des valeurs distinctes de la colonne dans un QScrollArea a hauteur
     * bornee (~12 lignes visibles, le reste scrolle). Peuplement via le pont
     * runtime owl_ui_table_distinct_values (gate par le seuil de cardinalite).
     * Chaque toggle applique LIVE un filtre IN {ensemble coche} via la MEME
     * fonction C que le hook de test (owl_ui_table_user_apply_filter_in) : un
     * seul systeme de filtrage ; tout coche => owl_ui_table_user_clear_filter
     * (aucune restriction). Un QWidgetAction garde le menu ouvert quand on
     * interagit avec le widget (les clics vont au widget, pas au menu) — la
     * table se remet a jour pendant que le menu reste ouvert. Plus d'OK/Annuler.
     * Tout / Aucun (re)cochent en bloc ET reappliquent live. */
    void populateValuesSubmenu(QMenu* sub, int col) {
        OwlControl* tbl = reinterpret_cast<OwlControl*>(m_view);
        char dbuf[65536];
        dbuf[0] = '\0';
        owl_ui_table_distinct_values(tbl, col, dbuf, (int)sizeof dbuf);
        QString all = QString::fromUtf8(dbuf);
        if (all.isEmpty()) return;  /* garde-fou : sous-menu deja grise en amont */
        const QStringList values = all.split(QStringLiteral("\r\n"));

        /* Etat de pre-cochage a la reouverture. « Un filtre est actif sur cette
         * colonne » se lit sur owl_ui_table_col_is_filtered, PAS sur la valeur :
         * un IN {} (ex-« Aucun ») a une valeur vide mais reste un filtre actif
         * (table vide) -> toutes les cases doivent rester DECOCHEES. Sans filtre
         * actif : tout coche par defaut. */
        char fbuf[65536];
        fbuf[0] = '\0';
        owl_ui_table_user_get_filter_value(tbl, col, fbuf, (int)sizeof fbuf);
        QStringList active;
        if (fbuf[0] != '\0')
            active = QString::fromUtf8(fbuf).split(QStringLiteral("\r\n"),
                                                   Qt::SkipEmptyParts);
        bool hasActive = owl_ui_table_col_is_filtered(tbl, col) != 0;

        QWidget* panel = new QWidget();
        QVBoxLayout* lay = new QVBoxLayout(panel);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(3);

        QHBoxLayout* topBtns = new QHBoxLayout();
        QPushButton* btnTout = new QPushButton(owl_ui_label("Tout", "All"), panel);
        QPushButton* btnAucun = new QPushButton(owl_ui_label("Aucun", "None"), panel);
        topBtns->addWidget(btnTout);
        topBtns->addWidget(btnAucun);
        topBtns->addStretch();
        lay->addLayout(topBtns);

        QScrollArea* scroll = new QScrollArea(panel);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QWidget* inner = new QWidget(scroll);
        QVBoxLayout* innerLay = new QVBoxLayout(inner);
        innerLay->setContentsMargins(0, 0, 0, 0);
        innerLay->setSpacing(2);

        /* Liste sur le tas pour que les lambdas la partagent ; liberee avec le
         * panel (duree de vie = le QWidgetAction, donc le menu). */
        QList<QCheckBox*>* boxes = new QList<QCheckBox*>();
        for (const QString& v : values) {
            QCheckBox* cb = new QCheckBox(v, inner);
            cb->setChecked(hasActive ? active.contains(v) : true);
            innerLay->addWidget(cb);
            *boxes << cb;
        }
        innerLay->addStretch();
        scroll->setWidget(inner);
        QObject::connect(panel, &QObject::destroyed, [boxes]() { delete boxes; });

        /* Hauteur bornee : ~12 lignes visibles au plus, le reste scrolle. */
        int rowH = boxes->isEmpty()
                       ? 22
                       : (*boxes)[0]->sizeHint().height() + innerLay->spacing();
        int visible = values.size() < 12 ? values.size() : 12;
        if (visible < 1) visible = 1;
        scroll->setMinimumWidth(180);
        scroll->setMaximumHeight(visible * rowH + 8);
        lay->addWidget(scroll);

        auto apply = [this, tbl, col, boxes, values]() {
            QStringList checked;
            for (int i = 0; i < boxes->size(); i++)
                if ((*boxes)[i]->isChecked()) checked << values[i];
            if (checked.size() == values.size()) {
                /* Tout coche = aucune restriction : on retire le filtre. */
                owl_ui_table_user_clear_filter(tbl, col);
            } else {
                /* Ensemble coche (y compris vide = ne rien afficher) en IN. */
                QByteArray joined = checked.join(QStringLiteral("\r\n")).toUtf8();
                owl_ui_table_user_apply_filter_in(tbl, col, joined.constData());
            }
            viewport()->update();
        };

        for (QCheckBox* cb : *boxes)
            QObject::connect(cb, &QCheckBox::toggled, panel,
                             [apply](bool) { apply(); });

        QObject::connect(btnTout, &QPushButton::clicked, panel, [boxes, apply]() {
            for (QCheckBox* cb : *boxes) {
                QSignalBlocker b(cb);
                cb->setChecked(true);
            }
            apply();
        });
        QObject::connect(btnAucun, &QPushButton::clicked, panel, [boxes, apply]() {
            for (QCheckBox* cb : *boxes) {
                QSignalBlocker b(cb);
                cb->setChecked(false);
            }
            apply();
        });

        QWidgetAction* wa = new QWidgetAction(sub);
        wa->setDefaultWidget(panel);
        sub->addAction(wa);
    }

    void onEditorText(const QString& text) {
        if (m_editCol < 0) return;
        OwlControl* tbl = reinterpret_cast<OwlControl*>(m_view);
        /*  — champ vide = AUCUN filtre, quel que soit le chemin
         * d'application. Traite en premier et inconditionnellement : la croix de
         * reset (clear button Qt) ou l'effacement manuel doit toujours de-masquer
         * la colonne et relancer le recompute Qt. Sans ce court-circuit, rouvrir
         * la loupe sur une colonne filtree via le menu remet m_forcedFilterOp au
         * sentinel : en mode Recherche (defaut), le clear retombait sur
         * applySearchPosition("") qui ne vide PAS le store $tblfilters -> filtre
         * fantome, lignes restees masquees. */
        if (text.isEmpty()) {
            owl_ui_table_user_clear_filter(tbl, m_editCol);
            viewport()->update();
            return;
        }
        /*  — un operateur force par le menu contextuel (« Autres
         * filtres ») prime sur ModeLoupe : la frappe masque les lignes hors
         * motif selon CET operateur (meme store $tblfilters). Comparaison au
         * sentinel kNoForcedFilterOp, PAS `>= 0` : les constantes filtre* sont
         * des sentinels bases sur INT32_MIN (donc negatives), un test de signe
         * retombait toujours sur le mode Recherche (surlignage, pas masquage). */
        if (m_forcedFilterOp != kNoForcedFilterOp) {
            QByteArray v = text.toUtf8();
            owl_ui_table_user_apply_filter(tbl, m_editCol,
                                           m_forcedFilterOp, v.constData());
            viewport()->update();
            return;
        }
        /*  — la loupe suit le mode configure (ModeLoupe). */
        int mode = owl_ui_table_user_mode_loupe(tbl);
        if (mode == OWL_FILTRE_CONTIENT) {
            /* Mode Filtre : masque les lignes hors motif via le store partage. */
            QByteArray v = text.toUtf8();
            owl_ui_table_user_apply_filter(tbl, m_editCol,
                                           OWL_FILTRE_CONTIENT, v.constData());
        } else {
            /* Mode Recherche (rechercheCommencePar, defaut) : trie + positionne. */
            applySearchPosition(text);
        }
        viewport()->update();
    }

    /* Mode Recherche : trie la table selon la colonne editee (ascendant) et
     * deplace la selection/current sur la 1re ligne dont la cellule « commence
     * par » le motif, sans masquer les autres (divergence assumee du filtre).
     * Opere sur le modele Qt (source de verite d'affichage) : le tri reordonne
     * les lignes, le type-ahead positionne dans cet ordre. */
    void applySearchPosition(const QString& text) {
        if (!m_view || m_editCol < 0) return;
        QAbstractItemModel* model = m_view->model();
        if (!model) return;
        m_view->sortByColumn(m_editCol, Qt::AscendingOrder);
        if (text.isEmpty()) return;
        int rows = model->rowCount();
        for (int r = 0; r < rows; r++) {
            QString cell = model->index(r, m_editCol)
                               .data(Qt::DisplayRole).toString();
            if (cell.startsWith(text, Qt::CaseInsensitive)) {
                QModelIndex target = model->index(r, m_editCol);
                m_view->selectRow(r);
                m_view->scrollTo(target);
                if (m_view->selectionModel()) {
                    m_view->selectionModel()->setCurrentIndex(
                        target, QItemSelectionModel::NoUpdate);
                }
                break;
            }
        }
    }

    /* Masque le champ inline et restaure le titre. Le filtre pose reste actif. */
    void hideEditor() {
        if (m_editor && m_editor->isVisible()) {
            m_editor->hide();
        }
        m_editCol = -1;
        viewport()->update();
    }

    void positionEditor() {
        if (!m_editor || !m_editor->isVisible() || m_editCol < 0) return;
        int vpos = sectionViewportPosition(m_editCol);
        int vw   = sectionSize(m_editCol);
        QRect sec(vpos, 0, vw, height());
        QRect ir = iconRect(sec, m_editCol);
        int left  = sec.left() + 3;
        int right = (ir.isEmpty() ? sec.right() : ir.left()) - 3;
        int w = right - left;
        if (w < 12) w = 12;
        m_editor->setGeometry(left, sec.top() + 2, w, sec.height() - 4);
    }

    QTableView* m_view;
    QLineEdit*  m_editor  = nullptr;
    int         m_editCol = -1;
    /*  — operateur filtre* force par le menu contextuel pour la
     * prochaine saisie inline. Sentinel dedie (hors plage filtre*, qui est
     * basee sur INT32_MIN) = pas de forcage -> la frappe suit ModeLoupe. */
    static constexpr int kNoForcedFilterOp = -1;
    int         m_forcedFilterOp = kNoForcedFilterOp;
};

/*  — repeint l'en-tete horizontal apres une ecriture runtime
 * TAB..ModeLoupe : l'icone (loupe / entonnoir) est calculee dans paintSection
 * depuis owl_ui_table_user_mode_loupe, dont la source (store d'instance) vient
 * d'etre mise a jour. No-op silencieux hors QTableView. */
extern "C" void owl_ui_table_header_refresh(OwlControl* table) {
    QTableView* view = qobject_cast<QTableView*>(
        reinterpret_cast<QWidget*>(table));
    if (!view || !view->horizontalHeader()) return;
    view->horizontalHeader()->viewport()->update();
}

/*  — pont TableSaisieLoupe -> saisie loupe visuelle. Mappe le widget
 * de rendu vers son header custom et delegue a openSearchProgrammatic. No-op
 * si le widget n'est pas une Table a header loupe. */
extern "C" void owl_ui_table_open_search(OwlControl* table, int col0,
                                         int forcedOp, const char* initialValue) {
    QTableView* view = qobject_cast<QTableView*>(
        reinterpret_cast<QWidget*>(table));
    if (!view) return;
    /* OwlFilterHeaderView n'a pas de Q_OBJECT (pas de qobject_cast) ; mais toute
     * Table creee par owl_ui_table_create installe ce header, et ce pont n'est
     * appele que pour une colonne de Table resolue (uici->decl kind TABLE). */
    OwlFilterHeaderView* hdr =
        static_cast<OwlFilterHeaderView*>(view->horizontalHeader());
    if (!hdr) return;
    hdr->openSearchProgrammatic(col0, forcedOp, initialValue);
}

/* Le QTableView porte son QStandardItemModel comme child QObject pour
 * que la destruction du widget nettoie automatiquement le modele. Les
 * accesseurs futurs (TableAjouteLigne, etc.) recupereront le modele
 * via tableView->model() puis qobject_cast<QStandardItemModel*>. */
extern "C" OwlControl* owl_ui_table_create(OwlWindow* parent,
                                           int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QTableView* view = new QTableView(central_of(parent));
    QStandardItemModel* model = new QStandardItemModel(view);
    view->setModel(model);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setAlternatingRowColors(true);

    /*  — header custom : icone loupe + recherche inline au clic. Pose
     * avant la config baseline (setHorizontalHeader remplace l'en-tete, la
     * config doit viser le nouveau). */
    view->setHorizontalHeader(new OwlFilterHeaderView(view));

    /* UX baseline du header :
     *   - Interactive  = resize a la souris (drag du separateur).
     *   - Movable      = drag pour reordonner les colonnes.
     *   - HighlightSections off : pas de gras sur l'en-tete au focus.
     *   - LastStretch  : la derniere colonne occupe l'espace restant
     *     (look propre meme si l'utilisateur n'a pas calibre les Largeur).
     *   - SortingEnabled : tri par clic sur l'en-tete (Phase 5). */
    QHeaderView* hh = view->horizontalHeader();
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setSectionsMovable(true);
    hh->setHighlightSections(false);
    hh->setStretchLastSection(true);
    /* Remplacer l'en-tete par un OwlFilterHeaderView (QHeaderView neuf, cf.
     * setHorizontalHeader plus haut) fait perdre le clickableSections=true que
     * QTableView pose sur ses en-tetes par defaut (QTableViewPrivate::init) :
     * un QHeaderView construit a la main naît clickableSections=false, et ni
     * setHorizontalHeader ni setSortingEnabled ne le retablissent. Sans clic
     * activable, QHeaderView::mouseReleaseEvent saute le bloc flipSortIndicator
     * -> sortIndicatorChanged -> tri (garde `if (d->clickableSections)`), donc
     * cliquer un titre ne trie plus rien. On le restaure explicitement pour que
     * le clic sur le texte / la fleche declenche le tri asc/desc (le clic sur
     * l'icone loupe reste consomme en amont dans mousePressEvent). */
    hh->setSectionsClickable(true);
    view->setSortingEnabled(true);

    view->verticalHeader()->setVisible(false);
    view->setGeometry(x, y, w, h);

    /* Phase 9 : context menu par defaut (Copier cellule/ligne, Export CSV).
     * S'installe directement, l'utilisateur peut le surcharger plus tard. */
    owl_ui_table_install_default_context_menu(reinterpret_cast<OwlControl*>(view));

    view->show();
    return reinterpret_cast<OwlControl*>(view);
}

extern "C" void owl_ui_table_add_column(OwlControl* table,
                                        const char* title_utf8,
                                        int col_type,
                                        int width) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int newCol = model->columnCount();
    model->insertColumn(newCol);
    QString title = QString::fromUtf8(title_utf8 ? title_utf8 : "");
    model->setHeaderData(newCol, Qt::Horizontal, title);
    if (width > 0) view->setColumnWidth(newCol, width);
    /* Stocke le col_type en property du header pour les phases ulterieures
     * (delegate par type, edition inline). */
    view->setProperty(QString("col_type_%1").arg(newCol).toUtf8(),
                      QVariant::fromValue(col_type));
}

/* Phase 2 — append/remove/clear/count. */

/*  Lot C — ré-applique à une cellule fraîche les couleurs de COLONNE
 * persistées sur le view (héritage des lignes ajoutées après l'affectation).
 * Défini plus bas (partage owl_table_bgr_to_qcolor). */
static void owl_table_apply_stored_col_colors(QTableView* view,
                                              QStandardItem* item, int col0);
/*  — predicat d'editabilite de colonne (property posee par
 * owl_ui_table_set_column_editable). Defini plus bas. */
static bool owl_table_column_is_editable(const QWidget* w, int col);

extern "C" void owl_ui_table_append_row(OwlControl* table,
                                        const char* const* cells_utf8,
                                        int n_cells) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int colCount = model->columnCount();
    int row = model->rowCount();
    model->insertRow(row);
    for (int c = 0; c < colCount; c++) {
        const char* txt = (c < n_cells && cells_utf8) ? cells_utf8[c] : nullptr;
        QStandardItem* item = new QStandardItem(QString::fromUtf8(txt ? txt : ""));
        /* Boolean column : checkbox via Qt::ItemIsUserCheckable. La string
         * "Vrai"/"Faux"/"true"/"false"/"1"/"0" determine l'etat initial. */
        QVariant prop = view->property(QString("col_type_%1").arg(c).toUtf8());
        int colType = prop.isValid() ? prop.toInt() : 0;
        if (colType == OWL_COL_TYPE_BOOLEEN) {
            item->setCheckable(true);
            QString s = item->text().trimmed().toLower();
            bool checked = (s == "vrai" || s == "true" || s == "1" || s == "oui");
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            item->setText(""); /* la check-state porte la valeur */
        }
        /*  Lot C — héritage de la couleur de colonne : une ligne ajoutée
         * APRÈS l'affectation `COL_X..Couleur` reprend la couleur persistée sur
         * le view. La cellule est neuve (jamais marquée cellule/ligne), donc rien
         * à préserver. */
        owl_table_apply_stored_col_colors(view, item, c);
        /*  — colonne Etat = AffichageSeulement : la cellule texte ne doit
         * pas s'ouvrir en edition au double-clic. On retire ItemIsEditable pour
         * cette colonne (les autres restent editables). Le circuit checkbox des
         * colonnes booleennes (ItemIsUserCheckable, pose plus haut) n'est pas
         * touche : son toggle reste garde par owl_table_checkbox_toggle_allowed. */
        if (!owl_table_column_is_editable(view, c))
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        model->setItem(row, c, item);
    }
}

extern "C" void owl_ui_table_remove_row(OwlControl* table, int row_1based) {
    if (!table || row_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = row_1based - 1;
    if (idx >= model->rowCount()) return;
    model->removeRow(idx);
}

extern "C" void owl_ui_table_clear(OwlControl* table) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    if (model->rowCount() > 0) model->removeRows(0, model->rowCount());
}

extern "C" int owl_ui_table_row_count(OwlControl* table) {
    if (!table) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 0;
    return model->rowCount();
}

extern "C" int owl_ui_table_column_count(OwlControl* table) {
    if (!table) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 0;
    return model->columnCount();
}

/* Masque/reaffiche une colonne. setColumnHidden gere nativement le
 * non-decalage des donnees : la colonne reste dans le modele (columnCount
 * inchange), seule sa vue est cachee, les indices restent coherents. */
extern "C" void owl_ui_table_set_column_hidden(OwlControl* table,
                                               int col_1based, int hidden) {
    if (!table || col_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = col_1based - 1;
    if (idx >= model->columnCount()) return;
    view->setColumnHidden(idx, hidden != 0);
}

extern "C" void owl_ui_table_set_row(OwlControl* table, int row_1based,
                                     const char* const* cells_utf8, int n_cells) {
    if (!table || row_1based <= 0 || !cells_utf8) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = row_1based - 1;
    if (idx >= model->rowCount()) return;
    int colCount = model->columnCount();
    int n = (n_cells < colCount) ? n_cells : colCount;
    for (int c = 0; c < n; c++) {
        const char* txt = cells_utf8[c];
        if (!txt) continue;
        QStandardItem* item = model->item(idx, c);
        if (!item) {
            item = new QStandardItem();
            model->setItem(idx, c, item);
            /*  Lot C — cellule créée à la volée (colonne jamais alimentée
             * sur cette ligne) : hérite la couleur de colonne persistée. */
            owl_table_apply_stored_col_colors(view, item, c);
        }
        QVariant prop = view->property(QString("col_type_%1").arg(c).toUtf8());
        int colType = prop.isValid() ? prop.toInt() : 0;
        if (colType == OWL_COL_TYPE_BOOLEEN) {
            item->setCheckable(true);
            QString s = QString::fromUtf8(txt).trimmed().toLower();
            bool checked = (s == "vrai" || s == "true" || s == "1" || s == "oui");
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            item->setText("");
        } else {
            item->setText(QString::fromUtf8(txt));
        }
    }
}

extern "C" void owl_ui_table_select_row(OwlControl* table, int row_1based) {
    if (!table || row_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = row_1based - 1;
    if (idx >= model->rowCount()) return;
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return;
    /* TableSelectPlus = AJOUTE a la selection (multi-select). */
    QModelIndex first = model->index(idx, 0);
    QModelIndex last = model->index(idx, model->columnCount() - 1);
    sm->select(QItemSelection(first, last),
               QItemSelectionModel::Select | QItemSelectionModel::Rows);
    view->scrollTo(first);
    sm->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
}

extern "C" void owl_ui_table_deselect_row(OwlControl* table, int row_1based) {
    if (!table || row_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    QItemSelectionModel* sm = view->selectionModel();
    if (!model || !sm) return;
    int idx = row_1based - 1;
    if (idx >= model->rowCount()) return;
    QModelIndex first = model->index(idx, 0);
    QModelIndex last = model->index(idx, model->columnCount() - 1);
    sm->select(QItemSelection(first, last),
               QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
}

extern "C" int owl_ui_table_get_selected_row_n(OwlControl* table, int rang_1based) {
    if (!table || rang_1based <= 0) return -1;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return -1;
    QModelIndexList sel = sm->selectedRows();
    if (rang_1based > sel.size()) return -1;
    /* Tri pour offrir un ordre stable (canon retourne dans l'ordre des lignes). */
    QList<int> rows;
    for (const QModelIndex& mi : sel) rows.append(mi.row());
    std::sort(rows.begin(), rows.end());
    return rows.at(rang_1based - 1) + 1;
}

extern "C" int owl_ui_table_get_selected_col(OwlControl* table, int rang_1based) {
    if (!table || rang_1based <= 0) return -1;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return -1;
    /* En mode Row-selection, "colonne courante" = colonne du current index. */
    QModelIndex cur = sm->currentIndex();
    if (!cur.isValid()) return -1;
    return cur.column() + 1;
}

extern "C" int owl_ui_table_selection_count(OwlControl* table) {
    if (!table) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return 0;
    return sm->selectedRows().size();
}

extern "C" void owl_ui_table_set_position(OwlControl* table, int row_1based,
                                          int make_visible_only) {
    if (!table || row_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = row_1based - 1;
    if (idx >= model->rowCount()) idx = model->rowCount() - 1;
    QModelIndex mi = model->index(idx, 0);
    /* taPremier = scroll EnsureVisible (top), taVisible = EnsureVisible only. */
    view->scrollTo(mi, make_visible_only ? QAbstractItemView::EnsureVisible
                                          : QAbstractItemView::PositionAtTop);
}

extern "C" int owl_ui_table_get_first_visible_row(OwlControl* table) {
    if (!table) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model || model->rowCount() == 0) return 0;
    /* Approximation : indexAt(0, header_height + 1). */
    QModelIndex mi = view->indexAt(QPoint(0, 0));
    return mi.isValid() ? mi.row() + 1 : 1;
}

extern "C" void owl_ui_table_hit_test(OwlControl* table, int x, int y,
                                       int* out_row, int* out_col) {
    if (out_row) *out_row = 0;
    if (out_col) *out_col = 0;
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QModelIndex mi = view->indexAt(QPoint(x, y));
    if (!mi.isValid()) return;
    if (out_row) *out_row = mi.row() + 1;
    if (out_col) *out_col = mi.column() + 1;
}

extern "C" const char* owl_ui_table_get_column_name(OwlControl* table, int col_1based) {
    static thread_local QByteArray g_colname_buf;
    g_colname_buf.clear();
    if (!table || col_1based <= 0) return "";
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model || col_1based > model->columnCount()) return "";
    g_colname_buf = model->headerData(col_1based - 1, Qt::Horizontal).toString().toUtf8();
    return g_colname_buf.constData();
}

/* ============================================================
 *  — style typé du Champ Table (styleTable) + override par colonne.
 *
 * Deux niveaux :
 *   - styleTable : UNE feuille QSS sur le QTableView + son QHeaderView.
 *   - styleColonne : override par colonne. Feuilles STATIQUES (titre en-tete,
 *     texte/fond des cellules, alternees) posees par role sur le modele ;
 *     etats DYNAMIQUES (selection, survol) via un QStyledItemDelegate installe
 *     sur la colonne (QSS global ne cible pas une colonne precise).
 * ============================================================ */

/* Empaquete un BGR canon (0x00BBGGRR) en QString CSS rgb(). */
static QString owl_table_rgb_css(int v) {
    return QString("rgb(%1, %2, %3)")
        .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF);
}

/*  — variante alpha-aware : byte-identique à owl_table_rgb_css quand
 * alpha >= 255 (opaque, cas dominant, snapshots inchangés) ; `rgba(...)` sinon
 * (couleur d'affichage translucide). Format espacé conservé (symétrie du
 * `rgb(r, g, b)` de la Table, distinct du owl_bg_css sans espace). */
static QString owl_table_rgb_css_a(int v, int alpha) {
    if (alpha >= 255) return owl_table_rgb_css(v);
    return QString("rgba(%1, %2, %3, %4)")
        .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF)
        .arg(alpha / 255.0, 0, 'g', 4);
}

/* Ligne survolee courante par view (source de verite PARTAGEE entre le
 * delegate global et les delegates colonne). Le survol canon est row-wide :
 * toute la ligne sous le curseur se surligne, pas juste la cellule. QSS
 * ::item:hover ne cible que la cellule -> on peint le survol en delegate et
 * on teste index.row() == hoveredRow.
 *
 * Nettoyage view-keyed calque sur g_owl_timers : destroyed() du view retire
 * son entree pour eviter une fuite cumulative de la QHash. */
static QHash<QObject*, int> g_owl_table_hover_row;

static int owl_table_hovered_row(const QObject* view) {
    return g_owl_table_hover_row.value(const_cast<QObject*>(view), -1);
}

/* QColor <- BGR canon. */
static QColor owl_table_bgr_to_qcolor(int v) {
    return QColor(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
}

/*  Lot C — mise en forme par COLONNE.
 *
 * Une cellule (QStandardItem) ne porte qu'un seul pinceau de fond et un seul de
 * texte : impossible d'y empiler cellule + ligne + colonne. La couleur de
 * colonne est le niveau le PLUS BAS (canon cellule > ligne > colonne > défaut),
 * donc elle ne doit jamais écraser une couleur posée au niveau cellule/ligne.
 * On marque chaque canal (fond/texte) qu'une couleur cellule/ligne a peint via
 * un rôle de donnée dédié ; l'application colonne saute les cellules marquées.
 *
 * Les lignes AJOUTÉES APRÈS l'affectation héritent : la couleur de colonne est
 * persistée en propriété du view (owl_table_colfmt_prop), et append_row/set_row
 * la ré-appliquent à chaque nouvelle cellule (jamais marquée à sa création). */
static const int OWL_TABLE_ROLE_BG_EXPLICIT = Qt::UserRole + 4001;
static const int OWL_TABLE_ROLE_FG_EXPLICIT = Qt::UserRole + 4002;

/* Nom de propriété view portant la couleur de colonne : canal (fond/texte),
 * composante (valeur BGR / alpha) et index de colonne 0-based. */
static QByteArray owl_table_colfmt_prop(bool is_bg, bool is_alpha, int col0) {
    return QString("owl_colfmt_%1%2_%3")
        .arg(is_bg ? "bg" : "fg")
        .arg(is_alpha ? "a" : "v")
        .arg(col0)
        .toUtf8();
}

/* Applique une couleur de NIVEAU COLONNE à une cellule, sauf si ce canal porte
 * déjà une couleur explicite cellule/ligne (priorité canon préservée). */
static void owl_table_apply_col_channel(QStandardItem* item, bool is_bg,
                                        const QColor& c) {
    int role = is_bg ? OWL_TABLE_ROLE_BG_EXPLICIT : OWL_TABLE_ROLE_FG_EXPLICIT;
    if (item->data(role).toBool()) return;
    if (is_bg) item->setBackground(QBrush(c));
    else       item->setForeground(QBrush(c));
}

static void owl_table_apply_stored_col_colors(QTableView* view,
                                              QStandardItem* item, int col0) {
    for (int ch = 0; ch < 2; ch++) {
        bool is_bg = (ch == 0);
        QVariant pv = view->property(
            owl_table_colfmt_prop(is_bg, false, col0).constData());
        if (!pv.isValid()) continue;
        int bgr = pv.toInt();
        if (bgr < 0) continue;
        QColor c = owl_table_bgr_to_qcolor(bgr);
        int a = view->property(
            owl_table_colfmt_prop(is_bg, true, col0).constData()).toInt();
        if (a >= 0 && a <= 255) c.setAlpha(a);
        owl_table_apply_col_channel(item, is_bg, c);
    }
}

/* Fragment police + couleurs d'une brique texte -> corps QSS. */
static QString owl_table_text_body(const OwlTextBrick& b) {
    QString body;
    if (b.fg > 0)  body += "color: " + owl_table_rgb_css_a(b.fg, b.fg_alpha) + "; ";
    if (b.bg >= 0) body += "background-color: " + owl_table_rgb_css_a(b.bg, b.bg_alpha) + "; ";
    if (b.font_family && *b.font_family)
        body += QString("font-family: \"%1\"; ").arg(QString::fromUtf8(b.font_family));
    if (b.font_size > 0) body += QString("font-size: %1pt; ").arg(b.font_size);
    if (b.bold == 1)        body += "font-weight: bold; ";
    else if (b.bold == 0)   body += "font-weight: normal; ";
    if (b.italic == 1)      body += "font-style: italic; ";
    else if (b.italic == 0) body += "font-style: normal; ";
    return body;
}

static const char* cadre_line_style_to_css(int line_style);   /* defini plus bas */

/* Toggle checkbox (colonne booleenne) autorise pour cette cellule ? Defini plus
 * bas (owl_ui_table_set_etat_initial). Les 2 delegates l'appellent depuis
 * editorEvent pour figer les cases quand la table/colonne n'est pas editable. */
static bool owl_table_checkbox_toggle_allowed(const QWidget* view,
                                              const QModelIndex& index);

static QString owl_table_border_css(const OwlCadreSpec* c) {
    if (!c) return QString();
    /*  — border-color alpha-aware : rgba(...) si la Couleur de la bordure
     * est translucide, rgb(...) byte-identique sinon (format spacé de la Table).
     *  — corps de bordure partagé : per-side si un côté porte un trait
     * propre, sinon shorthand global byte-identique. */
    QString b = cadre_border_body(c, owl_table_rgb_css_a, nullptr);
    /*  — arrondi émis même sans bordure (Epaisseur=0, Arrondi>0) :
     * ne plus jeter le radius avec le return anticipé du cas thickness<=0. */
    if (c->corner_radius > 0)
        b += QString("border-radius: %1px; ").arg(c->corner_radius);
    return b;
}

/* eventFilter installe sur le viewport du view : capte Leave pour remettre le
 * survol a -1 (le curseur a quitte la table -> plus aucune ligne surlignee).
 * L'entree dans une cellule passe par le signal entered() (cf. set_style). */
class OwlTableHoverFilter : public QObject {
public:
    QTableView* view;
    OwlTableHoverFilter(QTableView* v, QObject* parent)
        : QObject(parent), view(v) {}

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::Leave) {
            if (owl_table_hovered_row(view) != -1) {
                g_owl_table_hover_row.insert(view, -1);
                view->viewport()->update();
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

/* Delegate GLOBAL du view : peint TOUTES les cellules non-overridees par une
 * colonne. Couvre la base (LibelléLignes + alternees paires/impaires), la
 * selection (LigneSélectionnée, row-wide via SelectRows) et le survol
 * (LigneSurvolée) qui est ROW-WIDE : chaque cellule teste index.row() ==
 * hoveredRow(view), donc toute la ligne se surligne d'un coup. */
class OwlTableStyleDelegate : public QStyledItemDelegate {
public:
    OwlTableStyleSpec spec;

    OwlTableStyleDelegate(const OwlTableStyleSpec& s, QObject* parent)
        : QStyledItemDelegate(parent), spec(s) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        bool selected = (opt.state & QStyle::State_Selected);
        bool hovered  = (index.row() == owl_table_hovered_row(
                             const_cast<QWidget*>(opt.widget)));

        OwlTextBrick brick;
        if (selected) {
            brick = spec.selection;
        } else if (hovered) {
            brick = spec.survol;
        } else {
            /* Repos : texte des cellules + fond alterne selon parite. */
            brick = spec.lignes;
            bool even = (index.row() % 2 == 0);
            int altBg = even ? spec.impaires_bg : spec.paires_bg;
            int altA  = even ? spec.impaires_bg_alpha : spec.paires_bg_alpha;
            if (altBg >= 0) { brick.bg = altBg; brick.bg_alpha = altA; }
        }

        bool active = (brick.fg >= 0 || brick.bg >= 0 || brick.font_size > 0
                       || (brick.font_family && *brick.font_family)
                       || brick.bold >= 0 || brick.italic >= 0
                       || selected);
        if (!active) { QStyledItemDelegate::paint(painter, option, index); return; }

        /* Fond peint EXPLICITEMENT via fillRect : des qu'une stylesheet est
         * active sur le view (header/gridline/cadre), QStyleSheetStyle IGNORE
         * opt.backgroundBrush. On court-circuite en peignant le fond nous-memes.
         * Survol row-wide et alternees passent ainsi malgre le QSS. */
        if (brick.bg >= 0) {
            /*  — fillRect en SourceOver (défaut) : une couleur d'affichage
             * translucide (alpha < 255) se compose sur le fond déjà peint (backdrop
             * CadreExtérieur / AlternateBase). Opaque -> setAlpha(255) sans effet. */
            QColor bgc = owl_table_bgr_to_qcolor(brick.bg);
            bgc.setAlpha(brick.bg_alpha);
            painter->fillRect(opt.rect, bgc);
        } else if (selected) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
        }

        /* Prepare l'option pour le rendu du PREMIER PLAN seul (texte, case a
         * cocher, elision, focus). Fond deja peint -> on empeche le base paint
         * de le repeindre : NoBrush + retrait de State_Selected (sinon Qt
         * repeindrait le Highlight par-dessus notre fillRect). */
        opt.backgroundBrush = QBrush(Qt::NoBrush);
        opt.state &= ~QStyle::State_Selected;

        /* Couleur du texte : brique prioritaire, sinon couleur systeme selon
         * l'etat de selection. On aligne Text et HighlightedText pour que le
         * rendu soit stable quel que soit l'etat.  — alpha du texte
         * appliqué uniquement sur une couleur explicite (la palette reste opaque). */
        QColor fg;
        if (brick.fg > 0) {
            fg = owl_table_bgr_to_qcolor(brick.fg);
            fg.setAlpha(brick.fg_alpha);
        } else {
            fg = opt.palette.color(selected ? QPalette::HighlightedText
                                            : QPalette::Text);
        }
        opt.palette.setColor(QPalette::Text, fg);
        opt.palette.setColor(QPalette::HighlightedText, fg);

        /* Police. */
        QFont f = opt.font;
        if (brick.font_family && *brick.font_family)
            f.setFamily(QString::fromUtf8(brick.font_family));
        if (brick.font_size > 0) f.setPointSize(brick.font_size);
        if (brick.bold >= 0)   f.setBold(brick.bold == 1);
        if (brick.italic >= 0) f.setItalic(brick.italic == 1);
        opt.font = f;
        opt.fontMetrics = QFontMetrics(f); /* elision correcte a la nouvelle police */

        QStyledItemDelegate::paint(painter, opt, index);
    }

    /* Fige le toggle des cases a cocher quand la table/colonne n'est pas editable.
     * La checkbox reste DESSINEE (paint inchange) et son etat visible ; seul le
     * clic est neutralise. On intercepte AVANT la base : elle basculerait
     * Qt::ItemIsUserCheckable hors du circuit d'edition (edit triggers ignores). */
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if ((index.flags() & Qt::ItemIsUserCheckable)
            && !owl_table_checkbox_toggle_allowed(option.widget, index)) {
            return false;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

/* Active le tracking du survol row-wide sur un view (idempotent). Appele par
 * styleTable ET styleColonne : le survol doit marcher meme si seul un style
 * colonne est pose. mouseTracking pour que entered() fire sans clic, signal
 * entered() -> ligne courante, eventFilter Leave -> -1, destroyed -> cleanup
 * de la QHash view-keyed. */
static void owl_table_install_hover_tracking(QTableView* view) {
    /* entered() ne fire au survol sans clic que si le VIEWPORT a le
     * mouse-tracking (pas le view lui-meme). Idempotent -> hors guard. */
    view->viewport()->setMouseTracking(true);

    /* La connexion entered() + filtre + cleanup destroyed s'installent une
     * seule fois (set_style peut etre rappele). Pas de Qt::UniqueConnection :
     * Qt le refuse sur une lambda (exige un pointeur-vers-membre) et la
     * connexion echouerait silencieusement. Le garde "once" via la propriete
     * owl_hover_filter suffit. */
    if (!view->property("owl_hover_filter").toBool()) {
        QObject::connect(view, &QAbstractItemView::entered, view,
                         [view](const QModelIndex& idx) {
            int row = idx.isValid() ? idx.row() : -1;
            if (owl_table_hovered_row(view) != row) {
                g_owl_table_hover_row.insert(view, row);
                view->viewport()->update();
            }
        });

        OwlTableHoverFilter* hf = new OwlTableHoverFilter(view, view);
        view->viewport()->installEventFilter(hf);
        view->setProperty("owl_hover_filter", true);
        QObject::connect(view, &QObject::destroyed, [view]() {
            g_owl_table_hover_row.remove(view);
        });
    }
}

/*  — couleur de texte / de fond d'une cellule, ou d'une ligne entiere
 * (col_1based <= 0). Les roles Foreground/Background du QStandardItem sont le
 * seul niveau assez fin : une feuille QSS ne descend pas sous la colonne, et un
 * delegate par cellule demanderait un modele de style parallele.
 *
 * Voisine du bloc style (et non du bloc API Table plus haut) pour partager le
 * decodage BGR de owl_table_bgr_to_qcolor : dupliquer le decalage de bits
 * finirait par diverger.
 *
 * Un item peut manquer (colonne jamais alimentee sur cette ligne) : on le cree
 * vide plutot que d'abandonner, sinon colorer une ligne entiere laisserait des
 * trous la ou l'utilisateur a passe moins de valeurs que de colonnes. */
extern "C" void owl_ui_table_set_cell_color(OwlControl* table, int row_1based,
                                            int col_1based, int is_background,
                                            int bgr, int alpha) {
    if (!table || row_1based <= 0 || bgr < 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int r = row_1based - 1;
    if (r >= model->rowCount()) return;
    int colCount = model->columnCount();
    if (colCount <= 0) return;
    int first = (col_1based > 0) ? col_1based - 1 : 0;
    int last  = (col_1based > 0) ? col_1based - 1 : colCount - 1;
    if (first < 0 || last >= colCount) return;

    QColor c = owl_table_bgr_to_qcolor(bgr);
    if (alpha >= 0 && alpha <= 255) c.setAlpha(alpha);
    for (int cc = first; cc <= last; cc++) {
        QStandardItem* item = model->item(r, cc);
        if (!item) {
            item = new QStandardItem();
            model->setItem(r, cc, item);
        }
        /*  Lot C — marque ce canal comme explicitement peint au niveau
         * cellule/ligne : une couleur de colonne (niveau plus bas) ne l'écrasera
         * pas, ni à l'application immédiate ni sur une ligne future. */
        if (is_background) {
            item->setBackground(QBrush(c));
            item->setData(true, OWL_TABLE_ROLE_BG_EXPLICIT);
        } else {
            item->setForeground(QBrush(c));
            item->setData(true, OWL_TABLE_ROLE_FG_EXPLICIT);
        }
    }
    view->viewport()->update();
}

/*  Lot C — couleur de texte (is_background=0) ou de fond
 * (is_background=1) appliquée à TOUTE la colonne col_1based : lignes présentes
 * ET futures. Niveau le plus bas (cellule > ligne > colonne > défaut) : ne
 * réécrit pas une cellule déjà peinte au niveau cellule/ligne. La couleur est
 * persistée sur le view pour que append_row/set_row la propagent aux lignes
 * ajoutées ensuite (cœur du lot). */
extern "C" void owl_ui_table_set_column_color(OwlControl* table, int col_1based,
                                              int is_background, int bgr,
                                              int alpha) {
    if (!table || col_1based <= 0 || bgr < 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int c = col_1based - 1;
    if (c >= model->columnCount()) return;

    bool is_bg = (is_background != 0);
    int a = (alpha >= 0 && alpha <= 255) ? alpha : 255;
    /* Persistance pour les lignes futures. */
    view->setProperty(owl_table_colfmt_prop(is_bg, false, c).constData(), bgr);
    view->setProperty(owl_table_colfmt_prop(is_bg, true, c).constData(), a);

    QColor col = owl_table_bgr_to_qcolor(bgr);
    col.setAlpha(a);
    for (int r = 0; r < model->rowCount(); r++) {
        QStandardItem* item = model->item(r, c);
        if (!item) {
            item = new QStandardItem();
            model->setItem(r, c, item);
        }
        owl_table_apply_col_channel(item, is_bg, col);
    }
    view->viewport()->update();
}

extern "C" void owl_ui_table_set_style(OwlControl* table,
                                       const OwlTableStyleSpec* s) {
    if (!table || !s) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);

    /* Les lignes alternees restent actives : LignesPaires/LignesImpaires
     * s'expriment via QSS QTableView { background / alternate-background }. */
    view->setAlternatingRowColors(true);

    QString css;

    /* --- CadreExtérieur : fond du cadre/zone vide + bordure du QTableView.
     * Le fond des LIGNES (paires/impaires) n'est PLUS dans le QSS : le delegate
     * en est l'unique peintre (fillRect par parite), sinon QSS l'ecrase. On ne
     * garde ici que ce que le delegate ne peint pas. --- */
    {
        QString body;
        if (s->ext_bg >= 0)
            body += "background-color: " + owl_table_rgb_css_a(s->ext_bg, s->ext_bg_alpha) + "; ";
        /*  — gridline-color alpha-aware : un liseré translucide se compose
         * sur le fond des cellules déjà peint (rgba). Opaque => byte-identique. */
        if (s->quadrillage >= 0)
            body += "gridline-color: " + owl_table_rgb_css_a(s->quadrillage, s->quadrillage_alpha) + "; ";
        body += owl_table_border_css(s->ext_border);
        if (!body.isEmpty())
            css += "QTableView { " + body + "} ";
    }

    /* --- TitreColonne : QHeaderView::section (texte + fond des en-tetes). --- */
    {
        QString body = owl_table_text_body(s->titre);
        if (!body.isEmpty())
            css += "QHeaderView::section { " + body + "} ";
    }

    /* Le texte/fond des cellules (LibelléLignes), les alternees, la selection
     * (LigneSélectionnée) et le survol (LigneSurvolée) sont desormais peints
     * par le delegate global : QSS ::item:hover ne cible qu'une cellule alors
     * que le survol canon est ROW-WIDE. Le delegate lit hoveredRow(view) et
     * surligne toute la ligne. On ne laisse en QSS que ce que le delegate ne
     * peint pas (header, gridline, cadre). */
    view->setStyleSheet(css);
    if (s->quadrillage >= 0) view->setShowGrid(true);

    /* Delegate global : re-cree a chaque appel (spec peut avoir change).
     * Parente au view -> detruit avec lui, pas de dangling. */
    OwlTableStyleDelegate* d = new OwlTableStyleDelegate(*s, view);
    view->setItemDelegate(d);

    owl_table_install_hover_tracking(view);
}

/*  — edit-triggers coherents avec l'EtatInitial d'une Table.
 * 2=Selection, 3=NiSaisieNiSelection => non editable (NoEditTriggers) ;
 * 0=EnSaisie, 1=Grise, absent => triggers d'edition standards. Sert au retour
 * ..Etat = Actif pour restaurer le mode d'edition anterieur sans le clobber. */
static QAbstractItemView::EditTriggers owl_table_triggers_for_etat_initial(int etat) {
    if (etat == 2 || etat == 3) return QAbstractItemView::NoEditTriggers;
    return QAbstractItemView::DoubleClicked
         | QAbstractItemView::EditKeyPressed
         | QAbstractItemView::AnyKeyPressed;
}

extern "C" void owl_ui_table_set_etat_initial(OwlControl* table, int etat) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    /*  — memorise l'EtatInitial pour que ..Etat = Actif restaure le bon
     * mode d'edition (cf. owl_ui_control_set_etat, branche OWL_ETAT_ACTIF). */
    view->setProperty("owl_etat_initial", etat);
    /* Le flag "owl_table_editable" pilote le toggle des cases a cocher (colonnes
     * booleennes). Les edit triggers ne suffisent pas : QStyledItemDelegate
     * bascule Qt::ItemIsUserCheckable dans editorEvent, hors du circuit d'edition.
     * Seul EnSaisie autorise le toggle ; Selection / NiNi / Grise le figent. */
    switch (etat) {
        case 0: /* EnSaisie — editable. */
            view->setEnabled(true);
            view->setEditTriggers(QAbstractItemView::DoubleClicked
                                  | QAbstractItemView::EditKeyPressed
                                  | QAbstractItemView::AnyKeyPressed);
            view->setProperty("owl_table_editable", true);
            break;
        case 1: /* Grisé. */
            view->setEnabled(false);
            view->setProperty("owl_table_editable", false);
            break;
        case 2: /* Sélection — non editable mais selectionnable. */
            view->setEnabled(true);
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            view->setProperty("owl_table_editable", false);
            break;
        case 3: /* NiSaisieNiSélection. */
            view->setEnabled(true);
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            view->setSelectionMode(QAbstractItemView::NoSelection);
            view->setProperty("owl_table_editable", false);
            break;
        default:
            break;
    }
}

/* Etat editable d'une colonne (AffichageSeulement) : property par colonne sur le
 * view. Absence de property = colonne editable (defaut). col_1based converti en
 * index de property. */
extern "C" void owl_ui_table_set_column_editable(OwlControl* table, int col_1based,
                                                 int editable) {
    if (!table || col_1based <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    int col0 = col_1based - 1;
    view->setProperty(QString("owl_col_editable_%1").arg(col0).toUtf8(),
                      editable != 0);
    /*  — application retroactive aux items DEJA presents (une colonne
     * peut passer RO apres l'ajout de lignes). Ne touche que ItemIsEditable :
     * ItemIsUserCheckable (colonnes booleennes) reste intact. */
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model || col0 >= model->columnCount()) return;
    for (int r = 0; r < model->rowCount(); r++) {
        QStandardItem* item = model->item(r, col0);
        if (!item) continue;
        if (editable)
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        else
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
}

/* Predicats de lecture partages par les 2 delegates. Le view est la source de
 * verite (l'etat peut etre pose apres le style, les delegates sont recrees). */
static bool owl_table_is_editable(const QWidget* w) {
    if (!w) return true;
    QVariant v = w->property("owl_table_editable");
    /* Property absente = EnSaisie par defaut (table editable). */
    return v.isValid() ? v.toBool() : true;
}

static bool owl_table_column_is_editable(const QWidget* w, int col) {
    if (!w || col < 0) return true;
    QVariant v = w->property(QString("owl_col_editable_%1").arg(col).toUtf8());
    return v.isValid() ? v.toBool() : true;
}

/* Toggle checkbox autorise pour cette cellule ? Table EnSaisie ET colonne Active. */
static bool owl_table_checkbox_toggle_allowed(const QWidget* view,
                                              const QModelIndex& index) {
    return owl_table_is_editable(view)
        && owl_table_column_is_editable(view, index.column());
}

/*  — mesure test-only (hook __DebugTableCelluleEditable) : lit l'etat
 * Qt REEL d'une cellule (ligne/colonne 1-based). Vrai SSI un double-clic
 * ouvrirait effectivement un editeur : l'item porte Qt::ItemIsEditable ET la vue
 * n'est pas en NoEditTriggers. Garde-fous : widget non-Table, modele absent,
 * indices hors bornes -> Faux. */
extern "C" int owl_ui_table_cell_editable(OwlControl* table, int row_1based,
                                          int col_1based) {
    if (!table || row_1based <= 0 || col_1based <= 0) return 0;
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return 0;
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 0;
    int r = row_1based - 1, c = col_1based - 1;
    if (r >= model->rowCount() || c >= model->columnCount()) return 0;
    QStandardItem* item = model->item(r, c);
    if (!item) return 0;
    bool itemEditable = (item->flags() & Qt::ItemIsEditable) != 0;
    bool triggersOn   = view->editTriggers() != QAbstractItemView::NoEditTriggers;
    return (itemEditable && triggersOn) ? 1 : 0;
}

extern "C" void owl_ui_table_set_row_height(OwlControl* table, int height_px) {
    if (!table || height_px <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    view->verticalHeader()->setDefaultSectionSize(height_px);
}

/* Hauteur de la bande de titre. Trois regimes :
 *   px == 0  -> bande masquee (la place revient au viewport : Qt reflue
 *               l'espace du header cache vers la zone de donnees).
 *   px  > 0  -> hauteur imposee.
 *   px  < 0  -> retour a la hauteur naturelle (contraintes relachees).
 * setFixedHeight applique immediatement : le getter voit la valeur sans
 * attendre un tour de boucle d'evenements. */
extern "C" void owl_ui_table_set_title_height(OwlControl* table, int px) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QHeaderView* hh = view->horizontalHeader();
    if (px == 0) {
        hh->setVisible(false);
        return;
    }
    if (px > 0) {
        hh->setVisible(true);
        hh->setFixedHeight(px);
        return;
    }
    hh->setVisible(true);
    hh->setMinimumHeight(0);
    hh->setMaximumHeight(QWIDGETSIZE_MAX);
    hh->resize(hh->width(), hh->sizeHint().height());
}

extern "C" int owl_ui_table_get_title_height(OwlControl* table) {
    if (!table) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QHeaderView* hh = view->horizontalHeader();
    /* Un widget masque garde sa derniere geometrie : la hauteur rendue d'un
     * en-tete masque est 0, pas son ancienne taille. */
    if (hh->isHidden()) return 0;
    return hh->height();
}

extern "C" void owl_ui_table_set_stretch_column(OwlControl* table, int col_1based) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QHeaderView* hh = view->horizontalHeader();
    int idx = col_1based - 1;
    if (idx < 0 || idx >= hh->count()) return;
    /* La colonne ciblee occupe l'espace restant ; on desactive le stretch de
     * la derniere colonne (defaut pose a la creation) pour eviter le conflit. */
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(idx, QHeaderView::Stretch);
}

/*  — POINT D'EXTENSION du framework type-driven de colonne. Applique à
 * l'éditeur transitoire d'une cellule le comportement d'édition dérivé de sa
 * colonne, selon une précédence à deux étages :
 *
 *  1. OVERRIDE bas-niveau — un masque brut DÉCLARÉ (`col_mask_<col0>`,  :
 *     `MasqueSaisie = "99-99-9999"`) prime : masque seul, PAS de validateur de
 *     bornes. C'est un contrôle de FORMAT explicite voulu par l'auteur.
 *  2. DISPATCH sur le type de colonne (`col_type_<col0>`, posé par
 *     owl_ui_table_add_column). Chaque type câblé pose masque + validation via
 *     la MÊME machinerie que le champ Saisie (pas de 2e implémentation). Le
 *     format effectif est pré-résolu côté core (default/projet) et stocké
 *     per-colonne (`col_typeformat_<col0>` + masque pré-dérivé
 *     `col_typemask_<col0>`) — cf. owl_ui_table_set_column_format.
 *
 * AJOUTER UN TYPE FUTUR (Numérique / Monétaire, cf. suite du chantier) = 1 branche
 * `case` localisée ici + son setter de stockage côté core.
 * La mécanique commune (résolution du delegate, création de l'éditeur, ordre de
 * précédence) reste inchangée — même esprit que le récepteur data-driven
 * d'. Colonne sans masque ni type câblé -> éditeur laissé tel quel. */
static void owl_table_apply_col_editor_behavior(QWidget* editor,
                                                const QObject* view, int col0) {
    if (!editor || !view || col0 < 0) return;

    /* Étage 1 : masque brut déclaré = override bas-niveau (format seul). */
    QVariant mv = view->property(QString("col_mask_%1").arg(col0).toUtf8());
    if (mv.isValid() && !mv.toString().isEmpty()) {
        QByteArray wl = mv.toString().toUtf8();
        owl_ui_lineedit_set_mask(reinterpret_cast<OwlControl*>(editor),
                                 wl.constData());
        return;
    }

    /* Étage 2 : dispatch type-driven. */
    QVariant tv = view->property(QString("col_type_%1").arg(col0).toUtf8());
    if (!tv.isValid()) return;
    switch (tv.toInt()) {
    case OWL_COL_TYPE_DATE: {
        /* Date sans masque brut : piloté par le format (masque de frappe +
         * validateur de plages), miroir exact du champ Saisie Date. */
        OwlControl* ed = reinterpret_cast<OwlControl*>(editor);
        QVariant qm = view->property(QString("col_typemask_%1").arg(col0).toUtf8());
        if (qm.isValid() && !qm.toString().isEmpty())
            owl_ui_lineedit_set_mask(ed, qm.toString().toUtf8().constData());
        QVariant fm = view->property(QString("col_typeformat_%1").arg(col0).toUtf8());
        QByteArray fmt = (fm.isValid() && !fm.toString().isEmpty())
                             ? fm.toString().toUtf8()
                             : QByteArray("JJ/MM/AAAA");
        owl_ui_lineedit_set_date_time_validator(ed, fmt.constData());
        break;
    }
    case OWL_COL_TYPE_HEURE: {
        /* Heure sans masque brut : format défaut/projet ("HH:MI") -> masque de
         * frappe + validateur de plages HORAIRE (HH 00-23, MI 00-59). Même moteur
         * de validation que Date (owl_dt_parse_range_layout couvre les deux). */
        OwlControl* ed = reinterpret_cast<OwlControl*>(editor);
        QVariant qm = view->property(QString("col_typemask_%1").arg(col0).toUtf8());
        if (qm.isValid() && !qm.toString().isEmpty())
            owl_ui_lineedit_set_mask(ed, qm.toString().toUtf8().constData());
        QVariant fm = view->property(QString("col_typeformat_%1").arg(col0).toUtf8());
        QByteArray fmt = (fm.isValid() && !fm.toString().isEmpty())
                             ? fm.toString().toUtf8()
                             : QByteArray("HH:MI");
        owl_ui_lineedit_set_date_time_validator(ed, fmt.constData());
        break;
    }
    case OWL_COL_TYPE_DUREE: {
        /* Durée sans masque brut : format défaut/projet ("HH:MI:SS") -> masque +
         * validateur DÉDIÉ durée (HH non plafonné à 23, MI/SS 00-59), miroir du
         * champ Saisie Durée. NE PAS réutiliser le validateur horaire. */
        OwlControl* ed = reinterpret_cast<OwlControl*>(editor);
        QVariant qm = view->property(QString("col_typemask_%1").arg(col0).toUtf8());
        if (qm.isValid() && !qm.toString().isEmpty())
            owl_ui_lineedit_set_mask(ed, qm.toString().toUtf8().constData());
        QVariant fm = view->property(QString("col_typeformat_%1").arg(col0).toUtf8());
        QByteArray fmt = (fm.isValid() && !fm.toString().isEmpty())
                             ? fm.toString().toUtf8()
                             : QByteArray("HH:MI:SS");
        owl_ui_lineedit_set_duree_validator(ed, fmt.constData());
        break;
    }
    case OWL_COL_TYPE_NUMERIQUE:
    case OWL_COL_TYPE_MONETAIRE: {
        /* Numérique/Monétaire sans masque brut : PAS de masque à positions fixes
         * (nombres = longueur variable). Validateur numérique LENIENT, mêmes
         * classes de caractères que le champ Saisie numérique/monétaire
         * : rejette lettres/paste, accepte chiffres + séparateurs + signe. Le
         * format d'AFFICHAGE (séparateurs/symbole) est appliqué au rendu par le
         * delegate (initStyleOption), pas ici. */
        OwlControl* ed = reinterpret_cast<OwlControl*>(editor);
        owl_ui_lineedit_set_num_format_validator(ed, "");
        break;
    }
    case OWL_COL_TYPE_MOTDEPASSE: {
        /* Mot de passe : editeur en echo masque (caractere de la plateforme via
         * QLineEdit::Password), miroir du champ Saisie MotDePasse. Aucun
         * validateur (un mot de passe accepte n'importe quel caractere).
         * L'AFFICHAGE hors edition est masque separement par le delegate. */
        OwlControl* ed = reinterpret_cast<OwlControl*>(editor);
        owl_ui_lineedit_set_password_mode(ed, 1);
        break;
    }
    default:
        break; /* type non câblé (Texte/Booléen) : rien à poser. */
    }
}

/*  — AFFICHAGE (hors édition) d'une cellule de colonne typée : si la
 * colonne (col0) est Numérique/Monétaire, reformate opt->text (déjà rempli par
 * initStyleOption = DisplayRole brut) via le formateur core (format défaut/projet
 * résolu au rendu, repli plat sinon). Autres types : inchangé. Source de vérité
 * UNIQUE partagée par le paint des deux delegates ET la sonde de test
 * __DebugTableCelluleTexteAffiche -> le rendu et la sonde ne peuvent diverger. */
static void owl_table_apply_typed_display(const QObject* view,
                                          QStyleOptionViewItem* opt,
                                          const QModelIndex& index) {
    if (!view || !opt) return;
    int col0 = index.column();
    QVariant tv = view->property(QString("col_type_%1").arg(col0).toUtf8());
    if (!tv.isValid()) return;
    int ct = tv.toInt();
    /*  Numerique/Monetaire,  Date/Heure/Duree : types dont
     * l'AFFICHAGE cellule est reformate au masque defaut/projet. Texte/Booleen
     * restent inchanges. */
    if (ct != OWL_COL_TYPE_NUMERIQUE && ct != OWL_COL_TYPE_MONETAIRE &&
        ct != OWL_COL_TYPE_DATE && ct != OWL_COL_TYPE_HEURE &&
        ct != OWL_COL_TYPE_DUREE && ct != OWL_COL_TYPE_MOTDEPASSE) return;
    QByteArray raw = opt->text.toUtf8();
    char out[160];
    owl_ui_format_cell_typed(ct, raw.constData(), out, sizeof(out));
    opt->text = QString::fromUtf8(out);
}

/* Delegate paramétré pour l'override de colonne : peint le fond des etats
 * dynamiques (selection / survol) qu'un QSS global ne peut cibler colonne par
 * colonne. Les feuilles absentes retombent sur le comportement par defaut
 * (base = ce que le styleTable global a deja pose). */
class OwlColumnStyleDelegate : public QStyledItemDelegate {
public:
    OwlColumnStyleSpec spec;
    OwlTableStyleSpec fallback;
    bool hasFallback;

    OwlColumnStyleDelegate(const OwlColumnStyleSpec& s,
                           const OwlTableStyleSpec* fb, QObject* parent)
        : QStyledItemDelegate(parent), spec(s), hasFallback(fb != nullptr) {
        if (fb) fallback = *fb;
    }

    /*  — formatage AFFICHAGE colonne typée (Numérique/Monétaire) : injecté
     * ici pour couvrir une colonne à la fois stylée ET typée. */
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        owl_table_apply_typed_display(this->parent(), option, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        bool selected = (opt.state & QStyle::State_Selected);
        /* Survol ROW-WIDE partage : meme source de verite que le delegate
         * global (hoveredRow du view). State_MouseOver ne cible que la cellule
         * sous le curseur -> la colonne se surlignerait seule sinon. */
        bool hovered  = (index.row() == owl_table_hovered_row(
                             const_cast<QWidget*>(opt.widget)));

        /* Resout une brique (colonne prioritaire, fallback styleTable). */
        auto resolveBrick = [&](const OwlTextBrick& colB,
                                const OwlTextBrick* fbB) -> OwlTextBrick {
            OwlTextBrick r = colB;
            if (fbB) {
                if (r.fg < 0) { r.fg = fbB->fg; r.fg_alpha = fbB->fg_alpha; }
                if (r.bg < 0) { r.bg = fbB->bg; r.bg_alpha = fbB->bg_alpha; }
                if (!r.font_family || !*r.font_family) r.font_family = fbB->font_family;
                if (r.font_size <= 0) r.font_size = fbB->font_size;
                if (r.bold < 0) r.bold = fbB->bold;
                if (r.italic < 0) r.italic = fbB->italic;
            }
            return r;
        };

        OwlTextBrick brick;
        bool active = true;
        if (selected) {
            brick = resolveBrick(spec.selection,
                                 hasFallback ? &fallback.selection : nullptr);
        } else if (hovered) {
            brick = resolveBrick(spec.survol,
                                 hasFallback ? &fallback.survol : nullptr);
        } else {
            /* Etat repos : fond alterne (paires/impaires) + texte des cellules. */
            OwlTextBrick lignes = resolveBrick(spec.lignes,
                                    hasFallback ? &fallback.lignes : nullptr);
            bool even = (index.row() % 2 == 0);
            int altBg = even ? spec.impaires_bg : spec.paires_bg;
            int altA  = even ? spec.impaires_bg_alpha : spec.paires_bg_alpha;
            if (altBg < 0 && hasFallback) {
                altBg = even ? fallback.impaires_bg : fallback.paires_bg;
                altA  = even ? fallback.impaires_bg_alpha : fallback.paires_bg_alpha;
            }
            brick = lignes;
            if (altBg >= 0) { brick.bg = altBg; brick.bg_alpha = altA; }
            active = (brick.fg >= 0 || brick.bg >= 0 || brick.font_size > 0
                      || (brick.font_family && *brick.font_family)
                      || brick.bold >= 0 || brick.italic >= 0);
        }

        if (!active) { QStyledItemDelegate::paint(painter, option, index); return; }

        /* Fond peint EXPLICITEMENT via fillRect (meme raison que le delegate
         * global : QSS actif ignore opt.backgroundBrush).  — SourceOver
         * alpha-aware (composition sur le backdrop). */
        if (brick.bg >= 0) {
            QColor bgc = owl_table_bgr_to_qcolor(brick.bg);
            bgc.setAlpha(brick.bg_alpha);
            painter->fillRect(opt.rect, bgc);
        } else if (selected) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
        }

        /* Fond deja peint : on empeche le base paint de le repeindre. */
        opt.backgroundBrush = QBrush(Qt::NoBrush);
        opt.state &= ~QStyle::State_Selected;

        /* Couleur du texte : brique prioritaire, sinon couleur systeme.
         *  — alpha appliqué uniquement sur une couleur explicite. */
        QColor fg;
        if (brick.fg > 0) {
            fg = owl_table_bgr_to_qcolor(brick.fg);
            fg.setAlpha(brick.fg_alpha);
        } else {
            fg = opt.palette.color(selected ? QPalette::HighlightedText
                                            : QPalette::Text);
        }
        opt.palette.setColor(QPalette::Text, fg);
        opt.palette.setColor(QPalette::HighlightedText, fg);

        /* Police. */
        QFont f = opt.font;
        if (brick.font_family && *brick.font_family)
            f.setFamily(QString::fromUtf8(brick.font_family));
        if (brick.font_size > 0) f.setPointSize(brick.font_size);
        if (brick.bold >= 0)   f.setBold(brick.bold == 1);
        if (brick.italic >= 0) f.setItalic(brick.italic == 1);
        opt.font = f;
        opt.fontMetrics = QFontMetrics(f); /* elision correcte a la nouvelle police */

        QStyledItemDelegate::paint(painter, opt, index);
    }

    /* Meme garde que le delegate global : fige le toggle des cases a cocher
     * quand la table/colonne n'est pas editable (checkbox visible, clic inerte). */
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if ((index.flags() & Qt::ItemIsUserCheckable)
            && !owl_table_checkbox_toggle_allowed(option.widget, index)) {
            return false;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    /*  — le styleColonne prime sur le delegate de masque (setItemDelegate
     * ForColumn n'en garde qu'un) : ce delegate porte donc lui aussi l'application
     * du masque de saisie. parent() = le view (cf. new OwlColumnStyleDelegate(...,
     * view)), source de la property col_mask_<col>. */
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        owl_table_apply_col_editor_behavior(editor, this->parent(), index.column());
        return editor;
    }
};

extern "C" void owl_ui_table_set_column_style(OwlControl* table, int col_0based,
                                              const OwlColumnStyleSpec* s,
                                              const OwlTableStyleSpec* table_fallback) {
    if (!table || !s || col_0based < 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model || col_0based >= model->columnCount()) return;

    /* --- TitreColonne : en-tete de CETTE section via les roles du modele. --- */
    {
        const OwlTextBrick& t = s->titre;
        if (t.bg >= 0) {
            QColor hbg = owl_table_bgr_to_qcolor(t.bg);
            hbg.setAlpha(t.bg_alpha);   /*  */
            model->setHeaderData(col_0based, Qt::Horizontal, hbg, Qt::BackgroundRole);
        }
        if (t.fg > 0) {
            QColor hfg = owl_table_bgr_to_qcolor(t.fg);
            hfg.setAlpha(t.fg_alpha);   /*  */
            model->setHeaderData(col_0based, Qt::Horizontal, hfg, Qt::ForegroundRole);
        }
        if ((t.font_family && *t.font_family) || t.font_size > 0
            || t.bold >= 0 || t.italic >= 0) {
            QFont f = view->font();
            if (t.font_family && *t.font_family) f.setFamily(QString::fromUtf8(t.font_family));
            if (t.font_size > 0) f.setPointSize(t.font_size);
            if (t.bold >= 0)   f.setBold(t.bold == 1);
            if (t.italic >= 0) f.setItalic(t.italic == 1);
            model->setHeaderData(col_0based, Qt::Horizontal, f, Qt::FontRole);
        }
    }

    /* --- Etats + cellules de CETTE colonne : delegate paramétré. Il couvre
     * les feuilles statiques (texte/fond/alternees) ET dynamiques
     * (selection/survol) que le QSS global ne peut isoler par colonne. --- */
    OwlColumnStyleDelegate* d =
        new OwlColumnStyleDelegate(*s, table_fallback, view);
    view->setItemDelegateForColumn(col_0based, d);

    /* Le survol de cette colonne est aussi row-wide : garantir le tracking
     * meme si aucun styleTable global n'a ete pose sur ce view. */
    owl_table_install_hover_tracking(view);
}

/*  — delegate d'édition minimal pour une colonne masquée SANS styleColonne
 * (le styleColonne, lui, applique le masque via son propre createEditor). Peinture
 * par défaut ; seule createEditor est surchargée pour poser le masque de la colonne
 * sur l'éditeur transitoire. parent() = le view. */
class OwlColumnMaskDelegate : public QStyledItemDelegate {
public:
    explicit OwlColumnMaskDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    /*  — formatage AFFICHAGE colonne typée (Numérique/Monétaire) sans
     * styleColonne : la peinture par défaut (QStyledItemDelegate::paint) appelle
     * cet initStyleOption, donc opt.text est reformaté avant dessin. */
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        owl_table_apply_typed_display(this->parent(), option, index);
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        owl_table_apply_col_editor_behavior(editor, this->parent(), index.column());
        return editor;
    }
};

/*  — pose le masque de saisie canon d'une colonne (col_1based) : stocke la
 * forme canon en dynamic property du view (source lue par createEditor des delegates)
 * et installe le delegate de masque si la colonne n'a pas déjà un delegate (le
 * styleColonne, posé plus tard, applique le masque lui-même). Le store OWL-side
 * reste la source de vérité du get runtime (headless) ; ceci n'est que le miroir
 * d'application côté Qt (GUI). */
extern "C" void owl_ui_table_set_column_mask(OwlControl* table, int col_1based,
                                             const char* wl_mask) {
    if (!table || col_1based <= 0) return;
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return;
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int col0 = col_1based - 1;
    if (col0 >= model->columnCount()) return;
    view->setProperty(QString("col_mask_%1").arg(col0).toUtf8(),
                      QString::fromUtf8(wl_mask ? wl_mask : ""));
    if (view->itemDelegateForColumn(col0) == nullptr) {
        view->setItemDelegateForColumn(col0, new OwlColumnMaskDelegate(view));
    }
}

/*  (test-only) — masque EFFECTIF de l'éditeur transitoire de la cellule
 * (row/col 1-based) : ouvre l'éditeur via le delegate de la colonne, relit le
 * masque Qt reconverti en canon (owl_ui_lineedit_get_mask), puis détruit l'éditeur.
 * Chaîne vide si champ/cellule introuvable ou colonne sans masque. */
extern "C" const char* owl_ui_table_cell_editor_mask(OwlControl* table,
                                                     int row_1based,
                                                     int col_1based) {
    g_ctrl_text_buf.clear();
    if (!table || row_1based <= 0 || col_1based <= 0) return "";
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return "";
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return "";
    int r = row_1based - 1, c = col_1based - 1;
    if (r >= model->rowCount() || c >= model->columnCount()) return "";
    QModelIndex idx = model->index(r, c);
    QAbstractItemDelegate* del = view->itemDelegateForColumn(c);
    if (!del) del = view->itemDelegate();
    if (!del) return "";
    QStyleOptionViewItem opt;
    opt.rect = view->visualRect(idx);
    QWidget* editor = del->createEditor(view->viewport(), opt, idx);
    if (!editor) return "";
    const char* wl = owl_ui_lineedit_get_mask(reinterpret_cast<OwlControl*>(editor));
    QByteArray saved(wl ? wl : "");   /* copie avant destruction de l'éditeur */
    delete editor;
    g_ctrl_text_buf = saved;
    return g_ctrl_text_buf.constData();
}

/*  +  — colonne pilotée par FORMAT : stocke le format effectif
 * et le masque de frappe pré-dérivé (calculés côté core, où
 * compiler_projet_format_date/heure/duree est accessible) en dynamic properties
 * du view (col_typeformat_/col_typemask_), source lue par le dispatch de
 * createEditor (owl_table_apply_col_editor_behavior). Le discriminateur de type
 * reste col_type_<col0> (posé par owl_ui_table_add_column) : ce store est commun
 * à tous les types format-driven, le case du framework choisit le validateur.
 * Installe le delegate de masque si la colonne n'en a pas déjà un — même seed que
 * owl_ui_table_set_column_mask, pour que createEditor se déclenche. */
extern "C" void owl_ui_table_set_column_format(OwlControl* table,
                                               int col_1based,
                                               const char* fmt,
                                               const char* qtmask) {
    if (!table || col_1based <= 0) return;
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return;
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int col0 = col_1based - 1;
    if (col0 >= model->columnCount()) return;
    view->setProperty(QString("col_typeformat_%1").arg(col0).toUtf8(),
                      QString::fromUtf8(fmt ? fmt : ""));
    view->setProperty(QString("col_typemask_%1").arg(col0).toUtf8(),
                      QString::fromUtf8(qtmask ? qtmask : ""));
    if (view->itemDelegateForColumn(col0) == nullptr) {
        view->setItemDelegateForColumn(col0, new OwlColumnMaskDelegate(view));
    }
}

/*  (test-only) — interroge le validateur EFFECTIF de l'éditeur
 * transitoire d'une cellule (row/col 1-based) sur `text` : ouvre l'éditeur via le
 * delegate de la colonne (même mécanisme que owl_ui_table_cell_editor_mask), lit
 * son QValidator, applique validate(), rend 1 ssi l'état est Acceptable (0 pour
 * Intermediate/Invalid), puis détruit l'éditeur. Cellule sans validateur (colonne
 * non contrainte) -> 1 (tout accepté). 1 aussi si champ/cellule introuvable. */
extern "C" int owl_ui_table_cell_editor_valide_saisie(OwlControl* table,
                                                      int row_1based,
                                                      int col_1based,
                                                      const char* text) {
    if (!table || row_1based <= 0 || col_1based <= 0) return 1;
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return 1;
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 1;
    int r = row_1based - 1, c = col_1based - 1;
    if (r >= model->rowCount() || c >= model->columnCount()) return 1;
    QModelIndex idx = model->index(r, c);
    QAbstractItemDelegate* del = view->itemDelegateForColumn(c);
    if (!del) del = view->itemDelegate();
    if (!del) return 1;
    QStyleOptionViewItem opt;
    opt.rect = view->visualRect(idx);
    QWidget* editor = del->createEditor(view->viewport(), opt, idx);
    if (!editor) return 1;
    int accept = 1;
    QLineEdit* le = qobject_cast<QLineEdit*>(editor);
    if (le && le->validator()) {
        QString s = QString::fromUtf8(text ? text : "");
        int pos = s.length();
        accept = (le->validator()->validate(s, pos) == QValidator::Acceptable)
                     ? 1 : 0;
    }
    delete editor;
    return accept;
}

/*  (test-only) — texte AFFICHÉ d'une cellule (row/col 1-based) HORS
 * édition : lit le DisplayRole brut du modèle et applique EXACTEMENT le même
 * formatage que le rendu (owl_ui_format_cell_typed, aussi utilisé par
 * owl_table_apply_typed_display dans initStyleOption), garantissant l'égalité
 * sonde == cellule peinte. "" si champ/cellule introuvable. */
extern "C" const char* owl_ui_table_cell_texte_affiche(OwlControl* table,
                                                       int row_1based,
                                                       int col_1based) {
    g_ctrl_text_buf.clear();
    if (!table || row_1based <= 0 || col_1based <= 0) return "";
    QTableView* view = qobject_cast<QTableView*>(reinterpret_cast<QWidget*>(table));
    if (!view) return "";
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return "";
    int r = row_1based - 1, c = col_1based - 1;
    if (r >= model->rowCount() || c >= model->columnCount()) return "";
    QModelIndex idx = model->index(r, c);
    QByteArray raw = model->data(idx, Qt::DisplayRole).toString().toUtf8();
    int ct = 0;
    QVariant tv = view->property(QString("col_type_%1").arg(c).toUtf8());
    if (tv.isValid()) ct = tv.toInt();
    char out[160];
    owl_ui_format_cell_typed(ct, raw.constData(), out, sizeof(out));
    g_ctrl_text_buf = QByteArray(out);
    return g_ctrl_text_buf.constData();
}

/* Helper : structure d'un handler d'event Table (tient cb + userdata + sender). */
struct OwlTableHandler { OwlCallback cb; void* userdata; OwlControl* sender; };

extern "C" void owl_ui_table_on_click(OwlControl* table, OwlCallback cb, void* userdata) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    OwlTableHandler* h = new OwlTableHandler{cb, userdata, table};
    owl_track_conn(view,
        QObject::connect(view, &QTableView::clicked, view, [h](const QModelIndex&) {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

extern "C" void owl_ui_table_on_double_click(OwlControl* table, OwlCallback cb, void* userdata) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    OwlTableHandler* h = new OwlTableHandler{cb, userdata, table};
    owl_track_conn(view,
        QObject::connect(view, &QTableView::doubleClicked, view, [h](const QModelIndex&) {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

extern "C" void owl_ui_table_on_selection(OwlControl* table, OwlCallback cb, void* userdata) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return;
    OwlTableHandler* h = new OwlTableHandler{cb, userdata, table};
    owl_track_conn(view,
        QObject::connect(sm, &QItemSelectionModel::selectionChanged, view,
                         [h](const QItemSelection&, const QItemSelection&) {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

/* surEntreeLigne / surSortieLigne : derives de currentRowChanged.
 * Quand la ligne courante change : exit ancienne -> enter nouvelle. */
extern "C" void owl_ui_table_on_row_enter(OwlControl* table, OwlCallback cb, void* userdata) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return;
    OwlTableHandler* h = new OwlTableHandler{cb, userdata, table};
    owl_track_conn(view,
        QObject::connect(sm, &QItemSelectionModel::currentRowChanged, view,
                         [h](const QModelIndex& cur, const QModelIndex& /*prev*/) {
            if (cur.isValid() && h->cb) h->cb(h->sender, h->userdata);
        }));
}

extern "C" void owl_ui_table_on_row_exit(OwlControl* table, OwlCallback cb, void* userdata) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QItemSelectionModel* sm = view->selectionModel();
    if (!sm) return;
    OwlTableHandler* h = new OwlTableHandler{cb, userdata, table};
    owl_track_conn(view,
        QObject::connect(sm, &QItemSelectionModel::currentRowChanged, view,
                         [h](const QModelIndex& /*cur*/, const QModelIndex& prev) {
            if (prev.isValid() && h->cb) h->cb(h->sender, h->userdata);
        }));
}

/* RFC 4180-ish CSV escape : si contient , " \n ou \r, on quote + double
 * les " internes. Sinon raw. */
static QString csv_escape(const QString& s) {
    if (s.contains(',') || s.contains('"') || s.contains('\n') || s.contains('\r')) {
        QString q = s;
        q.replace('"', "\"\"");
        return '"' + q + '"';
    }
    return s;
}

/*  — extraction 2D d'une table (entêtes + lignes VISIBLES), source de
 * vérité unique partagée par l'export CSV, l'export Excel et « Tout copier ».
 * Les cellules booléennes sont rendues Vrai/Faux (cohérent avec le store), les
 * lignes filtrées (hidden) sont ignorées. */
struct OwlTableExtract {
    QStringList          headers;
    QVector<QStringList> rows;
};

static OwlTableExtract owl_ui_table_extract(QTableView* view) {
    OwlTableExtract out;
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return out;
    int colCount = model->columnCount();
    int rowCount = model->rowCount();
    for (int c = 0; c < colCount; c++)
        out.headers << model->headerData(c, Qt::Horizontal).toString();
    for (int r = 0; r < rowCount; r++) {
        if (view->isRowHidden(r)) continue;
        QStringList row;
        for (int c = 0; c < colCount; c++) {
            QStandardItem* it = model->item(r, c);
            QString cell;
            if (it) {
                QVariant prop = view->property(QString("col_type_%1").arg(c).toUtf8());
                int colType = prop.isValid() ? prop.toInt() : 0;
                if (colType == OWL_COL_TYPE_BOOLEEN) {
                    cell = (it->checkState() == Qt::Checked) ? "Vrai" : "Faux";
                } else {
                    cell = it->text();
                }
            }
            row << cell;
        }
        out.rows << row;
    }
    return out;
}

extern "C" int owl_ui_table_export_csv(OwlControl* table, const char* path_utf8) {
    if (!table || !path_utf8) return 0;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    if (!qobject_cast<QStandardItemModel*>(view->model())) return 0;
    OwlTableExtract data = owl_ui_table_extract(view);
    QFile f(QString::fromUtf8(path_utf8));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return 0;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);  /* Qt6 native UTF-8. */
    QStringList header;
    for (const QString& h : data.headers) header << csv_escape(h);
    out << header.join(',') << '\n';
    for (const QStringList& row : data.rows) {
        QStringList cols;
        for (const QString& cell : row) cols << csv_escape(cell);
        out << cols.join(',') << '\n';
    }
    f.close();
    return 1;
}

/*  — pont vers l'export xlsx pure-données (stdlib_ui.c). Même chemin
 * d'extraction que CSV / Tout copier ; ne fait que mettre les QString en arrays
 * de char* UTF-8 pour la fonction C. */
static int owl_ui_table_export_xlsx_view(QTableView* view, const QString& path) {
    OwlTableExtract data = owl_ui_table_extract(view);
    int ncols = data.headers.size();
    if (ncols == 0) return 0;
    /* Les QByteArray restent vivants tant que ces vecteurs existent : les
     * const char* pointent dans leurs buffers. Vecteurs non redimensionnés
     * après remplissage -> pointeurs stables. */
    QVector<QByteArray> hbytes;
    hbytes.reserve(ncols);
    for (const QString& h : data.headers) hbytes << h.toUtf8();
    QVector<const char*> hptr;
    hptr.reserve(ncols);
    for (const QByteArray& b : hbytes) hptr << b.constData();

    int nrows = data.rows.size();
    QVector<QByteArray> cbytes;
    cbytes.reserve(nrows * ncols);
    for (const QStringList& row : data.rows)
        for (int c = 0; c < ncols; c++)
            cbytes << (c < row.size() ? row[c].toUtf8() : QByteArray());
    QVector<const char*> cptr;
    cptr.reserve(cbytes.size());
    for (const QByteArray& b : cbytes) cptr << b.constData();

    return owl_ui_table_export_xlsx(path.toUtf8().constData(),
                                    hptr.constData(), ncols,
                                    nrows ? cptr.constData() : nullptr, nrows);
}

/*  — nom d'icone du catalogue Iconify embarqué -> QIcon. Retourne un
 * QIcon nul si le nom est inconnu (le menu s'affiche alors sans icone). */
static QIcon owl_ui_icon_from_catalog(const char* name, int size, unsigned int color_bgr) {
    unsigned char* rgba = nullptr;
    int w = 0, h = 0;
    if (!owl_ui_icone_catalog_rgba(name, size, color_bgr, &rgba, &w, &h) || !rgba)
        return QIcon();
    QImage img = rgba_to_qimage(rgba, w, h);
    free(rgba);
    if (img.isNull()) return QIcon();
    return QIcon(QPixmap::fromImage(img));
}

extern "C" void owl_ui_table_install_default_context_menu(OwlControl* table) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(view, &QTableView::customContextMenuRequested, view,
                     [view](const QPoint& pos) {
        QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
        if (!model) return;
        QModelIndex idx = view->indexAt(pos);
        const int ICON = 16;  /* taille icone de menu */
        /* Gris clair neutre (format OWL 0x00BBGGRR ; R=G=B donc symétrique) :
         * teinte les monochromes pour alléger le menu. Les 3 "Copier" partagent
         * la MEME icone (content-copy-outline), "Tout copier" sa version doublée
         * (copy-all-outline). L'export Excel garde ses couleurs bakées (vert). */
        const unsigned int GREY = 0x707070u;
        QMenu menu(view);
        QAction* aCell = menu.addAction(owl_ui_icon_from_catalog("content-copy-outline", ICON, GREY),
                                        owl_ui_label("Copier la cellule", "Copy cell"));
        QAction* aRow  = menu.addAction(owl_ui_icon_from_catalog("content-copy-outline", ICON, GREY),
                                        owl_ui_label("Copier la ligne", "Copy row"));
        QAction* aCol  = menu.addAction(owl_ui_icon_from_catalog("content-copy-outline", ICON, GREY),
                                        owl_ui_label("Copier la colonne", "Copy column"));
        QAction* aAll  = menu.addAction(owl_ui_icon_from_catalog("copy-all-outline", ICON, GREY),
                                        owl_ui_label("Tout copier", "Copy all"));
        menu.addSeparator();
        QAction* aXls  = menu.addAction(owl_ui_icon_from_catalog("file-type-excel", ICON, GREY),
                                        owl_ui_label("Exporter vers Excel\xE2\x80\xA6",
                                                     "Export to Excel\xE2\x80\xA6"));
        QAction* aCsv  = menu.addAction(owl_ui_icon_from_catalog("file-delimited-outline", ICON, GREY),
                                        owl_ui_label("Exporter en CSV\xE2\x80\xA6",
                                                     "Export to CSV\xE2\x80\xA6"));
        if (!idx.isValid()) {
            aCell->setEnabled(false);
            aRow->setEnabled(false);
            aCol->setEnabled(false);
        }
        QAction* chosen = menu.exec(view->viewport()->mapToGlobal(pos));
        if (!chosen) return;
        QClipboard* cb = QGuiApplication::clipboard();
        if (chosen == aCell && idx.isValid()) {
            QStandardItem* it = model->item(idx.row(), idx.column());
            cb->setText(it ? it->text() : QString());
        } else if (chosen == aRow && idx.isValid()) {
            QStringList parts;
            for (int c = 0; c < model->columnCount(); c++) {
                QStandardItem* it = model->item(idx.row(), c);
                parts << (it ? it->text() : QString());
            }
            cb->setText(parts.join('\t'));
        } else if (chosen == aCol && idx.isValid()) {
            /* Colonne cliquée, toutes les lignes visibles (RC-jointes). */
            OwlTableExtract data = owl_ui_table_extract(view);
            int col = idx.column();
            QStringList parts;
            for (const QStringList& row : data.rows)
                parts << (col < row.size() ? row[col] : QString());
            cb->setText(parts.join('\n'));
        } else if (chosen == aAll) {
            /* Toute la table visible : tab entre colonnes, RC entre lignes,
             * entêtes en première ligne. Même extraction que CSV / Excel. */
            OwlTableExtract data = owl_ui_table_extract(view);
            QStringList lines;
            lines << data.headers.join('\t');
            for (const QStringList& row : data.rows)
                lines << row.join('\t');
            cb->setText(lines.join('\n'));
        } else if (chosen == aXls) {
            QString path = QFileDialog::getSaveFileName(view,
                owl_ui_label("Exporter la table vers Excel", "Export table to Excel"),
                QString(), owl_ui_label("Classeur Excel (*.xlsx)", "Excel workbook (*.xlsx)"));
            if (!path.isEmpty()) {
                if (!path.endsWith(".xlsx", Qt::CaseInsensitive)) path += ".xlsx";
                owl_ui_table_export_xlsx_view(view, path);
            }
        } else if (chosen == aCsv) {
            QString path = QFileDialog::getSaveFileName(view,
                owl_ui_label("Exporter la table en CSV", "Export table to CSV"),
                QString(), owl_ui_label("CSV (*.csv);;Tous fichiers (*.*)",
                                        "CSV (*.csv);;All files (*.*)"));
            if (!path.isEmpty()) {
                owl_ui_table_export_csv(reinterpret_cast<OwlControl*>(view),
                                         path.toUtf8().constData());
            }
        }
    });
}

/* Test un filtre canon contre une cellule. type est OWL_FILTRE_*. */
static bool owl_filter_match(const QString& cell, int type, const QString& valeur) {
    QString c = cell.toLower();
    QString v = valeur.toLower();
    if (type == OWL_FILTRE_IN) {
        /* Filtre multi-valeurs « IN {ensemble} » : valeur = valeurs a retenir
         * jointes par RC. Matche ssi la cellule egale l'une d'elles (casse-
         * insensible, comme les autres operateurs). Ensemble vide (« Aucun ») :
         * aucune valeur retenue -> rien ne matche (table vide). En tete, avant
         * le split : "".split("\r\n") donne [""] qui matcherait les cellules
         * vides. */
        if (v.isEmpty()) return false;
        const QStringList opts = v.split(QStringLiteral("\r\n"));
        for (const QString& o : opts) {
            if (c == o.trimmed()) return true;
        }
        return false;
    }
    switch (type) {
        case OWL_FILTRE_CONTIENT:        return c.contains(v);
        case OWL_FILTRE_NE_CONTIENT_PAS: return !c.contains(v);
        case OWL_FILTRE_COMMENCE_PAR:    return c.startsWith(v);
        case OWL_FILTRE_NE_COMMENCE_PAS: return !c.startsWith(v);
        case OWL_FILTRE_TERMINE_PAR:     return c.endsWith(v);
        case OWL_FILTRE_NE_TERMINE_PAS:  return !c.endsWith(v);
        case OWL_FILTRE_EGAL:            return c == v;
        case OWL_FILTRE_DIFFERENT:       return c != v;
        case OWL_FILTRE_INFERIEUR:       return c < v;
        case OWL_FILTRE_INFERIEUR_EGAL:  return c <= v;
        case OWL_FILTRE_SUPERIEUR:       return c > v;
        case OWL_FILTRE_SUPERIEUR_EGAL:  return c >= v;
    }
    return true;
}

extern "C" void owl_ui_table_apply_filters(OwlControl* table,
                                           const int* col_indices,
                                           const int* types,
                                           const char* const* values,
                                           int n) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int colCount = model->columnCount();
    int rowCount = model->rowCount();

    /* Chaque ligne est visible ssi elle matche TOUS les filtres actifs (ET).
     * Un seul filtre qui echoue masque la ligne ; aucun filtre => tout visible. */
    for (int r = 0; r < rowCount; r++) {
        bool visible = true;
        for (int f = 0; f < n && visible; f++) {
            int c = col_indices[f];
            if (c < 0 || c >= colCount) continue;  /* colonne hors modele : filtre ignore */
            QString valeur = QString::fromUtf8(values[f] ? values[f] : "").trimmed();
            /* garde-fou : valeur vide != filtre, SAUF IN {} (ensemble vide =
             * filtre actif qui masque tout, cf. owl_filter_match). */
            if (valeur.isEmpty() && types[f] != OWL_FILTRE_IN) continue;
            QStandardItem* it = model->item(r, c);
            QString cell = it ? it->text() : QString();
            if (!owl_filter_match(cell, types[f], valeur)) visible = false;
        }
        view->setRowHidden(r, !visible);
    }
}

/*  — valeurs de la colonne col0 (0-based) pour les lignes NON masquees
 * (rowHidden reel du QTableView monte), dans l'ordre du modele, jointes par RC.
 * Lit l'etat Qt REEL : c'est la sonde qui rend observable l'effet des filtres
 * (op+valeur ET IN{set}) depuis OWL. out mis a chaine vide si modele absent,
 * col0 hors bornes ou aucune ligne visible. */
extern "C" void owl_ui_table_visible_column_values(OwlControl* table, int col0,
                                                    char* out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!table || !out || cap <= 0) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model || col0 < 0 || col0 >= model->columnCount()) return;
    QStringList parts;
    for (int r = 0; r < model->rowCount(); r++) {
        if (view->isRowHidden(r)) continue;
        QStandardItem* it = model->item(r, col0);
        parts << (it ? it->text() : QString());
    }
    QByteArray joined = parts.join(QStringLiteral("\r\n")).toUtf8();
    snprintf(out, (size_t)cap, "%s", joined.constData());
}

extern "C" void owl_ui_table_sort_by_column(OwlControl* table, int col_1based,
                                            const char* col_name_utf8,
                                            int ascending) {
    if (!table) return;
    QTableView* view = reinterpret_cast<QTableView*>(table);
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return;
    int idx = -1;
    if (col_1based > 0) {
        idx = col_1based - 1;
    } else if (col_name_utf8 && *col_name_utf8) {
        QString name = QString::fromUtf8(col_name_utf8);
        for (int c = 0; c < model->columnCount(); c++) {
            if (model->headerData(c, Qt::Horizontal).toString().compare(name, Qt::CaseInsensitive) == 0) {
                idx = c;
                break;
            }
        }
    }
    if (idx < 0 || idx >= model->columnCount()) return;
    view->sortByColumn(idx, ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

/* ------- Saisie (QLineEdit) ------- */

extern "C" OwlControl* owl_ui_lineedit_create(OwlWindow* parent,
                                              const char* placeholder_utf8,
                                              int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QLineEdit* le = new QLineEdit(central_of(parent));
    if (placeholder_utf8) le->setPlaceholderText(QString::fromUtf8(placeholder_utf8));
    le->setGeometry(x, y, w, h);
    le->show();
    /*  — la fenetre OWL est un QMainWindow (pas un QDialog), donc le
     * mecanisme Qt « Entree declenche le bouton par defaut » ne s'applique pas
     * automatiquement quand un champ a le focus : le QLineEdit avale Entree. On
     * installe le dispatcher des la creation (meme sans aucun handler cable sur
     * le champ) pour que l'eventFilter puisse, en fallback, router Entree vers
     * le bouton btnValider de la fenetre. */
    install_owl_event_filter(le);
    return reinterpret_cast<OwlControl*>(le);
}

/* =====  — libelle integre du Saisie (composite QLabel|QLineEdit) =====
 *
 * Le QLineEdit reste le controle logique : uici->widget, get_text/set_text,
 * focus, l'event filter et le live-read continuent de le viser sans changement
 * (zero ruissellement). Quand un libelle est demande (texte non vide ET largeur
 * > 0), on materialise un wrapper QWidget(QHBoxLayout) = [QLabel largeur fixe |
 * QLineEdit]. Ce wrapper est le "representant de layout" du controle :
 * owl_ui_layout_add_child l'ajoute au layout parent a la place du champ nu.
 *
 * Le texte et la largeur sont memorises separement sur le QLineEdit (dynamic
 * properties) pour que le live-update d'une seule des deux props recompose
 * correctement. map = QLineEdit interne -> wrapper (absence = pas de libelle). */
static QHash<QWidget*, QWidget*> g_lineedit_label_wrappers;

/*  — compteur d'objectName uniques pour le wrapper (CadreExtérieur).
 * Le style du conteneur est emis en QSS `#objectName`-scope : sans objectName
 * unique la feuille cascaderait via `*` sur le QLabel + le QLineEdit enfants
 * et ecraserait leur rendu (regression). */
static unsigned g_saisie_wrap_seq = 0;

/* Applique la largeur du QLabel du libelle integre selon LargeurLibellé.
 *   width > 0  -> largeur fixe demandee (setFixedWidth pose min == max).
 *   width <= 0 -> auto-width : le libelle se dimensionne a son texte. On defait
 *                 une largeur fixe eventuellement posee auparavant (setFixedWidth
 *                 verrouille min ET max) en relachant les deux bornes. */
static void owl_label_apply_width(QLabel* lbl, int width) {
    if (!lbl) return;
    if (width > 0) {
        lbl->setFixedWidth(width);
    } else {
        lbl->setMinimumWidth(0);
        lbl->setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

/* Construit le wrapper composite autour de `le`. Si `le` est deja place dans un
 * layout parent (live-update apres montage), le wrapper reprend exactement sa
 * position ; sinon (build / dispLibre) il reprend sa geometrie absolue. */
static QWidget* owl_lineedit_build_wrapper(QLineEdit* le,
                                           const QString& text, int width) {
    QWidget* parent = le->parentWidget();
    QLayout* ply    = parent ? parent->layout() : nullptr;
    QRect    geo    = le->geometry();
    bool     vis    = le->isVisible();

    int boxIdx = -1, boxStretch = 0;
    int gRow = 0, gCol = 0, gRs = 1, gCs = 1;
    Qt::Alignment align = Qt::Alignment();
    bool inBox = false, inGrid = false;
    if (auto* box = qobject_cast<QBoxLayout*>(ply)) {
        boxIdx = box->indexOf(le);
        if (boxIdx >= 0) {
            inBox = true;
            boxStretch = box->stretch(boxIdx);
            align = box->itemAt(boxIdx)->alignment();
        }
    } else if (auto* grid = qobject_cast<QGridLayout*>(ply)) {
        int gi = grid->indexOf(le);
        if (gi >= 0) {
            inGrid = true;
            grid->getItemPosition(gi, &gRow, &gCol, &gRs, &gCs);
            align = grid->itemAt(gi)->alignment();
        }
    }
    if (inBox || inGrid) ply->removeWidget(le);

    /*  — le libellé peut être absent (always-wrap : le CadreExtérieur
     * enveloppe TOUS les Saisie stylés, avec ou sans libellé). Sans libellé, le
     * QLabel est caché et le spacing tombe à 0 : le wrapper est neutre pour le
     * layout (le QLineEdit remplit tout le conteneur, géométrie inchangée). */
    bool hasLabel = !text.isEmpty();

    QWidget* wrapper = new QWidget(parent);
    /* objectName UNIQUE : cible du QSS `#id`-scope du CadreExtérieur. */
    wrapper->setObjectName(QString("owl_saisie_wrap_%1").arg(g_saisie_wrap_seq++));
    QHBoxLayout* hb = new QHBoxLayout(wrapper);
    hb->setContentsMargins(0, 0, 0, 0);
    hb->setSpacing(hasLabel ? 6 : 0);
    QLabel* lbl = new QLabel(text, wrapper);
    lbl->setObjectName("owl_saisie_label");
    if (hasLabel) owl_label_apply_width(lbl, width);
    else          lbl->setFixedWidth(0);
    if (!hasLabel) lbl->hide();
    hb->addWidget(lbl, 0);
    hb->addWidget(le, 1);   /* reparente `le` dans le wrapper */
    /* Le wrapper remplace le QLineEdit nu dans le layout parent : il doit se
     * dimensionner EXACTEMENT comme un Saisie non enveloppe, sinon le champ
     * stylé rend a une hauteur differente d'un champ temoin ( : forcer
     * le QLineEdit en Expanding sans transmettre cette politique au wrapper
     * cassait la parite — le QWidget wrapper, Preferred par defaut, se
     * dimensionnait autrement que le QLineEdit qu'il remplacait). On recopie
     * donc la politique de taille d'origine du QLineEdit sur le wrapper, PUIS on
     * passe le QLineEdit interne en Expanding vertical pour qu'il remplisse la
     * hauteur que le wrapper recoit du layout (ou sa geometrie absolue en
     * dispLibre). */
    wrapper->setSizePolicy(le->sizePolicy());
    le->setSizePolicy(le->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);

    if (inBox) {
        QBoxLayout* box = qobject_cast<QBoxLayout*>(ply);
        if (align != Qt::Alignment()) box->insertWidget(boxIdx, wrapper, boxStretch, align);
        else                          box->insertWidget(boxIdx, wrapper, boxStretch);
    } else if (inGrid) {
        QGridLayout* grid = qobject_cast<QGridLayout*>(ply);
        grid->addWidget(wrapper, gRow, gCol, gRs, gCs, align);
    } else {
        /* dispLibre / pas encore en layout (build) : geometrie absolue. */
        wrapper->setGeometry(geo);
    }
    if (vis) { wrapper->show(); if (hasLabel) lbl->show(); le->show(); }
    return wrapper;
}

/* Defait le wrapper : remet `le` a la place du wrapper (layout ou geometrie
 * absolue), puis detruit le wrapper (+ son QLabel). */
static void owl_lineedit_unwrap(QLineEdit* le, QWidget* wrapper) {
    QWidget* parent = wrapper->parentWidget();
    QLayout* ply    = parent ? parent->layout() : nullptr;
    QRect    geo    = wrapper->geometry();
    bool     vis    = wrapper->isVisible();

    int boxIdx = -1, boxStretch = 0;
    int gRow = 0, gCol = 0, gRs = 1, gCs = 1;
    Qt::Alignment align = Qt::Alignment();
    bool inBox = false, inGrid = false;
    if (auto* box = qobject_cast<QBoxLayout*>(ply)) {
        boxIdx = box->indexOf(wrapper);
        if (boxIdx >= 0) {
            inBox = true; boxStretch = box->stretch(boxIdx);
            align = box->itemAt(boxIdx)->alignment();
        }
    } else if (auto* grid = qobject_cast<QGridLayout*>(ply)) {
        int gi = grid->indexOf(wrapper);
        if (gi >= 0) {
            inGrid = true;
            grid->getItemPosition(gi, &gRow, &gCol, &gRs, &gCs);
            align = grid->itemAt(gi)->alignment();
        }
    }
    if (inBox || inGrid) ply->removeWidget(wrapper);
    le->setParent(parent);
    if (inBox) {
        QBoxLayout* box = qobject_cast<QBoxLayout*>(ply);
        if (align != Qt::Alignment()) box->insertWidget(boxIdx, le, boxStretch, align);
        else                          box->insertWidget(boxIdx, le, boxStretch);
    } else if (inGrid) {
        QGridLayout* grid = qobject_cast<QGridLayout*>(ply);
        grid->addWidget(le, gRow, gCol, gRs, gCs, align);
    } else {
        le->setGeometry(geo);
    }
    if (vis) le->show();
    wrapper->deleteLater();
}

/* Cree le wrapper autour de `le` et l'enregistre (+ hook de purge sur
 * destruction). Facteur commun de refresh_label (libelle demande) et de
 * ensure_wrapper (always-wrap pour le CadreExtérieur). */
static QWidget* owl_lineedit_create_wrapper_for(QLineEdit* le, const QString& text,
                                                int width) {
    /* Purge l'entree si le QLineEdit est detruit avant la fenetre : evite un
     * wrapper fantome reutilise sur une adresse recyclee. */
    if (!le->property("owl_lbl_destroy_hooked").toBool()) {
        le->setProperty("owl_lbl_destroy_hooked", true);
        QObject::connect(le, &QObject::destroyed, [](QObject* o) {
            g_lineedit_label_wrappers.remove(static_cast<QWidget*>(o));
        });
    }
    QWidget* wrapper = owl_lineedit_build_wrapper(le, text, width);
    g_lineedit_label_wrappers.insert(le, wrapper);
    return wrapper;
}

/* Recompose le composite a partir de l'etat memorise sur le QLineEdit. Cree,
 * met a jour, ou defait le wrapper selon que le libelle est demande ou non.
 *
 *  — un wrapper « force » (owl_wrap_forced, pose par ensure_wrapper
 * pour materialiser le CadreExtérieur) n'est JAMAIS defait : le conteneur
 * survit meme si le libelle disparait ; on cache seulement le QLabel. */
static void owl_lineedit_refresh_label(QLineEdit* le) {
    QString text = le->property("owl_lbl_text").toString();
    int width    = le->property("owl_lbl_width").toInt();
    bool want    = !text.isEmpty();
    QWidget* wrapper = g_lineedit_label_wrappers.value(le, nullptr);

    if (wrapper) {
        QHBoxLayout* hb = qobject_cast<QHBoxLayout*>(wrapper->layout());
        QLabel* lbl = (hb && hb->count() > 0)
                      ? qobject_cast<QLabel*>(hb->itemAt(0)->widget()) : nullptr;
        if (want) {
            if (lbl) { lbl->setText(text); owl_label_apply_width(lbl, width); lbl->show(); }
            if (hb) hb->setSpacing(6);
        } else if (wrapper->property("owl_wrap_forced").toBool()) {
            /* wrapper conserve (CadreExtérieur) : cacher le libelle, spacing 0. */
            if (lbl) { lbl->hide(); lbl->setFixedWidth(0); }
            if (hb) hb->setSpacing(0);
        } else {
            owl_lineedit_unwrap(le, wrapper);
            g_lineedit_label_wrappers.remove(le);
        }
    } else if (want) {
        owl_lineedit_create_wrapper_for(le, text, width);
    }
}

extern "C" void owl_ui_lineedit_set_label_text(OwlControl* edit, const char* text_utf8) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QObject*>(edit));
    if (!le) return;   /* multiligne (QPlainTextEdit) : pas de libelle integre */
    le->setProperty("owl_lbl_text", QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    owl_lineedit_refresh_label(le);
}

extern "C" void owl_ui_lineedit_set_label_width(OwlControl* edit, int width) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QObject*>(edit));
    if (!le) return;
    le->setProperty("owl_lbl_width", width);
    owl_lineedit_refresh_label(le);
}

/*  — always-wrap : materialise (si besoin) le conteneur du Saisie et le
 * marque « force » (jamais defait ensuite). Le CadreExtérieur d'un styleSaisie
 * peint sur ce wrapper. Retourne le wrapper (QWidget) comme OwlControl*, ou NULL
 * si `edit` n'est pas un QLineEdit (multiligne : pas de libelle integre). */
extern "C" OwlControl* owl_ui_saisie_ensure_wrapper(OwlControl* edit) {
    if (!edit) return nullptr;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QObject*>(edit));
    if (!le) return nullptr;
    QWidget* wrapper = g_lineedit_label_wrappers.value(le, nullptr);
    if (!wrapper) {
        QString text = le->property("owl_lbl_text").toString();
        int width    = le->property("owl_lbl_width").toInt();
        wrapper = owl_lineedit_create_wrapper_for(le, text, width);
    }
    if (wrapper) wrapper->setProperty("owl_wrap_forced", true);
    return reinterpret_cast<OwlControl*>(wrapper);
}

/*  — le QLabel du libelle integre (sous-element Libellé du style), ou
 * NULL si aucun wrapper materialise. */
extern "C" OwlControl* owl_ui_saisie_get_label(OwlControl* edit) {
    if (!edit) return nullptr;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QObject*>(edit));
    if (!le) return nullptr;
    QWidget* wrapper = g_lineedit_label_wrappers.value(le, nullptr);
    if (!wrapper) return nullptr;
    QHBoxLayout* hb = qobject_cast<QHBoxLayout*>(wrapper->layout());
    if (!hb || hb->count() == 0) return nullptr;
    QLabel* lbl = qobject_cast<QLabel*>(hb->itemAt(0)->widget());
    return reinterpret_cast<OwlControl*>(lbl);
}

static const char* cadre_line_style_to_css(int line_style);   /* defini plus bas */

/*  — style statique du CadreExtérieur (fond + bordure) sur le wrapper.
 * QSS `#objectName`-scope : la feuille ne matche QUE le conteneur, jamais ses
 * enfants (QLabel/QLineEdit) via `*` (sinon cascade -> ecrase leur rendu). La
 * bordure du wrapper inset le QLineEdit (contents-rect QStyleSheetStyle), ce qui
 * rend le cadre visible sur le pourtour hors zone interne du champ. */
extern "C" void owl_ui_saisie_wrapper_set_style(OwlControl* wrapperCtrl,
                                                int fg, int bg, int bgAlpha,
                                                const OwlCadreSpec* border) {
    if (!wrapperCtrl) return;
    QWidget* q = reinterpret_cast<QWidget*>(wrapperCtrl);
    QString id = q->objectName();
    if (id.isEmpty()) return;
    auto rgb_css = [](int v) {
        return QString("rgb(%1,%2,%3)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF);
    };
    QString body;
    if (fg >= 0) body += "color: " + rgb_css(fg) + "; ";
    /*  — fond alpha-aware : owl_bg_css émet `rgb(...)` byte-identique si
     * opaque (alpha >= 255), `rgba(...)` si translucide. */
    if (bg >= 0) body += "background-color: " + owl_bg_css(bg, bgAlpha) + "; ";
    /* Marges par côté : global -> tous égaux à l'épaisseur (rendu inchangé) ;
     * per-side -> épaisseur effective de chaque bord. */
    int bTop = 0, bRight = 0, bBottom = 0, bLeft = 0;
    if (border) {
        bool perSide = false;
        /*  — corps de bordure partagé (owl_bg_css : `rgb(...)` opaque
         * byte-identique à l'ancien rgb_css, alpha porté en per-side). */
        QString bb = cadre_border_body(border, owl_bg_css, &perSide);
        if (!bb.isEmpty()) {
            body += bb;
            if (!perSide) {
                bTop = bRight = bBottom = bLeft = border->thickness;
            } else {
                auto effThick = [&](int c, int t) {
                    if (t != 0 || c != 0) return t > 0 ? t : 0;
                    return border->thickness > 0 ? border->thickness : 0;
                };
                bTop    = effThick(border->trait_haut_color,   border->trait_haut_thick);
                bRight  = effThick(border->trait_droite_color, border->trait_droite_thick);
                bBottom = effThick(border->trait_bas_color,    border->trait_bas_thick);
                bLeft   = effThick(border->trait_gauche_color, border->trait_gauche_thick);
            }
        }
    }
    /*  — arrondi émis même sans bordure (Epaisseur=0, Arrondi>0). */
    if (border && border->corner_radius > 0)
        body += QString("border-radius: %1px; ").arg(border->corner_radius);
    /* QStyleSheetStyle n'inset PAS l'enfant par la bordure du wrapper : sans
     * marges, le QLineEdit (vertical Expanding) recouvrirait le cadre. On reserve
     * l'epaisseur de la bordure en marges de layout -> le champ s'inscrit dans le
     * contour et la bordure du CadreExtérieur reste visible sur le pourtour. */
    if (QLayout* ly = q->layout())
        ly->setContentsMargins(bLeft, bTop, bRight, bBottom);
    if (body.isEmpty()) {
        q->setStyleSheet(QString());
        q->setAttribute(Qt::WA_StyledBackground, false);
    } else {
        /* Un QWidget nu ne peint PAS son fond QSS (meme #id-scope) tant que
         * WA_StyledBackground n'est pas actif (cf. container_set_style_impl). Sans
         * cet attribut, la CouleurFond du CadreExtérieur restait invisible : le
         * fond peint provenait du QLineEdit interne, pas du wrapper. */
        q->setAttribute(Qt::WA_StyledBackground, true);
        q->setStyleSheet("#" + id + " { " + body + "}");
    }
    /* Le CadreExtérieur est le fond de la boite entiere ; la ZoneSaisie (QLineEdit)
     * se superpose. Quand le wrapper porte un fond mais que la ZoneSaisie n'a pas de
     * CouleurFond propre (owl_bg absent), le fond opaque par defaut du QLineEdit
     * masquerait le CadreExtérieur : on rend alors le QLineEdit transparent pour que
     * le fond du cadre transparaisse. Une CouleurFond explicite de ZoneSaisie prime. */
    if (QLineEdit* childLE = q->findChild<QLineEdit*>()) {
        bool zoneHasBg = childLE->property("owl_bg").isValid();
        if (bg >= 0 && !zoneHasBg)
            childLE->setStyleSheet("QLineEdit { background: transparent; }");
    }
}

/* ============================================================
 *  — libelle integre du Champ Combo (composite {QLabel|QComboBox}).
 * Calque strict du wrapper Saisie (g_lineedit_label_wrappers) mais scope aux
 * QComboBox. Meme mecanique : etat (texte/largeur) memorise en proprietes sur
 * le QComboBox, wrapper HBox [QLabel|combo] materialise a la demande, wrapper
 * « force » jamais defait (support du CadreExtérieur du styleCombo).
 * ============================================================ */
static QHash<QWidget*, QWidget*> g_combo_label_wrappers;
static unsigned g_combo_wrap_seq = 0;

/* Reparente `cmb` dans un HBox [QLabel|combo] a la place du combo nu (layout
 * parent ou geometrie absolue). Calque de owl_lineedit_build_wrapper. */
static QWidget* owl_combo_build_wrapper(QComboBox* cmb, const QString& text, int width) {
    QWidget* parent = cmb->parentWidget();
    QLayout* ply    = parent ? parent->layout() : nullptr;
    QRect    geo    = cmb->geometry();
    bool     vis    = cmb->isVisible();
    bool     hasLabel = !text.isEmpty();

    int boxIdx = -1, boxStretch = 0;
    int gRow = 0, gCol = 0, gRs = 1, gCs = 1;
    Qt::Alignment align = Qt::Alignment();
    bool inBox = false, inGrid = false;
    if (auto* box = qobject_cast<QBoxLayout*>(ply)) {
        boxIdx = box->indexOf(cmb);
        if (boxIdx >= 0) {
            inBox = true; boxStretch = box->stretch(boxIdx);
            align = box->itemAt(boxIdx)->alignment();
        }
    } else if (auto* grid = qobject_cast<QGridLayout*>(ply)) {
        int gi = grid->indexOf(cmb);
        if (gi >= 0) {
            inGrid = true;
            grid->getItemPosition(gi, &gRow, &gCol, &gRs, &gCs);
            align = grid->itemAt(gi)->alignment();
        }
    }
    if (inBox || inGrid) ply->removeWidget(cmb);

    QWidget* wrapper = new QWidget(parent);
    wrapper->setObjectName(QString("owl_combo_wrap_%1").arg(g_combo_wrap_seq++));
    QHBoxLayout* hb = new QHBoxLayout(wrapper);
    hb->setContentsMargins(0, 0, 0, 0);
    hb->setSpacing(hasLabel ? 6 : 0);
    QLabel* lbl = new QLabel(text, wrapper);
    lbl->setObjectName("owl_combo_label");
    if (hasLabel) owl_label_apply_width(lbl, width);
    else          lbl->setFixedWidth(0);
    if (!hasLabel) lbl->hide();
    hb->addWidget(lbl, 0);
    hb->addWidget(cmb, 1);   /* reparente `cmb` dans le wrapper */
    /* Le wrapper remplace le combo nu : il doit se dimensionner exactement
     * comme lui (meme politique), et le combo doit REMPLIR la hauteur que le
     * wrapper recoit — sa geometrie absolue en dispLibre, sa cellule sous
     * layout. Sans l'Expanding vertical, le QComboBox garde sa politique Fixed :
     * le HBox lui sert son sizeHint et rien d'autre, donc la hauteur declaree
     * etait plafonnee a la taille naturelle du champ (mesure : 40 declares,
     * 23 rendus, et le champ recentre 8 px plus bas dans son emplacement).
     * Meme correctif que le wrapper Saisie, meme raison. */
    wrapper->setSizePolicy(cmb->sizePolicy());
    cmb->setSizePolicy(cmb->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);

    if (inBox) {
        QBoxLayout* box = qobject_cast<QBoxLayout*>(ply);
        if (align != Qt::Alignment()) box->insertWidget(boxIdx, wrapper, boxStretch, align);
        else                          box->insertWidget(boxIdx, wrapper, boxStretch);
    } else if (inGrid) {
        QGridLayout* grid = qobject_cast<QGridLayout*>(ply);
        grid->addWidget(wrapper, gRow, gCol, gRs, gCs, align);
    } else {
        wrapper->setGeometry(geo);
    }
    if (vis) { wrapper->show(); if (hasLabel) lbl->show(); cmb->show(); }
    return wrapper;
}

static QWidget* owl_combo_create_wrapper_for(QComboBox* cmb, const QString& text,
                                             int width) {
    if (!cmb->property("owl_clbl_destroy_hooked").toBool()) {
        cmb->setProperty("owl_clbl_destroy_hooked", true);
        QObject::connect(cmb, &QObject::destroyed, [](QObject* o) {
            g_combo_label_wrappers.remove(static_cast<QWidget*>(o));
        });
    }
    QWidget* wrapper = owl_combo_build_wrapper(cmb, text, width);
    g_combo_label_wrappers.insert(cmb, wrapper);
    return wrapper;
}

/* Cree/met a jour le libelle depuis l'etat memorise sur le QComboBox. Un wrapper
 * « force » (owl_wrap_forced) n'est jamais defait ; on cache seulement le QLabel. */
static void owl_combo_refresh_label(QComboBox* cmb) {
    QString text = cmb->property("owl_clbl_text").toString();
    int width    = cmb->property("owl_clbl_width").toInt();
    bool want    = !text.isEmpty();
    QWidget* wrapper = g_combo_label_wrappers.value(cmb, nullptr);

    if (wrapper) {
        QHBoxLayout* hb = qobject_cast<QHBoxLayout*>(wrapper->layout());
        QLabel* lbl = (hb && hb->count() > 0)
                      ? qobject_cast<QLabel*>(hb->itemAt(0)->widget()) : nullptr;
        if (want) {
            if (lbl) { lbl->setText(text); owl_label_apply_width(lbl, width); lbl->show(); }
            if (hb) hb->setSpacing(6);
        } else {
            /* pas de defait automatique : le wrapper survit (CadreExtérieur). */
            if (lbl) { lbl->hide(); lbl->setFixedWidth(0); }
            if (hb) hb->setSpacing(0);
        }
    } else if (want) {
        owl_combo_create_wrapper_for(cmb, text, width);
    }
}

extern "C" void owl_ui_combo_set_label_text(OwlControl* cmb, const char* text_utf8) {
    QComboBox* c = qobject_cast<QComboBox*>(reinterpret_cast<QObject*>(cmb));
    if (!c) return;
    c->setProperty("owl_clbl_text", QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    owl_combo_refresh_label(c);
}

extern "C" void owl_ui_combo_set_label_width(OwlControl* cmb, int width) {
    QComboBox* c = qobject_cast<QComboBox*>(reinterpret_cast<QObject*>(cmb));
    if (!c) return;
    c->setProperty("owl_clbl_width", width);
    owl_combo_refresh_label(c);
}

/* always-wrap : materialise (si besoin) le conteneur du combo et le marque
 * « force ». Le CadreExtérieur d'un styleCombo peint sur ce wrapper. */
extern "C" OwlControl* owl_ui_combo_ensure_wrapper(OwlControl* cmb) {
    QComboBox* c = qobject_cast<QComboBox*>(reinterpret_cast<QObject*>(cmb));
    if (!c) return nullptr;
    QWidget* wrapper = g_combo_label_wrappers.value(c, nullptr);
    if (!wrapper) {
        QString text = c->property("owl_clbl_text").toString();
        int width    = c->property("owl_clbl_width").toInt();
        wrapper = owl_combo_create_wrapper_for(c, text, width);
    }
    if (wrapper) wrapper->setProperty("owl_wrap_forced", true);
    return reinterpret_cast<OwlControl*>(wrapper);
}

extern "C" OwlControl* owl_ui_combo_get_label(OwlControl* cmb) {
    QComboBox* c = qobject_cast<QComboBox*>(reinterpret_cast<QObject*>(cmb));
    if (!c) return nullptr;
    QWidget* wrapper = g_combo_label_wrappers.value(c, nullptr);
    if (!wrapper) return nullptr;
    QHBoxLayout* hb = qobject_cast<QHBoxLayout*>(wrapper->layout());
    if (!hb || hb->count() == 0) return nullptr;
    QLabel* lbl = qobject_cast<QLabel*>(hb->itemAt(0)->widget());
    return reinterpret_cast<OwlControl*>(lbl);
}

/*  — representant de layout d'un widget : le conteneur qui doit entrer
 * dans le layout parent a la place du widget nu (wrapper composite du Saisie a
 * libelle, [QLabel|QLineEdit], ou du Combo a libelle). Sans cette resolution, un
 * champ enveloppe sous Alignement=Étire verrait le widget NU reparente dans le
 * layout fenetre, echappant a son wrapper. */
static QWidget* owl_widget_layout_rep(QWidget* inner) {
    if (!inner) return nullptr;
    QWidget* rep = inner;
    if (QWidget* le = g_lineedit_label_wrappers.value(rep, nullptr)) rep = le;
    if (QWidget* cw = g_combo_label_wrappers.value(rep, nullptr))    rep = cw;   /*  */
    return (rep == inner) ? nullptr : rep;
}

extern "C" const char* owl_ui_lineedit_get_text(OwlControl* edit) {
    if (!edit) return "";
    QObject* obj = reinterpret_cast<QObject*>(edit);
    if (auto* te = qobject_cast<QPlainTextEdit*>(obj)) {
        g_lineedit_buf = te->toPlainText().toUtf8();
    } else {
        QLineEdit* le = reinterpret_cast<QLineEdit*>(edit);
        g_lineedit_buf = le->text().toUtf8();
    }
    return g_lineedit_buf.constData();
}

/* ====== Event filter dispatcher pour focusIn/focusOut/closeEvent ======
 *
 * Qt n'expose pas de signal direct pour focus in/out (QLineEdit) ni pour
 * la fermeture (QMainWindow). On installe un eventFilter unique qui
 * dispatch via une map widget → callbacks.
 *
 * Lifetime : le dispatcher est un singleton heap-alloue, jamais detruit.
 * Les entrees de la map fuient si le widget est detruit avant la fenetre
 * — acceptable pour le POC, a fixer en V2 via QObject::destroyed signal. */

/*  — clavier : etat des touches + mapping Qt::Key -> OWL.
 *
 * g_keys_down : set des codes OWL actuellement enfonces (alimente par le
 * filtre clavier ci-dessous). ToucheEnfoncee le lit via owl_ui_key_is_down.
 *
 * Mapping QKeyEvent -> code OWL (owl_qt_key_to_owl) :
 *   - touche speciale (Qt::Key >= 0x01000000) : le code OWL EST la valeur
 *     Qt::Key (les constantes tk* ont volontairement ces memes valeurs).
 *   - touche textuelle (lettre/chiffre/espace) : code ASCII issu du texte
 *     tape (ou de key() pour les majuscules ASCII), identique a Asc("X").
 *     Les QKeyEvent de owl_ui_post_text_input portent key()==0 + un texte :
 *     on derive alors le code du 1er caractere. */
static QSet<int> g_keys_down;

static int owl_qt_key_to_owl(const QKeyEvent* ev) {
    int qk = ev->key();
    /* Touche speciale : la valeur Qt::Key est directement le code tk*. */
    if (qk >= 0x01000000) return qk;
    /* Espace : code ASCII 32 (= tkEspace). */
    if (qk == Qt::Key_Space) return 0x20;
    /* Touche textuelle avec key() ASCII connu (A-Z, 0-9 majuscule). */
    QString txt = ev->text();
    if (!txt.isEmpty()) {
        QChar c = txt.at(0);
        ushort u = c.unicode();
        if (u > 0 && u < 128) return (int)u;   /* code ASCII reel de la frappe */
    }
    if (qk > 0 && qk < 128) return qk;          /* fallback : key() ASCII */
    return qk;
}

/*  — pont vers le moteur num de stdlib_ui : (re)formate (focus-out) ou
 * de-formate (focus-in) l'affichage d'un Champ Saisie num a FormatNombre
 * effectif. Hook INTERNE, distinct des callbacks user surEntree/surSortie :
 * il tire pour tout QLineEdit num quel que soit le cablage OWL du champ. */
extern "C" void owl_ui_input_num_on_focus(OwlControl* w, int focusIn);

struct FocusCallbacks {
    OwlCallback on_entry = nullptr;  void* entry_data = nullptr;
    OwlCallback on_exit  = nullptr;  void* exit_data  = nullptr;
};
/*  — clic/double-clic generiques par event filter. Les champs bases
 * sur un widget Qt sans signal `clicked` (Libelle, Selecteur, Interrupteur,
 * ZoneRepetee) declarent pourtant surClic/surDoubleClic : le filtre les rend
 * reels sans imposer une sous-classe par famille de champ. */
struct ClickCallbacks {
    OwlCallback on_click     = nullptr;  void* click_data = nullptr;
    OwlCallback on_dbl_click = nullptr;  void* dbl_data   = nullptr;
};
struct CloseCallback {
    OwlCallback     on_close = nullptr;  void* data       = nullptr;
    /* Interroge avant fermeture ; 0 = fermeture refusee (cf. owl_ui.h). */
    OwlCloseQueryCb on_query = nullptr;  void* query_data = nullptr;
};
/*  — callbacks clavier (1 entier : code touche). on_down/on_up sont
 * NULL si l'objet n'a pas son propre handler (cas d'un champ enregistre
 * uniquement pour permettre la remontee a la fenetre). */
struct KeyCallbacks {
    OwlEventCb1i on_down = nullptr;  void* down_data = nullptr;
    OwlEventCb1i on_up   = nullptr;  void* up_data   = nullptr;
};
/*  — callback resize fenetre (surChangementTaille). last_w/last_h
 * memorisent la derniere taille notifiee : on ne tire que sur changement
 * effectif (les QResizeEvent redondants de l'exposition initiale, de meme
 * dimension que la geometrie deja posee, sont ignores). */
struct ResizeCallback {
    OwlEventCb2i on_resize = nullptr;  void* data = nullptr;
    int last_w = -1;  int last_h = -1;
};

class OwlEventDispatcher : public QObject {
public:
    QHash<QObject*, FocusCallbacks> focus_map;
    QHash<QObject*, ClickCallbacks> click_map;
    QHash<QObject*, CloseCallback>  close_map;
    /* key_map : entrees pour les champs (handler propre OU vide pour bubble)
     * ET pour les fenetres (QMainWindow -> handler fenetre). */
    QHash<QObject*, KeyCallbacks>   key_map;
    /*  — resize_map : QMainWindow -> handler surChangementTaille. */
    QHash<QObject*, ResizeCallback> resize_map;

    /* Resout les callbacks clavier de la fenetre racine d'un widget enfant
     * (modele bubble-up : un champ sans handler propre delegue au handler
     * fenetre). Retourne nullptr si aucun handler fenetre. */
    const KeyCallbacks* window_key_cb_for(QWidget* child) {
        if (!child) return nullptr;
        QWidget* top = child->window();
        if (!top) return nullptr;
        auto it = key_map.constFind(top);
        if (it != key_map.constEnd() && (it->on_down || it->on_up)) {
            return &it.value();
        }
        return nullptr;
    }

    /* Vrai si le champ qui a le focus clavier (ou un de ses ancetres sous la
     * fenetre `window`) porte le filtre clavier, avec ou sans handler propre.
     * Dans les deux cas la touche a DEJA ete traitee au niveau du champ : son
     * handler a tire, ou le filtre a lui-meme notifie le handler fenetre
     * (bubble-up). Comme le filtre ne consomme plus l'evenement, une touche
     * que le widget Qt ignore remonte la chaine de parents jusqu'a la
     * fenetre ; sans ce test, le handler fenetre tirerait une seconde fois.
     * On remonte la chaine parente car le focus peut se poser sur un widget
     * interne du champ (viewport, editeur de cellule). */
    bool focused_field_handled_keys(QWidget* window) {
        QWidget* f = QApplication::focusWidget();
        if (!f || !window || f == window) return false;
        for (QWidget* w = f; w && w != window; w = w->parentWidget()) {
            if (key_map.constFind(w) != key_map.constEnd()) return true;
        }
        return false;
    }

    /*  — cherche le bouton par defaut (btnValider) de la fenetre
     * top-level du champ et le clique. Lazy au moment de la frappe : pas de
     * registre a maintenir. Retourne true si un bouton a ete clique. Un champ
     * qui porte son propre handler returnPressed (owl_has_return_handler) ne
     * doit pas declencher le bouton par defaut : Entree lui appartient. */
    bool try_click_default_button(QWidget* field) {
        if (!field) return false;
        if (field->property("owl_has_return_handler").toBool()) return false;
        QWidget* top = field->window();
        if (!top) return false;
        const QList<QPushButton*> btns = top->findChildren<QPushButton*>();
        for (QPushButton* b : btns) {
            if (b && b->isDefault() && b->isEnabled() && b->isVisible()) {
                b->click();
                return true;
            }
        }
        return false;
    }

    /* Dispatch press/release vers un KeyCallbacks. Retourne true si un
     * handler a tire (l'event est alors consomme par l'appelant). */
    bool fire_key(const KeyCallbacks* kc, QObject* obj, bool down, int code) {
        if (!kc) return false;
        if (down && kc->on_down) {
            kc->on_down(reinterpret_cast<OwlControl*>(obj), kc->down_data, code);
            return true;
        }
        if (!down && kc->on_up) {
            kc->on_up(reinterpret_cast<OwlControl*>(obj), kc->up_data, code);
            return true;
        }
        return false;
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        QEvent::Type t = ev->type();
        if (t == QEvent::FocusIn) {
            /*  — reaffiche la valeur brute editable AVANT surEntree :
             * un handler user lit alors la forme non-formatee (celle qu'il
             * editera), coherent avec l'affichage. No-op hors num a format. */
            owl_ui_input_num_on_focus(reinterpret_cast<OwlControl*>(obj), 1);
            auto it = focus_map.constFind(obj);
            if (it != focus_map.constEnd() && it->on_entry) {
                it->on_entry(reinterpret_cast<OwlControl*>(obj), it->entry_data);
            }
        } else if (t == QEvent::FocusOut) {
            /*  — reformate l'affichage AVANT surSortie : ..Valeur relu
             * dans le handler (et ensuite) rend la valeur arrondie/formatee,
             * pas le brut tape. No-op hors num a format. */
            owl_ui_input_num_on_focus(reinterpret_cast<OwlControl*>(obj), 0);
            auto it = focus_map.constFind(obj);
            if (it != focus_map.constEnd() && it->on_exit) {
                it->on_exit(reinterpret_cast<OwlControl*>(obj), it->exit_data);
            }
        } else if (t == QEvent::MouseButtonRelease) {
            /*  — surClic au RELACHEMENT, comme le Champ Image
             * (OwlImageLabel::mouseReleaseEvent) : semantique de clic humain
             * (appuyer puis relacher sur le champ), pas de simple appui. */
            auto it = click_map.constFind(obj);
            if (it != click_map.constEnd() && it->on_click
                && static_cast<QMouseEvent*>(ev)->button() == Qt::LeftButton) {
                it->on_click(reinterpret_cast<OwlControl*>(obj), it->click_data);
            }
        } else if (t == QEvent::MouseButtonDblClick) {
            auto it = click_map.constFind(obj);
            if (it != click_map.constEnd() && it->on_dbl_click
                && static_cast<QMouseEvent*>(ev)->button() == Qt::LeftButton) {
                it->on_dbl_click(reinterpret_cast<OwlControl*>(obj), it->dbl_data);
            }
        } else if (t == QEvent::Close) {
            auto it = close_map.constFind(obj);
            if (it != close_map.constEnd() && it->on_query) {
                if (!it->on_query(reinterpret_cast<OwlControl*>(obj),
                                  it->query_data)) {
                    /* Fermeture refusee. ignore() suffit a laisser la fenetre
                     * visible ET a faire rendre false a QWidget::close, mais il
                     * FAUT consommer l'event : sinon QWidget::closeEvent, dont
                     * l'implementation par defaut accepte, le re-accepterait. */
                    ev->ignore();
                    return true;
                }
            }
            if (it != close_map.constEnd() && it->on_close) {
                it->on_close(reinterpret_cast<OwlControl*>(obj), it->data);
            }
        } else if (t == QEvent::Resize) {
            /*  — surChangementTaille fenetre. On lit la nouvelle
             * taille et on ne notifie que si elle change reellement (filtre
             * les resize redondants : exposition initiale, relayout). */
            auto it = resize_map.find(obj);
            if (it != resize_map.end() && it->on_resize) {
                QResizeEvent* re = static_cast<QResizeEvent*>(ev);
                int w = re->size().width();
                int h = re->size().height();
                if (w > 0 && h > 0 && (w != it->last_w || h != it->last_h)) {
                    it->last_w = w;
                    it->last_h = h;
                    it->on_resize(reinterpret_cast<OwlControl*>(obj),
                                  it->data, w, h);
                }
            }
        } else if (t == QEvent::KeyPress || t == QEvent::KeyRelease) {
            QKeyEvent* ke = static_cast<QKeyEvent*>(ev);
            /*  — champ num a FormatNombre effectif (modele strict config) :
             * remappe A LA FRAPPE toute touche decimale (. OU ,, y compris le point
             * du pave numerique) vers le separateur decimal CONFIGURE, avec regle
             * "un seul decimal". Le separateur de milliers ne se tape pas -> jamais
             * insere par ce chemin. Consomme la touche (insertion faite ici) ; les
             * chiffres et le reste continuent leur route normale (le validateur
             * lenient bloque lettres/paste). setText programmatique (reformat au
             * blur) ne passe PAS par ici -> forme formatee preservee. */
            if (t == QEvent::KeyPress) {
                if (QLineEdit* nle = qobject_cast<QLineEdit*>(obj)) {
                    QVariant decv = nle->property("owl_num_fmt_dec");
                    QString txt = ke->text();
                    if (decv.isValid() && txt.length() == 1 &&
                        (txt.at(0) == QChar('.') || txt.at(0) == QChar(','))) {
                        QString dec = decv.toString();
                        QString sel = nle->selectedText();
                        /* un seul decimal : bloque si un decimal subsiste hors
                         * selection (la selection sera remplacee par l'insert). */
                        bool hasDec = nle->text().contains(dec) &&
                                      !(sel.contains(dec) &&
                                        nle->text().count(dec) == sel.count(dec));
                        if (!hasDec) nle->insert(dec);
                        return true;  /* touche consommee */
                    }
                }
            }
            /* Qt repete les KeyPress en maintien (autoRepeat) : on ne compte
             * que la 1ere pression et le relachement (semantique appuyee/
             * relachee humaine, pas de spam de repetition). */
            bool down = (t == QEvent::KeyPress);
            int code = owl_qt_key_to_owl(ke);
            if (down && !ke->isAutoRepeat()) g_keys_down.insert(code);
            else if (!down) g_keys_down.remove(code);

            auto it = key_map.constFind(obj);
            bool isWindow = (qobject_cast<QMainWindow*>(obj) != nullptr);
            if (it != key_map.constEnd() && (it->on_down || it->on_up)) {
                /* L'objet a son propre handler (champ avec surTouche* ou
                 * fenetre). NOTIFIER SANS CONSOMMER : le handler est prevenu,
                 * puis l'evenement poursuit sa route vers le widget Qt, qui
                 * garde son comportement natif (la frappe s'affiche dans une
                 * Saisie, les fleches naviguent dans une Table). Consommer ici
                 * rendait tout champ portant surToucheAppuyee inutilisable a
                 * la saisie (..Valeur restait vide).
                 * La fenetre, elle, ne doit pas retirer sur une touche deja
                 * traitee au niveau du champ focuse : c'est le role du test
                 * focused_field_handled_keys (le champ ne consommant plus, une
                 * touche qu'il ignore remonte jusqu'ici par la propagation
                 * Qt). */
                bool field_first = isWindow
                    && focused_field_handled_keys(qobject_cast<QWidget*>(obj));
                if (!field_first && !(down && ke->isAutoRepeat())) {
                    fire_key(&it.value(), obj, down, code);
                }
            } else if (!isWindow) {
                /* Champ SANS handler propre : remonte au handler fenetre.
                 * Sans consommer non plus — le champ garde son comportement
                 * natif (une Saisie continue de recevoir la frappe, un Bouton
                 * reste activable a l'espace) alors meme que la fenetre ecoute
                 * le clavier. Le second passage du meme evenement par le
                 * filtre de la fenetre est neutralise par
                 * focused_field_handled_keys. */
                const KeyCallbacks* wkc =
                    window_key_cb_for(qobject_cast<QWidget*>(obj));
                if (wkc) {
                    if (!(down && ke->isAutoRepeat())) {
                        fire_key(wkc, obj, down, code);
                    }
                    return QObject::eventFilter(obj, ev);
                }
                /*  — fallback Entree -> bouton par defaut. La fenetre
                 * OWL est un QMainWindow (pas un QDialog) : Qt ne route pas
                 * Entree vers le bouton btnValider automatiquement quand un
                 * champ a le focus. On le fait ici, en dernier recours, apres
                 * avoir laisse le champ (surTouche*) et la fenetre traiter la
                 * touche. Un champ avec son propre handler returnPressed
                 * (owl_has_return_handler) est exclu : c'est lui qui doit
                 * reagir, sinon double-declenchement et returnPressed jamais
                 * emis (on consommerait avant l'emission Qt). */
                if (down && (ke->key() == Qt::Key_Return ||
                             ke->key() == Qt::Key_Enter) &&
                    !ke->isAutoRepeat()) {
                    if (try_click_default_button(qobject_cast<QWidget*>(obj))) {
                        return true;
                    }
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }

    /*  — purge les entrees focus_map/close_map dont la cle est `root`
     * ou un de ses descendants QObject. A appeler quand une fenetre se ferme
     * (Ouvre imbrique) : sans ca, les QObject* cles deviennent dangling apres
     * deleteLater et les QHash gardent des entrees mortes (fuite cumulative a
     * chaque cycle Ouvre/Ferme imbrique, cf. ui_qt.cpp ~1415). */
    void purge_for(QObject* root) {
        if (!root) return;
        QWidget* rootW = qobject_cast<QWidget*>(root);
        /* Un objet est concerne s'il EST `root` ou un widget descendant de
         * `root` (les champs vivent dans le central widget de la fenetre). */
        auto concerns = [root, rootW](QObject* k) -> bool {
            if (k == root) return true;
            if (!rootW) return false;
            QWidget* kw = qobject_cast<QWidget*>(k);
            return kw && rootW->isAncestorOf(kw);
        };
        for (auto it = focus_map.begin(); it != focus_map.end(); ) {
            if (concerns(it.key())) it = focus_map.erase(it);
            else                    ++it;
        }
        for (auto it = click_map.begin(); it != click_map.end(); ) {
            if (concerns(it.key())) it = click_map.erase(it);
            else                    ++it;
        }
        for (auto it = close_map.begin(); it != close_map.end(); ) {
            if (concerns(it.key())) it = close_map.erase(it);
            else                    ++it;
        }
        for (auto it = resize_map.begin(); it != resize_map.end(); ) {
            if (concerns(it.key())) it = resize_map.erase(it);
            else                    ++it;
        }
        /*  — purge l'etat clavier a la fermeture : sans ca, une touche
         * relachee hors fenetre (ou non vue par le filtre detruit) resterait
         * fantome dans g_keys_down et fausserait ToucheEnfoncee de la fenetre
         * suivante (etat inter-fenetres). */
        g_keys_down.clear();
    }
};

static OwlEventDispatcher* g_dispatcher = nullptr;
static OwlEventDispatcher* dispatcher() {
    if (!g_dispatcher) g_dispatcher = new OwlEventDispatcher();
    return g_dispatcher;
}

/*  — voir forward declaration plus haut. No-op si pas de dispatcher
 * (aucun handler focus/close jamais cable). */
static void purge_dispatcher_for(QObject* root) {
    if (g_dispatcher) g_dispatcher->purge_for(root);
}

/*  — voir forward declaration plus haut. */
static void install_owl_event_filter(QObject* obj) {
    if (obj) obj->installEventFilter(dispatcher());
}

/* ----- Saisie : signaux Qt natifs (Validation, Modification) ----- */

/*  — trace opt-in du cablage (build-time) des events Saisie.
 * Permet de confirmer, OWL_TRACE_UI=1, que le connect Qt a bien eu lieu
 * pour un signal donne (diagnostic « surAChaqueModification muet » : si la
 * ligne `connect onEachChange SAI_X` n'apparait pas, le bloc PROCEDURE n'a
 * pas ete trouve par find_event ; si elle apparait mais qu'aucun
 * `textChanged` ne suit a la frappe, le grab clavier n'est pas sur le champ). */
static void trace_ui_connect(const char* signalName, QWidget* w) {
    static int trace_cached = -1;
    if (trace_cached < 0) {
        trace_cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
    }
    if (!trace_cached) return;
    QByteArray nm = w ? w->objectName().toUtf8() : QByteArray();
    fprintf(stderr, "[ui-event] connect %s %s\n",
            signalName, nm.isEmpty() ? "?" : nm.constData());
    fflush(stderr);
}

extern "C" void owl_ui_lineedit_on_validation(OwlControl* edit,
                                               OwlCallback cb, void* userdata) {
    if (!edit || !cb) return;
    QLineEdit* le = reinterpret_cast<QLineEdit*>(edit);
    trace_ui_connect("onValidation", le);
    /*  — marque le champ comme porteur d'un handler returnPressed pour
     * que le fallback Entree -> bouton par defaut ne le court-circuite pas :
     * Entree doit ici laisser Qt emettre returnPressed (sinon il serait
     * consomme par l'eventFilter avant l'emission, et le handler ne tirerait
     * jamais). */
    le->setProperty("owl_has_return_handler", true);
    owl_track_conn(le,
        QObject::connect(le, &QLineEdit::returnPressed, le, [cb, userdata, edit]() {
            cb(edit, userdata);
        }));
}

extern "C" void owl_ui_lineedit_on_change(OwlControl* edit,
                                           OwlCallback cb, void* userdata) {
    if (!edit || !cb) return;
    QLineEdit* le = reinterpret_cast<QLineEdit*>(edit);
    trace_ui_connect("onEachChange", le);
    owl_track_conn(le,
    QObject::connect(le, &QLineEdit::textChanged, le,
                     [cb, userdata, edit, le](const QString& s) {
        /*  addendum — trace opt-in : voir si les frappes
         * utilisateur remontent jusqu'au handler OWL. */
        static int trace_cached = -1;
        if (trace_cached < 0) {
            trace_cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
        }
        if (trace_cached) {
            QByteArray nm = le->objectName().toUtf8();
            QByteArray tx = s.toUtf8();
            fprintf(stderr, "[ui-event] textChanged %s text='%s'\n",
                    nm.isEmpty() ? "?" : nm.constData(),
                    tx.constData());
            fflush(stderr);
        }
        cb(edit, userdata);
    }));
}

/* ----- Champ : focus in/out et clic/double-clic via event filter -----
 *
 * Aucun de ces quatre hooks n'a besoin d'un type de widget precis : le filtre
 * lit QEvent::FocusIn/FocusOut/MouseButtonRelease/MouseButtonDblClick, que tout
 * QWidget produit. Ils servent donc les champs dont le backend n'offre pas de
 * signal Qt equivalent (Libelle, Table, ZoneRepetee) autant que la Saisie. */

extern "C" void owl_ui_control_on_entry(OwlControl* c,
                                         OwlCallback cb, void* userdata) {
    if (!c || !cb) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    trace_ui_connect("onEntry", w);
    auto& fc = dispatcher()->focus_map[w];
    fc.on_entry = cb;
    fc.entry_data = userdata;
    w->installEventFilter(dispatcher());
}

extern "C" void owl_ui_control_on_exit(OwlControl* c,
                                        OwlCallback cb, void* userdata) {
    if (!c || !cb) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    trace_ui_connect("onExit", w);
    auto& fc = dispatcher()->focus_map[w];
    fc.on_exit = cb;
    fc.exit_data = userdata;
    w->installEventFilter(dispatcher());
}

extern "C" void owl_ui_control_on_clic(OwlControl* c,
                                        OwlCallback cb, void* userdata) {
    if (!c || !cb) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    trace_ui_connect("onClick", w);
    auto& cc = dispatcher()->click_map[w];
    cc.on_click = cb;
    cc.click_data = userdata;
    w->installEventFilter(dispatcher());
}

extern "C" void owl_ui_control_on_double_clic(OwlControl* c,
                                               OwlCallback cb, void* userdata) {
    if (!c || !cb) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    trace_ui_connect("onDoubleClick", w);
    auto& cc = dispatcher()->click_map[w];
    cc.on_dbl_click = cb;
    cc.dbl_data = userdata;
    w->installEventFilter(dispatcher());
}

/* ----- Fenetre : closeEvent via event filter ----- */

extern "C" void owl_ui_window_on_close(OwlWindow* w,
                                        OwlCallback cb, void* userdata) {
    if (!w || !cb) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    auto& cc = dispatcher()->close_map[mw];
    cc.on_close = cb;
    cc.data = userdata;
    mw->installEventFilter(dispatcher());
}

extern "C" void owl_ui_window_on_close_query(OwlWindow* w,
                                              OwlCloseQueryCb cb, void* userdata) {
    if (!w || !cb) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    auto& cc = dispatcher()->close_map[mw];
    cc.on_query = cb;
    cc.query_data = userdata;
    mw->installEventFilter(dispatcher());
}

/* -----  — clavier : filtres + injection ----- */

/* Installe le filtre clavier sur un champ. Si on_down/on_up sont non-NULL, le
 * champ a son propre handler (surTouche*). Si TOUS deux NULL, on installe quand
 * meme le filtre (entree key_map "vide") pour que les touches de ce champ
 * remontent au handler fenetre au lieu d'etre avalees par le widget Qt
 * (modele bubble-up de la propagation). */
extern "C" void owl_ui_install_key_filter(OwlControl* c,
                                          OwlEventCb1i on_down, void* down_data,
                                          OwlEventCb1i on_up,   void* up_data) {
    if (!c) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    auto& kc = dispatcher()->key_map[w];
    kc.on_down = on_down;  kc.down_data = down_data;
    kc.on_up   = on_up;    kc.up_data   = up_data;
    w->installEventFilter(dispatcher());
    /*  : un widget porteur d'un VRAI handler clavier doit etre
     * focusable, sinon il ne recoit jamais de KeyPress (cas Image = canvas de
     * jeu, NoFocus par defaut). Focus au clic ET au tab (StrongFocus). On ne
     * promeut que les widgets NoFocus (jamais ecraser une politique existante,
     * ex. QLineEdit) et seulement s'il y a un handler (pas les entrees
     * bubble-up vides). */
    if ((on_down || on_up) && w->focusPolicy() == Qt::NoFocus) {
        w->setFocusPolicy(Qt::StrongFocus);
    }
}

/* Installe le filtre clavier sur la fenetre (handler fenetre + cible du
 * bubble-up). */
extern "C" void owl_ui_install_window_key_filter(OwlWindow* win,
                                          OwlEventCb1i on_down, void* down_data,
                                          OwlEventCb1i on_up,   void* up_data) {
    if (!win) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(win);
    auto& kc = dispatcher()->key_map[mw];
    kc.on_down = on_down;  kc.down_data = down_data;
    kc.on_up   = on_up;    kc.up_data   = up_data;
    mw->installEventFilter(dispatcher());
}

/*  — installe le filtre resize fenetre (surChangementTaille).
 * Initialise last_w/last_h a la taille courante de la fenetre pour que le
 * resize d'exposition (meme dimension) ne declenche pas l'event. */
extern "C" void owl_ui_install_window_resize_filter(OwlWindow* win,
                                          OwlEventCb2i cb, void* userdata) {
    if (!win) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(win);
    auto& rc = dispatcher()->resize_map[mw];
    rc.on_resize = cb;  rc.data = userdata;
    rc.last_w = mw->width();
    rc.last_h = mw->height();
    mw->installEventFilter(dispatcher());
}

/*  — applique le mode d'affichage : 0 normal, 1 reduit, 2 agrandi. */
extern "C" void owl_ui_window_set_display_mode(OwlWindow* w, int mode) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    switch (mode) {
        case 1:  mw->showMinimized(); break;
        case 2:  mw->showMaximized(); break;
        default: mw->showNormal();    break;
    }
}

/* Poste un cycle press+release d'une touche au widget focuse de la fenetre
 * (EnvoieTouche en mode reel : « envoyer une touche » = appui ET relachement,
 * comme un humain). Le code OWL est traduit en (Qt::Key, texte) pour que le
 * filtre clavier le re-mappe correctement. Le caller pompe ensuite. */
extern "C" void owl_ui_post_key(OwlWindow* win, int owl_code) {
    if (!win) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(win);
    /* Flush le focus differe (DonneFocus pose le focus via QTimer 0ms) AVANT
     * de lire focusWidget : sans ce drain, un EnvoieTouche juste apres un
     * DonneFocus ciblerait l'ancien champ focuse (ou la fenetre). */
    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents(
        QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents, 20);
    /* Cible : le widget focuse de la fenetre, sinon la fenetre elle-meme. */
    QWidget* target = mw->focusWidget();
    if (!target) target = mw;

    int qtKey;
    QString text;
    if (owl_code >= 0x01000000) {
        /* Touche speciale : owl_code EST la valeur Qt::Key, pas de texte. */
        qtKey = owl_code;
    } else if (owl_code > 0 && owl_code < 128) {
        /* Touche textuelle : owl_code = code ASCII. Qt::Key majuscule + texte
         * du caractere reel (owl_qt_key_to_owl relit le texte). */
        qtKey = (owl_code >= 'a' && owl_code <= 'z')
                ? (owl_code - 'a' + Qt::Key_A)
                : owl_code;
        text = QString(QChar((ushort)owl_code));
    } else {
        qtKey = owl_code;
    }

    QKeyEvent* press = new QKeyEvent(QEvent::KeyPress, qtKey,
                                     Qt::NoModifier, text);
    QKeyEvent* release = new QKeyEvent(QEvent::KeyRelease, qtKey,
                                       Qt::NoModifier, text);
    QCoreApplication::postEvent(target, press);
    QCoreApplication::postEvent(target, release);
}

extern "C" OwlControl* owl_ui_password_create(OwlWindow* parent,
                                              const char* placeholder_utf8,
                                              int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QLineEdit* le = new QLineEdit(central_of(parent));
    le->setEchoMode(QLineEdit::Password);
    if (placeholder_utf8) le->setPlaceholderText(QString::fromUtf8(placeholder_utf8));
    le->setGeometry(x, y, w, h);
    le->show();
    return reinterpret_cast<OwlControl*>(le);
}

extern "C" OwlControl* owl_ui_multiline_create(OwlWindow* parent,
                                                const char* placeholder_utf8,
                                                int x, int y, int w, int h) {
    if (!parent) return nullptr;
    QPlainTextEdit* te = new QPlainTextEdit(central_of(parent));
    if (placeholder_utf8) te->setPlaceholderText(QString::fromUtf8(placeholder_utf8));
    te->setGeometry(x, y, w, h);
    te->show();
    return reinterpret_cast<OwlControl*>(te);
}

/* ------- Generique ------- */

extern "C" void owl_ui_control_set_text(OwlControl* c, const char* text_utf8) {
    if (!c) return;
    QObject* obj = reinterpret_cast<QObject*>(c);
    QString s = QString::fromUtf8(text_utf8 ? text_utf8 : "");
    if (auto* te = qobject_cast<QPlainTextEdit*>(obj)) {
        te->setPlainText(s);
        return;
    }
    obj->setProperty("text", s);
}

extern "C" void owl_ui_control_set_visible(OwlControl* c, int visible) {
    if (!c) return;
    reinterpret_cast<QWidget*>(c)->setVisible(visible != 0);
}

extern "C" void owl_ui_control_set_enabled(OwlControl* c, int enabled) {
    if (!c) return;
    reinterpret_cast<QWidget*>(c)->setEnabled(enabled != 0);
}

/*  — true si `widget` est `ctrl` ou un descendant Qt de `ctrl`. Sert a
 * rattacher un focus pose sur un sous-widget (QLineEdit interne d'un Combo,
 * sous-champ d'une ZR) au controle OWL proprietaire. */
extern "C" int owl_ui_control_owns_widget(OwlControl* ctrl, OwlControl* widget) {
    if (!ctrl || !widget) return 0;
    QWidget* cw = reinterpret_cast<QWidget*>(ctrl);
    QWidget* ww = reinterpret_cast<QWidget*>(widget);
    if (cw == ww) return 1;
    return cw->isAncestorOf(ww) ? 1 : 0;
}

/*  — timers setFocus differes en vol, indexes par widget cible.
 *  reporte le setFocus via QTimer pour survivre au cycle de clic /
 * activation fenetre. Garde de vie d'origine = le `wdg` comme contexte : Qt
 * annule le timer si le WIDGET est detruit. Insuffisant : owl_ui_window_close
 * ne fait qu'un hide (mw->close()), le widget survit. Le timer tire alors
 * pendant un pump ulterieur et son setFocus emet focusChanged ->
 * surSortie/surEntree dispatches sur un binding OWL deja demonte (instance
 * fermee) -> use-after-free / contamination cross-instance.
 *
 * On garde donc une reference explicite au QTimer par widget pour pouvoir
 * l'ANNULER au teardown de la fenetre (owl_ui_window_close), avant que le
 * binding ne devienne perime. Un setFocus dont la fenetre se ferme n'a aucune
 * raison de tirer. */
static QHash<QWidget*, QTimer*> g_focus_timers;

extern "C" void owl_ui_control_set_focus(OwlControl* c) {
    if (!c) return;
    QWidget* wdg = reinterpret_cast<QWidget*>(c);
    /*  — focus differe (QTimer 0ms) plutot qu'immediat.
     *
     * Deux contextes appellent ce setter, tous deux pieges par un setFocus
     * synchrone :
     *   1. Focus initial auto : applique a l'ouverture AVANT que l'event loop
     *      n'ait active la fenetre (Windows ne pose le keyboard grab qu'une
     *      fois la fenetre activee). Un setFocus avant activation pose l'etat
     *      focus visuel (curseur) mais PAS le grab clavier reel -> les frappes
     *      ne remontent jamais a textChanged (surAChaqueModification muet).
     *   2. DonneFocus depuis un handler surClic : Qt finit son cycle de
     *      mouse-release APRES le retour du slot et re-pose le focus sur le
     *      bouton -> le focus ne revient jamais visuellement dans le champ.
     *
     * Le timer est parente a `wdg` (garde de vie widget : detruit avec lui) ET
     * tracke dans g_focus_timers pour annulation explicite au close fenetre
     *. Un setFocus deja en vol sur ce widget est remplace (re-arme
     * le tick a la prochaine iteration, pas d'empilement). */
    QTimer* existing = g_focus_timers.value(wdg, nullptr);
    if (existing) { existing->stop(); existing->deleteLater(); g_focus_timers.remove(wdg); }

    QTimer* timer = new QTimer(wdg);
    timer->setSingleShot(true);
    timer->setInterval(0);
    QObject::connect(timer, &QTimer::timeout, wdg, [wdg, timer]() {
        g_focus_timers.remove(wdg);
        timer->deleteLater();
        wdg->setFocus(Qt::OtherFocusReason);
        static int trace_cached = -1;
        if (trace_cached < 0) {
            trace_cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
        }
        if (trace_cached) {
            QByteArray nm = wdg->objectName().toUtf8();
            fprintf(stderr, "[ui-event] setFocus(deferred) %s\n",
                    nm.isEmpty() ? "?" : nm.constData());
            fflush(stderr);
        }
    });
    g_focus_timers.insert(wdg, timer);
    timer->start();
}

/*  — annule tous les setFocus differes en vol cibles sur `root` ou un
 * de ses descendants. Appele au teardown fenetre (Ferme/destroy) AVANT toute
 * invalidation : le binding OWL est encore valide ici, mais le timer ne tirera
 * plus sur l'instance fermee. */
static void cancel_focus_timers_for(QWidget* root) {
    if (!root || g_focus_timers.isEmpty()) return;
    const QList<QWidget*> targets = root->findChildren<QWidget*>();
    auto cancel = [](QWidget* w) {
        QTimer* t = g_focus_timers.value(w, nullptr);
        if (t) { t->stop(); t->deleteLater(); g_focus_timers.remove(w); }
    };
    cancel(root);
    for (QWidget* w : targets) cancel(w);
}

extern "C" void owl_ui_cancel_focus_timers(OwlWindow* w) {
    if (!w) return;
    cancel_focus_timers_for(reinterpret_cast<QMainWindow*>(w));
}

extern "C" void owl_ui_purge_window_focus(OwlWindow* w) {
    if (!w) return;
    QMainWindow* mw = reinterpret_cast<QMainWindow*>(w);
    /* (1) purge dispatcher AVANT le clearFocus : le FocusOut emis par
     * clearFocus ne doit plus trouver d'entree focus_map (sinon il
     * dispatcherait surSortie sur l'instance en cours de fermeture, deja
     * poppee du window_stack). */
    purge_dispatcher_for(mw);
    /* (2) retire le focus clavier Qt s'il est pose dans cette fenetre. Sans ca,
     * le champ fantome de la fenetre fermee garderait le focus ; l'ouverture
     * suivante le lui volerait via un FocusOut residuel sur un binding reutilise. */
    QWidget* focused = QApplication::focusWidget();
    if (focused && (focused == mw || mw->isAncestorOf(focused))) {
        focused->clearFocus();
    }
}

/*  — pompe l'event loop Qt sans bloquer : applique les events postes
 * (QKeyEvent/QMouseEvent de TestSimuleXxx, setFocus differe de DonneFocus) +
 * traite les timers a echeance. Sans display reel (offscreen), c'est ce qui
 * rend les tests UI deterministes ligne-a-ligne : a la sortie d'une native de
 * simulation, tous les signaux declenches (textChanged, focus, surEntree) ont
 * deja tire et le store/widget sont coherents. No-op si Qt jamais initialise. */
extern "C" void owl_ui_pump_events(int max_ms) {
    if (g_app == nullptr) return;
    if (max_ms <= 0) max_ms = 50;
    /* DeferredDelete inclus pour drainer les widgets en attente de destruction
     * (cycle Ferme/re-Ouvre). ExcludeUserInput evite de re-consommer un input
     * physique parasite pendant la campagne. */
    QCoreApplication::processEvents(
        QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents, max_ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

extern "C" int owl_ui_key_is_down(int owl_code) {
    /*  — boutons souris (teBoutonG/teBoutonD) : etat lu en direct
     * via QGuiApplication, pas via g_keys_down (le filtre clavier ne voit pas
     * les events souris). En headless ToucheEnfoncee court-circuite avant
     * d'arriver ici, donc ce chemin n'est emprunte qu'en mode reel. */
    if (owl_code == OWL_TK_MOUSE_LEFT)
        return (QGuiApplication::mouseButtons() & Qt::LeftButton) ? 1 : 0;
    if (owl_code == OWL_TK_MOUSE_RIGHT)
        return (QGuiApplication::mouseButtons() & Qt::RightButton) ? 1 : 0;
    return g_keys_down.contains(owl_code) ? 1 : 0;
}

/*  — poste de vrais QKeyEvent (press+release) au QLineEdit, un par
 * caractere, comme une frappe humaine : chaque caractere declenche textChanged
 * -> surAChaqueModification. C'est le VRAI chemin d'entree (pas un setText
 * programmatique ni une ecriture du store). Le caller doit pomper (owl_ui_pump_
 * events) pour que les events postes s'appliquent avant lecture de ..Valeur.
 * Pas de dependance QtTest (refus design) : QKeyEvent + postEvent suffisent. */
extern "C" void owl_ui_post_text_input(OwlControl* edit, const char* utf8) {
    if (!edit || !utf8) return;
    QWidget* w = reinterpret_cast<QWidget*>(edit);
    /* Le focus differe (QTimer 0ms) de DonneFocus n'a peut-etre pas encore
     * tire : on pose un focus synchrone ici pour que le widget cible recoive
     * bien les frappes (offscreen n'a pas de grab clavier OS a respecter). */
    w->setFocus(Qt::OtherFocusReason);
    /*  — la frappe simulee remplace le contenu (le caller a deja vide
     * le champ) : on repositionne le curseur au debut avant de poster les
     * touches. Sur un QLineEdit masque (setInputMask), setText("") laisse le
     * curseur en FIN de gabarit (position = longueur du masque) : les touches
     * s'inserent alors au-dela du dernier slot et sont perdues. Sur un champ
     * non masque, le curseur est deja a 0 apres vidage -> no-op. */
    if (QLineEdit* le = qobject_cast<QLineEdit*>(w))
        le->setCursorPosition(0);
    QString text = QString::fromUtf8(utf8);
    for (const QChar& ch : text) {
        QKeyEvent* press = new QKeyEvent(QEvent::KeyPress, 0,
                                         Qt::NoModifier, QString(ch));
        QKeyEvent* release = new QKeyEvent(QEvent::KeyRelease, 0,
                                           Qt::NoModifier, QString(ch));
        QCoreApplication::postEvent(w, press);
        QCoreApplication::postEvent(w, release);
    }
}

/*  — poste un vrai cycle clic souris (press+release) sur le widget.
 * button : 0 = gauche, 1 = droit. (x,y) en coords locales widget ; (-1,-1) =
 * centre du widget. Un vrai clic vole le focus clavier (comme un clic humain
 * sur un Bouton), ce qui permet de tester qu'un DonneFocus dans surClic repose
 * effectivement le focus (classe ). Caller pompe ensuite.
 *
 *  — `dbl` non nul poste la sequence complete d'un double-clic humain :
 * press, release, DBLCLICK, release. La detection du double-clic appartient a
 * la couche plateforme de Qt (intervalle entre deux clics natifs) : elle ne
 * s'applique PAS a des QMouseEvent postes directement au widget. Sans le
 * MouseButtonDblClick explicite, un double-clic simule se degradait donc en
 * simple clic et aucun surDoubleClic ne pouvait tirer (mesure : Champ Image,
 * surClic +2 / surDoubleClic 0). */
extern "C" void owl_ui_post_mouse_click(OwlControl* c, int button, int x, int y,
                                        int dbl) {
    if (!c) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    QPointF pos = (x < 0 || y < 0)
                  ? QPointF(w->width() / 2.0, w->height() / 2.0)
                  : QPointF(x, y);
    QPointF global = w->mapToGlobal(pos.toPoint());
    Qt::MouseButton btn = (button == 1) ? Qt::RightButton : Qt::LeftButton;
    auto post = [&](QEvent::Type type, Qt::MouseButtons held) {
        QCoreApplication::postEvent(w, new QMouseEvent(type, pos, global,
                                                       btn, held,
                                                       Qt::NoModifier));
    };
    post(QEvent::MouseButtonPress,   btn);
    post(QEvent::MouseButtonRelease, Qt::NoButton);
    if (dbl) {
        post(QEvent::MouseButtonDblClick, btn);
        post(QEvent::MouseButtonRelease,  Qt::NoButton);
    }
    /*  — un widget focusable doit recevoir le focus clavier au clic
     * (comportement humain). Sur la plateforme offscreen, un QMouseEvent poste
     * directement au widget ne declenche PAS le focus-on-click du backend
     * fenetre, et un simple show() ne rend pas la fenetre active : sans fenetre
     * active, Qt refuse setFocus et n'emet aucun focusChanged. On force donc
     * l'activation (+ une mini-pompe pour qu'elle prenne) avant de poser le
     * focus, comme owl_ui_post_text_input le fait deja pour les frappes. Le
     * focusChanged resultant synchronise focused_index cote OWL. Respecte
     * focusPolicy : un widget NoFocus ne capte pas le focus. */
    if (w->focusPolicy() != Qt::NoFocus) {
        QWidget* tl = w->window();
        if (tl && !tl->isActiveWindow()) {
            tl->activateWindow();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        }
        w->setFocus(Qt::MouseFocusReason);
    }
}

extern "C" void owl_ui_control_set_tooltip(OwlControl* c, const char* tooltip_utf8) {
    if (!c) return;
    reinterpret_cast<QWidget*>(c)->setToolTip(QString::fromUtf8(tooltip_utf8 ? tooltip_utf8 : ""));
}

extern "C" const char* owl_ui_control_get_tooltip(OwlControl* c) {
    if (!c) return "";
    g_ctrl_text_buf = reinterpret_cast<QWidget*>(c)->toolTip().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_button_set_shortcut(OwlControl* btn, const char* shortcut_utf8) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    if (!shortcut_utf8 || !*shortcut_utf8) {
        /* Raccourci vide : on ne laisse pas le bouton sans raccourci, on rend la
         * main au mnemonic du libelle ('&Lettre' -> Alt+Lettre). Sinon poser puis
         * vider ToucheRaccourci tuerait aussi l'activation Alt+lettre native. */
        b->setShortcut(QKeySequence::mnemonic(b->text()));
        return;
    }
    b->setShortcut(QKeySequence::fromString(QString::fromUtf8(shortcut_utf8)));
}

extern "C" const char* owl_ui_button_get_shortcut(OwlControl* btn) {
    if (!btn) return "";
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = b->shortcut().toString().toUtf8();
    return g_ctrl_text_buf.constData();
}

/*  — raccourci "donne le focus" pour les champs interactifs non-bouton
 * (Saisie, Combo, Liste, Selecteur, Interrupteur, Table, ZoneRepetee). A la
 * difference du Bouton (setShortcut -> clic), on installe un QShortcut de
 * contexte fenetre, parente au widget, dont l'activation pose le focus sur le
 * champ. Idempotent : reappliquer remplace la sequence ; chaine vide supprime le
 * raccourci. Le QShortcut est retrouve par objectName pour le readback. */
static QShortcut* owl_find_focus_shortcut(QWidget* w) {
    return w->findChild<QShortcut*>(QStringLiteral("owl_focus_shortcut"),
                                    Qt::FindDirectChildrenOnly);
}

/*  — Selecteur/Interrupteur/ZoneRepetee sont des QFrame CONTENEURS :
 * setFocus() sur le conteneur focalise le cadre, pas le controle interne
 * navigable (radios/checkboxes/contenu scrollable), donc fleches/espace restent
 * morts. owl_focus_target_for redirige vers l'enfant interactif ; pour les
 * champs deja focusables directs (Saisie, Combo, Liste, Table...) il renvoie le
 * widget tel quel. Defini apres les classes conteneur (bas du fichier). */
static QWidget* owl_focus_target_for(QWidget* w);

extern "C" void owl_ui_set_focus_shortcut(OwlControl* c, const char* shortcut_utf8) {
    if (!c) return;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    QShortcut* sc = owl_find_focus_shortcut(w);
    if (!shortcut_utf8 || !*shortcut_utf8) {
        if (sc) delete sc;
        return;
    }
    QKeySequence seq = QKeySequence::fromString(QString::fromUtf8(shortcut_utf8));
    if (!sc) {
        sc = new QShortcut(seq, w);
        sc->setObjectName(QStringLiteral("owl_focus_shortcut"));
        sc->setContext(Qt::WindowShortcut);
        QObject::connect(sc, &QShortcut::activated, w, [w]() {
            QWidget* target = owl_focus_target_for(w);
            if (!target) target = w;
            target->setFocus(Qt::ShortcutFocusReason);
        });
    } else {
        sc->setKey(seq);
    }
}

extern "C" const char* owl_ui_get_focus_shortcut(OwlControl* c) {
    if (!c) { g_ctrl_text_buf.clear(); return ""; }
    QWidget* w = reinterpret_cast<QWidget*>(c);
    QShortcut* sc = owl_find_focus_shortcut(w);
    if (!sc) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = sc->key().toString().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_button_set_icon(OwlControl* btn, const char* path_utf8) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    if (!path_utf8 || !*path_utf8) {
        b->setIcon(QIcon());
        b->setProperty("owl_image", QVariant());
        return;
    }
    b->setIcon(QIcon(QString::fromUtf8(path_utf8)));
    /* Stocke le path pour read-back fiable (Qt n'expose pas le path source). */
    b->setProperty("owl_image", QString::fromUtf8(path_utf8));
}

extern "C" void owl_ui_button_set_icon_rgba(OwlControl* btn,
                                            const unsigned char* rgba,
                                            int width, int height) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    QImage img = rgba_to_qimage(rgba, width, height);
    if (img.isNull()) {
        b->setIcon(QIcon());
        b->setProperty("owl_image", QVariant());
        return;
    }
    /*  : addPixmap explicite a tous les modes (Normal/Disabled/Active/
     * Selected). Sans ca, QIcon genere les modes manquants en appliquant un
     * tint monochromatique automatique du style courant, ce qui colorize
     * en noir les emoji color (pizza, coeur) dans l'etat Active du bouton
     * snapshot. */
    QPixmap pix = QPixmap::fromImage(img);
    QIcon icon;
    icon.addPixmap(pix, QIcon::Normal,   QIcon::Off);
    icon.addPixmap(pix, QIcon::Normal,   QIcon::On);
    icon.addPixmap(pix, QIcon::Active,   QIcon::Off);
    icon.addPixmap(pix, QIcon::Active,   QIcon::On);
    icon.addPixmap(pix, QIcon::Selected, QIcon::Off);
    icon.addPixmap(pix, QIcon::Selected, QIcon::On);
    /* QIcon::Disabled : Qt applique typiquement un gray-out. On garde la
     * pixmap originale aussi pour preserver les couleurs natives meme grise. */
    icon.addPixmap(pix, QIcon::Disabled, QIcon::Off);
    icon.addPixmap(pix, QIcon::Disabled, QIcon::On);
    b->setIcon(icon);
    b->setIconSize(pix.size());
    b->setProperty("owl_image", QString("<ObjImage>"));
}

/*  — swapper d'icone au survol. QIcon::Active n'est PAS declenche par
 * un simple mouse-over sur un QPushButton (Fusion ne mappe pas le hover a
 * Active) :  posait l'icone survol dans Active, elle ne s'affichait
 * jamais. On installe donc un event filter sur le bouton qui swappe l'icone
 * (setIcon) a l'entree/sortie souris.
 *
 * Cycle de vie : le filter est un QObject enfant du bouton (parente au
 * setParent), donc detruit avec lui — pas de dangling. Il lit les icones
 * base/survol depuis des properties du bouton a chaque event (toujours a jour,
 * survit a un re-set d'icone). Un seul filter par bouton (garde par property).
 * grisé (QIcon::Disabled) reste gere par Qt nativement, hors de ce filter. */
class OwlButtonHoverSwapper : public QObject {
public:
    explicit OwlButtonHoverSwapper(QPushButton* b) : QObject(b), btn(b) {}

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == btn) {
            if (ev->type() == QEvent::Enter) {
                QVariant v = btn->property("owl_icon_survol");
                if (v.isValid() && v.canConvert<QIcon>())
                    btn->setIcon(v.value<QIcon>());
            } else if (ev->type() == QEvent::Leave) {
                QVariant v = btn->property("owl_icon_base");
                if (v.isValid() && v.canConvert<QIcon>())
                    btn->setIcon(v.value<QIcon>());
            }
        }
        return QObject::eventFilter(obj, ev);
    }
private:
    QPushButton* btn;
};

/*  /  — surcharge d'icone par etat.
 *   mode 1 (grisé)  : ecrase la pixmap du seul mode QIcon::Disabled sur l'icone
 *     deja posee (Qt declenche Disabled quand le bouton est grisé — natif OK).
 *   mode 0 (survol) : stocke une icone survol dediee (property owl_icon_survol)
 *     + snapshot de l'icone de base (property owl_icon_base) et arme un event
 *     filter qui swappe l'affichage a l'entree/sortie souris. QIcon::Active
 *     n'etant pas cable au hover, c'est le filter qui fait le travail.
 * A appeler APRES owl_ui_button_set_icon_rgba (icone de base posee). */
extern "C" void owl_ui_button_set_icon_rgba_state(OwlControl* btn,
                                                  const unsigned char* rgba,
                                                  int width, int height,
                                                  int mode) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    QImage img = rgba_to_qimage(rgba, width, height);
    if (img.isNull()) return;
    QPixmap pix = QPixmap::fromImage(img);

    if (mode == 1) {
        QIcon icon = b->icon();
        icon.addPixmap(pix, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(pix, QIcon::Disabled, QIcon::On);
        b->setIcon(icon);
        return;
    }

    /* mode 0 : survol via event filter. */
    QIcon survol;
    survol.addPixmap(pix, QIcon::Normal,   QIcon::Off);
    survol.addPixmap(pix, QIcon::Normal,   QIcon::On);
    survol.addPixmap(pix, QIcon::Active,   QIcon::Off);
    survol.addPixmap(pix, QIcon::Active,   QIcon::On);
    b->setProperty("owl_icon_survol", QVariant::fromValue(survol));
    b->setProperty("owl_icon_base",   QVariant::fromValue(b->icon()));
    b->setIconSize(pix.size());
    /* Installe le filter une seule fois (garde par property). */
    if (!b->property("owl_hover_swapper").toBool()) {
        b->installEventFilter(new OwlButtonHoverSwapper(b));
        b->setProperty("owl_hover_swapper", true);
    }
}

/*  — getter test-only : 1 si une icone survol est armee sur le bouton
 * (le hover reel n'etant pas postable en offscreen). Miroir structurel du
 * cablage event filter. */
extern "C" int owl_ui_button_has_hover_icon(OwlControl* btn) {
    if (!btn) return 0;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return 0;
    QVariant v = b->property("owl_icon_survol");
    return (v.isValid() && v.canConvert<QIcon>()
            && !v.value<QIcon>().isNull()
            && b->property("owl_hover_swapper").toBool()) ? 1 : 0;
}

extern "C" const char* owl_ui_button_get_icon(OwlControl* btn) {
    if (!btn) return "";
    QWidget* w = reinterpret_cast<QWidget*>(btn);
    QVariant v = w->property("owl_image");
    if (!v.isValid()) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = v.toString().toUtf8();
    return g_ctrl_text_buf.constData();
}

/* Memorise la position dans la property lue par OwlButtonProxyStyle (maillon du
 * style applicatif, cf. owl_app_style).  — plus de setStyle par-widget
 * ici : le proxy de placement est herite au niveau application, il suffit de
 * poser la property et de demander un repaint. Poser un style par-widget en
 * plus d'une feuille QSS (Style..CadreExtérieur -> setStyleSheet) etait le
 * montage non supporte qui provoquait l'UAF au show (classe ). */
extern "C" void owl_ui_button_set_position_image(OwlControl* btn, int position) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    b->setProperty("owl_position_image", position);
    b->update();
}

extern "C" int owl_ui_button_get_position_image(OwlControl* btn) {
    if (!btn) return OWL_POSBTNIMG_GAUCHE;
    QVariant v = reinterpret_cast<QWidget*>(btn)->property("owl_position_image");
    return v.isValid() ? v.toInt() : OWL_POSBTNIMG_GAUCHE;
}

extern "C" void owl_ui_button_set_type_bouton(OwlControl* btn, int type_bouton) {
    if (!btn) return;
    QPushButton* b = qobject_cast<QPushButton*>(reinterpret_cast<QWidget*>(btn));
    if (!b) return;
    b->setProperty("owl_type_bouton", type_bouton);
    /* Reset prealable pour idempotence : TypeBouton ecrase l'ancien defaut+shortcut.
     * On restaure le mnemonic derive du libelle ('&Lettre' -> Alt+Lettre) plutot
     * qu'un QKeySequence vide : setText l'avait enregistre a la creation, un reset
     * a vide l'effacerait et Alt+lettre ne declencherait plus le bouton. Les cas
     * VALIDER/ABANDON/AIDE ci-dessous ecrasent volontairement (Esc/F1/default). */
    b->setDefault(false);
    b->setShortcut(QKeySequence::mnemonic(b->text()));
    switch (type_bouton) {
        case OWL_BTN_VALIDER: b->setDefault(true); break;
        case OWL_BTN_ABANDON: b->setShortcut(QKeySequence(QStringLiteral("Esc"))); break;
        case OWL_BTN_AIDE:    b->setShortcut(QKeySequence(QStringLiteral("F1"))); break;
        default: break; /* OWL_BTN_NORMAL / OWL_BTN_INTERRUPTION : pas d'effet */
    }
}

extern "C" int owl_ui_button_get_type_bouton(OwlControl* btn) {
    if (!btn) return OWL_BTN_NORMAL;
    QVariant v = reinterpret_cast<QWidget*>(btn)->property("owl_type_bouton");
    return v.isValid() ? v.toInt() : OWL_BTN_NORMAL;
}

extern "C" void owl_ui_label_set_word_wrap(OwlControl* lbl, int wrap) {
    if (!lbl) return;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (q) q->setWordWrap(wrap != 0);
}

extern "C" int owl_ui_label_get_word_wrap(OwlControl* lbl) {
    if (!lbl) return 0;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    return (q && q->wordWrap()) ? 1 : 0;
}

/* Bucket E — texte riche/plain pour QLabel et QTextEdit (multiligne). */
extern "C" void owl_ui_label_set_text_rich(OwlControl* lbl, const char* html_utf8) {
    if (!lbl) return;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (!q) return;
    q->setTextFormat(Qt::RichText);
    q->setText(QString::fromUtf8(html_utf8 ? html_utf8 : ""));
}

extern "C" void owl_ui_lineedit_set_text_rich(OwlControl* edit, const char* html_utf8) {
    if (!edit) return;
    QWidget* w = reinterpret_cast<QWidget*>(edit);
    /* Multiligne : QPlainTextEdit ne fait pas le rich text. Pour V1, on bascule
     * vers QTextEdit si le widget initial est multiligne. Sinon (single-line
     * QLineEdit), pas de rich support natif — on tombe en plain. */
    if (QTextEdit* te = qobject_cast<QTextEdit*>(w)) {
        te->setHtml(QString::fromUtf8(html_utf8 ? html_utf8 : ""));
        return;
    }
    /* Pas de support rich pour QLineEdit single-line — fallback plain. */
    if (QLineEdit* le = qobject_cast<QLineEdit*>(w)) {
        le->setText(QString::fromUtf8(html_utf8 ? html_utf8 : ""));
    }
}

/* ------- Cadrage (alignement) ------- */

static Qt::Alignment current_alignment(QWidget* w) {
    if (QLabel* lbl = qobject_cast<QLabel*>(w)) return lbl->alignment();
    if (QLineEdit* le = qobject_cast<QLineEdit*>(w)) return le->alignment();
    return Qt::Alignment();
}

static void apply_alignment(QWidget* w, Qt::Alignment a) {
    if (QLabel* lbl = qobject_cast<QLabel*>(w)) { lbl->setAlignment(a); return; }
    if (QLineEdit* le = qobject_cast<QLineEdit*>(w)) { le->setAlignment(a); return; }
    /* QPushButton n'a pas de setAlignment — no-op silencieux. */
}

static Qt::Alignment owl_h_to_qt(int h) {
    switch (h) {
        case OWL_CH_CENTRE: return Qt::AlignHCenter;
        case OWL_CH_DROITE: return Qt::AlignRight;
        default:            return Qt::AlignLeft;  /* OWL_CH_GAUCHE */
    }
}

static Qt::Alignment owl_v_to_qt(int v) {
    switch (v) {
        case OWL_CV_HAUT: return Qt::AlignTop;
        case OWL_CV_BAS:  return Qt::AlignBottom;
        default:          return Qt::AlignVCenter;  /* OWL_CV_CENTRE */
    }
}

static int qt_h_to_owl(Qt::Alignment a) {
    if (a & Qt::AlignHCenter) return OWL_CH_CENTRE;
    if (a & Qt::AlignRight)   return OWL_CH_DROITE;
    return OWL_CH_GAUCHE;
}

static int qt_v_to_owl(Qt::Alignment a) {
    if (a & Qt::AlignTop)    return OWL_CV_HAUT;
    if (a & Qt::AlignBottom) return OWL_CV_BAS;
    return OWL_CV_CENTRE;
}

extern "C" void owl_ui_control_set_h_align(OwlControl* w, int h) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    /* Memorise le cadrage H demande : apply_cadrage_to_label le relit pour
     * ne pas ecraser l'alignement a chaque (re)pose de pixmap runtime. */
    widget->setProperty("owl_halign", h);
    Qt::Alignment cur = current_alignment(widget);
    Qt::Alignment newA = owl_h_to_qt(h)
                       | (cur & Qt::AlignVertical_Mask ? cur & Qt::AlignVertical_Mask : Qt::AlignVCenter);
    apply_alignment(widget, newA);
}

extern "C" int owl_ui_control_get_h_align(OwlControl* w) {
    if (!w) return 0;
    return qt_h_to_owl(current_alignment(reinterpret_cast<QWidget*>(w)));
}

extern "C" void owl_ui_control_set_v_align(OwlControl* w, int v) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    /* Cf. set_h_align : memorise le cadrage V pour apply_cadrage_to_label. */
    widget->setProperty("owl_valign", v);
    Qt::Alignment cur = current_alignment(widget);
    Qt::Alignment newA = owl_v_to_qt(v)
                       | (cur & Qt::AlignHorizontal_Mask ? cur & Qt::AlignHorizontal_Mask : Qt::AlignLeft);
    apply_alignment(widget, newA);
}

extern "C" int owl_ui_control_get_v_align(OwlControl* w) {
    if (!w) return 1;
    return qt_v_to_owl(current_alignment(reinterpret_cast<QWidget*>(w)));
}

/* ------- Etat (Actif/AffichageSeulement/Grise/Invisible) ------- */

extern "C" void owl_ui_control_set_etat(OwlControl* w, int etat) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QLineEdit* le = qobject_cast<QLineEdit*>(widget);
    QTableView* tv = qobject_cast<QTableView*>(widget);
    /* Toute transition repart d'un etat propre : on retire le filtre Inactif et
     * son marqueur, reposes seulement par la branche AffichageSeulement non-le. */
    widget->removeEventFilter(owl_inactive_filter());
    widget->setProperty("owl_etat_inactif", QVariant());
    /* Table : on ne restaure la selection utilisateur que si l'etat courant
     * QUITTE AffichageSansSelection (marqueur pose par la branche dediee). Sans
     * ce garde, un reset inconditionnel ecraserait un mode de selection pose
     * ailleurs (ex EtatInitial = NiSaisieNiSelection -> NoSelection). */
    bool tableLeavingNoSel =
        tv && widget->property("owl_etat_sans_selection").toBool()
        && etat != OWL_ETAT_AFFICHAGE_SANS_SELECTION;
    switch (etat) {
        case OWL_ETAT_ACTIF:
            widget->setVisible(true);
            widget->setEnabled(true);
            if (le) le->setReadOnly(false);
            /*  — Table : restaure les edit-triggers coherents avec
             * l'EtatInitial en vigueur (defaut EnSaisie si non declare). Sans ce
             * restore, une Table sortie d'AffichageSeulement resterait figee sur
             * NoEditTriggers. */
            if (tv) {
                QVariant ei = widget->property("owl_etat_initial");
                tv->setEditTriggers(owl_table_triggers_for_etat_initial(
                    ei.isValid() ? ei.toInt() : 0));
            }
            break;
        case OWL_ETAT_AFFICHAGE_SANS_SELECTION:
            /* Table uniquement : affichee et active, mais aucune selection
             * utilisateur (souris/clavier). La selection par programmation
             * passe par QItemSelectionModel::select (owl_ui_table_select_row),
             * qui n'est pas bride par le selectionMode -> TableSelectPlus reste
             * operant. No-op de selection sur les autres widgets. */
            widget->setVisible(true);
            widget->setEnabled(true);
            if (tv) {
                tv->setSelectionMode(QAbstractItemView::NoSelection);
                widget->setProperty("owl_etat_sans_selection", true);
            }
            break;
        case OWL_ETAT_AFFICHAGE_SEUL: /* alias: Inactif */
            widget->setVisible(true);
            widget->setEnabled(true);
            if (le) {
                le->setReadOnly(true);
            } else if (tv) {
                /*  — Table : « affichage seulement » = plus d'edition
                 * texte mais la vue reste active et selectionnable. On coupe les
                 * edit-triggers (le double-clic n'ouvre plus d'editeur) plutot
                 * que d'installer le filtre inactif — ce dernier ne capte pas les
                 * events du viewport (double-clic passe) et tuerait la selection.
                 * selectionMode inchange volontairement. */
                tv->setEditTriggers(QAbstractItemView::NoEditTriggers);
            } else {
                /* Pas de lecture-seule native (Bouton/Libellé…) : Inactif =
                 * non-interactif MAIS couleur normale (distinct de Grisé qui
                 * grise via disabled). On garde enabled=true pour la palette
                 * normale et on avale les interactions via le filtre. */
                widget->installEventFilter(owl_inactive_filter());
                widget->setProperty("owl_etat_inactif", true);
            }
            break;
        case OWL_ETAT_GRISE:
            widget->setVisible(true);
            widget->setEnabled(false);
            if (le) le->setReadOnly(false);
            break;
        case OWL_ETAT_INVISIBLE:
            widget->setVisible(false);
            break;
        default: break;
    }
    /* Restaure la selection utilisateur en sortie d'AffichageSansSelection. */
    if (tableLeavingNoSel) {
        tv->setSelectionMode(QAbstractItemView::ExtendedSelection);
        widget->setProperty("owl_etat_sans_selection", QVariant());
    }
}

extern "C" int owl_ui_control_get_etat(OwlControl* w) {
    if (!w) return 0;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (!widget->isVisible())   return 3;  /* Invisible */
    if (!widget->isEnabled())   return 2;  /* Grise */
    /* Inactif non-le : enabled=true + marqueur (couleur normale, non-interactif). */
    if (widget->property("owl_etat_inactif").toBool()) return 1;
    QLineEdit* le = qobject_cast<QLineEdit*>(widget);
    if (le && le->isReadOnly()) return 1;  /* AffichageSeulement (Saisie) */
    return 0;                              /* Actif */
}

/* ------- Cadre / Image / Opacite -------
 * Cadre legacy (enum simple cadreAucun/Simple/3DEnfonce/3DSorti) supprime —
 * utiliser le type compose Cadre via owl_ui_widget_set_cadre. */

/* D.1.b — Cadre compose. Genere un QSS dynamique sur le QLabel :
 *
 *   QLabel#<name> {
 *       border-top:    <th>px <style> rgb(R,G,B);
 *       border-right:  ...                              (selectif si edges != 15)
 *       border-bottom: ...
 *       border-left:   ...
 *       border-radius: <radius>px;
 *   }
 *
 * Pour les bords absents on emet 'border-...: 0 none' explicitement.
 * Pour edges == 15 on emet le shorthand 'border:' unique. */
static const char* cadre_line_style_to_css(int line_style) {
    /* OWL : 5 styles 1:1 avec CSS Qt6. Cf docs/public_documentation/candidats/type_Cadre.md. */
    switch (line_style) {
        case OWL_TC_CONTINU:    return "solid";
        case OWL_TC_AUCUN:      return "none";
        case OWL_TC_TIRET:      return "dashed";
        case OWL_TC_POINTILLE:  return "dotted";
        case OWL_TC_DOUBLE:     return "double";
        default: return "solid";
    }
}

/* Lot Cadre Phase 4 : helper qui packe RGB + style + thickness en CSS line.
 * Si thick=0 ou line_style invalide => "0 none". */
static QString cadre_css_line(int color_rgb, int color_alpha, int thick, int line_style) {
    if (thick <= 0) return "0 none";
    /*  — couleur alpha-aware (owl_bg_css : rgb(...) byte-identique si
     * opaque, rgba(...) sinon). */
    return QString("%1px %2 %3")
        .arg(thick)
        .arg(cadre_line_style_to_css(line_style))
        .arg(owl_bg_css(color_rgb, color_alpha));
}

/*  — corps QSS de bordure d'un cadre, partagé par les émetteurs Style
 * (Conteneur, Saisie, Table/ZoneRépétée). Si AU MOINS un côté porte un trait
 * propre (Epaisseur ou Couleur renseignée sur TraitHaut/Bas/Gauche/Droite),
 * émet les quatre border-{top,right,bottom,left} distincts (couleur + épaisseur
 * + style par côté, alpha porté) ; un côté non personnalisé retombe sur le
 * scalaire global. Sinon (aucun côté posé) rend le shorthand global
 * `border: Npx style couleur` STRICTEMENT byte-identique à l'historique (chaîne
 * vide si Epaisseur<=0). `colorFn(rgb, alpha)` formate la couleur selon la
 * surface (owl_bg_css sans espace pour Conteneur/Saisie, owl_table_rgb_css_a
 * spacé pour la Table) afin de préserver la byte-identité de chaque émetteur.
 * Le critère « côté posé » (thick!=0 || color!=0, pas le Type) est repris de
 * owl_ui_widget_set_cadre : le template Trait initialise Type=Continu (sentinel
 * non-nul), donc le Type seul ne compte pas comme surcharge.
 * `perSideOut` (optionnel) signale le mode per-side (marges/clip côté caller). */
static QString cadre_border_body(const OwlCadreSpec* s,
                                 QString (*colorFn)(int, int),
                                 bool* perSideOut) {
    if (perSideOut) *perSideOut = false;
    if (!s) return QString();
    auto posed = [](int c, int t) { return t != 0 || c != 0; };
    bool anySide = posed(s->trait_haut_color,   s->trait_haut_thick)
                || posed(s->trait_bas_color,    s->trait_bas_thick)
                || posed(s->trait_gauche_color, s->trait_gauche_thick)
                || posed(s->trait_droite_color, s->trait_droite_thick);
    if (!anySide) {
        if (s->thickness <= 0) return QString();
        return QString("border: %1px %2 %3; ")
                   .arg(s->thickness)
                   .arg(cadre_line_style_to_css(s->line_style))
                   .arg(colorFn(s->color, s->color_alpha));
    }
    if (perSideOut) *perSideOut = true;
    /* Ligne d'un bord : per-bord si posé, sinon repli sur le scalaire global. */
    auto line = [&](int c, int t, int ty, int a) -> QString {
        if (posed(c, t)) {
            if (t <= 0) return "0 none";
            return QString("%1px %2 %3").arg(t)
                       .arg(cadre_line_style_to_css(ty)).arg(colorFn(c, a));
        }
        if (s->thickness <= 0) return "0 none";
        return QString("%1px %2 %3").arg(s->thickness)
                   .arg(cadre_line_style_to_css(s->line_style))
                   .arg(colorFn(s->color, s->color_alpha));
    };
    QString b = "border: 0 none; ";
    if (s->edges & 1) b += "border-top: "    + line(s->trait_haut_color,   s->trait_haut_thick,   s->trait_haut_type,   s->trait_haut_alpha)   + "; ";
    if (s->edges & 2) b += "border-right: "  + line(s->trait_droite_color, s->trait_droite_thick, s->trait_droite_type, s->trait_droite_alpha) + "; ";
    if (s->edges & 4) b += "border-bottom: " + line(s->trait_bas_color,    s->trait_bas_thick,    s->trait_bas_type,    s->trait_bas_alpha)    + "; ";
    if (s->edges & 8) b += "border-left: "   + line(s->trait_gauche_color, s->trait_gauche_thick, s->trait_gauche_type, s->trait_gauche_alpha) + "; ";
    return b;
}

/* Regeneration unifiee de la feuille de style d'un widget (couleurs + bordure
 * + etats). Definie plus bas ; declaree ici car set_cadre l'invoque. */
static void apply_color_stylesheet(QWidget* w);

/* Lot Cadre Phase 4 : apply Cadre compose etendu sur n'importe quel widget.
 * Genere QSS avec selector #objectName si possible (sinon cascade). */
extern "C" void owl_ui_widget_set_cadre(OwlControl* w, const OwlCadreSpec* s) {
    if (!w || !s) return;
    QWidget* q = reinterpret_cast<QWidget*>(w);
    /* Reset le QFrame natif si applicable. */
    if (auto* qf = qobject_cast<QFrame*>(q)) qf->setFrameStyle(QFrame::NoFrame);

    /* Default per-bord = scalaires (le sentinel "non override" est l'absence
     * de set_field cote OWL — on detecte par "all 0" vs "scalaire defini"). */
    auto bordLine = [&](int side_color, int side_thick, int side_type) -> QString {
        /* Override per-bord uniquement si le bord porte sa PROPRE epaisseur ou
         * couleur. Le champ Type seul ne compte pas : le template Trait initialise
         * Type=TraitContinu (sentinel OWL_TC_* non-nul), donc tout bord non
         * personnalise aurait l'air d'un override -> on retombait sur thick=0 ->
         * "0 none" (cadre invisible). On herite alors le style scalaire global,
         * en gardant le Type per-bord s'il a ete redefini avec une epaisseur. */
        if (side_thick != 0 || side_color != 0) {
            /* Per-bord (TraitHaut/…) : pas d'alpha porté dans ce lot -> opaque. */
            return cadre_css_line(side_color, 255, side_thick, side_type);
        }
        return cadre_css_line(s->color, s->color_alpha, s->thickness, s->line_style);
    };

    /* On ne genere QUE le fragment interne (declarations border-*), stocke en
     * dynamic property. La feuille finale est assemblee par
     * apply_color_stylesheet, qui fusionne ce fragment avec le fond/couleur
     * dans un unique bloc "* { ... }". Sans cette fusion, le setStyleSheet du
     * cadre et celui des couleurs se remplacaient mutuellement (le dernier
     * appele gagnait) -> fond + bordure ne coexistaient jamais. */
    QString cadreBody;
    cadreBody.reserve(512);

    /* Bordures par bord. edges bitmask : 1=Haut, 2=Droite, 4=Bas, 8=Gauche. */
    if (s->thickness > 0 || s->trait_haut_thick > 0 || s->trait_bas_thick > 0
        || s->trait_gauche_thick > 0 || s->trait_droite_thick > 0) {
        cadreBody += "border: 0 none; ";
        if (s->edges & 1)
            cadreBody += "border-top: " + bordLine(s->trait_haut_color, s->trait_haut_thick, s->trait_haut_type) + "; ";
        if (s->edges & 2)
            cadreBody += "border-right: " + bordLine(s->trait_droite_color, s->trait_droite_thick, s->trait_droite_type) + "; ";
        if (s->edges & 4)
            cadreBody += "border-bottom: " + bordLine(s->trait_bas_color, s->trait_bas_thick, s->trait_bas_type) + "; ";
        if (s->edges & 8)
            cadreBody += "border-left: " + bordLine(s->trait_gauche_color, s->trait_gauche_thick, s->trait_gauche_type) + "; ";
    } else {
        cadreBody += "border: 0 none; ";
    }

    /* Coins : per-coin si Largeur/Hauteur > 0, sinon arrondi global. */
    auto coinRadius = [&](int w_, int h_) -> QString {
        int rw = w_ > 0 ? w_ : s->corner_radius;
        int rh = h_ > 0 ? h_ : s->corner_radius;
        if (rw <= 0 && rh <= 0) return "";
        if (rw == rh) return QString::asprintf("%dpx", rw);
        return QString::asprintf("%dpx %dpx", rw, rh);
    };
    QString rHG = coinRadius(s->coin_hg_w, s->coin_hg_h);
    QString rHD = coinRadius(s->coin_hd_w, s->coin_hd_h);
    QString rBG = coinRadius(s->coin_bg_w, s->coin_bg_h);
    QString rBD = coinRadius(s->coin_bd_w, s->coin_bd_h);
    if (!rHG.isEmpty()) cadreBody += "border-top-left-radius: "    + rHG + "; ";
    if (!rHD.isEmpty()) cadreBody += "border-top-right-radius: "   + rHD + "; ";
    if (!rBG.isEmpty()) cadreBody += "border-bottom-left-radius: " + rBG + "; ";
    if (!rBD.isEmpty()) cadreBody += "border-bottom-right-radius: " + rBD + "; ";

    q->setProperty("owl_cadre_body", cadreBody);
    /*  — mémorise les rayons per-coin bruts (px) pour un re-borne à la
     * hauteur FINALE côté apply_color_stylesheet (le QPushButton sans liseré rend
     * carré si border-radius > hauteur/2 ; la hauteur au moment du set_cadre est
     * encore le sizeHint gonflé par le QSS, pas la hauteur posée par le layout). */
    q->setProperty("owl_cadre_r_hg", s->coin_hg_w > 0 ? s->coin_hg_w : s->corner_radius);
    q->setProperty("owl_cadre_r_hd", s->coin_hd_w > 0 ? s->coin_hd_w : s->corner_radius);
    q->setProperty("owl_cadre_r_bg", s->coin_bg_w > 0 ? s->coin_bg_w : s->corner_radius);
    q->setProperty("owl_cadre_r_bd", s->coin_bd_w > 0 ? s->coin_bd_w : s->corner_radius);

    /* Read-back state pour les getters. */
    q->setProperty("owl_cadre_composed", true);
    q->setProperty("owl_cadre_color",    s->color);
    q->setProperty("owl_cadre_thick",    s->thickness);
    q->setProperty("owl_cadre_line",     s->line_style);
    q->setProperty("owl_cadre_radius",   s->corner_radius);
    q->setProperty("owl_cadre_edges",    s->edges);

    apply_color_stylesheet(q);  /* regen unifiee : fond + bordure + etats */
}

/* Alignement du pixmap dans le Libelle : respecte le cadrage H/V demande
 * (owl_ui_control_set_h_align/set_v_align memorisent owl_halign/owl_valign),
 * defaut centre si non specifie. Sans ca, chaque (re)pose de pixmap au
 * runtime ecraserait le cadrage pose apres le set declaratif initial. */
static Qt::Alignment cadrage_pixmap_alignment(QLabel* q) {
    QVariant hv = q->property("owl_halign");
    QVariant vv = q->property("owl_valign");
    Qt::Alignment h = hv.isValid() ? owl_h_to_qt(hv.toInt()) : Qt::AlignHCenter;
    Qt::Alignment v = vv.isValid() ? owl_v_to_qt(vv.toInt()) : Qt::AlignVCenter;
    return h | v;
}

/* Cadrage Libelle : 0=centre (defaut), 1=etire, 2=homothetique. La pixmap
 * source est stockee en dynamic property "owl_pixmap_src" pour pouvoir
 * la reappliquer si le cadrage change apres le set. */
static void apply_cadrage_to_label(QLabel* q) {
    QVariant srcVar = q->property("owl_pixmap_src");
    if (!srcVar.isValid() || !srcVar.canConvert<QPixmap>()) return;
    QPixmap src = srcVar.value<QPixmap>();
    if (src.isNull()) { q->setPixmap(QPixmap()); return; }

    /* Champ Image V2 : prop Zoom (1..1000, 100 = native). Applique avant
     * le cadrage : on scale la pixmap source, puis le cadrage decide
     * comment l'afficher dans le widget. Pour un Libelle classique sans
     * Zoom defini, owl_zoom est absent → percent reste 100. */
    int percent = q->property("owl_zoom").toInt();
    if (percent < 1) percent = 100;
    QPixmap zoomed = src;
    if (percent != 100) {
        QSize ns = src.size() * percent / 100;
        if (ns.width() < 1)  ns.setWidth(1);
        if (ns.height() < 1) ns.setHeight(1);
        zoomed = src.scaled(ns, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    int cadrage = q->property("owl_cadrage").toInt();
    switch (cadrage) {
        case OWL_CADIMG_ETIRE:
            q->setScaledContents(true);
            q->setPixmap(zoomed);
            break;
        case OWL_CADIMG_HOMOTHETIQUE: {
            q->setScaledContents(false);
            q->setAlignment(cadrage_pixmap_alignment(q));
            QSize target = q->size().isValid() && q->size().width() > 0
                         ? q->size() : zoomed.size();
            QPixmap scaled = zoomed.scaled(target, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
            q->setPixmap(scaled);
            break;
        }
        case OWL_CADIMG_CENTRE:
        default:
            q->setScaledContents(false);
            q->setAlignment(cadrage_pixmap_alignment(q));
            q->setPixmap(zoomed);
            break;
    }
}

extern "C" void owl_ui_label_set_image(OwlControl* lbl, const char* path_utf8) {
    if (!lbl) return;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (!q) return;
    if (!path_utf8 || !*path_utf8) {
        q->setPixmap(QPixmap());
        q->setProperty("owl_image",      QVariant());
        q->setProperty("owl_pixmap_src", QVariant());
        return;
    }
    QPixmap src(QString::fromUtf8(path_utf8));
    q->setProperty("owl_image",      QString::fromUtf8(path_utf8));
    q->setProperty("owl_pixmap_src", src);
    apply_cadrage_to_label(q);
}

extern "C" void owl_ui_label_set_image_rgba(OwlControl* lbl,
                                            const unsigned char* rgba,
                                            int width, int height,
                                            int cadrage) {
    if (!lbl) return;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (!q) return;
    QImage img = rgba_to_qimage(rgba, width, height);
    if (img.isNull()) {
        q->setPixmap(QPixmap());
        q->setProperty("owl_image",      QVariant());
        q->setProperty("owl_pixmap_src", QVariant());
        return;
    }
    QPixmap src = QPixmap::fromImage(img);
    q->setProperty("owl_image",      QString("<ObjImage>"));
    q->setProperty("owl_pixmap_src", src);
    q->setProperty("owl_cadrage",    cadrage);
    apply_cadrage_to_label(q);
}

extern "C" void owl_ui_label_set_cadrage(OwlControl* lbl, int cadrage) {
    if (!lbl) return;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (!q) return;
    q->setProperty("owl_cadrage", cadrage);
    apply_cadrage_to_label(q);
}

extern "C" int owl_ui_label_get_cadrage(OwlControl* lbl) {
    if (!lbl) return 0;
    QLabel* q = qobject_cast<QLabel*>(reinterpret_cast<QWidget*>(lbl));
    if (!q) return 0;
    QVariant v = q->property("owl_cadrage");
    return v.isValid() ? v.toInt() : 0;
}

/* ============================================================
 * Champ Image V2 — OwlImageLabel
 *
 * QLabel custom qui override les events souris/roulette/lifecycle pour
 * dispatcher vers les callbacks OWL register's via owl_ui_image_field_on_*.
 * Pas de Q_OBJECT (juste des virtuals override) → pas besoin de moc.
 * ============================================================ */
class OwlImageLabel : public QLabel {
public:
    explicit OwlImageLabel(QWidget* parent = nullptr) : QLabel(parent) {
        setMouseTracking(true);  /* mouseMoveEvent fire sans bouton enfonce */
    }

    /*  — la pixmap re-scalee au resize ne doit PAS imposer
     * un minimum qui grossit avec elle : sinon apres agrandissement la fenetre
     * ne peut plus etre reduite (le QLabel bloque au minimumSizeHint = taille
     * pixmap). Un champ image fill est gouverne par le layout ; son minimum
     * effectif vient soit d'une taille declaree (setMinimum* pose par le
     * layout), soit est nul (fill libre). */
    QSize minimumSizeHint() const override { return QSize(0, 0); }

    /* Callbacks et userdata — un par event. */
    OwlEventCb0  cbClic = nullptr;             void* dClic = nullptr;
    OwlEventCb0  cbDoubleClic = nullptr;       void* dDoubleClic = nullptr;
    OwlEventCb0  cbEntreeSurvol = nullptr;     void* dEntreeSurvol = nullptr;
    OwlEventCb0  cbSortieSurvol = nullptr;     void* dSortieSurvol = nullptr;
    OwlEventCb2i cbSurvol = nullptr;           void* dSurvol = nullptr;
    OwlEventCb2i cbBtnGauche = nullptr;        void* dBtnGauche = nullptr;
    OwlEventCb2i cbRelacheBtnGauche = nullptr; void* dRelacheBtnGauche = nullptr;
    OwlEventCb2i cbBtnDroit = nullptr;         void* dBtnDroit = nullptr;
    OwlEventCb2i cbRelacheBtnDroit = nullptr;  void* dRelacheBtnDroit = nullptr;
    OwlEventCb2i cbDoubleClicDroit = nullptr;  void* dDoubleClicDroit = nullptr;
    OwlEventCb3i cbMolette = nullptr;          void* dMolette = nullptr;
    OwlEventCb2i cbMenuContextuel = nullptr;   void* dMenuContextuel = nullptr;
    OwlEventCb2i cbDeplacement = nullptr;      void* dDeplacement = nullptr;
    OwlEventCb2i cbChangementTaille = nullptr; void* dChangementTaille = nullptr;

    /* Zoom : 100 = taille native. Re-applique au prochain setPixmap via
     * apply_cadrage_to_label (qui consulte owl_cadrage + owl_zoom). */

    /*  — Champ CodeBarres : re-rasterisation debounced au resize.
     * Le timer (par widget) est relance a chaque resize ; la regen ne tire
     * qu'une fois ~180ms apres le DERNIER resize (drag pose). m_cbLastRegen*
     * memorise la derniere taille regeneree pour eviter une regen inutile. */
    QTimer* m_cbRegenTimer = nullptr;
    int m_cbLastRegenW = -1;
    int m_cbLastRegenH = -1;

    /* Re-rasterise le code-barres a la taille reelle du champ. Chemin C pur
     * (owl_codebarre_generate_owlimage) : aucune allocation VM/GC. Skip si le
     * widget ne porte pas de config code-barres, si la taille est nulle, ou si
     * elle n'a pas change depuis la derniere regen. */
    void regenBarcodeIfNeeded() {
        if (!property("owl_cb_is_barcode").toBool()) return;
        int w = width();
        int h = height();
        if (w <= 0 || h <= 0) return;
        if (w == m_cbLastRegenW && h == m_cbLastRegenH) return;
        QByteArray contenu = property("owl_cb_contenu").toByteArray();
        if (contenu.isEmpty()) return;
        int64_t type = (int64_t)property("owl_cb_type").toLongLong();
        int64_t ecc  = (int64_t)property("owl_cb_ecc").toLongLong();
        int64_t opts = (int64_t)property("owl_cb_options").toLongLong();
        const char* err = nullptr;
        OwlImage* img = owl_codebarre_generate_owlimage(
            type, ecc, opts, contenu.constData(), (int)contenu.size(),
            w, h, false, 0, false, 0, &err);
        if (!img) return;
        /*  — recompositer le logo (image centrale QR) stocke : le
         * chemin de regen est plat (type/ecc/options/contenu) et ne repasse pas
         * par l'instance, donc sans ce recompositing le logo serait perdu au
         * resize. OwlImage transitoire construite depuis le RGBA stocke, libere
         * ici (100% C-level, aucune VM/GC). */
        QByteArray logo = property("owl_cb_logo_rgba").toByteArray();
        if (!logo.isEmpty()) {
            int lw = property("owl_cb_logo_w").toInt();
            int lh = property("owl_cb_logo_h").toInt();
            double prop = property("owl_cb_logo_prop").toDouble();
            OwlImage* centre = owl_image_new_from_rgba(
                lw, lh, reinterpret_cast<const uint8_t*>(logo.constData()));
            if (centre) {
                owl_codebarre_composite_center_image(img, type, ecc, centre, prop);
                owl_image_free(centre);
            }
        }
        const uint8_t* rgba = owl_image_pixels(img);
        int iw = owl_image_width(img);
        int ih = owl_image_height(img);
        if (rgba && iw > 0 && ih > 0) {
            QVariant cadV = property("owl_cadrage");
            int cad = cadV.isValid() ? cadV.toInt() : OWL_CADIMG_HOMOTHETIQUE;
            owl_ui_label_set_image_rgba(asCtrl(), rgba, iw, ih, cad);
            m_cbLastRegenW = w;
            m_cbLastRegenH = h;
        }
        owl_image_free(img);  /* transitoire non-wrappe : a nous de liberer */
    }

protected:
    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }

    void mousePressEvent(QMouseEvent* e) override {
        QLabel::mousePressEvent(e);
        int x = e->position().x(), y = e->position().y();
        if (e->button() == Qt::LeftButton  && cbBtnGauche)
            cbBtnGauche(asCtrl(), dBtnGauche, x, y);
        if (e->button() == Qt::RightButton && cbBtnDroit)
            cbBtnDroit(asCtrl(), dBtnDroit, x, y);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        QLabel::mouseReleaseEvent(e);
        int x = e->position().x(), y = e->position().y();
        if (e->button() == Qt::LeftButton) {
            if (cbRelacheBtnGauche) cbRelacheBtnGauche(asCtrl(), dRelacheBtnGauche, x, y);
            if (cbClic)             cbClic(asCtrl(), dClic);
        }
        if (e->button() == Qt::RightButton && cbRelacheBtnDroit)
            cbRelacheBtnDroit(asCtrl(), dRelacheBtnDroit, x, y);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        QLabel::mouseDoubleClickEvent(e);
        int x = e->position().x(), y = e->position().y();
        if (e->button() == Qt::LeftButton && cbDoubleClic)
            cbDoubleClic(asCtrl(), dDoubleClic);
        if (e->button() == Qt::RightButton && cbDoubleClicDroit)
            cbDoubleClicDroit(asCtrl(), dDoubleClicDroit, x, y);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        QLabel::mouseMoveEvent(e);
        if (cbSurvol) cbSurvol(asCtrl(), dSurvol, e->position().x(), e->position().y());
    }
    void enterEvent(QEnterEvent* e) override {
        QLabel::enterEvent(e);
        if (cbEntreeSurvol) cbEntreeSurvol(asCtrl(), dEntreeSurvol);
    }
    void leaveEvent(QEvent* e) override {
        QLabel::leaveEvent(e);
        if (cbSortieSurvol) cbSortieSurvol(asCtrl(), dSortieSurvol);
    }
    void wheelEvent(QWheelEvent* e) override {
        QLabel::wheelEvent(e);
        if (cbMolette) {
            /* Qt rapporte angleDelta en 1/8 de degre, 120/cran. On normalise
             * en nombre de crans signe (positif = vers le haut). */
            int mvt = e->angleDelta().y() / 120;
            if (mvt == 0) mvt = (e->angleDelta().y() >= 0) ? 1 : -1;
            cbMolette(asCtrl(), dMolette, mvt,
                      (int)e->position().x(), (int)e->position().y());
        }
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QLabel::contextMenuEvent(e);
        if (cbMenuContextuel)
            cbMenuContextuel(asCtrl(), dMenuContextuel, e->x(), e->y());
    }
    void moveEvent(QMoveEvent* e) override {
        QLabel::moveEvent(e);
        if (cbDeplacement)
            cbDeplacement(asCtrl(), dDeplacement, e->pos().x(), e->pos().y());
    }
    void resizeEvent(QResizeEvent* e) override {
        QLabel::resizeEvent(e);
        /*  — re-scale le pixmap a la nouvelle taille (cadrage
         * homothetique/etire). Sans ca l'image reste figee a sa taille de pose
         * initiale quand le champ grandit avec la fenetre. Suit le drag sans
         * lag (scale instantane). */
        apply_cadrage_to_label(this);
        /*  — Champ CodeBarres : re-rasterise NET a la taille reelle.
         * Debounce ~180ms (par widget) : pendant le drag, que du scale ci-dessus ;
         * la regen (couteuse) tire une seule fois apres le dernier resize.
         * Le layout initial declenche lui-meme un resize -> premier tir = regen
         * a la vraie taille initiale (corrige aussi le flou initial). */
        if (property("owl_cb_is_barcode").toBool()) {
            if (!m_cbRegenTimer) {
                m_cbRegenTimer = new QTimer(this);
                m_cbRegenTimer->setSingleShot(true);
                QObject::connect(m_cbRegenTimer, &QTimer::timeout, this,
                                 [this]() { regenBarcodeIfNeeded(); });
            }
            m_cbRegenTimer->start(180);
        }
        if (cbChangementTaille)
            cbChangementTaille(asCtrl(), dChangementTaille,
                               e->size().width(), e->size().height());
    }
};

extern "C" OwlControl* owl_ui_image_field_create(OwlWindow* parent,
                                                  int x, int y, int w, int h) {
    /* Resolution sure du parent : central_of gere fenetre (QMainWindow ->
     * centralWidget) ET Conteneur (QWidget generique). Un cast brut
     * QMainWindow* sur un Conteneur segfaultait (centralWidget sur vtable
     * garbage). cf . */
    QWidget* parentWidget = parent ? central_of(parent) : nullptr;
    OwlImageLabel* lbl = new OwlImageLabel(parentWidget);
    if (x >= 0 && y >= 0) lbl->setGeometry(x, y, w, h);
    else lbl->resize(w > 0 ? w : 100, h > 0 ? h : 100);
    lbl->setProperty("owl_zoom", 100);
    lbl->setProperty("owl_is_image_field", true);  /*  : remplit sous Étire */
    lbl->show();
    return reinterpret_cast<OwlControl*>(lbl);
}

extern "C" void owl_ui_image_field_set_zoom(OwlControl* w, int percent) {
    if (!w) return;
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    if (percent < 1) percent = 1;
    if (percent > 1000) percent = 1000;
    qw->setProperty("owl_zoom", percent);
    /* Re-apply cadrage qui consulte owl_zoom (cf apply_cadrage_to_label). */
    QLabel* q = qobject_cast<QLabel*>(qw);
    if (q) apply_cadrage_to_label(q);
}
/* Helper interne : récupère l'OwlImageLabel ou NULL. */
static OwlImageLabel* as_image_label(OwlControl* w) {
    return dynamic_cast<OwlImageLabel*>(reinterpret_cast<QWidget*>(w));
}

/*  — pose la config code-barres sur le widget pour que la regen au
 * resize sache quoi re-rasteriser. Appelee a chaque generation d'un Champ
 * CodeBarres (pose initiale + set-prop runtime). Reset des dims memorisees :
 * le contenu/type/ecc peut changer sans resize -> forcer une regen au prochain
 * tir. Un Champ Image normal (jamais cette config) ne regenere jamais. */
extern "C" void owl_ui_codebarre_set_config(OwlControl* w, int64_t type,
                                            int64_t ecc, int64_t options,
                                            const char* contenu, int len) {
    OwlImageLabel* l = as_image_label(w);
    if (!l) return;
    l->setProperty("owl_cb_is_barcode", true);
    l->setProperty("owl_cb_type",    (qlonglong)type);
    l->setProperty("owl_cb_ecc",     (qlonglong)ecc);
    l->setProperty("owl_cb_options", (qlonglong)options);
    l->setProperty("owl_cb_contenu",
                   (contenu && len > 0) ? QByteArray(contenu, len) : QByteArray());
    l->m_cbLastRegenW = -1;
    l->m_cbLastRegenH = -1;
}

/*  — stocke le logo (image centrale QR) sur le widget en RGBA brut,
 * pour que la regen au resize (chemin C plat, sans instance) le recompositie.
 * Appelee lors de l'adoption d'une instance CodeBarres portant ImageCentrale.
 * rgba NULL / dims nulles -> efface le logo stocke (une re-affectation sans
 * ImageCentrale doit retirer le logo). Le RGBA est copie immediatement dans un
 * QByteArray (aucune retention du pointeur source). */
extern "C" void owl_ui_codebarre_set_logo(OwlControl* w, const uint8_t* rgba,
                                          int lw, int lh, double proportion) {
    OwlImageLabel* l = as_image_label(w);
    if (!l) return;
    if (!rgba || lw <= 0 || lh <= 0) {
        l->setProperty("owl_cb_logo_rgba", QVariant());
        l->setProperty("owl_cb_logo_w",    QVariant());
        l->setProperty("owl_cb_logo_h",    QVariant());
        l->setProperty("owl_cb_logo_prop", QVariant());
        l->m_cbLastRegenW = -1;  /* force une regen : le logo a change */
        l->m_cbLastRegenH = -1;
        return;
    }
    QByteArray buf(reinterpret_cast<const char*>(rgba),
                   (int)((size_t)lw * (size_t)lh * 4));
    l->setProperty("owl_cb_logo_rgba", buf);
    l->setProperty("owl_cb_logo_w",    lw);
    l->setProperty("owl_cb_logo_h",    lh);
    l->setProperty("owl_cb_logo_prop", proportion);
    l->m_cbLastRegenW = -1;
    l->m_cbLastRegenH = -1;
}

#define OWL_IMG_REG_0(suffix, member)                                       \
    extern "C" void owl_ui_image_field_on_##suffix(                         \
            OwlControl* w, OwlEventCb0 cb, void* d) {                       \
        OwlImageLabel* l = as_image_label(w);                               \
        if (!l) return;                                                     \
        l->cb##member = cb; l->d##member = d;                               \
    }
#define OWL_IMG_REG_2(suffix, member)                                       \
    extern "C" void owl_ui_image_field_on_##suffix(                         \
            OwlControl* w, OwlEventCb2i cb, void* d) {                      \
        OwlImageLabel* l = as_image_label(w);                               \
        if (!l) return;                                                     \
        l->cb##member = cb; l->d##member = d;                               \
    }
#define OWL_IMG_REG_3(suffix, member)                                       \
    extern "C" void owl_ui_image_field_on_##suffix(                         \
            OwlControl* w, OwlEventCb3i cb, void* d) {                      \
        OwlImageLabel* l = as_image_label(w);                               \
        if (!l) return;                                                     \
        l->cb##member = cb; l->d##member = d;                               \
    }

OWL_IMG_REG_0(clic,                Clic)
OWL_IMG_REG_0(double_clic,         DoubleClic)
OWL_IMG_REG_0(entree_survol,       EntreeSurvol)
OWL_IMG_REG_0(sortie_survol,       SortieSurvol)
OWL_IMG_REG_2(survol,              Survol)
OWL_IMG_REG_2(bouton_gauche,       BtnGauche)
OWL_IMG_REG_2(relache_btn_gauche,  RelacheBtnGauche)
OWL_IMG_REG_2(bouton_droit,        BtnDroit)
OWL_IMG_REG_2(relache_btn_droit,   RelacheBtnDroit)
OWL_IMG_REG_2(double_clic_droit,   DoubleClicDroit)
OWL_IMG_REG_3(molette,             Molette)
OWL_IMG_REG_2(menu_contextuel,     MenuContextuel)
OWL_IMG_REG_2(deplacement,         Deplacement)
OWL_IMG_REG_2(changement_taille,   ChangementTaille)
#undef OWL_IMG_REG_0
#undef OWL_IMG_REG_2
#undef OWL_IMG_REG_3

extern "C" const char* owl_ui_label_get_image(OwlControl* lbl) {
    if (!lbl) return "";
    QVariant v = reinterpret_cast<QWidget*>(lbl)->property("owl_image");
    if (!v.isValid()) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = v.toString().toUtf8();
    return g_ctrl_text_buf.constData();
}

/*  — lecture de la feuille de style Qt courante du widget (contenu
 * passe a QWidget::setStyleSheet). Support test-only (__DebugFeuilleStyle) :
 * permet de verifier l'emission des fragments d'etat :hover/:focus sans
 * capture-pixel (aucun moyen headless de forcer le hover reel). */
extern "C" const char* owl_ui_control_get_stylesheet(OwlControl* w) {
    if (!w) return "";
    g_ctrl_text_buf = reinterpret_cast<QWidget*>(w)->styleSheet().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_control_set_opacite(OwlControl* w, int percent) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    widget->setProperty("owl_opacite", percent);
    if (percent >= 100) {
        /* 100% : retire l'effet pour eviter le cout. */
        widget->setGraphicsEffect(nullptr);
    } else {
        QGraphicsOpacityEffect* eff = new QGraphicsOpacityEffect(widget);
        eff->setOpacity(percent / 100.0);
        widget->setGraphicsEffect(eff);
    }
}

extern "C" int owl_ui_control_get_opacite(OwlControl* w) {
    if (!w) return 100;
    QVariant v = reinterpret_cast<QWidget*>(w)->property("owl_opacite");
    return v.isValid() ? v.toInt() : 100;
}

/*  — ombre portée / élévation / glow du widget entier via
 * QGraphicsDropShadowEffect (posée depuis CadreExtérieur..Ombre).
 *
 * Pièges Qt gérés :
 *   - Un seul QGraphicsEffect par widget : QGraphicsDropShadowEffect et
 *     QGraphicsOpacityEffect (owl_ui_control_set_opacite) partagent l'unique
 *     slot setGraphicsEffect. L'ombre prime quand elle est posée ; on ne retire
 *     que notre propre effet d'ombre, et seulement quand l'ombre est totalement
 *     vide (aucun flou, décalage ni étendue) — un effet d'opacité posé autrement
 *     reste intact. Ombre + opacité simultanées ne se cumulent pas (limite Qt),
 *     la dernière posée gagne. Le grisé (Etat=Grisé) passe par setEnabled(false)
 *     + QSS :disabled, indépendant du slot d'effet : ombre et grisé coexistent.
 *   - Ombre dure (néobrutalisme) : Flou=0 + décalage -> rayon de flou 0, ombre
 *     nette décalée. Rendue comme les autres tant qu'un décalage/étendue existe.
 *   - Débordement/clipping : l'effet déclare un boundingRect élargi, Qt réserve
 *     la place pour peindre le halo. Le parent (panneau/conteneur) ne clippe pas
 *     ses enfants par défaut ; en disposition libre l'ombre déborde donc
 *     librement tant que le widget n'est pas collé au bord du parent.
 *   - Coexistence QSS : l'effet est indépendant de setStyleSheet ; le widget
 *     garde son fond/bordure stylés, l'ombre s'ajoute par-dessus.
 *   - Étendue/Spread : pas de spread natif dans Qt -> approximé en élargissant
 *     le rayon de flou. La valeur brute est conservée au modèle (round-trip).
 *   - Glow (Néon) : ombre colorée, offsets 0, gros flou -> même chemin. */
extern "C" void owl_ui_control_set_shadow(OwlControl* w, int color, int offsetX,
                                          int offsetY, int blur, int spread,
                                          int alpha) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (blur <= 0 && offsetX == 0 && offsetY == 0 && spread <= 0) {
        /* Ombre totalement vide (aucun flou, aucun décalage, aucune étendue) :
         * rien à peindre. Retire notre effet d'ombre s'il existe, sans toucher
         * un eventuel effet d'opacite (type distinct). */
        if (qobject_cast<QGraphicsDropShadowEffect*>(widget->graphicsEffect()))
            widget->setGraphicsEffect(nullptr);
        return;
    }
    /* Ombre dure (néobrutalisme) : Flou=0 + décalage -> rayon 0 rend une ombre
     * NETTE décalée. On ne jette donc plus l'ombre quand Flou<=0 tant qu'un
     * décalage ou une étendue existe. */
    QGraphicsDropShadowEffect* eff = new QGraphicsDropShadowEffect(widget);
    /* OWL stocke en BGR (canon : R + V*256 + B*65536).  — alpha 0-255
     * de la couleur d'ombre appliqué au QColor (255 = opaque, byte-identique) ;
     * distinct de l'atténuation du flou (le flou adoucit les bords). */
    if (alpha < 0) alpha = 0; else if (alpha > 255) alpha = 255;
    eff->setColor(QColor(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF,
                         alpha));
    eff->setOffset(offsetX, offsetY);
    /* Étendue approximée : élargit le rayon de flou (2 px de flou par px
     * d'étendue), sans spread natif. Flou négatif traité comme 0. */
    eff->setBlurRadius((blur > 0 ? blur : 0) + (spread > 0 ? spread * 2 : 0));
    widget->setGraphicsEffect(eff);
}

/*  — ombre dynamique selon l'état d'interaction. Qt n'a PAS de
 * box-shadow QSS : un :hover ne peut pas piloter le QGraphicsDropShadowEffect.
 * On mémorise les specs d'ombre base/survol/appuyé en dynamic properties du
 * widget et on repeint l'effet programmatiquement sur les événements souris,
 * via un event filter. Miroir structurel d'OwlButtonHoverSwapper (icône au
 * survol). Index d'état : 0=base, 1=survol, 2=appuyé. */
static QString owl_shadow_prop(int stateIdx, const char* field) {
    return QString("owl_shadow_s%1_%2").arg(stateIdx).arg(QLatin1String(field));
}

/* Applique l'ombre mémorisée pour `stateIdx`. Si l'état n'a pas de surcharge
 * (present=0), retombe sur la base (index 0). Si même la base est absente,
 * retire l'effet. */
static void owl_shadow_apply_state(QWidget* w, int stateIdx) {
    int idx = stateIdx;
    if (idx != 0 && !w->property(owl_shadow_prop(idx, "present").toUtf8()).toBool())
        idx = 0;
    OwlControl* c = reinterpret_cast<OwlControl*>(w);
    if (!w->property(owl_shadow_prop(idx, "present").toUtf8()).toBool()) {
        owl_ui_control_set_shadow(c, 0, 0, 0, 0, 0, 255);
        return;
    }
    int color   = w->property(owl_shadow_prop(idx, "color").toUtf8()).toInt();
    int dx      = w->property(owl_shadow_prop(idx, "dx").toUtf8()).toInt();
    int dy      = w->property(owl_shadow_prop(idx, "dy").toUtf8()).toInt();
    int blur    = w->property(owl_shadow_prop(idx, "blur").toUtf8()).toInt();
    int spread  = w->property(owl_shadow_prop(idx, "spread").toUtf8()).toInt();
    /*  — alpha mémorisé (défaut 255 opaque si l'état est antérieur au champ). */
    QVariant alphaVar = w->property(owl_shadow_prop(idx, "alpha").toUtf8());
    int alpha   = alphaVar.isValid() ? alphaVar.toInt() : 255;
    owl_ui_control_set_shadow(c, color, dx, dy, blur, spread, alpha);
}

/* Cycle de vie : QObject enfant du widget (parenté au setParent), détruit avec
 * lui — pas de dangling. Un seul filter par widget (garde par property). Suit un
 * flag de survol (owl_shadow_hovered) pour distinguer le relâché-encore-survolé
 * du relâché-hors-widget sans dépendre de underMouse() (non fiable en synthétique). */
class OwlShadowStateFilter : public QObject {
public:
    explicit OwlShadowStateFilter(QWidget* w) : QObject(w), widget(w) {}
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == widget) {
            switch (ev->type()) {
            case QEvent::Enter:
                widget->setProperty("owl_shadow_hovered", true);
                owl_shadow_apply_state(widget, 1);
                break;
            case QEvent::Leave:
                widget->setProperty("owl_shadow_hovered", false);
                owl_shadow_apply_state(widget, 0);
                break;
            case QEvent::MouseButtonPress:
                owl_shadow_apply_state(widget, 2);
                break;
            case QEvent::MouseButtonRelease:
                owl_shadow_apply_state(widget,
                    widget->property("owl_shadow_hovered").toBool() ? 1 : 0);
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }
private:
    QWidget* widget;
};

extern "C" void owl_ui_control_set_shadow_state(OwlControl* w, int stateIdx,
        int color, int offsetX, int offsetY, int blur, int spread, int alpha,
        int present) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->setProperty(owl_shadow_prop(stateIdx, "present").toUtf8(), present != 0);
    widget->setProperty(owl_shadow_prop(stateIdx, "color").toUtf8(),  color);
    widget->setProperty(owl_shadow_prop(stateIdx, "dx").toUtf8(),     offsetX);
    widget->setProperty(owl_shadow_prop(stateIdx, "dy").toUtf8(),     offsetY);
    widget->setProperty(owl_shadow_prop(stateIdx, "blur").toUtf8(),   blur);
    widget->setProperty(owl_shadow_prop(stateIdx, "spread").toUtf8(), spread);
    widget->setProperty(owl_shadow_prop(stateIdx, "alpha").toUtf8(),  alpha);  /*  */
    /* Arme le filter dès qu'un état interactif (survol/appuyé) porte une ombre :
     * aucun coût si le thème ne définit pas d'Ombre d'état. */
    if ((stateIdx == 1 || stateIdx == 2) && present
        && !widget->property("owl_shadow_filter").toBool()) {
        widget->installEventFilter(new OwlShadowStateFilter(widget));
        widget->setProperty("owl_shadow_filter", true);
    }
}

/* Test-only : rayon de flou de l'effet d'ombre courant, -1 si aucun. */
extern "C" int owl_ui_control_shadow_blur(OwlControl* w) {
    if (!w) return -1;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QGraphicsDropShadowEffect* eff =
        qobject_cast<QGraphicsDropShadowEffect*>(widget->graphicsEffect());
    if (!eff) return -1;
    return (int)eff->blurRadius();
}

/* Test-only : pousse un événement d'interaction sur le widget pour
 * exercer l'event filter d'ombre (0=Enter, 1=Leave, 2=Press, 3=Release). */
extern "C" void owl_ui_control_push_interaction(OwlControl* w, int kind) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    switch (kind) {
    case 0: {
        QEnterEvent e(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
        QApplication::sendEvent(widget, &e);
        break;
    }
    case 1: {
        QEvent e(QEvent::Leave);
        QApplication::sendEvent(widget, &e);
        break;
    }
    case 2: {
        QMouseEvent e(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &e);
        break;
    }
    case 3: {
        QMouseEvent e(QEvent::MouseButtonRelease, QPointF(1, 1), QPointF(1, 1),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &e);
        break;
    }
    default:
        break;
    }
}

/*  — plancher de taille d'un bouton stylé.
 *
 * Poser une feuille sur un QPushButton fait basculer Qt sur la branche
 * QStyleSheetStyle de sizeFromContents, qui ne rend plus la meme taille que le
 * chemin natif. Mesure (meme libelle, police constante, sous layout) :
 *   nu 23x80 / couleur de libelle 38x114 / fond de libelle 30x100.
 * La cause n'etait pas la branche elle-meme mais l'ancrage  : la
 * feuille emettait `min-width/min-height` a la geometrie ancree du bouton,
 * c'est-a-dire la taille par DEFAUT (100x30) quand l'utilisateur n'a rien
 * declare — et en unites CSS content-box, donc Qt rajoutait par-dessus le cadre
 * natif du bouton (100x30 de contenu -> 114x38 de boite) des que le modele de
 * boite n'etait pas engage par un `border`.
 *
 * On ne re-pose donc plus de metrique en QSS. Le plancher est un minimumSize Qt
 * (vraies unites de boite, aucune inflation) dont la valeur vient de la MEME
 * source que le chemin non style : un exemplaire du meme champ hors de portee
 * de la feuille (donc sans parent), au texte, a la police et a l'icone
 * effectifs. Il est borne par la geometrie ancree : le plancher ne peut que
 * proteger le bouton d'un ecrasement par la feuille, jamais l'agrandir au-dela
 * de ce qu'il mesurerait nu — une Hauteur/Largeur declaree plus petite que la
 * taille naturelle reste donc honoree. */
static void owl_button_apply_size_floor(QWidget* w) {
    QPushButton* btn = qobject_cast<QPushButton*>(w);
    if (!btn) return;
    /* La police posee par la feuille n'atteint le widget qu'au polish. */
    btn->ensurePolished();
    QScopedPointer<QPushButton> temoin(new QPushButton(btn->text()));
    temoin->setFont(btn->font());
    if (!btn->icon().isNull()) {
        temoin->setIcon(btn->icon());
        temoin->setIconSize(btn->iconSize());
    }
    QSize canon = temoin->sizeHint();
    int aw = btn->property("owl_anchor_w").isValid()
                 ? btn->property("owl_anchor_w").toInt() : btn->width();
    int ah = btn->property("owl_anchor_h").isValid()
                 ? btn->property("owl_anchor_h").toInt() : btn->height();
    btn->setMinimumWidth(aw > 0 ? qMin(canon.width(), aw) : canon.width());
    btn->setMinimumHeight(ah > 0 ? qMin(canon.height(), ah) : canon.height());
}

/* Couleurs : on stocke la valeur RGB sur le widget via dynamic property
 * pour permettre un read-back fiable, et on applique via stylesheet.
 *
 * D.5 hover : si owl_fg_hover/owl_bg_hover sont definis, on emet un
 * second bloc QSS '<class>:hover { ... }' pour le passage souris.
 * Qt6 gere le tracking automatiquement, on a juste a livrer la regle. */
static void apply_color_stylesheet(QWidget* w) {
    int fg     = w->property("owl_fg").isValid() ? w->property("owl_fg").toInt() : -1;
    int bg     = w->property("owl_bg").isValid() ? w->property("owl_bg").toInt() : -1;
    /*  — canal alpha du fond (0-255). Absent = 255 (opaque) : la
     * feuille reste `rgb(...)` byte-identique. Seul le fond porte l'alpha ; la
     * couleur du texte (owl_fg) demeure `rgb(...)`. */
    int bgAlpha = w->property("owl_bg_alpha").isValid()
                      ? w->property("owl_bg_alpha").toInt() : 255;
    /*  — canal alpha du texte (0-255). Absent = 255 (opaque) : la
     * feuille reste `rgb(...)` byte-identique. Symétrique de owl_bg_alpha. */
    int fgAlpha = w->property("owl_fg_alpha").isValid()
                      ? w->property("owl_fg_alpha").toInt() : 255;
    /*  — fragments QSS par etat (survol/focus), corps complet
     * (couleur/fond/police/bordure) produit par owl_ui_control_set_state_style.
     * Concatenes dans la feuille unique (contrainte ). */
    QString hoverBody = w->property("owl_state_body_hover").isValid()
                            ? w->property("owl_state_body_hover").toString()
                            : QString();
    QString focusBody = w->property("owl_state_body_focus").isValid()
                            ? w->property("owl_state_body_focus").toString()
                            : QString();
    /*  — etats reconcilies avec Etat : grisé (:disabled, widget
     * desactive) et lectureSeule (:read-only, Saisie en lecture seule). Qt pose
     * la pseudo-classe tout seul quand Etat = Grisé / AffichageSeulement passe
     * le widget disabled/readonly. */
    QString disabledBody = w->property("owl_state_body_disabled").isValid()
                            ? w->property("owl_state_body_disabled").toString()
                            : QString();
    QString readonlyBody = w->property("owl_state_body_readonly").isValid()
                            ? w->property("owl_state_body_readonly").toString()
                            : QString();
    /*  — :pressed (bouton appuyé). Pseudo-classe universelle comme
     * hover/focus : Qt la pose pendant l'appui souris/clavier sur un bouton. */
    QString pressedBody = w->property("owl_state_body_pressed").isValid()
                            ? w->property("owl_state_body_pressed").toString()
                            : QString();

    /*  — fragments d'état du CadreExtérieur (fond/bordure/dégradé),
     * produits par owl_ui_control_set_frame_state. Distincts des états texte
     * ci-dessus : concaténés dans le même bloc de pseudo-classe. */
    QString frameHover = w->property("owl_frame_state_hover").isValid()
                            ? w->property("owl_frame_state_hover").toString()
                            : QString();
    QString frameFocus = w->property("owl_frame_state_focus").isValid()
                            ? w->property("owl_frame_state_focus").toString()
                            : QString();
    QString frameDisabled = w->property("owl_frame_state_disabled").isValid()
                            ? w->property("owl_frame_state_disabled").toString()
                            : QString();
    QString framePressed = w->property("owl_frame_state_pressed").isValid()
                            ? w->property("owl_frame_state_pressed").toString()
                            : QString();

    /* Fragment bordure produit par owl_ui_widget_set_cadre. Fusionne dans le
     * meme bloc "* { ... }" que le fond/couleur : les deux setStyleSheet
     * separes se remplacaient sinon (fond OU bordure, jamais les deux). */
    QString cadreBody = w->property("owl_cadre_body").isValid()
                            ? w->property("owl_cadre_body").toString()
                            : QString();

    /*  — dégradé de fond (qlineargradient/qradialgradient), fragment QSS
     * pré-construit par owl_ui_control_set_bg_gradient. Actif -> prime sur la
     * CouleurFond (emis `background:` a la place de `background-color:`). */
    QString grad = w->property("owl_bg_gradient").isValid()
                       ? w->property("owl_bg_gradient").toString()
                       : QString();

    /* Construit le corps "color/background" commun a un selecteur. Un dégradé
     * actif remplace la couleur de fond unie. */
    auto body = [&](int c, int b) {
        QString s;
        if (c >= 0) s += "color: " + owl_fg_css(c, fgAlpha) + "; ";
        if (!grad.isEmpty()) s += "background: " + grad + "; ";
        else if (b >= 0) s += "background-color: " + owl_bg_css(b, bgAlpha) + "; ";
        return s;
    };

    /*  — un dégradé de base actif doit PERSISTER à travers les états
     * (survol/focus/appuyé/grisé/lectureSeule). Le body d'état émet un
     * `background-color` PLAT (CouleurFond de l'état) qui, avec la spécificité
     * pseudo-classe (`*:focus` > base), écrasait le `background: qlineargradient`
     * de base -> le champ perdait son dégradé au focus. On ré-émet le dégradé de
     * base APRÈS le body d'état (même bloc, déclaration plus tardive => `background`
     * shorthand prime sur `background-color`). Sauf si l'état définit son PROPRE
     * dégradé (frag frame contient un q*gradient) : on le laisse alors gagner. */
    auto state_grad = [&](const QString& stateBody) -> QString {
        if (grad.isEmpty()) return QString();
        if (stateBody.contains("qlineargradient")
            || stateBody.contains("qradialgradient"))
            return QString();
        return "background: " + grad + "; ";
    };

    QString css;
    QString mainBody = body(fg, bg) + cadreBody;
    if (!mainBody.isEmpty()) {
        css += "* { " + mainBody + "} ";
    }
    if (fg >= 0 || bg >= 0 || !grad.isEmpty()) {
        /* Le selecteur universel '*' suffit pour la plupart des QWidget, MAIS
         * certains widgets composites dessinent leur texte via le QStyle natif
         * plutot que via la regle '*' :
         *  - QComboBox : le texte de la ligne affichee + les items du popup
         *    (QAbstractItemView) ne prennent pas le '* { color }'. Cibler
         *    explicitement le type et sa vue deroulante.
         *  - QListWidget / QAbstractItemView : les items passent par un
         *    delegate ; '::item' est le selecteur fiable pour leur couleur.
         * Ces regles ciblees ne matchent que si le widget (ou un enfant) est
         * du type vise : inoffensives sur les autres widgets. */
        /*  — comme le QPushButton, le chrome natif du
         * QComboBox IGNORE le fond QSS tant que le modele de boite n'est pas
         * engage par une bordure : il continue de peindre son degrade d'origine,
         * teinte de la couleur demandee. Mesure sur un RVB(255,0,0) demande sans
         * cadre : #FE8282 en haut de la boite, #FB6363 en bas — la couleur
         * demandee n'apparaissait nulle part ; un degrade demande n'etait, lui,
         * pas peint du tout. Des qu'un fond est pose, on route donc le Cadre du
         * widget dans la regle QComboBox (il engage le box-model), a defaut
         * `border: none` pour un aplat plat. */
        {
            QString cmbBody = body(fg, bg);
            if (bg >= 0 || !grad.isEmpty())
                cmbBody += cadreBody.isEmpty() ? "border: none; " : cadreBody;
            css += "QComboBox { " + cmbBody + "} ";
        }
        if (fg >= 0)
            css += "QComboBox QAbstractItemView { " + body(fg, bg) + "} ";
        if (fg >= 0)
            css += "QAbstractItemView::item { color: " + owl_fg_css(fg, fgAlpha) + "; } ";
        /* QPushButton peint son label via le QStyle natif : le '* { color }'
         * ne l'atteint pas. Cibler explicitement le type.
         *  — de plus, le chrome natif du bouton IGNORE background-color
         * tant que le modele de boite QSS n'est pas engage par une bordure. Des
         * qu'un fond est pose (bg >= 0), on route le Cadre du widget dans la
         * regle QPushButton (il engage le box-model) ; a defaut `border: none`
         * pour un aplat plat. Sans ca `Style..Bouton..CouleurFond` reste sans
         * effet visible (le bouton garde son degrade natif). */
        {
            QString btnBody = body(fg, bg);
            if (bg >= 0 || !grad.isEmpty())
                btnBody += cadreBody.isEmpty() ? "border: none; " : cadreBody;
            /*  — re-borne les rayons de coin à la MOITIÉ de la hauteur/largeur
             * FINALE du bouton. Un QPushButton sans liseré rend ses coins carrés dès
             * que border-radius dépasse hauteur/2 (le chrome ne clippe pas au-delà).
             * cadreBody porte le rayon brut (hauteur inconnue au set_cadre) ; ici la
             * feuille est régénérée après layout, q->height() est la hauteur réelle.
             * Les déclarations émises ici priment (mêmes propriétés, plus tardives)
             * sur celles de cadreBody. Restreint au bouton : les QFrame clippent bien. */
            if (qobject_cast<QPushButton*>(w)
                && (bg >= 0 || !grad.isEmpty()) && !cadreBody.isEmpty()) {
                /* Hauteur/largeur ANCRÉES (owl_anchor_*) = dimensions déclarées,
                 * capturées avant que le QStyleSheetStyle ne gonfle le sizeHint.
                 * Plus fiables que q->height() ici : la feuille n'est pas
                 * régénérée à la hauteur finale posée par le layout. */
                int hh = w->property("owl_anchor_h").isValid()
                             ? w->property("owl_anchor_h").toInt() : w->height();
                int ww = w->property("owl_anchor_w").isValid()
                             ? w->property("owl_anchor_w").toInt() : w->width();
                int cap = 0x7fffffff;
                if (hh > 0) cap = qMin(cap, hh / 2);
                if (ww > 0) cap = qMin(cap, ww / 2);
                auto rp = [&](const char* prop) {
                    QVariant v = w->property(prop);
                    return v.isValid() ? v.toInt() : 0;
                };
                struct { const char* prop; const char* css; } coins[4] = {
                    { "owl_cadre_r_hg", "border-top-left-radius" },
                    { "owl_cadre_r_hd", "border-top-right-radius" },
                    { "owl_cadre_r_bg", "border-bottom-left-radius" },
                    { "owl_cadre_r_bd", "border-bottom-right-radius" },
                };
                for (auto& c : coins) {
                    int r = rp(c.prop);
                    if (r > cap) btnBody += QString("%1: %2px; ").arg(c.css).arg(cap);
                }
            }
            /* Geometrie ANCREE du bouton (owl_anchor_*) : memorisee au premier
             * calcul de feuille, avant que le QStyleSheetStyle n'ait gonfle le
             * sizeHint. Le gate Étire re-appelle cette feuille APRES l'insertion
             * dans le layout ; relire `w->width()/height()` a ce moment donnerait
             * la taille deja gonflee. Sert de reference aux clamps de rayon
             * (, ) et au plancher de taille. */
            if (qobject_cast<QPushButton*>(w)) {
                if (!w->property("owl_anchor_w").isValid() && w->width() > 0)
                    w->setProperty("owl_anchor_w", w->width());
                if (!w->property("owl_anchor_h").isValid() && w->height() > 0)
                    w->setProperty("owl_anchor_h", w->height());
            }
            css += "QPushButton { " + btnBody + "} ";
        }
        /* QLineEdit : le '* { color }' suffit tant que la feuille ne contient
         * AUCUNE pseudo-classe. Mais des qu'un fragment d'etat (`*:hover` /
         * `*:focus`) coexiste et que le champ prend le focus (1er champ focalise
         * a l'ouverture sur Windows), le style natif/Fusion cesse d'honorer le
         * selecteur universel pour le texte du QLineEdit et la couleur retombe
         * au defaut (noir). Le selecteur de TYPE `QLineEdit` (specificite
         * superieure a '*', pseudo-classe non specifiee -> matche tout etat, y
         * compris focus) reancre la couleur de base ; `*:hover` (pseudo-classe,
         * specificite superieure) garde la priorite au survol. */
        QString lineBody = body(fg, bg);
        /*  — un QLineEdit rend ses coins CARRES des que le border-radius
         * (Arrondi du Cadre de la ZoneSaisie, porte par le selecteur '*') depasse
         * la moitie de sa hauteur : le chrome cesse de clipper au-dela et retombe
         * a l'angle droit (meme classe que le QPushButton, ). C'est le cas
         * d'un champ « bas » (Retraits qui rognent la zone, ou Hauteur serree)
         * combine a un Arrondi genereux. On re-borne les rayons a la demi-hauteur/
         * demi-largeur REELLE et on emet le resultat dans la regle de TYPE
         * QLineEdit (specificite > '*') pour qu'il prime : le champ rend alors une
         * pilule au lieu d'un rectangle. Rayon deja sous la borne (ou pas de Cadre
         * arrondi) : rien d'emis, le '*' clippe deja bien. */
        if (qobject_cast<QLineEdit*>(w)) {
            int hh = w->height();
            int ww = w->width();
            int cap = 0x7fffffff;
            if (hh > 0) cap = qMin(cap, hh / 2);
            if (ww > 0) cap = qMin(cap, ww / 2);
            auto rp = [&](const char* prop) {
                QVariant v = w->property(prop);
                return v.isValid() ? v.toInt() : 0;
            };
            struct { const char* prop; const char* css; } coins[4] = {
                { "owl_cadre_r_hg", "border-top-left-radius" },
                { "owl_cadre_r_hd", "border-top-right-radius" },
                { "owl_cadre_r_bg", "border-bottom-left-radius" },
                { "owl_cadre_r_bd", "border-bottom-right-radius" },
            };
            for (auto& c : coins) {
                int r = rp(c.prop);
                if (r > cap) lineBody += QString("%1: %2px; ").arg(c.css).arg(cap);
            }
            /*  — retraits de la ZoneSaisie émis en padding QSS (le fond
             * remplit toute la boite, le padding agrandit la hauteur/largeur ou
             * insère le contenu sous une Hauteur figée). Cf. set_margin_* qui
             * route les retraits d'un QLineEdit vers ces propriétés. */
            int pt = rp("owl_pad_t"), pb = rp("owl_pad_b");
            int pl = rp("owl_pad_l"), pr = rp("owl_pad_r");
            if (pt || pb || pl || pr) {
                lineBody += QString("padding: %1px %2px %3px %4px; ")
                                .arg(pt).arg(pr).arg(pb).arg(pl);
            }
        }
        css += "QLineEdit { " + lineBody + "} ";
    }
    if (!hoverBody.isEmpty() || !frameHover.isEmpty()) {
        css += "*:hover { " + hoverBody + frameHover
             + state_grad(hoverBody + frameHover) + "} ";
    }
    if (!focusBody.isEmpty() || !frameFocus.isEmpty()) {
        css += "*:focus { " + focusBody + frameFocus
             + state_grad(focusBody + frameFocus) + "} ";
    }
    if (!disabledBody.isEmpty() || !frameDisabled.isEmpty()) {
        /* :disabled — comme :hover/:focus, universel. La regle QSS prime sur la
         * palette "grise" par defaut de Qt : le fond utilisateur s'affiche. */
        css += "*:disabled { " + disabledBody + frameDisabled
             + state_grad(disabledBody + frameDisabled) + "} ";
    }
    if (!readonlyBody.isEmpty()) {
        /* :read-only — pseudo-classe specifique aux widgets de saisie. Scope au
         * type QLineEdit (pas '*') : seul le champ Saisie l'expose. */
        css += "QLineEdit:read-only { " + readonlyBody
             + state_grad(readonlyBody) + "} ";
    }
    if (!pressedBody.isEmpty() || !framePressed.isEmpty()) {
        /* :pressed — bouton appuyé. Universel comme hover/focus. */
        css += "*:pressed { " + pressedBody + framePressed
             + state_grad(pressedBody + framePressed) + "} ";
    }

    /*  — les clamps de rayon  (QPushButton) et 
     * (QLineEdit) ne bornaient QUE la règle de BASE. Les fragments d'ÉTAT
     * (`:focus`/`:pressed`/`:hover`/`:disabled`) portaient leur Arrondi BRUT,
     * émis dans les blocs `*:pseudo` ci-dessus (spécificité pseudo-classe >
     * type, donc prioritaires sur la base clampée). Un Arrondi d'état >
     * demi-hauteur rend le cadre d'état CARRÉ (le chrome ne clippe pas au-delà)
     * -> gros cadre rectangulaire au focus/appui. On re-borne chaque rayon
     * d'état à la même borne (min(h,w)/2) et on l'émet dans une règle
     * TYPE+pseudo-classe (`QPushButton:focus`, spécificité > `*:focus`) qui
     * prime. Rayon d'état sous la borne : rien d'émis, le fragment `*:pseudo`
     * clippe déjà bien. Restreint aux widgets dont le chrome ne clippe pas
     * (QPushButton, QLineEdit) ; les QFrame découpent correctement. */
    bool isBtn  = qobject_cast<QPushButton*>(w) != nullptr;
    bool isEdit = qobject_cast<QLineEdit*>(w) != nullptr;
    if (isBtn || isEdit) {
        /* Bouton : dimensions ANCRÉES (owl_anchor_*, capturées avant que le
         * QStyleSheetStyle ne gonfle le sizeHint, cf. ). Saisie : la
         * hauteur réelle suffit. */
        int hh, ww;
        if (isBtn) {
            hh = w->property("owl_anchor_h").isValid()
                     ? w->property("owl_anchor_h").toInt() : w->height();
            ww = w->property("owl_anchor_w").isValid()
                     ? w->property("owl_anchor_w").toInt() : w->width();
        } else {
            hh = w->height();
            ww = w->width();
        }
        int cap = 0x7fffffff;
        if (hh > 0) cap = qMin(cap, hh / 2);
        if (ww > 0) cap = qMin(cap, ww / 2);
        const char* type = isBtn ? "QPushButton" : "QLineEdit";
        struct { const char* frameProp; const char* bodyProp; const char* pseudo; } st[5] = {
            { "owl_frame_r_focus",    "owl_state_r_focus",    ":focus" },
            { "owl_frame_r_pressed",  "owl_state_r_pressed",  ":pressed" },
            { "owl_frame_r_hover",    "owl_state_r_hover",    ":hover" },
            { "owl_frame_r_disabled", "owl_state_r_disabled", ":disabled" },
            { nullptr,                "owl_state_r_readonly", ":read-only" },
        };
        for (auto& s : st) {
            int r = 0;
            if (s.frameProp) {
                QVariant fv = w->property(s.frameProp);
                if (fv.isValid()) r = qMax(r, fv.toInt());
            }
            QVariant bv = w->property(s.bodyProp);
            if (bv.isValid()) r = qMax(r, bv.toInt());
            if (r > cap)
                css += QString("%1%2 { border-radius: %3px; } ")
                           .arg(type).arg(s.pseudo).arg(cap);
        }
    }

    w->setStyleSheet(css);
    owl_button_apply_size_floor(w);
}

extern "C" void owl_ui_control_set_text_color_alpha(OwlControl* w, int rgb, int alpha) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (rgb < 0) {
        widget->setProperty("owl_fg", QVariant());
        widget->setProperty("owl_fg_alpha", QVariant());
    } else {
        widget->setProperty("owl_fg", rgb);
        /* alpha 255 -> ne pose pas la property (défaut opaque) : la feuille
         * émise reste `rgb(...)` byte-identique pour le texte opaque. */
        widget->setProperty("owl_fg_alpha",
                            alpha >= 255 ? QVariant() : QVariant(alpha));
    }
    apply_color_stylesheet(widget);
}

extern "C" void owl_ui_control_set_text_color(OwlControl* w, int rgb) {
    owl_ui_control_set_text_color_alpha(w, rgb, 255);
}

extern "C" int owl_ui_control_get_text_color(OwlControl* w) {
    if (!w) return -1;
    QVariant v = reinterpret_cast<QWidget*>(w)->property("owl_fg");
    return v.isValid() ? v.toInt() : -1;
}

extern "C" void owl_ui_control_set_bg_color_alpha(OwlControl* w, int rgb, int alpha) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (rgb < 0) {
        widget->setProperty("owl_bg", QVariant());
        widget->setProperty("owl_bg_alpha", QVariant());
    } else {
        widget->setProperty("owl_bg", rgb);
        /* alpha 255 -> ne pose pas la property (défaut opaque) : la feuille
         * émise reste `rgb(...)` byte-identique pour les fonds opaques. */
        widget->setProperty("owl_bg_alpha",
                            alpha >= 255 ? QVariant() : QVariant(alpha));
    }
    apply_color_stylesheet(widget);
}

extern "C" void owl_ui_control_set_bg_color(OwlControl* w, int rgb) {
    owl_ui_control_set_bg_color_alpha(w, rgb, 255);
}

extern "C" int owl_ui_control_get_bg_color(OwlControl* w) {
    if (!w) return -1;
    QVariant v = reinterpret_cast<QWidget*>(w)->property("owl_bg");
    return v.isValid() ? v.toInt() : -1;
}

/* /1027 — construit le fragment QSS d'un dégradé de fond
 * (qlineargradient/qradialgradient) à partir de (type, angle, couleurs BGR).
 * Retourne une chaîne VIDE si le type n'est pas linéaire/radial (dégradéAucun) :
 * l'appelant interprète le vide comme "pas de dégradé actif". Factorisé entre le
 * fond des widgets (owl_ui_control_set_bg_gradient) et le fond de la Fenêtre
 * (owl_ui_window_set_panel_style). */
static QString build_gradient_qss(int type, int angle,
                                  int startColor, int startAlpha,
                                  int endColor, int endAlpha) {
    if (type != OWL_GRAD_LINEAIRE && type != OWL_GRAD_RADIAL)
        return QString();
    /* Arrêts BGR canon (R + V*256 + B*65536), alpha 0-255 par arrêt : un
     * arrêt opaque (alpha>=255) reste `rgb(...)` byte-identique. */
    const QString startCss = owl_bg_css(startColor, startAlpha);
    const QString endCss   = owl_bg_css(endColor, endAlpha);
    if (type == OWL_GRAD_RADIAL) {
        /* Radial centré (angle ignoré) : coords en objectBoundingMode (0..1). */
        return QString("qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, "
                       "stop:0 %1, stop:1 %2)").arg(startCss, endCss);
    }
    /* Linéaire : angle -> ligne passant par le centre (0.5, 0.5).
     * Convention alignée dRectangleDégradé : 0° = horizontal G->D,
     * 90° = vertical H->B. direction = (cos θ, sin θ). */
    const double pi = 3.14159265358979323846;
    const double rad = angle * (pi / 180.0);
    const double c = std::cos(rad), s = std::sin(rad);
    const double x1 = 0.5 - 0.5 * c, y1 = 0.5 - 0.5 * s;
    const double x2 = 0.5 + 0.5 * c, y2 = 0.5 + 0.5 * s;
    return QString("qlineargradient(x1:%1, y1:%2, x2:%3, y2:%4, "
                   "stop:0 %5, stop:1 %6)")
               .arg(x1, 0, 'g', 4).arg(y1, 0, 'g', 4)
               .arg(x2, 0, 'g', 4).arg(y2, 0, 'g', 4)
               .arg(startCss, endCss);
}

/*  — dégradé de fond via QSS (qlineargradient/qradialgradient). Le
 * fragment est stocké en dynamic property owl_bg_gradient, consommé par
 * apply_color_stylesheet (prime sur background-color). type != linéaire/radial
 * (dont dégradéAucun) retire le dégradé -> le fond uni reprend. */
extern "C" void owl_ui_control_set_bg_gradient(OwlControl* w, int type, int angle,
                                               int startColor, int startAlpha,
                                               int endColor, int endAlpha) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QString grad = build_gradient_qss(type, angle, startColor, startAlpha,
                                      endColor, endAlpha);
    if (grad.isEmpty()) {
        /* dégradéAucun : ne rien faire s'il n'y avait aucun dégradé posé.
         * Reappliquer apply_color_stylesheet ici écraserait une feuille QSS
         * spécialisée déjà en place (interrupteur/sélecteur QCheckBox). On ne
         * clear + reapply que sur une vraie transition dégradé -> aucun. */
        if (!widget->property("owl_bg_gradient").isValid()) return;
        widget->setProperty("owl_bg_gradient", QVariant());
        apply_color_stylesheet(widget);
        return;
    }
    widget->setProperty("owl_bg_gradient", grad);
    apply_color_stylesheet(widget);
}

/*  — style complet d'un etat (survol/focus). Construit un fragment
 * QSS (couleur, fond, police, bordure) stocke en dynamic property
 * owl_state_body_hover/focus, puis regenere la feuille UNIQUE via
 * apply_color_stylesheet (contrainte  : jamais un setStyleSheet
 * additionnel). Couleurs formatees 'rgb(R, G, B)' (avec espaces) — lisible
 * et symetrique de la feuille inspectable via __DebugFeuilleStyle. */
extern "C" void owl_ui_control_set_state_style(OwlControl* w, int stateIdx,
                                               const OwlStateTextStyle* t,
                                               const OwlCadreSpec* border) {
    if (!w) return;
    QWidget* q = reinterpret_cast<QWidget*>(w);
    auto rgb_sp = [](int v) {
        return QString("rgb(%1, %2, %3)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF);
    };
    /*  — texte d'état alpha-aware. Opaque -> rgb_sp byte-identique
     * (format espacé propre aux fragments d'état) ; alpha<255 -> rgba espacé. */
    auto fg_sp = [&](int v, int alpha) {
        if (alpha >= 255) return rgb_sp(v);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF)
            .arg(alpha / 255.0, 0, 'g', 4);
    };
    QString body;
    if (t) {
        /*  — `> 0` (et non `>= 0`) : un etat non configure (survol/focus/
         * grise/lectureSeule) seed sa Couleur a 0 (=noir) ; emettre `color: black`
         * dans le fragment `*:focus`/`*:hover` ecraserait la Couleur de base (ex.
         * une ZoneSaisie rouge devient noire une fois focus). Une couleur d'etat a
         * 0 est indistinguable du seed -> traitee comme "non posee" (le texte garde
         * la couleur de base). Une couleur d'etat explicite (> 0) prime. */
        if (t->fg > 0) body += "color: " + fg_sp(t->fg, t->fgAlpha) + "; ";
        /*  — fond d'état alpha-aware (fg_sp = même format rgba espacé) ;
         * opaque => rgb_sp byte-identique. */
        if (t->bg >= 0) body += "background-color: " + fg_sp(t->bg, t->bgAlpha) + "; ";
        if (t->bold == 1)      body += "font-weight: bold; ";
        else if (t->bold == 0) body += "font-weight: normal; ";
        if (t->italic == 1)      body += "font-style: italic; ";
        else if (t->italic == 0) body += "font-style: normal; ";
        if (t->font_size > 0)
            body += QString("font-size: %1pt; ").arg(t->font_size);
        if (t->pad_l > 0) body += QString("padding-left: %1px; ").arg(t->pad_l);
        if (t->pad_r > 0) body += QString("padding-right: %1px; ").arg(t->pad_r);
        if (t->pad_t > 0) body += QString("padding-top: %1px; ").arg(t->pad_t);
        if (t->pad_b > 0) body += QString("padding-bottom: %1px; ").arg(t->pad_b);
    }
    if (border && border->thickness > 0) {
        body += QString("border: %1px %2 %3; ")
                    .arg(border->thickness)
                    .arg(cadre_line_style_to_css(border->line_style))
                    .arg(rgb_sp(border->color));
    }
    /*  — arrondi d'un état émis même sans bordure (Epaisseur=0). */
    if (border && border->corner_radius > 0)
        body += QString("border-radius: %1px; ").arg(border->corner_radius);
    const char* prop;
    /*  — mémorise le rayon BRUT de l'état (symétrique de set_frame_state)
     * pour re-borner le cadre d'état d'un QLineEdit à la demi-hauteur. */
    const char* rprop;
    switch (stateIdx) {
        case 1:  prop = "owl_state_body_focus";    rprop = "owl_state_r_focus";    break;
        case 2:  prop = "owl_state_body_disabled"; rprop = "owl_state_r_disabled"; break;
        case 3:  prop = "owl_state_body_readonly"; rprop = "owl_state_r_readonly"; break;
        case 4:  prop = "owl_state_body_pressed";  rprop = "owl_state_r_pressed";  break;   /*  */
        default: prop = "owl_state_body_hover";    rprop = "owl_state_r_hover";    break;
    }
    int stateRadius = (border && border->corner_radius > 0) ? border->corner_radius : 0;
    if (body.isEmpty()) {
        q->setProperty(prop, QVariant());
        q->setProperty(rprop, QVariant());
    } else {
        q->setProperty(prop, body);
        if (stateRadius > 0) q->setProperty(rprop, stateRadius);
        else                 q->setProperty(rprop, QVariant());
    }
    apply_color_stylesheet(q);
}

/*  — surcharge d'état du CadreExtérieur (fond/bordure/dégradé). Fragment
 * QSS stocké dans owl_frame_state_* puis fusionné dans le bloc de pseudo-classe
 * par apply_color_stylesheet. Couleurs au format compact `rgb(r,g,b)` (symétrique
 * du dégradé , distinct du `rgb(r, g, b)` des états texte). */
extern "C" void owl_ui_control_set_frame_state(OwlControl* w, int stateIdx,
                                               int bg, int bgAlpha,
                                               int borderColor, int borderColorAlpha,
                                               int borderThick, int borderStyle,
                                               int borderRadius, int gradType,
                                               int gradAngle, int gradStart,
                                               int gradStartAlpha, int gradEnd,
                                               int gradEndAlpha) {
    if (!w) return;
    QWidget* q = reinterpret_cast<QWidget*>(w);
    /* OWL stocke en BGR (canon : R + V*256 + B*65536).  — alpha-aware :
     * opaque => rgb(r,g,b) byte-identique, rgba(...) sinon (état translucide). */
    auto rgb = [](int v, int alpha = 255) {
        int r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
        if (alpha >= 255) return QString("rgb(%1,%2,%3)").arg(r).arg(g).arg(b);
        return QString("rgba(%1,%2,%3,%4)")
            .arg(r).arg(g).arg(b).arg(alpha / 255.0, 0, 'g', 4);
    };
    QString body;
    /* Un dégradé actif prime sur CouleurFond : les deux fragments
     * cohabitent, `background` (shorthand) l'emporte visuellement. */
    if (bg >= 0) body += "background-color: " + rgb(bg, bgAlpha) + "; ";
    /* Arrêts de dégradé d'état, alpha porté ( : un arrêt non défini rend
     * transparent — alpha 0 — comme le dégradé de base, pas blanc/noir parasite). */
    QString grad = build_gradient_qss(gradType, gradAngle, gradStart, gradStartAlpha,
                                      gradEnd, gradEndAlpha);
    if (!grad.isEmpty()) body += "background: " + grad + "; ";
    if (borderThick > 0) {
        body += QString("border: %1px %2 %3; ")
                    .arg(borderThick)
                    .arg(cadre_line_style_to_css(borderStyle))
                    .arg(borderColor > 0 ? rgb(borderColor, borderColorAlpha) : rgb(0));
    } else if (borderColor > 0) {
        /* Cadre..Couleur seule (ex. focus) : le seed du template vaut 0 (noir),
         * indistinguable d'un noir explicite -> gate `> 0` (comme la Couleur
         * texte des états, ). */
        body += "border-color: " + rgb(borderColor, borderColorAlpha) + "; ";
    }
    if (borderRadius > 0)
        body += QString("border-radius: %1px; ").arg(borderRadius);

    const char* prop;
    /*  — mémorise le rayon BRUT de l'état pour le re-borner à la
     * demi-hauteur au moment de l'assemblage (cf. apply_color_stylesheet) :
     * un Arrondi d'état > hauteur/2 rendrait le cadre d'état CARRÉ. */
    const char* rprop;
    switch (stateIdx) {
        case 1:  prop = "owl_frame_state_focus";    rprop = "owl_frame_r_focus";    break;
        case 2:  prop = "owl_frame_state_disabled"; rprop = "owl_frame_r_disabled"; break;
        case 4:  prop = "owl_frame_state_pressed";  rprop = "owl_frame_r_pressed";  break;
        default: prop = "owl_frame_state_hover";    rprop = "owl_frame_r_hover";    break;
    }
    if (body.isEmpty()) {
        /* Slot vide (cas dominant : widget sans surcharge d'état). Ne réappliquer
         * la feuille QUE sur une vraie transition posé -> vide : sinon on
         * écraserait une feuille QSS spécialisée déjà en place (Interrupteur/
         * Sélecteur composites, patron owl_ui_control_set_bg_gradient ). */
        if (!q->property(prop).isValid()) return;
        q->setProperty(prop, QVariant());
        q->setProperty(rprop, QVariant());
    } else {
        q->setProperty(prop, body);
        if (borderRadius > 0) q->setProperty(rprop, borderRadius);
        else                  q->setProperty(rprop, QVariant());
    }
    apply_color_stylesheet(q);
}

/* ------- Police / typo (QFont) ------- */

extern "C" void owl_ui_control_set_font_name(OwlControl* w, const char* name_utf8) {
    if (!w || !name_utf8 || !*name_utf8) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QString fam = QString::fromUtf8(name_utf8);
    /* Une constante iPolice* (iPoliceArial, iSwiss, ...) arrive ici sous forme
     * de sentinelle entiere serialisee : ce n'est PAS un nom de famille valide.
     * On la resout vers la fonte embarquee equivalente AVANT setFamily, sinon
     * Qt ne trouve pas la famille et retombe sur un fallback. Une chaine libre
     * ("Arial", "Liberation Sans") n'est pas numerique -> passe telle quelle. */
    bool isSentinel = false;
    long long sv = fam.toLongLong(&isSentinel);
    if (isSentinel) {
        const char* emb = owl_ui_ipol_embedded_family(sv);
        if (emb != nullptr) {
            if (*emb == '\0') return;   /* iDefaut : garde la police heritee */
            fam = QString::fromUtf8(emb);
        }
        /* emb == NULL : entier hors iFamille (improbable) -> laisse tel quel. */
    }
    QFont f = widget->font();
    f.setFamily(fam);
    widget->setFont(f);
}

extern "C" const char* owl_ui_control_get_font_name(OwlControl* w) {
    if (!w) return "";
    g_ctrl_text_buf = reinterpret_cast<QWidget*>(w)->font().family().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_control_set_font_size(OwlControl* w, int point_size) {
    if (!w || point_size <= 0) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setPointSize(point_size);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_size(OwlControl* w) {
    if (!w) return 0;
    int s = reinterpret_cast<QWidget*>(w)->font().pointSize();
    return s > 0 ? s : 0;
}

extern "C" void owl_ui_control_set_font_bold(OwlControl* w, int bold) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setBold(bold != 0);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_bold(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->font().bold() ? 1 : 0;
}

extern "C" void owl_ui_control_set_font_italic(OwlControl* w, int italic) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setItalic(italic != 0);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_italic(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->font().italic() ? 1 : 0;
}

extern "C" void owl_ui_control_set_font_underline(OwlControl* w, int underline) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setUnderline(underline != 0);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_underline(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->font().underline() ? 1 : 0;
}

extern "C" void owl_ui_control_set_font_strikeout(OwlControl* w, int strikeout) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setStrikeOut(strikeout != 0);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_strikeout(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->font().strikeOut() ? 1 : 0;
}

/* ---- Etirement horizontal des caracteres (QFont::stretch) ----
 * Reglage unique deterministe : la valeur de stretch passee fait foi
 * (100 = nominal, 75 = condense, 125 = etendu, 150 = elargi). */
extern "C" void owl_ui_control_set_font_stretch(OwlControl* w, int stretch) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    QFont f = widget->font();
    f.setStretch(stretch > 0 ? stretch : QFont::Unstretched);
    widget->setFont(f);
}

extern "C" int owl_ui_control_get_font_stretch(OwlControl* w) {
    if (!w) return 100;
    int s = reinterpret_cast<QWidget*>(w)->font().stretch();
    /* QFont::Unstretched vaut 0 quand jamais positionne ; on le presente
     * comme le nominal 100 pour un round-trip stable cote enum. */
    return s > 0 ? s : 100;
}

/* PoliceTailleMin — option "Reduire la police" (canon).
 * V1 : fit initial seulement. On stocke la valeur min en dynamic property,
 * puis on diminue la taille de police pas-a-pas tant que le texte ne tient
 * pas dans la largeur du widget (jusqu'au floor min_size). En V2 on
 * installera un event filter sur QResizeEvent + setText pour re-fit. */
static void apply_font_min_size_fit(QWidget* widget, int min_size) {
    if (!widget || min_size <= 0) return;
    QLabel* lbl = qobject_cast<QLabel*>(widget);
    if (!lbl) return;  /* V1 : QLabel uniquement (canon : Libelle only). */
    QString text = lbl->text();
    if (text.isEmpty()) return;
    int avail = lbl->width() - lbl->contentsMargins().left()
                              - lbl->contentsMargins().right();
    if (avail <= 0) return;
    QFont f = lbl->font();
    int size = f.pointSize() > 0 ? f.pointSize() : 11;
    while (size > min_size) {
        QFontMetrics fm(f);
        if (fm.horizontalAdvance(text) <= avail) break;
        size--;
        f.setPointSize(size);
    }
    lbl->setFont(f);
}
extern "C" void owl_ui_control_set_font_min_size(OwlControl* w, int size) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->setProperty("owl_font_min_size", size);
    apply_font_min_size_fit(widget, size);
}
extern "C" int owl_ui_control_get_font_min_size(OwlControl* w) {
    if (!w) return 0;
    QVariant v = reinterpret_cast<QWidget*>(w)->property("owl_font_min_size");
    return v.isValid() ? v.toInt() : 0;
}

/* ---- Lot C : marges interieures (canon).
 * QWidget::contentsMargins encapsule (left, top, right, bottom). Setters
 * atomiques : on read-modify-write. */
/*  — sur un Champ Saisie (QLineEdit), un retrait doit se comporter comme
 * un padding CSS : le fond (CouleurFond) remplit TOUTE la boite et le retrait
 * agrandit la hauteur/largeur (ou insère le contenu quand une Hauteur est figée),
 * au lieu d'un setContentsMargins qui rogne le contents-rect du QLineEdit et carve
 * des bandes transparentes hors du fond (le champ paraissait rester à sa hauteur
 * minimale malgré Hauteur/retraits). Le padding est mémorisé en propriété et émis
 * dans la règle QSS `QLineEdit { padding: ... }` (cf. apply_color_stylesheet). Les
 * autres widgets gardent le setContentsMargins historique. */
static bool owl_lineedit_store_pad(QWidget* widget, const char* prop, int px) {
    if (!qobject_cast<QLineEdit*>(widget)) return false;
    widget->setProperty(prop, px > 0 ? px : 0);
    apply_color_stylesheet(widget);
    return true;
}
extern "C" void owl_ui_control_set_margin_left(OwlControl* w, int px) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (owl_lineedit_store_pad(widget, "owl_pad_l", px)) return;
    QMargins m = widget->contentsMargins();
    m.setLeft(px > 0 ? px : 0);
    widget->setContentsMargins(m);
}
extern "C" void owl_ui_control_set_margin_right(OwlControl* w, int px) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (owl_lineedit_store_pad(widget, "owl_pad_r", px)) return;
    QMargins m = widget->contentsMargins();
    m.setRight(px > 0 ? px : 0);
    widget->setContentsMargins(m);
}
extern "C" void owl_ui_control_set_margin_top(OwlControl* w, int px) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (owl_lineedit_store_pad(widget, "owl_pad_t", px)) return;
    QMargins m = widget->contentsMargins();
    m.setTop(px > 0 ? px : 0);
    widget->setContentsMargins(m);
}
extern "C" void owl_ui_control_set_margin_bottom(OwlControl* w, int px) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    if (owl_lineedit_store_pad(widget, "owl_pad_b", px)) return;
    QMargins m = widget->contentsMargins();
    m.setBottom(px > 0 ? px : 0);
    widget->setContentsMargins(m);
}

extern "C" void owl_ui_lineedit_set_max_length(OwlControl* edit, int max_length) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    /* 0 ou valeur negative = illimite. Qt utilise INT32_MAX comme defaut. */
    le->setMaxLength(max_length > 0 ? max_length : 32767);
}

extern "C" int owl_ui_lineedit_get_max_length(OwlControl* edit) {
    if (!edit) return 0;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return 0;
    int m = le->maxLength();
    return m == 32767 ? 0 : m;
}


extern "C" void owl_ui_lineedit_set_placeholder(OwlControl* edit, const char* text_utf8) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (le) le->setPlaceholderText(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
}

extern "C" const char* owl_ui_lineedit_get_placeholder(OwlControl* edit) {
    if (!edit) return "";
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = le->placeholderText().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_lineedit_set_password_mode(OwlControl* edit, int password) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    le->setEchoMode(password ? QLineEdit::Password : QLineEdit::Normal);
}

extern "C" int owl_ui_lineedit_get_password_mode(OwlControl* edit) {
    if (!edit) return 0;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    return (le && le->echoMode() == QLineEdit::Password) ? 1 : 0;
}

extern "C" int owl_ui_lineedit_get_cursor(OwlControl* edit) {
    if (!edit) return 1;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return 1;
    int p = le->hasSelectedText() ? le->selectionStart() : le->cursorPosition();
    return p + 1; /* canon = 1-based */
}

extern "C" void owl_ui_lineedit_set_cursor(OwlControl* edit, int pos_1based) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    int p = pos_1based - 1;
    if (p < 0) p = 0;
    int len = le->text().length();
    if (p > len) p = len;
    if (le->hasSelectedText()) {
        int end = le->selectionStart() + le->selectedText().length();
        le->setSelection(p, end - p > 0 ? end - p : 0);
    } else {
        le->setCursorPosition(p);
    }
}

extern "C" int owl_ui_lineedit_get_end_cursor(OwlControl* edit) {
    if (!edit) return 1;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return 1;
    int p;
    if (le->hasSelectedText()) {
        p = le->selectionStart() + le->selectedText().length();
    } else {
        p = le->cursorPosition();
    }
    return p + 1;
}

extern "C" void owl_ui_lineedit_set_end_cursor(OwlControl* edit, int pos_1based) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    int end = pos_1based - 1;
    if (end < 0) end = 0;
    int len = le->text().length();
    if (end > len) end = len;
    int start = le->hasSelectedText() ? le->selectionStart() : le->cursorPosition();
    int sel_len = end - start;
    if (sel_len < 0) sel_len = 0;
    le->setSelection(start, sel_len);
}

extern "C" const char* owl_ui_lineedit_get_selected_text(OwlControl* edit) {
    if (!edit) return "";
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) { g_ctrl_text_buf.clear(); return ""; }
    g_ctrl_text_buf = le->selectedText().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" void owl_ui_lineedit_set_selected_text(OwlControl* edit, const char* text_utf8) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    QString s = QString::fromUtf8(text_utf8 ? text_utf8 : "");
    if (le->hasSelectedText()) {
        /* Remplace la selection. */
        QString full = le->text();
        int start = le->selectionStart();
        int sellen = le->selectedText().length();
        full.replace(start, sellen, s);
        le->setText(full);
        le->setCursorPosition(start + s.length());
    } else {
        /* Pas de selection : insert au cursor. */
        le->insert(s);
    }
}

extern "C" int owl_ui_lineedit_get_modified(OwlControl* edit) {
    if (!edit) return 0;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    return (le && le->isModified()) ? 1 : 0;
}

extern "C" void owl_ui_lineedit_set_modified(OwlControl* edit, int modified) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (le) le->setModified(modified != 0);
}

/* Conversion masque canon → Qt input mask :
 *   canon 9 → Qt 9 (digit required)
 *   canon A → Qt A (letter required)
 *   canon C → Qt N (alphanumeric required)
 *   autre → literal (Qt utilise '\\' pour echapper si char est reserve).
 * Pour la conversion inverse (read-back), on fait l'inverse. */
extern "C" void owl_ui_lineedit_set_mask(OwlControl* edit, const char* wl_mask) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    if (!wl_mask || !*wl_mask) {
        le->setInputMask(QString());
        return;
    }
    QString qt_mask;
    for (const char* p = wl_mask; *p; p++) {
        char c = *p;
        if (c == '9' || c == 'A') {
            qt_mask += QChar(c);
        } else if (c == 'C') {
            qt_mask += QChar('N');
        } else if (c == '#' || c == 'x' || c == 'X' ||
                   c == 'B' || c == 'b' || c == 'H' ||
                   c == 'h' || c == 'D' || c == 'd' ||
                   c == 'a' || c == 'n' || c == 'o' || c == 'O' ||
                   c == '0' || c == '\\') {
            /* Caracteres reserves Qt — escape avec '\\' pour literal. */
            qt_mask += QChar('\\');
            qt_mask += QChar(c);
        } else {
            qt_mask += QChar(c);
        }
    }
    le->setInputMask(qt_mask);
}

extern "C" const char* owl_ui_lineedit_get_mask(OwlControl* edit) {
    if (!edit) return "";
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) { g_ctrl_text_buf.clear(); return ""; }
    QString qt_mask = le->inputMask();
    /* Qt ajoute parfois ";char" pour le blank — on le strippe pour la
     * conversion vers canon. */
    int sep = qt_mask.indexOf(QChar(';'));
    if (sep >= 0) qt_mask.truncate(sep);
    QString wl_mask;
    for (int i = 0; i < qt_mask.length(); i++) {
        QChar c = qt_mask.at(i);
        if (c == QChar('\\') && i + 1 < qt_mask.length()) {
            /* Escape Qt -> literal en canon. */
            wl_mask += qt_mask.at(++i);
        } else if (c == QChar('9') || c == QChar('A')) {
            wl_mask += c;
        } else if (c == QChar('N')) {
            wl_mask += QChar('C');
        } else {
            wl_mask += c;
        }
    }
    g_ctrl_text_buf = wl_mask.toUtf8();
    return g_ctrl_text_buf.constData();
}

/* TypeSaisie : le type est memorise en dynamic property sur le widget des sa
 * creation. Le GET runtime le relit tel quel (round-trip fidele) au lieu de le
 * deduire de l'echo-mode (qui ne distingue que MotDePasse) — corrige aussi le
 * repli Texte errone sur un champ multiligne (QPlainTextEdit). */

/* (Re)pose le validateur numerique d'un QLineEdit selon son type de saisie et
 * ses bornes memorisees. Numerique/Monetaire -> QDoubleValidator borne ; tout
 * autre type -> retire un validateur numerique eventuel. Monetaire = 2
 * decimales, numerique = jusqu'a 6 (entier ou reel). Bornes 0/0 = non bornees. */
static void owl_lineedit_apply_numeric_validator(QLineEdit* le) {
    if (!le) return;
    int type = le->property("owl_input_type").toInt();
    bool numeric = (type == OWL_TYPESAISIE_NUM || type == OWL_TYPESAISIE_MONETAIRE ||
                    type == OWL_TYPESAISIE_NUM_PRECIS);
    if (!numeric) {
        if (qobject_cast<const QDoubleValidator*>(le->validator()) ||
            le->property("owl_has_num_fmt_validator").toBool()) {
            le->setValidator(nullptr);
            le->setProperty("owl_has_num_fmt_validator", QVariant());
        }
        return;
    }
    /*  — un champ num a FormatNombre effectif porte un validateur
     * config-aware (remap decimal). Ne pas le clobberer par le QDoubleValidator
     * locale-C ; il prime. */
    if (le->property("owl_has_num_fmt_validator").toBool()) return;
    /* Monetaire = 2 decimales, numerique general = 6. Le type "precis" adosse a
     * owlNumerique (decimal exact) doit accepter la frappe de decimales bien
     * au-dela de la precision d'un double : cap tres large (le range du
     * QDoubleValidator reste borne, mais le nombre de decimales n'est plus le
     * facteur limitant ; la valeur exacte est reparse cote GET via mpdecimal). */
    int decimals = (type == OWL_TYPESAISIE_MONETAIRE) ? 2
                 : (type == OWL_TYPESAISIE_NUM_PRECIS) ? 1000 : 6;
    QDoubleValidator* val =
        qobject_cast<QDoubleValidator*>(const_cast<QValidator*>(le->validator()));
    if (!val) { val = new QDoubleValidator(le); le->setValidator(val); }
    val->setNotation(QDoubleValidator::StandardNotation);
    int lo = le->property("owl_borne_min").toInt();
    int hi = le->property("owl_borne_max").toInt();
    if (lo == 0 && hi == 0)
        val->setRange(-1.0e18, 1.0e18, decimals);
    else
        val->setRange((double)lo, (double)hi, decimals);
}

extern "C" void owl_ui_control_set_input_type(OwlControl* c, int type) {
    if (!c) return;
    QWidget* wdg = reinterpret_cast<QWidget*>(c);
    wdg->setProperty("owl_input_type", type);
    owl_lineedit_apply_numeric_validator(qobject_cast<QLineEdit*>(wdg));
}

/*  — validateur de PLAGES token-aware pour un champ Date/Heure. En plus
 * du masque de frappe (, qui n'accepte que des chiffres aux bons
 * emplacements), ce validateur borne la VALEUR de chaque jeton A LA FRAPPE :
 * jour 01-31, mois 01-12, heure 00-23, minute/seconde 00-59. Bornes grossieres
 * (pas de controle calendaire croise : 30/02 reste typable, assume).
 *
 * Piege Qt (mesure, pas deduction) : sur un QLineEdit avec setInputMask, Qt
 * appelle validate() sur le texte MASQUE COMPLET (separateurs + blancs pour les
 * slots non remplis, ex "15/0 /    "), puis reverte la frappe si le retour est
 * Invalid. Un simple QRegularExpressionValidator sur le format brut echoue donc
 * (il ne matche pas les blancs/separateurs pendant la frappe). D'ou ce
 * validateur qui parse la position, extrait les chiffres par jeton et borne
 * chacun independamment, en tolerant les etats Intermediate (prefixe partiel)
 * sans quoi la saisie serait impossible. */
struct OwlDtSeg {
    bool token;          /* true = jeton borne ; false = separateur litteral */
    int  width;          /* nb de positions occupees dans le texte masque */
    QRegularExpression re; /* jeton : motif ancre du jeton complet */
};

class OwlDateTimeValidator : public QValidator {
public:
    OwlDateTimeValidator(const QVector<OwlDtSeg>& segs, QObject* parent)
        : QValidator(parent), m_segs(segs) {}

    State validate(QString& input, int& pos) const override {
        Q_UNUSED(pos);
        int idx = 0;
        int len = input.length();
        State agg = Acceptable;
        for (const OwlDtSeg& s : m_segs) {
            if (!s.token) { idx += s.width; continue; } /* separateur : saute */
            /* Extrait les chiffres du jeton (les slots non remplis sont des
             * blancs — espace par defaut du masque Qt ; on les ignore). */
            QString digits;
            for (int k = 0; k < s.width && idx + k < len; k++) {
                QChar ch = input.at(idx + k);
                if (ch.isDigit()) digits += ch;
            }
            idx += s.width;
            if (digits.isEmpty()) { agg = Intermediate; continue; }
            QRegularExpressionMatch m =
                s.re.match(digits, 0, QRegularExpression::PartialPreferCompleteMatch);
            if (m.hasMatch()) {
                /* jeton complet et dans les bornes : rien a degrader */
            } else if (m.hasPartialMatch()) {
                agg = Intermediate;
            } else {
                return Invalid; /* chiffre hors-borne : frappe revertee par Qt */
            }
        }
        return agg;
    }

private:
    QVector<OwlDtSeg> m_segs;
};

/* Parse un format a jetons date/heure en couches de segments bornes. Renvoie
 * true si un validateur de plages a un sens (au moins un jeton numerique borne
 * reconnu, aucun jeton NOMME). Renvoie false pour un format a jetons nommes
 * (mois/jour en lettres -> saisie libre, cf ) ou vide. La
 * correspondance des jetons est alignee sur input_frappe_mask_from_format
 * (stdlib_ui.c) pour que les positions coincident avec le masque. */
static bool owl_dt_parse_range_layout(const char* fmt, QVector<OwlDtSeg>& out) {
    out.clear();
    if (!fmt) return false;
    int flen = (int)strlen(fmt);
    int i = 0;
    bool anyToken = false;
    auto pushTok = [&](int w, const char* pat) {
        OwlDtSeg s;
        s.token = true;
        s.width = w;
        s.re = QRegularExpression(QRegularExpression::anchoredPattern(
                   QString::fromLatin1("(?:%1)").arg(QString::fromLatin1(pat))));
        out.push_back(s);
        anyToken = true;
    };
    while (i < flen) {
        int rem = flen - i;
        /* Jetons NOMMES (texte) -> pas de validateur, saisie libre. */
        if ((rem >= 4 && strncasecmp(fmt + i, "MMMM", 4) == 0) ||
            (rem >= 4 && strncasecmp(fmt + i, "JJJJ", 4) == 0) ||
            (rem >= 3 && strncasecmp(fmt + i, "MMM", 3) == 0) ||
            (rem >= 3 && strncasecmp(fmt + i, "JJJ", 3) == 0)) {
            out.clear();
            return false;
        }
        if (rem >= 4 && (strncmp(fmt + i, "AAAA", 4) == 0 ||
                         strncmp(fmt + i, "YYYY", 4) == 0)) {
            pushTok(4, "\\d{4}"); i += 4; continue;
        }
        if (rem >= 2 && (strncmp(fmt + i, "AA", 2) == 0 ||
                         strncmp(fmt + i, "CC", 2) == 0)) {
            pushTok(2, "\\d{2}"); i += 2; continue;
        }
        if (rem >= 2 && strncmp(fmt + i, "MM", 2) == 0) {
            pushTok(2, "0[1-9]|1[0-2]"); i += 2; continue;
        }
        if (rem >= 2 && (strncmp(fmt + i, "JJ", 2) == 0 ||
                         strncmp(fmt + i, "DD", 2) == 0)) {
            pushTok(2, "0[1-9]|[12][0-9]|3[01]"); i += 2; continue;
        }
        if (rem >= 2 && strncmp(fmt + i, "HH", 2) == 0) {
            pushTok(2, "[01][0-9]|2[0-3]"); i += 2; continue;
        }
        if (rem >= 2 && (strncmp(fmt + i, "mm", 2) == 0 ||
                         strncmp(fmt + i, "MI", 2) == 0 ||
                         strncmp(fmt + i, "SS", 2) == 0)) {
            pushTok(2, "[0-5][0-9]"); i += 2; continue;
        }
        /* separateur litteral : une position dans le texte masque */
        if (!out.isEmpty() && !out.last().token) {
            out.last().width += 1;
        } else {
            OwlDtSeg s; s.token = false; s.width = 1; out.push_back(s);
        }
        i += 1;
    }
    return anyToken;
}

/*  — (re)pose (ou retire) le validateur de plages date/heure d'un champ
 * selon son format effectif. Applique EN PLUS du masque de frappe,
 * au meme point d'injection. Format a jetons nommes ou sans jeton borne ->
 * retire le validateur (saisie libre). No-op hors QLineEdit. */
extern "C" void owl_ui_lineedit_set_date_time_validator(OwlControl* edit,
                                                        const char* fmt) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    QVector<OwlDtSeg> segs;
    if (!owl_dt_parse_range_layout(fmt, segs)) {
        /* Jetons nommes / format vide : on retire NOTRE validateur s'il est la
         * (marque par dynamic-property), sans toucher un eventuel validateur
         * d'un autre type. OwlDateTimeValidator n'a pas de Q_OBJECT/moc, donc
         * pas de qobject_cast possible -> on trace la pose par une property. */
        if (le->property("owl_has_dt_validator").toBool()) {
            le->setValidator(nullptr);
            le->setProperty("owl_has_dt_validator", QVariant());
        }
        return;
    }
    le->setValidator(new OwlDateTimeValidator(segs, le));
    le->setProperty("owl_has_dt_validator", true);
}

/*  — layout de segments pour un champ DURÉE (typSaisieDurée). Miroir de
 * owl_dt_parse_range_layout mais avec les BORNES DURÉE : HH n'est PAS plafonné à
 * 23 (\d{2} = 0-99), MM/SS restent 00-59, JJ/CC = 0-99, CCC = 0-999. MM y est
 * une MINUTE (pas un mois). Un jeton jour VARIABLE (J seul) ou une notation +N
 * n'a pas de largeur fixe -> pas de guidage possible -> renvoie false (saisie
 * libre, comme les jetons nommés côté date). */
static bool owl_duree_parse_range_layout(const char* fmt, QVector<OwlDtSeg>& out) {
    out.clear();
    if (!fmt) return false;
    int flen = (int)strlen(fmt);
    int i = 0;
    bool anyToken = false;
    auto pushTok = [&](int w, const char* pat) {
        OwlDtSeg s;
        s.token = true;
        s.width = w;
        s.re = QRegularExpression(QRegularExpression::anchoredPattern(
                   QString::fromLatin1("(?:%1)").arg(QString::fromLatin1(pat))));
        out.push_back(s);
        anyToken = true;
    };
    while (i < flen) {
        int rem = flen - i;
        /* +N : unité significative variable -> saisie libre. */
        if (fmt[i] == '+') { out.clear(); return false; }
        if (rem >= 3 && strncmp(fmt + i, "CCC", 3) == 0) {
            pushTok(3, "\\d{3}"); i += 3; continue;
        }
        if (rem >= 2 && strncmp(fmt + i, "CC", 2) == 0) {
            pushTok(2, "\\d{2}"); i += 2; continue;
        }
        if (rem >= 2 && strncmp(fmt + i, "JJ", 2) == 0) {
            pushTok(2, "\\d{2}"); i += 2; continue;
        }
        if (rem >= 2 && strncmp(fmt + i, "HH", 2) == 0) {
            pushTok(2, "\\d{2}"); i += 2; continue; /* durée : 0-99, PAS 0-23 */
        }
        if (rem >= 2 && (strncmp(fmt + i, "MI", 2) == 0 ||
                         strncmp(fmt + i, "mm", 2) == 0)) {
            pushTok(2, "[0-5][0-9]"); i += 2; continue; /* minutes (MI/mm ; MM=mois ailleurs) */
        }
        if (rem >= 2 && strncmp(fmt + i, "SS", 2) == 0) {
            pushTok(2, "[0-5][0-9]"); i += 2; continue;
        }
        /* J seul : jours largeur variable -> saisie libre. */
        if (fmt[i] == 'J') { out.clear(); return false; }
        /* séparateur littéral : une position dans le texte masqué */
        if (!out.isEmpty() && !out.last().token) {
            out.last().width += 1;
        } else {
            OwlDtSeg s; s.token = false; s.width = 1; out.push_back(s);
        }
        i += 1;
    }
    return anyToken;
}

/*  — (re)pose (ou retire) le validateur de plages DURÉE. Format à jetons
 * fixes -> validateur borné (HH 0-99, MI/SS 0-59) ; format variable (J seul, +N)
 * -> retire le validateur (saisie libre). Réutilise OwlDateTimeValidator (le
 * moteur de segments est identique, seules les bornes changent). */
extern "C" void owl_ui_lineedit_set_duree_validator(OwlControl* edit,
                                                    const char* fmt) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    QVector<OwlDtSeg> segs;
    if (!owl_duree_parse_range_layout(fmt, segs)) {
        if (le->property("owl_has_dt_validator").toBool()) {
            le->setValidator(nullptr);
            le->setProperty("owl_has_dt_validator", QVariant());
        }
        return;
    }
    le->setValidator(new OwlDateTimeValidator(segs, le));
    le->setProperty("owl_has_dt_validator", true);
}

/*  — validateur LENIENT d'un champ num a FormatNombre effectif (modele
 * strict config). Il ne fait QUE filtrer la CLASSE de caracteres autorisee
 * (chiffres, les deux separateurs '.'/',', un '-' en tete) sans jamais MODIFIER
 * le texte : c'est indispensable car QLineEdit::setText() passe AUSSI par
 * validate() (mesure ) — un validateur qui reecrit le texte mangerait la
 * forme formatee reaffichee au blur ("1.234,5600" -> mangle). Le REMAP de la
 * touche decimale (. ou , -> separateur configure) et la regle "un seul decimal"
 * sont faits en amont, a la frappe, par l'interception KeyPress d'OwlEventDispatcher
 * (qui, elle, ne s'applique pas au setText programmatique). Ce validateur borne
 * juste ce qui peut entrer (lettres/paste rejetes). */
class OwlNumFormatValidator : public QValidator {
public:
    explicit OwlNumFormatValidator(QObject* parent) : QValidator(parent) {}

    State validate(QString& input, int& /*pos*/) const override {
        for (int i = 0; i < input.length(); i++) {
            QChar ch = input.at(i);
            if (ch.isDigit()) continue;
            if (ch == QChar('.') || ch == QChar(',')) continue;
            if (ch == QChar('-') && i == 0) continue;
            return Invalid;
        }
        return Acceptable;
    }
};

extern "C" void owl_ui_lineedit_set_num_format_validator(OwlControl* edit,
                                                         const char* decSep) {
    if (!edit) return;
    QLineEdit* le = qobject_cast<QLineEdit*>(reinterpret_cast<QWidget*>(edit));
    if (!le) return;
    QString dec = (decSep != nullptr && *decSep) ? QString::fromUtf8(decSep)
                                                 : QStringLiteral(".");
    le->setValidator(new OwlNumFormatValidator(le));
    le->setProperty("owl_has_num_fmt_validator", true);
    /* separateur decimal configure : lu par l'interception KeyPress pour remapper
     * toute touche decimale (. ou ,) vers cette valeur. Presence = champ format. */
    le->setProperty("owl_num_fmt_dec", dec);
}

extern "C" int owl_ui_control_get_input_type(OwlControl* c) {
    if (!c) return OWL_TYPESAISIE_TEXTE;
    QWidget* wdg = reinterpret_cast<QWidget*>(c);
    QVariant v = wdg->property("owl_input_type");
    if (v.isValid()) return v.toInt();
    /* Repli pour un widget cree hors dispatch type (robustesse). */
    if (qobject_cast<QPlainTextEdit*>(wdg)) return OWL_TYPESAISIE_MULTILIGNE;
    QLineEdit* le = qobject_cast<QLineEdit*>(wdg);
    if (le && le->echoMode() == QLineEdit::Password) return OWL_TYPESAISIE_MOTDEPASSE;
    return OWL_TYPESAISIE_TEXTE;
}

/* BorneMin/Max : storage R/W via dynamic property + application reelle au
 * validateur d'un champ numerique/monetaire (bornes du QDoubleValidator). */
extern "C" int owl_ui_lineedit_get_min_value(OwlControl* edit) {
    if (!edit) return 0;
    QVariant v = reinterpret_cast<QWidget*>(edit)->property("owl_borne_min");
    return v.isValid() ? v.toInt() : 0;
}

extern "C" void owl_ui_lineedit_set_min_value(OwlControl* edit, int v) {
    if (!edit) return;
    QWidget* wdg = reinterpret_cast<QWidget*>(edit);
    wdg->setProperty("owl_borne_min", v);
    owl_lineedit_apply_numeric_validator(qobject_cast<QLineEdit*>(wdg));
}

extern "C" int owl_ui_lineedit_get_max_value(OwlControl* edit) {
    if (!edit) return 0;
    QVariant v = reinterpret_cast<QWidget*>(edit)->property("owl_borne_max");
    return v.isValid() ? v.toInt() : 0;
}

extern "C" void owl_ui_lineedit_set_max_value(OwlControl* edit, int v) {
    if (!edit) return;
    QWidget* wdg = reinterpret_cast<QWidget*>(edit);
    wdg->setProperty("owl_borne_max", v);
    owl_lineedit_apply_numeric_validator(qobject_cast<QLineEdit*>(wdg));
}

/* Phase E.X : lectures runtime des proprietes. */

extern "C" const char* owl_ui_control_get_text(OwlControl* w) {
    if (!w) return "";
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    /* Try via Qt meta-property "text" (works for QLabel/QPushButton/QLineEdit). */
    QVariant v = widget->property("text");
    if (!v.isValid()) return "";
    g_ctrl_text_buf = v.toString().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" int owl_ui_control_get_visible(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->isVisible() ? 1 : 0;
}

extern "C" int owl_ui_control_get_enabled(OwlControl* w) {
    if (!w) return 0;
    return reinterpret_cast<QWidget*>(w)->isEnabled() ? 1 : 0;
}

/*  — widget porteur de la GEOMETRIE du champ. Pour un champ composite
 * (libelle integre : [QLabel|QLineEdit], [QLabel|QComboBox]), la boite du champ
 * est celle du wrapper, pas celle du widget logique : c'est le wrapper qui entre
 * dans le layout parent (owl_ui_layout_add_child) et qui recoit la Largeur / le
 * X declares, le widget logique n'occupant que la part restee libre a droite du
 * libelle. Lire le widget logique retranchait donc la largeur du libelle a la
 * Largeur et l'ajoutait au X, dans les quatre contextes de placement (mesure :
 * 20 / 280 declares rendus 106 / 194). L'ecriture et la lecture doivent designer
 * la meme boite. */
static QWidget* owl_control_geometry_widget(OwlControl* w) {
    QWidget* wid = reinterpret_cast<QWidget*>(w);
    QWidget* rep = owl_widget_layout_rep(wid);
    return rep ? rep : wid;
}

extern "C" int owl_ui_control_get_width(OwlControl* w) {
    if (!w) return 0;
    return owl_control_geometry_widget(w)->width();
}

extern "C" int owl_ui_control_get_height(OwlControl* w) {
    if (!w) return 0;
    return owl_control_geometry_widget(w)->height();
}

extern "C" void owl_ui_control_set_icon_size_override(OwlControl* w, int size_px) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setProperty(
        "owl_icon_size", size_px > 0 ? QVariant(size_px) : QVariant());
}

extern "C" int owl_ui_control_get_icon_size_override(OwlControl* w) {
    if (!w) return 0;
    QVariant v = reinterpret_cast<QWidget*>(w)->property("owl_icon_size");
    return v.isValid() ? v.toInt() : 0;
}

/*  — X/Y sont exprimes dans le repere de la ZONE CLIENT de la fenetre
 * (canon), pas dans celui du parent immediat. Un champ pose dans un conteneur
 * decale, ou dans un volet, rend donc sa position absolue dans la fenetre et
 * non (0,0). Le systeme de coordonnees d'un widget top-level Qt EST la zone
 * client (le cadre de fenetre en est exclu ; une barre de menus en fait
 * partie, ce qui est le comportement voulu). */
static QPoint owl_control_pos_in_window(QWidget* wid) {
    QWidget* top = wid->window();
    if (!top || top == wid) return wid->pos();
    return wid->mapTo(top, QPoint(0, 0));
}

extern "C" int owl_ui_control_get_x(OwlControl* w) {
    if (!w) return 0;
    return owl_control_pos_in_window(owl_control_geometry_widget(w)).x();
}

extern "C" int owl_ui_control_get_y(OwlControl* w) {
    if (!w) return 0;
    return owl_control_pos_in_window(owl_control_geometry_widget(w)).y();
}

/*  — force la mise a jour de la geometrie avant une lecture.
 * Sans ca, une lecture faite avant que la boucle d'evenements ait tourne
 * (typiquement depuis FinInitialisation) rend une taille provisoire : le
 * layout n'a pas encore place les enfants, chaque widget porte encore sa
 * taille naturelle. Les QEvent::LayoutRequest en attente sont livres, puis
 * les layouts sont actives de la fenetre vers les feuilles (un parent doit
 * connaitre sa taille avant de repartir la place a ses enfants). */
static void owl_layout_activate_recursive(QWidget* wid) {
    if (!wid) return;
    if (QLayout* lay = wid->layout()) lay->activate();
    const QList<QWidget*> kids =
        wid->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* kid : kids) owl_layout_activate_recursive(kid);
}

extern "C" void owl_ui_control_sync_layout(OwlControl* w) {
    if (!w) return;
    QWidget* wid = reinterpret_cast<QWidget*>(w);
    QWidget* top = wid->window();
    if (!top) top = wid;
    top->ensurePolished();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    owl_layout_activate_recursive(top);
}

/*  — pendant de owl_ui_control_sync_layout pour la fenetre elle-meme.
 * Une lecture de taille faite depuis FinInitialisation, avant que la boucle
 * d'evenements ait tourne, verrait sinon une taille provisoire : c'est
 * l'activation du layout qui remonte la contrainte de taille minimale des
 * champs jusqu'a la fenetre. */
extern "C" void owl_ui_window_sync_layout(OwlWindow* w) {
    if (!w) return;
    QWidget* top = reinterpret_cast<QWidget*>(w);
    top->ensurePolished();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    owl_layout_activate_recursive(top);
}

extern "C" const char* owl_ui_window_get_title(OwlWindow* w) {
    if (!w) return "";
    g_ctrl_text_buf = reinterpret_cast<QMainWindow*>(w)->windowTitle().toUtf8();
    return g_ctrl_text_buf.constData();
}

extern "C" int owl_ui_window_get_width(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->width();
}

extern "C" int owl_ui_window_get_height(OwlWindow* w) {
    if (!w) return 0;
    return reinterpret_cast<QMainWindow*>(w)->height();
}

/*  — un champ est « placé par une disposition automatique » quand le
 * QWidget qui porte sa géométrie (le rep de layout, wrapper composite compris)
 * a un parent muni d'un QLayout (pile box ou grille). En disposition libre le
 * parent n'a pas de layout : la position/taille sont absolues. Ce prédicat
 * distingue les deux régimes pour l'écriture de géométrie. */
extern "C" int owl_ui_control_is_layout_managed(OwlControl* w) {
    if (!w) return 0;
    QWidget* rep = owl_control_geometry_widget(w);
    if (!rep) return 0;
    QWidget* parent = rep->parentWidget();
    return (parent && parent->layout() != nullptr) ? 1 : 0;
}

/* Sous disposition automatique, une taille écrite au runtime doit être une
 * contrainte PERSISTANTE : le layout relit min/max à chaque passe (dont un
 * redimensionnement de fenêtre), là où un resize() serait réécrasé au prochain
 * recompute — le piège documenté par . setFixedWidth/Height fige donc
 * min == max sur le rep de layout (le même widget que owl_ui_layout_add_child
 * borne à la déclaration), puis on réactive le layout parent pour un rendu
 * immédiat. En disposition libre, resize() absolu inchangé. */
extern "C" void owl_ui_control_set_width(OwlControl* w, int width) {
    if (!w) return;
    if (owl_ui_control_is_layout_managed(w)) {
        QWidget* rep = owl_control_geometry_widget(w);
        rep->setFixedWidth(width);
        if (QWidget* p = rep->parentWidget())
            if (QLayout* l = p->layout()) l->activate();
        return;
    }
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->resize(width, widget->height());
}

extern "C" void owl_ui_control_set_height(OwlControl* w, int height) {
    if (!w) return;
    if (owl_ui_control_is_layout_managed(w)) {
        QWidget* rep = owl_control_geometry_widget(w);
        rep->setFixedHeight(height);
        if (QWidget* p = rep->parentWidget())
            if (QLayout* l = p->layout()) l->activate();
        return;
    }
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->resize(widget->width(), height);
}

extern "C" void owl_ui_control_set_x(OwlControl* w, int x) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->move(x, widget->y());
}

extern "C" void owl_ui_control_set_y(OwlControl* w, int y) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->move(widget->x(), y);
}

/* =====================================================================
 *  Lot A — props canon Libelle (doc 1013002).
 *  9 setters/getters appliques aux QWidget enfants d'une fenetre OWL.
 * ===================================================================== */

/* AffichageActif : bypass des repaints. setUpdatesEnabled(false) suspend
 * le repaint du widget (et de ses enfants). En Qt6 les paint events sont
 * deja batches via l'event loop, mais ce hook reste utile pour les
 * boucles intensives ou pour eviter le flicker visible lors de gros
 * remplissages. */
extern "C" void owl_ui_control_set_display_active(OwlControl* w, int active) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setUpdatesEnabled(active != 0);
}

/* Altitude : z-order intra-fenetre. raise() met au-dessus de tous les
 * siblings, lower() au-dessous. On stocke la valeur OWL en dynamic
 * property pour read-back. Quand on set, on re-stack tous les siblings
 * de meme parent dans l'ordre des altitudes (stable sort). */
extern "C" void owl_ui_control_set_altitude(OwlControl* w, int z) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->setProperty("owl_altitude", z);
    /* Re-stack siblings : on raise les widgets dans l'ordre croissant
     * d'altitude. Les widgets sans propriete owl_altitude restent en bas
     * (altitude 0 par defaut). */
    QWidget* parent = widget->parentWidget();
    if (!parent) { widget->raise(); return; }
    QList<QWidget*> siblings = parent->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    QList<QWidget*> sorted = siblings;
    std::sort(sorted.begin(), sorted.end(), [](QWidget* a, QWidget* b) {
        int za = a->property("owl_altitude").toInt();
        int zb = b->property("owl_altitude").toInt();
        return za < zb;
    });
    for (QWidget* sib : sorted) sib->raise();
}

/* CurseurSouris : 14 formes canon → Qt::CursorShape. Index stable defini
 * dans ui_catalog.c (E_Curseurs_values). */
static Qt::CursorShape curseur_owl_to_qt(int cur_id) {
    switch (cur_id) {
        case OWL_CUR_FLECHE:        return Qt::ArrowCursor;
        case OWL_CUR_MAIN:          return Qt::PointingHandCursor;
        case OWL_CUR_MAIN_SYSTEME:  return Qt::OpenHandCursor;
        case OWL_CUR_CARET:         return Qt::IBeamCursor;
        case OWL_CUR_CROIX:         return Qt::CrossCursor;
        case OWL_CUR_SABLIER:       return Qt::WaitCursor;
        case OWL_CUR_AIDE:          return Qt::WhatsThisCursor;
        case OWL_CUR_INTERDIT:      return Qt::ForbiddenCursor;
        case OWL_CUR_HFLECHE:       return Qt::UpArrowCursor;
        case OWL_CUR_TAILLE:        return Qt::SizeAllCursor;
        case OWL_CUR_TAILLE_NS:     return Qt::SizeVerCursor;
        case OWL_CUR_TAILLE_OE:     return Qt::SizeHorCursor;
        case OWL_CUR_TAILLE_NESO:   return Qt::SizeBDiagCursor;
        case OWL_CUR_TAILLE_NOSE:   return Qt::SizeFDiagCursor;
        default: return Qt::ArrowCursor;
    }
}
extern "C" void owl_ui_control_set_cursor_shape(OwlControl* w, int cur_id) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    /* Piege sentinelle : les formes valides (OWL_CUR_*) sont des sentinelles
     * NEGATIVES (base INT32_MIN), pas des index 0..13. Un garde `< 0`
     * captait donc TOUTE forme valide dans la branche unsetCursor. Seul
     * -1 (defaut prop_int absent) signifie "pas de curseur". */
    if (cur_id == -1) {
        widget->unsetCursor();
        widget->setProperty("owl_cursor_id", QVariant());
    } else {
        widget->setCursor(QCursor(curseur_owl_to_qt(cur_id)));
        widget->setProperty("owl_cursor_id", cur_id);
    }
}

/* Ellipse : applique sur QLabel via setTextFormat(PlainText) +
 * setWordWrap(false). Pour l'elide reel (3 modes canon), on installe le
 * QFontMetrics::elidedText au paint. V1 minimal : on stocke la valeur
 * et on bascule entre Qt::ElideNone / ElideRight (=fin) / ElideMiddle
 * (=chemin). QLabel n'expose pas directement elideMode : on emule via
 * QFontMetrics et setText dynamique. Pour V1 on stocke seulement —
 * implementation visuelle complete en V2. */
extern "C" void owl_ui_control_set_ellipsis_mode(OwlControl* w, int mode) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    widget->setProperty("owl_ellipsis_mode", mode);
    /* QLabel : ajuste wordwrap comme heuristique V1.
     * OWL_ELL_INACTIVE → wordwrap on (laisse le texte deborder/wrapper).
     * OWL_ELL_FIN / OWL_ELL_CHEMIN → wordwrap off + clipping naturel. */
    QLabel* lbl = qobject_cast<QLabel*>(widget);
    if (lbl) {
        lbl->setWordWrap(mode == OWL_ELL_INACTIVE);
    }
}

/* Bornes redimensionnement. h<=0 sur min = 0 (pas de min) ; sur max = pas
 * de max (QWIDGETSIZE_MAX). */
extern "C" void owl_ui_control_set_min_height(OwlControl* w, int h) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setMinimumHeight(h > 0 ? h : 0);
}
extern "C" void owl_ui_control_set_max_height(OwlControl* w, int h) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setMaximumHeight(h > 0 ? h : QWIDGETSIZE_MAX);
}
extern "C" void owl_ui_control_set_min_width(OwlControl* w, int wpx) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setMinimumWidth(wpx > 0 ? wpx : 0);
}
extern "C" void owl_ui_control_set_max_width(OwlControl* w, int wpx) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setMaximumWidth(wpx > 0 ? wpx : QWIDGETSIZE_MAX);
}

/* Message → QWidget::setStatusTip. Affiche dans la barre de statut de la
 * fenetre principale au survol. Si la fenetre n'a pas de statusbar, le
 * texte n'apparait simplement pas (no-op silencieux cote Qt). */
extern "C" void owl_ui_control_set_status_tip(OwlControl* w, const char* text_utf8) {
    if (!w) return;
    reinterpret_cast<QWidget*>(w)->setStatusTip(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
}

/* sensÉcriture → setLayoutDirection. sensDéfaut (0) = LayoutDirectionAuto,
 * sensGaucheDroite (1) = LeftToRight, sensDroiteGauche (2) = RightToLeft. */
extern "C" void owl_ui_control_set_text_direction(OwlControl* w, int dir) {
    if (!w) return;
    QWidget* widget = reinterpret_cast<QWidget*>(w);
    switch (dir) {
        case OWL_SE_GAUCHE_DROITE: widget->setLayoutDirection(Qt::LeftToRight); break;
        case OWL_SE_DROITE_GAUCHE: widget->setLayoutDirection(Qt::RightToLeft); break;
        default:                   widget->setLayoutDirection(Qt::LayoutDirectionAuto); break;
    }
}

/* ------- Dialogues -------
 *
 * Lazy init de QApplication : la 1ere dialog appelee initialise le moteur
 * Qt si pas deja fait (canon : Info("msg") marche sans Ouvre() prealable).
 *
 * Ctrl+C fiable (fix 1764) : le handler de copie natif de QMessageBox
 * (keyPressEvent) ne s'exécute que si l'événement clavier remonte jusqu'au
 * dialogue. Dès que le label du message est sélectionnable (SH_MessageBox_
 * TextInteractionFlags donne TextSelectableByMouse sous Windows) et qu'il a
 * pris le focus — typiquement quand l'utilisateur a cliqué dans un message
 * multi-lignes pour tenter une sélection — le QLabel consomme le Ctrl+C et
 * copie sa sélection (vide → presse-papier vide). Le dialogue ne voit jamais
 * l'événement. On corrige en interceptant Copy à deux niveaux : keyPressEvent
 * du dialogue (chemin bouton/dialogue) ET eventFilter sur les labels enfants
 * (chemin label focus), les deux composant titre + corps. */

namespace {
class OwlMessageBox : public QMessageBox {
public:
    explicit OwlMessageBox(QWidget* parent = nullptr) : QMessageBox(parent) {}

    /* Installe le filtre de copie sur les labels enfants (message +
     * informatif). A appeler apres setText, une fois les labels crees. */
    void hookCopyOnLabels() {
        const QList<QLabel*> labels = findChildren<QLabel*>();
        for (QLabel* l : labels) l->installEventFilter(this);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->matches(QKeySequence::Copy)) {
            copyContentToClipboard();
            e->accept();
            return;
        }
        QMessageBox::keyPressEvent(e);
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::KeyPress) {
            QKeyEvent* ke = static_cast<QKeyEvent*>(ev);
            if (ke->matches(QKeySequence::Copy)) {
                copyContentToClipboard();
                return true; /* consomme : evite la copie de selection vide */
            }
        }
        return QMessageBox::eventFilter(obj, ev);
    }

private:
    void copyContentToClipboard() {
        const QString sep = QStringLiteral("---------------------------\n");
        QString out = sep + windowTitle() + u'\n' + sep + text() + u'\n' + sep;
        const QString info = informativeText();
        if (!info.isEmpty()) out += info + u'\n' + sep;
        QGuiApplication::clipboard()->setText(out);
    }
};

static void owl_show_simple_message(QMessageBox::Icon icon,
                                    const char* title_utf8,
                                    const char* default_title,
                                    const char* message_utf8) {
    owl_ui_init();
    OwlMessageBox box(nullptr);
    box.setIcon(icon);
    box.setWindowTitle(QString::fromUtf8(title_utf8 ? title_utf8 : default_title));
    box.setText(QString::fromUtf8(message_utf8 ? message_utf8 : ""));
    box.setStandardButtons(QMessageBox::Ok);
    box.setDefaultButton(QMessageBox::Ok);
    box.hookCopyOnLabels();
    box.exec();
}
} /* namespace */

extern "C" void owl_ui_message_info(const char* title_utf8, const char* message_utf8) {
    owl_show_simple_message(QMessageBox::Information, title_utf8, "Information", message_utf8);
}

extern "C" void owl_ui_message_error(const char* title_utf8, const char* message_utf8) {
    owl_show_simple_message(QMessageBox::Critical, title_utf8, "Erreur", message_utf8);
}

extern "C" void owl_ui_message_warning(const char* title_utf8, const char* message_utf8) {
    owl_show_simple_message(QMessageBox::Warning, title_utf8, "Avertissement", message_utf8);
}

extern "C" int owl_ui_message_yesno(const char* title_utf8,
                                      const char* message_utf8,
                                      int default_button) {
    owl_ui_init();
    OwlMessageBox box(nullptr);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QString::fromUtf8(title_utf8 ? title_utf8 : "Question"));
    box.setText(QString::fromUtf8(message_utf8 ? message_utf8 : ""));
    QPushButton* yes = box.addButton(QMessageBox::Yes);
    QPushButton* no  = box.addButton(QMessageBox::No);
    box.setDefaultButton(default_button == 0 ? no : yes);
    box.hookCopyOnLabels();
    box.exec();
    QAbstractButton* clicked = box.clickedButton();
    /* X-close ou rien → Non (0) */
    if (clicked == yes) return 1;
    return 0;
}

extern "C" int owl_ui_message_confirm(const char* title_utf8,
                                        const char* message_utf8,
                                        int default_button) {
    owl_ui_init();
    OwlMessageBox box(nullptr);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QString::fromUtf8(title_utf8 ? title_utf8 : "Confirmer"));
    box.setText(QString::fromUtf8(message_utf8 ? message_utf8 : ""));
    QPushButton* yes    = box.addButton(QMessageBox::Yes);
    QPushButton* no     = box.addButton(QMessageBox::No);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    QPushButton* def    = (default_button == 0 ? no
                         : default_button == 2 ? cancel
                         : yes);
    box.setDefaultButton(def);
    box.hookCopyOnLabels();
    box.exec();
    QAbstractButton* clicked = box.clickedButton();
    if (clicked == yes)    return 1;
    if (clicked == cancel) return 2;
    return 0;
}

/* ------- Selecteurs fichier / repertoire (QFileDialog) -------
 *
 * Le mode headless est filtre en amont (stdlib_ui.c) : on n'arrive ici qu'en
 * GUI reelle. La chaine retournee est dupliquee via strdup (malloc) ; le
 * caller la libere. */

static char* owl_strdup_qstring(const QString& s) {
    QByteArray utf8 = s.toUtf8();
    char* out = (char*)malloc((size_t)utf8.size() + 1);
    if (!out) return nullptr;
    memcpy(out, utf8.constData(), (size_t)utf8.size());
    out[utf8.size()] = '\0';
    return out;
}

extern "C" char* owl_ui_file_dialog(const char* rep_initial_utf8,
                                    const char* fichier_defaut_utf8,
                                    const char* titre_utf8,
                                    const char* filtre_qt_utf8,
                                    int mode) {
    owl_ui_init();
    QString dir   = QString::fromUtf8(rep_initial_utf8 ? rep_initial_utf8 : "");
    QString fic   = QString::fromUtf8(fichier_defaut_utf8 ? fichier_defaut_utf8 : "");
    QString titre = QString::fromUtf8(titre_utf8 ? titre_utf8 : "");
    QString filtre = QString::fromUtf8(filtre_qt_utf8 ? filtre_qt_utf8 : "");

    /* Repertoire de depart : si un fichier defaut est fourni, on le concatene
     * au repertoire pour pre-selectionner le nom. */
    QString start = dir;
    if (!fic.isEmpty()) {
        if (!start.isEmpty() && !start.endsWith('/') && !start.endsWith('\\'))
            start += "/";
        start += fic;
    }

    if (mode & OWL_FSEL_MULTI) {
        QStringList files = QFileDialog::getOpenFileNames(nullptr, titre, start, filtre);
        if (files.isEmpty()) return nullptr;
        /* Chemins joints par RC canon (CRLF). */
        return owl_strdup_qstring(files.join("\r\n"));
    }

    QString chosen;
    if (mode & OWL_FSEL_CREE) {
        chosen = QFileDialog::getSaveFileName(nullptr, titre, start, filtre);
    } else {
        chosen = QFileDialog::getOpenFileName(nullptr, titre, start, filtre);
    }
    if (chosen.isEmpty()) return nullptr;
    return owl_strdup_qstring(chosen);
}

extern "C" char* owl_ui_dir_dialog(const char* rep_defaut_utf8,
                                   const char* titre_utf8,
                                   const char* racine_utf8) {
    owl_ui_init();
    QString titre = QString::fromUtf8(titre_utf8 ? titre_utf8 : "");
    /* Racine d'arborescence si fournie, sinon le repertoire par defaut. */
    QString start = QString::fromUtf8(racine_utf8 && *racine_utf8
                                          ? racine_utf8
                                          : (rep_defaut_utf8 ? rep_defaut_utf8 : ""));
    QString chosen = QFileDialog::getExistingDirectory(
        nullptr, titre, start, QFileDialog::ShowDirsOnly);
    if (chosen.isEmpty()) return nullptr;
    return owl_strdup_qstring(chosen);
}

/* ============================================================
 * Toast — notification ephemere non bloquante
 *
 * QWidget frameless top-most translucide, QLabel central, fade-out via
 * QPropertyAnimation, QTimer pour auto-dismiss. Empilement spatial :
 * chaque appel affiche immediatement, plusieurs toasts coexistent en
 * s'empilant verticalement dans la meme zone de cadrage (cf. canon
 * Superposable). Cadrage relatif a la fenetre Qt active si presente,
 * sinon ecran principal.
 *
 * `rgb == -1` = couleur par defaut du theme toast (semi-transparent noir,
 * texte blanc). Sinon `rgb` est interprete 0xRRGGBB (canon/QRgb).
 * Le QLabel est en setTextFormat(Qt::PlainText) — aucune interpretation
 * HTML, le message s'affiche litteral meme si l'utilisateur passe du
 * markup.
 * ============================================================ */

#include <QPropertyAnimation>
#include <QPointer>
#include <QList>
#include <climits>

namespace {

struct ToastRequest {
    QString msg;
    int     duration_ms;
    int     cv;
    int     ch;
    int     rgb;
};

/* Toast actif tracke pour l'empilement spatial. cv = ancrage vertical
 * d'origine (utilise pour empiler dans la meme zone), height = hauteur
 * effective une fois rendu, y = position en pixels ecran/fenetre. */
struct ActiveToast {
    QPointer<QWidget> widget;
    int cv;
    int height;
    int y;
};

class ToastManager : public QObject {
public:
    static ToastManager& instance() {
        static ToastManager mgr;
        return mgr;
    }

    /* Empilement spatial : pas d'attente, on affiche immediatement et on
     * positionne au-dessus / en-dessous des toasts actifs partageant le meme cv. */
    void enqueue(const ToastRequest& req) {
        showOne(req);
    }

    /* Compte les toasts encore actifs (widget vivant). Appelle pruneInactive
     * pour ne pas surestimer (les widgets fermes par close() laissent une
     * entree QPointer nulle dans m_active jusqu'au prochain prune). */
    int active_count() {
        pruneInactive();
        return m_active.size();
    }

private:
    ToastManager() {}

    /* Gap vertical entre 2 toasts empiles. */
    static constexpr int STACK_GAP = 8;
    /* Largeur min pour eviter les toasts riquiqui sur texte court. */
    static constexpr int MIN_WIDTH = 300;

    void showOne(const ToastRequest& req) {
        QWidget* toast = new QWidget(nullptr,
            Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
          | Qt::WindowDoesNotAcceptFocus);
        toast->setAttribute(Qt::WA_TranslucentBackground);
        toast->setAttribute(Qt::WA_ShowWithoutActivating);
        toast->setAttribute(Qt::WA_DeleteOnClose);

        QString bgCss;
        QString fgCss = "color: white;";
        if (req.rgb < 0) {
            bgCss = "background-color: rgba(40, 40, 40, 220);";
        } else {
            int r = (req.rgb >> 16) & 0xFF;
            int g = (req.rgb >>  8) & 0xFF;
            int b =  req.rgb        & 0xFF;
            bgCss = QString("background-color: rgba(%1, %2, %3, 230);").arg(r).arg(g).arg(b);
        }
        QLabel* label = new QLabel(req.msg, toast);
        label->setStyleSheet(
            QString("QLabel { %1 %2 padding: 12px 20px; border-radius: 8px; font-size: 13px; }")
                .arg(bgCss).arg(fgCss));
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        /* Securite : message affiche litteral, aucune interpretation HTML
         * (Qt::AutoText par defaut tenterait du rich text si <tag> detecte). */
        label->setTextFormat(Qt::PlainText);

        QVBoxLayout* lay = new QVBoxLayout(toast);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(label);

        /* Cadrage relatif : fenetre active si presente, sinon ecran. */
        QRect anchor;
        QWidget* anchorWidget = QApplication::activeWindow();
        if (anchorWidget) {
            anchor = anchorWidget->frameGeometry();
        } else if (QScreen* scr = QGuiApplication::primaryScreen()) {
            anchor = scr->availableGeometry();
        } else {
            anchor = QRect(0, 0, 800, 600);
        }
        int margin = 32;
        /* Largeur min absolue + plafond a 80% de la zone d'ancrage. Le min
         * gagne tant qu'il reste sous le plafond. */
        int maxW = qMax(MIN_WIDTH, (anchor.width() * 8) / 10);
        int minW = qMin(MIN_WIDTH, maxW);
        toast->setMinimumWidth(minW);
        toast->setMaximumWidth(maxW);
        toast->adjustSize();

        int w = toast->width();
        int h = toast->height();
        int x;
        switch (req.ch) {
            case OWL_CH_GAUCHE: x = anchor.left() + margin; break;
            case OWL_CH_DROITE: x = anchor.right() - w - margin; break;
            case OWL_CH_CENTRE:
            default:            x = anchor.left() + (anchor.width() - w) / 2; break;
        }
        /* Position de base selon le cadrage vertical demande. */
        int y;
        switch (req.cv) {
            case OWL_CV_HAUT:   y = anchor.top() + margin; break;
            case OWL_CV_CENTRE: y = anchor.top() + (anchor.height() - h) / 2; break;
            case OWL_CV_BAS:
            default:            y = anchor.bottom() - h - margin; break;
        }

        /* Empilement spatial : on parcourt les toasts actifs partageant le
         * meme cadrage vertical et on decale le nouveau pour qu'il
         * apparaisse juste au-dessus (cvBas / cvCentre) ou juste en
         * dessous (cvHaut) des deja-affiches, sans bouger les anciens.
         * Pas de compactage : si un toast intermediaire disparait, le trou
         * reste. */
        pruneInactive();
        const int stackCv = (req.cv == OWL_CV_HAUT) ? OWL_CV_HAUT : OWL_CV_BAS;
        if (stackCv == OWL_CV_HAUT) {
            /* Empilement vers le bas : trouver l'edge bas le plus eleve
             * parmi les toasts cvHaut, se positionner juste sous. */
            int lowestBottom = INT_MIN;
            for (const ActiveToast& at : m_active) {
                if (at.cv != OWL_CV_HAUT || !at.widget) continue;
                int bottom = at.y + at.height;
                if (bottom > lowestBottom) lowestBottom = bottom;
            }
            if (lowestBottom != INT_MIN) y = lowestBottom + STACK_GAP;
        } else {
            /* Empilement vers le haut : trouver le top le plus bas parmi
             * les toasts ancres en bas / centre, se positionner juste
             * au-dessus. */
            int highestTop = INT_MAX;
            for (const ActiveToast& at : m_active) {
                if (at.cv == OWL_CV_HAUT || !at.widget) continue;
                if (at.y < highestTop) highestTop = at.y;
            }
            if (highestTop != INT_MAX) y = highestTop - h - STACK_GAP;
        }
        toast->move(x, y);

        QGraphicsOpacityEffect* eff = new QGraphicsOpacityEffect(toast);
        eff->setOpacity(0.0);
        toast->setGraphicsEffect(eff);
        toast->show();

        ActiveToast tracker;
        tracker.widget = toast;
        tracker.cv     = stackCv;
        tracker.height = h;
        tracker.y      = y;
        m_active.append(tracker);

        /* Fade-in 200ms */
        QPropertyAnimation* fadeIn = new QPropertyAnimation(eff, "opacity", toast);
        fadeIn->setDuration(200);
        fadeIn->setStartValue(0.0);
        fadeIn->setEndValue(1.0);
        fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

        /* Auto-dismiss : visible pendant duration_ms, puis fade-out 300ms,
         * puis retrait de la liste des actifs (pas de compactage). */
        QPointer<QWidget> guard(toast);
        QTimer::singleShot(req.duration_ms, this, [this, guard, eff]() {
            QWidget* w = guard.data();
            if (!w) {
                pruneInactive();
                return;
            }
            QPropertyAnimation* fadeOut = new QPropertyAnimation(eff, "opacity", w);
            fadeOut->setDuration(300);
            fadeOut->setStartValue(1.0);
            fadeOut->setEndValue(0.0);
            QObject::connect(fadeOut, &QPropertyAnimation::finished, this, [this, guard]() {
                QWidget* w2 = guard.data();
                if (w2) w2->close();
                pruneInactive();
            });
            fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
        });
    }

    /* Retire les entrees dont le widget a ete detruit. Pas de compactage
     * des positions des survivants. */
    void pruneInactive() {
        for (int i = m_active.size() - 1; i >= 0; --i) {
            if (!m_active[i].widget) m_active.removeAt(i);
        }
    }

    QList<ActiveToast> m_active;
};

} /* anon namespace */

extern "C" void owl_ui_toast_show(const char* msg, int duration_ms,
                                  int cv, int ch, int rgb) {
    owl_ui_init();
    ToastRequest req;
    req.msg         = QString::fromUtf8(msg ? msg : "");
    req.duration_ms = (duration_ms > 0 ? duration_ms : OWL_TOAST_COURT_MS);
    req.cv          = cv;
    req.ch          = ch;
    req.rgb         = rgb;
    ToastManager::instance().enqueue(req);
}

/* Pompe l'event loop Qt jusqu'a ce que tous les toasts actifs soient
 * termines (fade-out compris), ou jusqu'a expiration de max_ms. Utilise
 * par main.c a la sortie d'un script pour eviter que le programme se
 * termine avant que Qt ait pu painter le toast (toasts non bloquants
 * par design — dans une Fenetre c'est l'event loop active qui les
 * dispatch, mais un script court n'en a pas).
 *
 * Si Qt n'a jamais ete initialise (pas de g_app), no-op : un script
 * sans appel UI ne paie pas le wait.
 *
 * max_ms <= 0 : applique un cap par defaut de 10 secondes (securite).
 * Sinon : respecte la valeur passee mais plafonne quand meme a 10s. */
extern "C" void owl_ui_toast_wait_all(int max_ms) {
    if (g_app == nullptr) return;
    if (QCoreApplication::eventDispatcher() == nullptr) return;

    constexpr int HARD_CAP_MS = 10000;
    constexpr int SLICE_MS    = 50;
    int budget = (max_ms <= 0 || max_ms > HARD_CAP_MS) ? HARD_CAP_MS : max_ms;

    while (budget > 0 && ToastManager::instance().active_count() > 0) {
        QEventLoop loop;
        QTimer::singleShot(SLICE_MS, &loop, &QEventLoop::quit);
        loop.exec();
        budget -= SLICE_MS;
    }
}

/* ============================================================
 * Champ Liste — V1
 *
 * Backend : OwlListWidget (QListWidget custom) qui override
 * focusInEvent/focusOutEvent pour dispatcher surEntree/surSortie.
 * itemSelectionChanged est wire'd via signal dans owl_ui_list_on_selection.
 * ============================================================ */

class OwlListWidget : public QListWidget {
public:
    explicit OwlListWidget(QWidget* parent = nullptr) : QListWidget(parent) {}

    OwlCallback cbEntry = nullptr;     void* dEntry = nullptr;
    OwlCallback cbExit  = nullptr;     void* dExit  = nullptr;

protected:
    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }

    void focusInEvent(QFocusEvent* e) override {
        QListWidget::focusInEvent(e);
        if (cbEntry) cbEntry(asCtrl(), dEntry);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QListWidget::focusOutEvent(e);
        if (cbExit) cbExit(asCtrl(), dExit);
    }
};

static OwlListWidget* as_list(OwlControl* w) {
    return dynamic_cast<OwlListWidget*>(reinterpret_cast<QWidget*>(w));
}

extern "C" OwlControl* owl_ui_list_create(OwlWindow* parent,
                                           int x, int y, int w, int h) {
    if (!parent) return nullptr;
    OwlListWidget* lst = new OwlListWidget(central_of(parent));
    lst->setSelectionMode(QAbstractItemView::SingleSelection);
    if (x >= 0 && y >= 0) lst->setGeometry(x, y, w, h);
    else                  lst->resize(w > 0 ? w : 200, h > 0 ? h : 150);
    lst->show();
    return reinterpret_cast<OwlControl*>(lst);
}

/* Helper : applique la valeur memorisee + l'icone optionnelle. */
static void apply_list_item_data(QListWidgetItem* it,
                                  const char* stored_utf8,
                                  const char* image_path_utf8) {
    if (stored_utf8 && *stored_utf8) {
        it->setData(Qt::UserRole, QString::fromUtf8(stored_utf8));
    }
    if (image_path_utf8 && *image_path_utf8) {
        it->setIcon(QIcon(QString::fromUtf8(image_path_utf8)));
    }
}

extern "C" void owl_ui_list_add_item(OwlControl* lst, const char* text_utf8,
                                      const char* stored_utf8,
                                      const char* image_path_utf8) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    QListWidgetItem* it = new QListWidgetItem(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    apply_list_item_data(it, stored_utf8, image_path_utf8);
    l->addItem(it);
    if (l->isSortingEnabled()) l->sortItems(Qt::AscendingOrder);
}

extern "C" void owl_ui_list_insert_item(OwlControl* lst, int index_1based,
                                         const char* text_utf8,
                                         const char* stored_utf8,
                                         const char* image_path_utf8) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0) idx = 0;
    if (idx > l->count()) idx = l->count();
    QListWidgetItem* it = new QListWidgetItem(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    apply_list_item_data(it, stored_utf8, image_path_utf8);
    l->insertItem(idx, it);
    if (l->isSortingEnabled()) l->sortItems(Qt::AscendingOrder);
}

extern "C" void owl_ui_list_remove_item(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return;
    QListWidgetItem* it = l->takeItem(idx);
    delete it;
}

extern "C" void owl_ui_list_clear(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (l) l->clear();
}

extern "C" void owl_ui_list_modify_item(OwlControl* lst, int index_1based,
                                         const char* text_utf8,
                                         const char* stored_utf8,
                                         const char* image_path_utf8) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return;
    QListWidgetItem* it = l->item(idx);
    it->setText(QString::fromUtf8(text_utf8 ? text_utf8 : ""));
    apply_list_item_data(it, stored_utf8, image_path_utf8);
    if (l->isSortingEnabled()) l->sortItems(Qt::AscendingOrder);
}

extern "C" int owl_ui_list_count(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    return l ? l->count() : 0;
}

/* Buffer statique pour get_item / get_content / get_stored. Invalide au prochain appel. */
static QByteArray g_owl_list_buf;

extern "C" const char* owl_ui_list_get_stored(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return NULL;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return NULL;
    QVariant v = l->item(idx)->data(Qt::UserRole);
    if (!v.isValid()) return NULL;
    g_owl_list_buf = v.toString().toUtf8();
    return g_owl_list_buf.constData();
}

extern "C" const char* owl_ui_list_get_item(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return nullptr;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return nullptr;
    g_owl_list_buf = l->item(idx)->text().toUtf8();
    return g_owl_list_buf.constData();
}

extern "C" int owl_ui_list_find(OwlControl* lst, const char* text_utf8) {
    OwlListWidget* l = as_list(lst);
    if (!l || !text_utf8) return -1;
    QString needle = QString::fromUtf8(text_utf8);
    for (int i = 0; i < l->count(); i++) {
        if (l->item(i)->text() == needle) return i + 1;
    }
    return -1;
}

extern "C" void owl_ui_list_sort(OwlControl* lst, int ascending) {
    OwlListWidget* l = as_list(lst);
    if (l) l->sortItems(ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

extern "C" int owl_ui_list_get_selection(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 0;
    QList<QListWidgetItem*> sel = l->selectedItems();
    if (sel.isEmpty()) return 0;
    return l->row(sel.first()) + 1;
}

extern "C" void owl_ui_list_set_selection(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0) { l->clearSelection(); return; }
    if (idx >= l->count()) return;
    l->setCurrentRow(idx);
}

extern "C" void owl_ui_list_set_multiselect(OwlControl* lst, int multi) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->setSelectionMode(multi ? QAbstractItemView::ExtendedSelection
                              : QAbstractItemView::SingleSelection);
}

extern "C" int owl_ui_list_get_multiselect(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 0;
    return l->selectionMode() != QAbstractItemView::SingleSelection ? 1 : 0;
}

extern "C" void owl_ui_list_set_sorted(OwlControl* lst, int sorted) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->setSortingEnabled(sorted ? true : false);
    if (sorted) l->sortItems(Qt::AscendingOrder);
}

extern "C" int owl_ui_list_get_sorted(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    return (l && l->isSortingEnabled()) ? 1 : 0;
}

extern "C" void owl_ui_list_set_content(OwlControl* lst, const char* multiline_utf8) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->clear();
    if (!multiline_utf8 || !*multiline_utf8) return;
    QString s = QString::fromUtf8(multiline_utf8);
    /* Canon : separateur RC ('\n'). Tolerant CRLF. */
    QStringList rows = s.split('\n');
    for (QString& r : rows) {
        if (r.endsWith('\r')) r.chop(1);
        l->addItem(r);
    }
    if (l->isSortingEnabled()) l->sortItems(Qt::AscendingOrder);
}

extern "C" void owl_ui_list_refresh(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (l) l->viewport()->update();
}

extern "C" void owl_ui_liste_set_style(OwlControl* lst,
                                       const OwlListeStyleSpec* s) {
    OwlListWidget* l = as_list(lst);
    if (!l || !s) return;

    auto rgb_css = [](int v) {
        return QString("rgb(%1, %2, %3)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF);
    };
    /*  — alpha-aware : byte-identique à rgb_css quand opaque, `rgba(...)`
     * sinon (couleur d'affichage translucide, composée en QSS sur le backdrop). */
    auto rgba_css = [&](int v, int alpha) -> QString {
        if (alpha >= 255) return rgb_css(v);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF)
            .arg(alpha / 255.0, 0, 'g', 4);
    };
    auto border_css = [&](const OwlCadreSpec* c) -> QString {
        if (!c) return QString();
        QString b;
        /*  — la bordure n'est émise que si l'épaisseur > 0, mais
         * l'arrondi est indépendant : un CadreExtérieur { Epaisseur=0, Arrondi>0 }
         * doit produire des coins ronds sans liseré (aligné owl_table_border_css). */
        if (c->thickness > 0)
            b += QString("border: %1px %2 %3; ")
                     .arg(c->thickness)
                     .arg(cadre_line_style_to_css(c->line_style))
                     .arg(rgb_css(c->color));
        if (c->corner_radius > 0)
            b += QString("border-radius: %1px; ").arg(c->corner_radius);
        return b;
    };
    /* Fragment police + couleurs commun aux lignes / a la ligne selectionnee.
     *  — emit_font=false pour les lignes (::item) : Qt n'honore pas de
     * facon fiable font-family/size en QSS sur QListWidget::item (meme classe
     * que les items de menu, ). La police des lignes est posee via
     * setFont sur le QListWidget (cf. bloc plus bas) ; on ne garde ici que
     * couleur + fond. LigneSélectionnée conserve ses font-* (emit_font=true) :
     * QSS reste le seul levier par-etat pour l'item selectionne. */
    auto text_body = [&](int fg, int fgA, int bg, int bgA, const char* family,
                         int size, int bold, int italic, bool emit_font) -> QString {
        QString body;
        if (fg > 0)  body += "color: " + rgba_css(fg, fgA) + "; ";
        if (bg >= 0) body += "background-color: " + rgba_css(bg, bgA) + "; ";
        if (emit_font) {
            if (family && *family)
                body += QString("font-family: \"%1\"; ").arg(QString::fromUtf8(family));
            if (size > 0) body += QString("font-size: %1pt; ").arg(size);
            if (bold == 1)        body += "font-weight: bold; ";
            else if (bold == 0)   body += "font-weight: normal; ";
            if (italic == 1)      body += "font-style: italic; ";
            else if (italic == 0) body += "font-style: normal; ";
        }
        return body;
    };

    /*  — police effective des lignes : posee sur le QListWidget par
     * setFont en fusionnant sur la police courante, uniquement si un attribut
     * de police des lignes est specifie. Un Style sans police de lignes ne
     * clobbe donc pas la police existante (ex. ecrite directement via ..Police),
     * et une police directe reste le socle de fusion. */
    {
        bool has_row_font = (s->row_font_family && *s->row_font_family)
                            || s->row_font_size > 0
                            || s->row_bold >= 0 || s->row_italic >= 0;
        if (has_row_font) {
            QFont f = l->font();
            if (s->row_font_family && *s->row_font_family)
                f.setFamily(QString::fromUtf8(s->row_font_family));
            if (s->row_font_size > 0) f.setPointSize(s->row_font_size);
            if (s->row_bold >= 0)   f.setBold(s->row_bold != 0);
            if (s->row_italic >= 0) f.setItalic(s->row_italic != 0);
            l->setFont(f);
        }
    }

    QString css;

    /* --- CadreExtérieur : fond + bordure de la liste (QListWidget-scope). --- */
    {
        QString body;
        if (s->ext_bg >= 0) body += "background-color: " + rgba_css(s->ext_bg, s->ext_bg_alpha) + "; ";
        body += border_css(s->ext_border);
        if (!body.isEmpty())
            css += "QListWidget { " + body + "} ";
    }

    /* --- LibelléLignes : couleur + police du texte des lignes (::item). --- */
    {
        QString body = text_body(s->row_fg, s->row_fg_alpha, s->row_bg,
                                 s->row_bg_alpha, s->row_font_family,
                                 s->row_font_size, s->row_bold, s->row_italic,
                                 /*emit_font=*/false);
        if (!body.isEmpty())
            css += "QListWidget::item { " + body + "} ";
    }

    /* --- LigneSélectionnée : couleur + fond de la ligne selectionnee. ---
     * Deux pieges Qt sur l'item selectionne :
     *   1. Sans `background-color` explicite, le style natif dessine le texte
     *      via la palette (HighlightedText) et ECRASE le `color:` du QSS. Il
     *      faut donc TOUJOURS forcer un background dans ce body pour que Qt
     *      emprunte le chemin QSS ou `color:` est respecte : si sel_bg n'est
     *      pas pose, on retombe sur la couleur Highlight de la palette.
     *   2. `::item:selected` seul ne couvre pas de facon fiable les deux etats
     *      de focus. On emet explicitement `:selected:active` (liste focus) ET
     *      `:selected:!active` (liste sans focus). */
    {
        int sel_bg = s->sel_bg;
        int sel_bg_alpha = s->sel_bg_alpha;
        if (sel_bg < 0) {
            /* Fond de selection par defaut = Highlight de la palette du widget,
             * pour ne pas repeindre en transparent (qui reactive le chemin
             * palette qui ecrase la couleur du texte). Opaque. */
            QColor hl = l->palette().color(QPalette::Highlight);
            sel_bg = (hl.blue() << 16) | (hl.green() << 8) | hl.red();
            sel_bg_alpha = 255;
        }
        QString body = text_body(s->sel_fg, s->sel_fg_alpha, sel_bg, sel_bg_alpha,
                                 s->sel_font_family, s->sel_font_size,
                                 s->sel_bold, s->sel_italic, /*emit_font=*/true);
        /* Priorite LigneSélectionnée sur LibelléLignes ( C1).
         * text_body n'emet `color:` que si sel_fg > 0 ; or la Couleur plate de
         * la brique LigneSélectionnée vaut 0 (noir) par defaut. Sans `color:`
         * explicite, l'item selectionne HERITE du `color:` de `::item`
         * (LibelléLignes) — LibelléLignes gagnerait a tort. Des lors que
         * LibelléLignes impose une couleur (row_fg > 0), on FORCE la couleur de
         * LigneSélectionnée (sel_fg, noir 0 inclus) pour qu'elle prime. */
        bool sel_has_color = body.startsWith("color:") || body.contains("; color:");
        if (s->row_fg > 0 && s->sel_fg >= 0 && !sel_has_color)
            body = "color: " + rgba_css(s->sel_fg, s->sel_fg_alpha) + "; " + body;
        if (!body.isEmpty()) {
            css += "QListWidget::item:selected:active { " + body + "} ";
            css += "QListWidget::item:selected:!active { " + body + "} ";
        }
    }

    l->setStyleSheet(css);
}

/*  — recompose la feuille QSS d'un Champ Combo stylé en bornant
 * l'arrondi de la ZoneSaisie a la geometrie REELLE du widget.
 *
 * Deux contraintes se cumulent :
 *  1. Qt jette purement les rayons qui ne tiennent pas dans la boite
 *     (qNormalizeRadii : tlr.height() + blr.height() > hauteur => les deux a
 *     zero) — un Arrondi 18 sur un combo de 25px rend donc des coins CARRES.
 *     Meme classe que le QLineEdit et le QPushButton.
 *  2. La geometrie au moment du ..Style n'est pas la geometrie finale sous
 *     layout (mesure : 100x48 au style, 80x25 apres layout). Le bornage doit
 *     donc etre rejoue a chaque redimensionnement.
 * On memorise la feuille sans arrondi + le rayon brut sur le widget, et on
 * recompose ici. Le bouton deroulant est aligne sur le meme rayon : sans ca le
 * sous-controle peint ses coins carres par-dessus l'arrondi a droite. */
static void owl_combo_apply_sheet(QComboBox* c) {
    if (!c) return;
    QVariant baseV = c->property("owl_combo_css_base");
    if (!baseV.isValid()) return;
    QString css = baseV.toString();
    /*  — un degrade de fond (CadreExtérieur..Dégradé) est pose par
     * apply_color_stylesheet, dont la feuille est ECRASEE ici (rejouee a chaque
     * resize) : le degrade demande n'atteignait jamais un Combo style. Mesure :
     * pixels identiques au jumeau nu, et feuille finale sans aucun
     * qlineargradient. On le re-seme donc dans la feuille du combo, APRES la base
     * (declaration plus tardive => le shorthand `background` prime sur le
     * `background-color` de l'aplat), box-model engage comme pour l'aplat. */
    QString grad = c->property("owl_bg_gradient").toString();
    if (!grad.isEmpty()) {
        css += "QComboBox { background: " + grad + "; ";
        if (!c->property("owl_combo_zs_border").toBool()) css += "border: none; ";
        css += "} ";
        /* Le bouton deroulant reste NON style ici : c'est un sous-controle, il
         * peint son propre fond et n'herite pas du shorthand `background` (la
         * couleur unie, elle, le traverse) — une bande opaque subsiste donc a
         * droite d'un Combo a degrade. Le rendre transparent supprime la bande
         * mais fait disparaitre la fleche native (Qt cesse de la peindre des que
         * le sous-controle est style, cf. la branche arrondi ci-dessous, meme
         * collateral). Perdre la fleche est pire que la bande : mesure a l'appui,
         * on ne touche pas. */
    }
    int r = c->property("owl_combo_zs_radius").toInt();
    if (r > 0) {
        int cap = r;
        if (c->height() > 0) cap = qMin(cap, c->height() / 2);
        if (c->width()  > 0) cap = qMin(cap, c->width()  / 2);
        if (cap > 0)
            css += QString("QComboBox { border-radius: %1px; } "
                           "QComboBox::drop-down { border: none; "
                           "background: transparent; "
                           "border-top-right-radius: %1px; "
                           "border-bottom-right-radius: %1px; } ").arg(cap);
    }
    /* Garde anti-recursion : setStyleSheet peut relancer un layout donc un
     * resize. On ne re-pose que si la feuille change reellement. */
    if (c->styleSheet() != css) c->setStyleSheet(css);
}

/*  — style typé du Champ Combo (styleCombo). UNE feuille QSS sur le
 * QComboBox, decomposee par sous-element (calque de owl_ui_liste_set_style
 * adapte au QComboBox : le champ affiche + son popup QAbstractItemView). Le
 * libellé integre est peint a part sur le QLabel du wrapper. */
extern "C" void owl_ui_combo_set_style(OwlControl* cmb, const OwlComboStyleSpec* s) {
    QComboBox* c = qobject_cast<QComboBox*>(reinterpret_cast<QObject*>(cmb));
    if (!c || !s) return;

    auto rgb_css = [](int v) {
        return QString("rgb(%1, %2, %3)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF);
    };
    /*  — alpha-aware : byte-identique à rgb_css quand opaque. */
    auto rgba_css = [&](int v, int alpha) -> QString {
        if (alpha >= 255) return rgb_css(v);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(v & 0xFF).arg((v >> 8) & 0xFF).arg((v >> 16) & 0xFF)
            .arg(alpha / 255.0, 0, 'g', 4);
    };
    auto border_css = [&](const OwlCadreSpec* cd) -> QString {
        if (!cd) return QString();
        QString b;
        /*  — bordure gatée par l'épaisseur, arrondi indépendant :
         * Epaisseur=0/Arrondi>0 => coins ronds sans liseré. */
        if (cd->thickness > 0)
            b += QString("border: %1px %2 %3; ")
                     .arg(cd->thickness)
                     .arg(cadre_line_style_to_css(cd->line_style))
                     .arg(rgb_css(cd->color));
        if (cd->corner_radius > 0)
            b += QString("border-radius: %1px; ").arg(cd->corner_radius);
        return b;
    };
    auto text_body = [&](int fg, int fgA, int bg, int bgA, const char* family,
                         int size, int bold, int italic) -> QString {
        QString body;
        if (fg > 0)  body += "color: " + rgba_css(fg, fgA) + "; ";
        if (bg >= 0) body += "background-color: " + rgba_css(bg, bgA) + "; ";
        if (family && *family)
            body += QString("font-family: \"%1\"; ").arg(QString::fromUtf8(family));
        if (size > 0) body += QString("font-size: %1pt; ").arg(size);
        if (bold == 1)        body += "font-weight: bold; ";
        else if (bold == 0)   body += "font-weight: normal; ";
        if (italic == 1)      body += "font-style: italic; ";
        else if (italic == 0) body += "font-style: normal; ";
        return body;
    };

    /*  — `setStyleSheet(css)` en fin de fonction ECRASE le stylesheet de
     * base pose a la creation. On re-seme donc `combobox-popup: 0` ici pour que
     * MaxLignesAffichées reste respecte meme apres qu'un Style utilisateur a ete
     * applique (popup force non-natif, cf. owl_ui_combo_create). */
    QString css = QStringLiteral("QComboBox { combobox-popup: 0; } ");

    /* --- CadreExtérieur + ZoneSaisie : fond, bordure, texte du champ combo. --- */
    {
        QString body = text_body(s->field_fg, s->field_fg_alpha, s->field_bg,
                                 s->field_bg_alpha, s->field_font_family,
                                 s->field_font_size, s->field_bold, s->field_italic);
        if (s->ext_bg >= 0) body += "background-color: " + rgba_css(s->ext_bg, s->ext_bg_alpha) + "; ";
        /*  — le chrome natif du QComboBox IGNORE le fond QSS tant que le
         * modele de boite n'est pas engage par une bordure : sans cadre, il
         * continue de peindre son degrade d'origine, teinte de la couleur
         * demandee. Mesure sur un RVB(255,0,0) demande sans cadre : #FE8282 en
         * haut de la boite, #FB6363 en bas — la couleur demandee n'apparaissait
         * nulle part. `border: none` engage le box-model et rend l'aplat. Emis
         * AVANT les fragments de bordure : une bordure reelle (ou un arrondi)
         * declaree plus tard prime. Meme parade que le QPushButton. */
        bool wantsFlatBox = (s->field_bg >= 0 || s->ext_bg >= 0);
        if (wantsFlatBox) body += "border: none; ";
        body += border_css(s->ext_border);
        /*  — le Cadre de la ZoneSaisie habille le QComboBox lui-meme
         * (miroir du QLineEdit d'un Champ Saisie). Emis APRES ext_border : quand
         * les deux sont poses, la ZoneSaisie gagne sur le champ ; le
         * CadreExtérieur garde le wrapper englobant. L'arrondi n'est PAS emis
         * ici : il part en fin de fonction, borne a la geometrie reelle. */
        if (s->field_border && s->field_border->thickness > 0)
            body += QString("border: %1px %2 %3; ")
                        .arg(s->field_border->thickness)
                        .arg(cadre_line_style_to_css(s->field_border->line_style))
                        .arg(rgb_css(s->field_border->color));
        if (!body.isEmpty())
            css += "QComboBox { " + body + "} ";
    }

    /* packe un QColor vers l'entier BGR (0x00BBGGRR) attendu par rgb_css. */
    auto pack_bgr = [](const QColor& c2) {
        return (c2.blue() << 16) | (c2.green() << 8) | c2.red();
    };

    /* --- LibelléLignes : couleur + police des items du popup (::item).
     *  — la couleur du champ (ZoneSaisie -> `QComboBox { color }`)
     * cascade en Qt sur `QComboBox QAbstractItemView::item` et teinte les items
     * du popup. On coupe cet heritage en FORCANT toujours un `color:` explicite
     * sur `::item` : couleur LibelléLignes si posee, sinon la couleur de texte
     * neutre de la vue du popup (thème-aware, pas de hardcode). --- */
    {
        int item_fg = (s->row_fg > 0) ? s->row_fg
                                      : pack_bgr(c->view()->palette().color(QPalette::Text));
        int item_fg_alpha = (s->row_fg > 0) ? s->row_fg_alpha : 255;
        QString body = "color: " + rgba_css(item_fg, item_fg_alpha) + "; ";
        body += text_body(-1, 255, s->row_bg, s->row_bg_alpha, s->row_font_family,
                          s->row_font_size, s->row_bold, s->row_italic);
        css += "QComboBox QAbstractItemView::item { " + body + "} ";
    }

    /* --- LigneSélectionnée : couleur + fond de l'item surligné. Mêmes pieges
     * Qt que la Liste : forcer un background pour engager le chemin
     * QSS ou `color:` est respecte.  — comme `::item`, on force toujours
     * un `color:` explicite sur l'item selectionne (sel_fg si posee, sinon la
     * couleur de texte surligne neutre de la vue) pour couper l'heritage
     * ZoneSaisie. --- */
    {
        int sel_bg = s->sel_bg;
        int sel_bg_alpha = s->sel_bg_alpha;
        if (sel_bg < 0) {
            QColor hl = c->palette().color(QPalette::Highlight);
            sel_bg = pack_bgr(hl);
            sel_bg_alpha = 255;
        }
        int sel_fg = (s->sel_fg >= 0) ? s->sel_fg
                                      : pack_bgr(c->view()->palette().color(QPalette::HighlightedText));
        int sel_fg_alpha = (s->sel_fg >= 0) ? s->sel_fg_alpha : 255;
        QString body = "color: " + rgba_css(sel_fg, sel_fg_alpha) + "; ";
        body += text_body(-1, 255, sel_bg, sel_bg_alpha, s->sel_font_family,
                          s->sel_font_size, s->sel_bold, s->sel_italic);
        css += "QComboBox QAbstractItemView::item:selected:active { " + body + "} ";
        css += "QComboBox QAbstractItemView::item:selected:!active { " + body + "} ";
    }

    /*  — la feuille composee ci-dessus est la BASE (sans l'arrondi de la
     * ZoneSaisie) ; owl_combo_apply_sheet y ajoute le rayon borne a la taille
     * courante, et la rejoue a chaque resize (cf. OwlComboBox::resizeEvent). */
    c->setProperty("owl_combo_css_base", css);
    c->setProperty("owl_combo_zs_radius",
                   s->field_border ? s->field_border->corner_radius : 0);
    /*  — une bordure REELLE engage deja le box-model : la regle de
     * degrade rejouee par owl_combo_apply_sheet n'a alors pas a la neutraliser. */
    c->setProperty("owl_combo_zs_border",
                   (s->field_border && s->field_border->thickness > 0)
                   || (s->ext_border && s->ext_border->thickness > 0));
    owl_combo_apply_sheet(c);

    /* --- Libellé integre : peint sur le QLabel du wrapper (hors QSS combo). --- */
    QWidget* wrapper = g_combo_label_wrappers.value(c, nullptr);
    if (wrapper) {
        QHBoxLayout* hb = qobject_cast<QHBoxLayout*>(wrapper->layout());
        QLabel* lbl = (hb && hb->count() > 0)
                      ? qobject_cast<QLabel*>(hb->itemAt(0)->widget()) : nullptr;
        if (lbl) {
            QString body = text_body(s->label_fg, s->label_fg_alpha, s->label_bg,
                                     s->label_bg_alpha, s->label_font_family,
                                     s->label_font_size, s->label_bold, s->label_italic);
            lbl->setStyleSheet(body.isEmpty() ? QString()
                                              : "QLabel { " + body + "}");
        }
        /* CadreExtérieur peint aussi sur le wrapper (fond + bordure englobante),
         * calque de owl_ui_saisie_wrapper_set_style. */
        QString id = wrapper->objectName();
        if (!id.isEmpty()) {
            QString wbody;
            if (s->ext_bg >= 0) wbody += "background-color: " + rgb_css(s->ext_bg) + "; ";
            /*  — le radius du CadreExtérieur doit habiller le wrapper même
             * sans bordure (Epaisseur=0/Arrondi>0). border_css émet désormais le
             * radius seul ; les marges de layout ne réservent que l'épaisseur. */
            int borderThick = (s->ext_border && s->ext_border->thickness > 0)
                                  ? s->ext_border->thickness : 0;
            wbody += border_css(s->ext_border);
            if (QLayout* ly = wrapper->layout())
                ly->setContentsMargins(borderThick, borderThick, borderThick, borderThick);
            wrapper->setStyleSheet(wbody.isEmpty() ? QString()
                                                   : "#" + id + " { " + wbody + "}");
        }
    }
}

extern "C" void owl_ui_list_on_selection(OwlControl* lst,
                                          OwlCallback cb, void* userdata) {
    OwlListWidget* l = as_list(lst);
    if (!l || !cb) return;
    OwlClickHandler* h = new OwlClickHandler{ cb, userdata,
                                              reinterpret_cast<OwlControl*>(l) };
    owl_track_conn(l,
        QObject::connect(l, &QListWidget::itemSelectionChanged, l, [h]() {
            if (h->cb) h->cb(h->sender, h->userdata);
        }));
}

extern "C" void owl_ui_list_on_entry(OwlControl* lst,
                                      OwlCallback cb, void* userdata) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->cbEntry = cb;
    l->dEntry  = userdata;
}

extern "C" void owl_ui_list_on_exit(OwlControl* lst,
                                     OwlCallback cb, void* userdata) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->cbExit = cb;
    l->dExit  = userdata;
}

/* ---- Multiselection ---- */

extern "C" void owl_ui_list_select_plus(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return;
    l->item(idx)->setSelected(true);
}

extern "C" void owl_ui_list_select_moins(OwlControl* lst, int index_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= l->count()) return;
    l->item(idx)->setSelected(false);
}

extern "C" int owl_ui_list_select_count(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 0;
    return (int)l->selectedItems().size();
}

extern "C" int owl_ui_list_select_at(OwlControl* lst, int rang_1based) {
    OwlListWidget* l = as_list(lst);
    if (!l || rang_1based <= 0) return 0;
    /* Iterer en ordre d'apparition pour matcher le canon. */
    int seen = 0;
    for (int i = 0; i < l->count(); i++) {
        if (l->item(i)->isSelected()) {
            seen++;
            if (seen == rang_1based) return i + 1;
        }
    }
    return 0;
}

/* ---- DeplaceLigne ---- */

extern "C" int owl_ui_list_move_row(OwlControl* lst, int src_1based,
                                     int dest_1based, int op_bitmask) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 0;
    int src = src_1based - 1;
    int dst = dest_1based - 1;
    if (src < 0 || src >= l->count()) return 0;
    if (dst < 0 || dst >= l->count()) return 0;
    if (src == dst) return src_1based;

    /* op_bitmask bit 0 = ECHANGE, bit 1 = AFFICHE.
     * Mode ECHANGE : swap items. Mode DEPLACE (defaut) : take src, insert at dst. */
    bool echange = (op_bitmask & 0x01) != 0;
    bool affiche = (op_bitmask & 0x02) != 0;
    int newIdx;
    if (echange) {
        QListWidgetItem* a = l->item(src);
        QListWidgetItem* b = l->item(dst);
        QString ta = a->text(), tb = b->text();
        QIcon ia = a->icon(), ib = b->icon();
        a->setText(tb); a->setIcon(ib);
        b->setText(ta); b->setIcon(ia);
        newIdx = dst + 1;
    } else {
        QListWidgetItem* it = l->takeItem(src);
        l->insertItem(dst, it);
        newIdx = dst + 1;
    }
    if (affiche) l->scrollToItem(l->item(newIdx - 1));
    return newIdx;
}

/* ---- Hit-test ---- */

extern "C" int owl_ui_list_hit_test(OwlControl* lst, int x, int y) {
    OwlListWidget* l = as_list(lst);
    if (!l) return -1;
    QListWidgetItem* it = l->itemAt(QPoint(x, y));
    if (!it) return -1;
    return l->row(it) + 1;
}

/* ---- Zoom ---- */

extern "C" void owl_ui_list_set_zoom(OwlControl* lst, int percent) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    if (percent < 1) percent = 1;
    if (percent > 1000) percent = 1000;
    l->setProperty("owl_zoom", percent);
    /* Applique le facteur sur la taille de police courante. La police
     * de base est celle definie via setFont (PoliceTaille); on capture
     * la taille originale au premier appel. */
    QVariant baseV = l->property("owl_font_base_pt");
    int basePt = baseV.isValid() ? baseV.toInt() : l->font().pointSize();
    if (basePt <= 0) basePt = 10;
    if (!baseV.isValid()) l->setProperty("owl_font_base_pt", basePt);
    QFont f = l->font();
    f.setPointSize((basePt * percent) / 100);
    l->setFont(f);
}

extern "C" int owl_ui_list_get_zoom(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 100;
    QVariant v = l->property("owl_zoom");
    return v.isValid() ? v.toInt() : 100;
}

/* ---- OptionTri (custom comparator) ---- */

/* Bits canon — repris de stdlib_string.c::CC_*. */
#define OWL_CC_SANS_CASSE              1
#define OWL_CC_SANS_ACCENT             2
#define OWL_CC_SANS_ESPACE             4
#define OWL_CC_SANS_ESPACE_INTERIEUR   8
#define OWL_CC_SANS_PONCTUATION        16
#define OWL_CC_ORDRE_LEXICOGRAPHIQUE   32
#define OWL_CC_RESPECTE_NUMERIQUE      64

/* Strip simple : minuscule + accent (tolerant Latin-1 pour les accents
 * frequents — la stripping complete UTF-8 vit ailleurs, on reste KISS V1). */
static QString owl_list_normalize_for_sort(const QString& in, int opt) {
    QString s = in;
    if (opt & OWL_CC_SANS_ESPACE) s = s.trimmed();
    if (opt & OWL_CC_SANS_CASSE)  s = s.toLower();
    if (opt & OWL_CC_SANS_ACCENT) {
        /* Strip diacritiques via NFD + filter combining marks. */
        s = s.normalized(QString::NormalizationForm_D);
        QString out;
        out.reserve(s.size());
        for (QChar c : s) {
            ushort cat = c.category();
            if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining)
                continue;
            out.append(c);
        }
        s = out;
    }
    if (opt & OWL_CC_SANS_ESPACE_INTERIEUR) s.remove(QRegularExpression("\\s+"));
    if (opt & OWL_CC_SANS_PONCTUATION) {
        QString out; out.reserve(s.size());
        for (QChar c : s) {
            if (c.isLetterOrNumber()) out.append(c);
        }
        s = out;
    }
    return s;
}

/* Compare numerique-aware si bit RESPECTE_NUMERIQUE actif (ex: "10" > "9"). */
static int owl_list_cmp_numeric_aware(const QString& a, const QString& b) {
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        QChar ca = a[i], cb = b[j];
        if (ca.isDigit() && cb.isDigit()) {
            int va = 0, vb = 0;
            while (i < a.size() && a[i].isDigit()) { va = va*10 + a[i].digitValue(); i++; }
            while (j < b.size() && b[j].isDigit()) { vb = vb*10 + b[j].digitValue(); j++; }
            if (va != vb) return va - vb;
        } else {
            int c = ca.unicode() - cb.unicode();
            if (c != 0) return c;
            i++; j++;
        }
    }
    return (a.size() - i) - (b.size() - j);
}

extern "C" void owl_ui_list_set_sort_option(OwlControl* lst, int cc_bitmask) {
    OwlListWidget* l = as_list(lst);
    if (!l) return;
    l->setProperty("owl_sort_opt", cc_bitmask);
    /* Re-trie immediatement si on a deja le mode trie actif. */
    if (!l->isSortingEnabled() && l->count() <= 1) return;
    /* Custom sort : extrait textes + icones, sort, repose. */
    int n = l->count();
    if (n <= 1) return;
    struct Row { QString text; QIcon icon; QString key; };
    QList<Row> rows; rows.reserve(n);
    for (int i = 0; i < n; i++) {
        QListWidgetItem* it = l->item(i);
        rows.append({ it->text(), it->icon(),
                      owl_list_normalize_for_sort(it->text(), cc_bitmask) });
    }
    bool numeric = (cc_bitmask & OWL_CC_RESPECTE_NUMERIQUE) != 0;
    std::sort(rows.begin(), rows.end(), [numeric](const Row& a, const Row& b){
        if (numeric) return owl_list_cmp_numeric_aware(a.key, b.key) < 0;
        return a.key < b.key;
    });
    bool wasSort = l->isSortingEnabled();
    if (wasSort) l->setSortingEnabled(false);
    l->clear();
    for (const Row& r : rows) {
        QListWidgetItem* it = new QListWidgetItem(r.icon, r.text);
        l->addItem(it);
    }
    if (wasSort) l->setSortingEnabled(true);
}

extern "C" int owl_ui_list_get_sort_option(OwlControl* lst) {
    OwlListWidget* l = as_list(lst);
    if (!l) return 0;
    QVariant v = l->property("owl_sort_opt");
    return v.isValid() ? v.toInt() : 0;
}

/* ============================================================
 * Champ Combo — V1
 *
 * QComboBox custom pour intercepter focusIn/focusOut/showPopup/hidePopup.
 * ============================================================ */

class OwlComboBox : public QComboBox {
public:
    explicit OwlComboBox(QWidget* parent = nullptr) : QComboBox(parent) {}

    OwlCallback cbEntry = nullptr;     void* dEntry = nullptr;
    OwlCallback cbExit  = nullptr;     void* dExit  = nullptr;
    OwlCallback cbOpen  = nullptr;     void* dOpen  = nullptr;
    OwlCallback cbClose = nullptr;     void* dClose = nullptr;

    /* showPopup / hidePopup sont publics dans QComboBox — on garde public
     * pour permettre l'invocation programmatique (ComboOuvre/ComboFerme). */
    void showPopup() override {
        QComboBox::showPopup();
        if (cbOpen) cbOpen(reinterpret_cast<OwlControl*>(this), dOpen);
    }
    void hidePopup() override {
        QComboBox::hidePopup();
        if (cbClose) cbClose(reinterpret_cast<OwlControl*>(this), dClose);
    }

protected:
    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }

    void focusInEvent(QFocusEvent* e) override {
        QComboBox::focusInEvent(e);
        if (cbEntry) cbEntry(asCtrl(), dEntry);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QComboBox::focusOutEvent(e);
        if (cbExit) cbExit(asCtrl(), dExit);
    }

    /*  — l'arrondi de la ZoneSaisie est borne a la demi-hauteur reelle,
     * inconnue au moment du ..Style sous layout : on rejoue le bornage ici. */
    void resizeEvent(QResizeEvent* e) override {
        QComboBox::resizeEvent(e);
        owl_combo_apply_sheet(this);
    }
};

static OwlComboBox* as_combo(OwlControl* w) {
    return dynamic_cast<OwlComboBox*>(reinterpret_cast<QWidget*>(w));
}

extern "C" OwlControl* owl_ui_combo_create(OwlWindow* parent,
                                            int x, int y, int w, int h) {
    if (!parent) return nullptr;
    OwlComboBox* cmb = new OwlComboBox(central_of(parent));
    /*  — un QComboBox NON editable utilise par defaut un popup NATIF
     * (Windows) qui ignore le QSS `::item`/`::item:selected` et setMaxVisibleItems.
     * En posant une vue Qt explicite, le popup devient non-natif dans les DEUX
     * modes : les couleurs LibelléLignes/LigneSélectionnée s'appliquent et
     * MaxLignesAffichées est respecte, editable comme non-editable. */
    cmb->setView(new QListView(cmb));
    /*  — `combobox-popup: 0` force le popup non-natif : sans elle Qt
     * peut retomber sur un popup natif qui IGNORE setMaxVisibleItems (le popup
     * affiche alors tous les items). Posee en stylesheet de base a la creation,
     * TOUJOURS (pas seulement quand un Style utilisateur est ensuite pose). */
    cmb->setStyleSheet(QStringLiteral("QComboBox { combobox-popup: 0; }"));
    cmb->setMaxVisibleItems(10);
    if (x >= 0 && y >= 0) cmb->setGeometry(x, y, w, h);
    else                  cmb->resize(w > 0 ? w : 200, h > 0 ? h : 26);
    cmb->show();
    return reinterpret_cast<OwlControl*>(cmb);
}

extern "C" void owl_ui_combo_set_editable(OwlControl* cmb, int editable) {
    OwlComboBox* c = as_combo(cmb);
    if (c) c->setEditable(editable ? true : false);
}
extern "C" int owl_ui_combo_get_editable(OwlControl* cmb) {
    OwlComboBox* c = as_combo(cmb);
    return (c && c->isEditable()) ? 1 : 0;
}
extern "C" void owl_ui_combo_set_max_visible(OwlControl* cmb, int max) {
    OwlComboBox* c = as_combo(cmb);
    if (c && max > 0) c->setMaxVisibleItems(max);
}
extern "C" int owl_ui_combo_get_max_visible(OwlControl* cmb) {
    OwlComboBox* c = as_combo(cmb);
    return c ? c->maxVisibleItems() : 0;
}
extern "C" void owl_ui_combo_open(OwlControl* cmb) {
    OwlComboBox* c = as_combo(cmb);
    if (c) c->showPopup();
}
extern "C" void owl_ui_combo_close(OwlControl* cmb) {
    OwlComboBox* c = as_combo(cmb);
    if (c) c->hidePopup();
}

extern "C" void owl_ui_combo_on_selection(OwlControl* cmb,
                                           OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c || !cb) return;
    OwlClickHandler* h = new OwlClickHandler{ cb, userdata,
                                              reinterpret_cast<OwlControl*>(c) };
    owl_track_conn(c,
        QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         c, [h](int) { if (h->cb) h->cb(h->sender, h->userdata); }));
}
extern "C" void owl_ui_combo_on_entry(OwlControl* cmb,
                                       OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c) return;
    c->cbEntry = cb; c->dEntry = userdata;
}
extern "C" void owl_ui_combo_on_exit(OwlControl* cmb,
                                      OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c) return;
    c->cbExit = cb; c->dExit = userdata;
}
extern "C" void owl_ui_combo_on_change(OwlControl* cmb,
                                        OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c || !cb) return;
    OwlClickHandler* h = new OwlClickHandler{ cb, userdata,
                                              reinterpret_cast<OwlControl*>(c) };
    owl_track_conn(c,
        QObject::connect(c, &QComboBox::editTextChanged, c,
                         [h](const QString&) { if (h->cb) h->cb(h->sender, h->userdata); }));
}
extern "C" void owl_ui_combo_on_open(OwlControl* cmb,
                                      OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c) return;
    c->cbOpen = cb; c->dOpen = userdata;
}
extern "C" void owl_ui_combo_on_close(OwlControl* cmb,
                                       OwlCallback cb, void* userdata) {
    OwlComboBox* c = as_combo(cmb);
    if (!c) return;
    c->cbClose = cb; c->dClose = userdata;
}

/* ============================================================
 * Helpers generiques Liste OU Combo — dispatch via dynamic_cast.
 * ============================================================ */

extern "C" void owl_ui_listcombo_add_item(OwlControl* w, const char* text,
                                           const char* stored, const char* image) {
    if (as_list(w)) {
        owl_ui_list_add_item(w, text, stored, image);
        return;
    }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    QIcon icon = (image && *image) ? QIcon(QString::fromUtf8(image)) : QIcon();
    QVariant data = (stored && *stored) ? QVariant(QString::fromUtf8(stored))
                                         : QVariant();
    c->addItem(icon, QString::fromUtf8(text ? text : ""), data);
    /* Triee : QComboBox::model()->sort(0). */
    QVariant sortV = c->property("owl_sorted");
    if (sortV.isValid() && sortV.toBool()) c->model()->sort(0);
}

extern "C" void owl_ui_listcombo_insert_item(OwlControl* w, int index_1based,
                                              const char* text,
                                              const char* stored,
                                              const char* image) {
    if (as_list(w)) {
        owl_ui_list_insert_item(w, index_1based, text, stored, image);
        return;
    }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    int idx = index_1based - 1;
    if (idx < 0) idx = 0;
    if (idx > c->count()) idx = c->count();
    QIcon icon = (image && *image) ? QIcon(QString::fromUtf8(image)) : QIcon();
    QVariant data = (stored && *stored) ? QVariant(QString::fromUtf8(stored))
                                         : QVariant();
    c->insertItem(idx, icon, QString::fromUtf8(text ? text : ""), data);
}

extern "C" void owl_ui_listcombo_remove_item(OwlControl* w, int index_1based) {
    if (as_list(w)) { owl_ui_list_remove_item(w, index_1based); return; }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= c->count()) return;
    c->removeItem(idx);
}

extern "C" void owl_ui_listcombo_clear(OwlControl* w) {
    if (as_list(w)) { owl_ui_list_clear(w); return; }
    OwlComboBox* c = as_combo(w);
    if (c) c->clear();
}

extern "C" void owl_ui_listcombo_modify_item(OwlControl* w, int index_1based,
                                              const char* text,
                                              const char* stored,
                                              const char* image) {
    if (as_list(w)) {
        owl_ui_list_modify_item(w, index_1based, text, stored, image);
        return;
    }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= c->count()) return;
    c->setItemText(idx, QString::fromUtf8(text ? text : ""));
    if (stored && *stored)
        c->setItemData(idx, QString::fromUtf8(stored));
    if (image && *image)
        c->setItemIcon(idx, QIcon(QString::fromUtf8(image)));
}

extern "C" int owl_ui_listcombo_count(OwlControl* w) {
    if (as_list(w)) return owl_ui_list_count(w);
    OwlComboBox* c = as_combo(w);
    return c ? c->count() : 0;
}

extern "C" const char* owl_ui_listcombo_get_item(OwlControl* w, int index_1based) {
    if (as_list(w)) return owl_ui_list_get_item(w, index_1based);
    OwlComboBox* c = as_combo(w);
    if (!c) return nullptr;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= c->count()) return nullptr;
    g_owl_list_buf = c->itemText(idx).toUtf8();
    return g_owl_list_buf.constData();
}

extern "C" const char* owl_ui_listcombo_get_stored(OwlControl* w, int index_1based) {
    if (as_list(w)) return owl_ui_list_get_stored(w, index_1based);
    OwlComboBox* c = as_combo(w);
    if (!c) return nullptr;
    int idx = index_1based - 1;
    if (idx < 0 || idx >= c->count()) return nullptr;
    QVariant v = c->itemData(idx);
    if (!v.isValid()) return nullptr;
    g_owl_list_buf = v.toString().toUtf8();
    return g_owl_list_buf.constData();
}

extern "C" int owl_ui_listcombo_find(OwlControl* w, const char* text) {
    if (as_list(w)) return owl_ui_list_find(w, text);
    OwlComboBox* c = as_combo(w);
    if (!c || !text) return -1;
    int idx = c->findText(QString::fromUtf8(text), Qt::MatchExactly);
    return (idx >= 0) ? (idx + 1) : -1;
}

extern "C" void owl_ui_listcombo_sort(OwlControl* w, int ascending) {
    if (as_list(w)) { owl_ui_list_sort(w, ascending); return; }
    OwlComboBox* c = as_combo(w);
    if (c) c->model()->sort(0, ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

extern "C" int owl_ui_listcombo_get_selection(OwlControl* w) {
    if (as_list(w)) return owl_ui_list_get_selection(w);
    OwlComboBox* c = as_combo(w);
    if (!c) return 0;
    int idx = c->currentIndex();
    return (idx >= 0) ? (idx + 1) : 0;
}

extern "C" void owl_ui_listcombo_set_selection(OwlControl* w, int index_1based) {
    if (as_list(w)) { owl_ui_list_set_selection(w, index_1based); return; }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    int idx = index_1based - 1;
    if (idx < -1) idx = -1;
    if (idx >= c->count()) return;
    c->setCurrentIndex(idx);
}

extern "C" void owl_ui_listcombo_set_sorted(OwlControl* w, int sorted) {
    if (as_list(w)) { owl_ui_list_set_sorted(w, sorted); return; }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    c->setProperty("owl_sorted", sorted ? true : false);
    if (sorted) c->model()->sort(0);
}

extern "C" int owl_ui_listcombo_get_sorted(OwlControl* w) {
    if (as_list(w)) return owl_ui_list_get_sorted(w);
    OwlComboBox* c = as_combo(w);
    if (!c) return 0;
    QVariant v = c->property("owl_sorted");
    return (v.isValid() && v.toBool()) ? 1 : 0;
}

extern "C" void owl_ui_listcombo_set_content(OwlControl* w, const char* multiline) {
    if (as_list(w)) { owl_ui_list_set_content(w, multiline); return; }
    OwlComboBox* c = as_combo(w);
    if (!c) return;
    c->clear();
    if (!multiline || !*multiline) return;
    QStringList rows = QString::fromUtf8(multiline).split('\n');
    for (QString& r : rows) {
        if (r.endsWith('\r')) r.chop(1);
        c->addItem(r);
    }
    QVariant sortV = c->property("owl_sorted");
    if (sortV.isValid() && sortV.toBool()) c->model()->sort(0);
}

extern "C" void owl_ui_listcombo_refresh(OwlControl* w) {
    if (as_list(w)) { owl_ui_list_refresh(w); return; }
    OwlComboBox* c = as_combo(w);
    if (c) c->update();
}

/* ============================================================
 * Champ Selecteur — V1 (radio button group exclusif)
 *
 * QFrame custom contenant un QButtonGroup exclusif + un layout (V/H/Grid)
 * contenant pour chaque option une QRadioButton (avec optionnellement un
 * QLabel pour le sous-libelle).
 *
 * setOptions : reconstruit completement le widget (clear + addItem N fois).
 * Setter modes : standard / sub_label / vista (icon) / vista+sub.
 * ============================================================ */

#include <QFrame>
#include <QGridLayout>
#include <QAbstractButton>
#include <QCheckBox>
#include <QRadioButton>

/*  — hauteur canonique d'une ligne d'option : celle du chemin NON
 * style. Des qu'une feuille QSS touche l'indicateur (::indicator), Qt bascule
 * sur une autre branche de sizeFromContents qui laisse tomber la marge
 * verticale du chemin natif. Mesure : ligne de 32 px sans cadre de coche,
 * 27 px avec, a police egale (18 pt) — 5 px par ligne, 30 sur six options, et
 * le texte se retrouve au contact.
 *
 * On ne re-pose pas cette marge a la main : un nombre grave finirait par
 * mentir (cf. la constante tabulee de ). On interroge la MEME source
 * que le chemin non style — un exemplaire du meme widget, hors de portee de la
 * feuille (donc sans parent), a la police effective de l'option. */
static int owl_option_canon_line_height(QAbstractButton* opt, bool isRadio) {
    /* La police posee par la feuille n'atteint le widget qu'au polish. */
    opt->ensurePolished();
    QScopedPointer<QAbstractButton> temoin(
        isRadio ? static_cast<QAbstractButton*>(new QRadioButton(opt->text()))
                : static_cast<QAbstractButton*>(new QCheckBox(opt->text())));
    temoin->setFont(opt->font());
    return temoin->sizeHint().height();
}

struct OwlSelOption {
    QString label;
    QString subLabel;
    QString stored;     /* ValeurRenvoyee, vide = non spécifié */
    QString imagePath;
};

class OwlSelecteur : public QFrame {
public:
    explicit OwlSelecteur(QWidget* parent = nullptr) : QFrame(parent) {
        m_group = new QButtonGroup(this);
        m_group->setExclusive(true);
        rebuildLayout();
        owl_track_conn(this,
            QObject::connect(m_group, &QButtonGroup::idToggled, this,
                [this](int /*id*/, bool checked) {
                    if (checked && m_cbChange) m_cbChange(asCtrl(), m_dChange);
                }));
    }

    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }

    OwlCallback m_cbChange = nullptr; void* m_dChange = nullptr;
    OwlCallback m_cbEntry  = nullptr; void* m_dEntry  = nullptr;
    OwlCallback m_cbExit   = nullptr; void* m_dExit   = nullptr;

    int m_mode    = 0;  /* OWL_SELMODE_STANDARD */
    int m_columns = 1;
    int m_optionHeight = 0;   /* 0 = hauteur naturelle (auto) */
    std::vector<OwlSelOption> m_options;

    void setMode(int m)    { m_mode = m;    rebuild(); }
    void setColumns(int c) { m_columns = c <= 0 ? 1 : c; rebuild(); }
    void setOptionHeight(int px) { m_optionHeight = px < 0 ? 0 : px; rebuild(); }
    int  optionHeight() const    { return m_optionHeight; }

    void addOption(const OwlSelOption& opt)            { m_options.push_back(opt); rebuild(); }
    void insertOption(int idx, const OwlSelOption& opt) {
        if (idx < 0) idx = 0;
        if (idx > (int)m_options.size()) idx = (int)m_options.size();
        m_options.insert(m_options.begin() + idx, opt);
        rebuild();
    }
    void removeOption(int idx) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options.erase(m_options.begin() + idx);
        rebuild();
    }
    void clearOptions() { m_options.clear(); rebuild(); }
    int  count() const  { return (int)m_options.size(); }

    int  selectionIndex() const {
        for (size_t i = 0; i < m_radios.size(); i++) {
            if (m_radios[i] && m_radios[i]->isChecked()) return (int)i + 1;
        }
        return 0;
    }
    void setSelectionIndex(int idx) {
        if (idx <= 0) {
            /* Deselectionne tout : group est exclusif donc on rend non-exclusif
             * temporairement pour decocher proprement. */
            m_group->setExclusive(false);
            for (auto* r : m_radios) if (r) r->setChecked(false);
            m_group->setExclusive(true);
            return;
        }
        int z = idx - 1;
        if (z < 0 || z >= (int)m_radios.size()) return;
        if (m_radios[z]) m_radios[z]->setChecked(true);
    }

    /*  — cible du focus pour le raccourci : le radio coche, sinon le
     * premier ; permet aux fleches de piloter la selection. Repli sur le
     * conteneur si aucune option. */
    QWidget* focusTarget() {
        for (auto* r : m_radios) if (r && r->isChecked()) return r;
        if (!m_radios.empty() && m_radios.front()) return m_radios.front();
        return this;
    }

    void modifyLabel(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].label = s;
        rebuild();
    }
    void modifySubLabel(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].subLabel = s;
        rebuild();
    }
    void modifyStored(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].stored = s;
    }
    void modifyImage(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].imagePath = s;
        rebuild();
    }

protected:
    void focusInEvent(QFocusEvent* e) override {
        QFrame::focusInEvent(e);
        if (m_cbEntry) m_cbEntry(asCtrl(), m_dEntry);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QFrame::focusOutEvent(e);
        if (m_cbExit) m_cbExit(asCtrl(), m_dExit);
    }

    /*  — une police ecrite DIRECTEMENT sur le champ (raccourci plat,
     * litteral compose, cascade, affectation en cours d'execution) n'emprunte
     * pas le chemin de la feuille de style : personne n'y rappelait la loi de
     * hauteur de ligne, et le bloc restait a la geometrie de la police par
     * defaut pendant que le texte, lui, grandissait. Qt propage la police aux
     * options AVANT de nous notifier (QWidgetPrivate::updateFont), donc la
     * police lue ici est deja la police rendue. */
    void changeEvent(QEvent* e) override {
        QFrame::changeEvent(e);
        if (e->type() == QEvent::FontChange && m_layout) refitAfterStyle();
    }

private:
    QButtonGroup* m_group = nullptr;
    std::vector<QRadioButton*> m_radios;
    QGridLayout*  m_layout = nullptr;

    void rebuildLayout() {
        if (m_layout) delete m_layout;
        m_layout = new QGridLayout(this);
        m_layout->setContentsMargins(4, 4, 4, 4);
        m_layout->setSpacing(2);
    }

    void rebuild() {
        /* Clear : retire tous les radios + sub-labels du layout, supprime. */
        int prevSel = selectionIndex();
        for (auto* r : m_radios) {
            if (r) {
                m_group->removeButton(r);
                delete r;
            }
        }
        m_radios.clear();
        /* Vide le layout des QLabel sub-labels eventuels. */
        QLayoutItem* item;
        while ((item = m_layout->takeAt(0)) != nullptr) {
            if (item->widget()) delete item->widget();
            delete item;
        }

        bool hasSubLabel = (m_mode == 1 /* sousLib */ || m_mode == 3 /* Vista+sub */);
        bool hasIcon     = (m_mode == 2 /* Vista */ || m_mode == 3);

        int rowsPerOption = hasSubLabel ? 2 : 1;
        int cols = m_columns > 0 ? m_columns : 1;

        for (size_t i = 0; i < m_options.size(); i++) {
            QRadioButton* r = new QRadioButton(m_options[i].label, this);
            if (hasIcon && !m_options[i].imagePath.isEmpty()) {
                r->setIcon(QIcon(m_options[i].imagePath));
            }
            /* HauteurOption > 0 : hauteur fixe de la ligne d'option. 0 = naturelle. */
            if (m_optionHeight > 0)
                r->setFixedHeight(m_optionHeight);
            m_group->addButton(r, (int)i);
            m_radios.push_back(r);

            int slotRow = (int)(i / cols) * rowsPerOption;
            int slotCol = (int)(i % cols);
            /* AlignTop : a hauteur fixe, une cellule de grid plus haute que son
             * widget le centre par defaut -> gap parasite. AlignTop tasse chaque
             * ligne au sommet de sa cellule (cf. Interrupteur). */
            m_layout->addWidget(r, slotRow, slotCol, Qt::AlignTop);

            if (hasSubLabel && !m_options[i].subLabel.isEmpty()) {
                QLabel* sub = new QLabel(m_options[i].subLabel, this);
                QFont f = sub->font();
                f.setPointSize(qMax(7, f.pointSize() - 2));
                sub->setFont(f);
                sub->setStyleSheet("color: #666; margin-left: 18px;");
                m_layout->addWidget(sub, slotRow + 1, slotCol, Qt::AlignTop);
            }
        }

        if (prevSel > 0) setSelectionIndex(prevSel);

        /* Packer les options en haut : le QGridLayout distribue l'espace vertical
         * residuel sur ses rows quand le QFrame depasse la somme des lignes. On
         * neutralise ce reflux en donnant tout le stretch a une row sentinelle
         * placee sous la derniere option (les rows d'options gardent stretch 0). */
        int usedRows = m_options.empty()
            ? 0
            : (((int)(m_options.size() - 1) / cols) + 1) * rowsPerOption;
        /* rowCount() ne decroit pas entre rebuilds : on reset tous les stretches
         * connus avant de poser la sentinelle.
         *
         * rowCount() capture UNE fois : setRowStretch(r, ...) agrandit la grille
         * des que r >= rowCount(). Une borne reevaluee a chaque tour ferait donc
         * croitre la grille en lockstep avec r -> boucle infinie jusqu'a
         * l'explosion du QVector interne (SIGSEGV). Borne fixe + '<' = sur. */
        int knownRows = m_layout->rowCount();
        for (int r = 0; r < knownRows; r++)
            m_layout->setRowStretch(r, 0);
        /* Top-pack uniquement a hauteur d'option fixe (HauteurOption > 0) : la row
         * sentinelle absorbe le surplus vertical. En hauteur naturelle (0), on
         * laisse le QGridLayout repartir l'espace uniformement (pas de sentinelle). */
        if (m_optionHeight > 0)
            m_layout->setRowStretch(usedRows, 1);

        applyCanonLineHeights();
        refitToContent();
    }

    /*  — la hauteur d'une ligne suit la meme loi, stylee ou non. */
    void applyCanonLineHeights() {
        if (m_optionHeight > 0) return;   /* HauteurOption : hauteur imposee */
        for (auto* r : m_radios) {
            if (r) r->setMinimumHeight(owl_option_canon_line_height(r, true));
        }
    }

    /* Auto-grow : borne la hauteur minimale au contenu et rafraichit le
     * sizeHint pour les layouts parents. */
    void refitToContent() {
        m_layout->invalidate();
        int contentH = m_layout->sizeHint().height();
        setMinimumHeight(contentH);
        if (height() < contentH) resize(width(), contentH);
        updateGeometry();
    }

public:
    /* A rappeler apres avoir pose une feuille de style : elle peut changer la
     * police effective des options, donc la hauteur canonique de la ligne. */
    void refitAfterStyle() {
        applyCanonLineHeights();
        refitToContent();
    }

    QSize sizeHint() const override {
        QSize s = QFrame::sizeHint();
        if (m_layout) s.setHeight(qMax(s.height(), m_layout->sizeHint().height()));
        return s;
    }
    QSize minimumSizeHint() const override {
        QSize s = QFrame::minimumSizeHint();
        if (m_layout) s.setHeight(qMax(s.height(), m_layout->sizeHint().height()));
        return s;
    }
};

static OwlSelecteur* as_selecteur(OwlControl* w) {
    return dynamic_cast<OwlSelecteur*>(reinterpret_cast<QWidget*>(w));
}

extern "C" OwlControl* owl_ui_selecteur_create(OwlWindow* parent,
                                                int x, int y, int w, int h) {
    if (!parent) return nullptr;
    OwlSelecteur* sel = new OwlSelecteur(central_of(parent));
    if (x >= 0 && y >= 0) sel->setGeometry(x, y, w, h);
    else                  sel->resize(w > 0 ? w : 200, h > 0 ? h : 100);
    sel->show();
    return reinterpret_cast<OwlControl*>(sel);
}

extern "C" void owl_ui_selecteur_set_display_mode(OwlControl* sel, int mode) {
    /* mode est une sentinelle OWL_SELMODE_* ; on convertit en index 0..3. */
    /* Cf OWL_SELMODE_BASE + 0..3 dans sentinels.h. On ne peut pas inclure
     * sentinels.h dans le .cpp facilement vu que le SENTINEL_BASE = INT32_MIN ;
     * on detecte la base par une heuristique : modulo 256. */
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    int idx = mode & 0xFF;  /* keep low 8 bits ; sentinels use SENTINEL_OFFSET(N) + i */
    if (idx < 0 || idx > 3) idx = 0;
    s->setMode(idx);
}
extern "C" int owl_ui_selecteur_get_display_mode(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    return s ? s->m_mode : 0;
}
extern "C" void owl_ui_selecteur_set_column_count(OwlControl* sel, int cols) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->setColumns(cols);
}
extern "C" int owl_ui_selecteur_get_column_count(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    return s ? s->m_columns : 1;
}
extern "C" void owl_ui_selecteur_set_option_height(OwlControl* sel, int px) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->setOptionHeight(px);
}
extern "C" int owl_ui_selecteur_get_option_height(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    return s ? s->optionHeight() : 0;
}

extern "C" void owl_ui_selecteur_add_option(OwlControl* sel,
                                             const char* label,
                                             const char* sub_label,
                                             const char* stored,
                                             const char* image_path) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    OwlSelOption opt;
    opt.label     = QString::fromUtf8(label ? label : "");
    opt.subLabel  = QString::fromUtf8(sub_label ? sub_label : "");
    opt.stored    = QString::fromUtf8(stored ? stored : "");
    opt.imagePath = QString::fromUtf8(image_path ? image_path : "");
    s->addOption(opt);
}

extern "C" void owl_ui_selecteur_insert_option(OwlControl* sel, int idx,
                                                const char* label,
                                                const char* sub_label,
                                                const char* stored,
                                                const char* image_path) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    OwlSelOption opt;
    opt.label     = QString::fromUtf8(label ? label : "");
    opt.subLabel  = QString::fromUtf8(sub_label ? sub_label : "");
    opt.stored    = QString::fromUtf8(stored ? stored : "");
    opt.imagePath = QString::fromUtf8(image_path ? image_path : "");
    s->insertOption(idx - 1, opt);
}

extern "C" void owl_ui_selecteur_remove_option(OwlControl* sel, int idx) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->removeOption(idx - 1);
}

extern "C" void owl_ui_selecteur_clear(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->clearOptions();
}

extern "C" int owl_ui_selecteur_count(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    return s ? s->count() : 0;
}

/* Buffer statique pour les getters (invalide au prochain appel). */
static QByteArray g_owl_sel_buf;

extern "C" const char* owl_ui_selecteur_get_option_label(OwlControl* sel, int idx) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return nullptr;
    int z = idx - 1;
    if (z < 0 || z >= s->count()) return nullptr;
    g_owl_sel_buf = s->m_options[z].label.toUtf8();
    return g_owl_sel_buf.constData();
}
extern "C" const char* owl_ui_selecteur_get_option_sub_label(OwlControl* sel, int idx) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return nullptr;
    int z = idx - 1;
    if (z < 0 || z >= s->count()) return nullptr;
    g_owl_sel_buf = s->m_options[z].subLabel.toUtf8();
    return g_owl_sel_buf.constData();
}
extern "C" const char* owl_ui_selecteur_get_option_stored(OwlControl* sel, int idx) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return nullptr;
    int z = idx - 1;
    if (z < 0 || z >= s->count()) return nullptr;
    g_owl_sel_buf = s->m_options[z].stored.toUtf8();
    return g_owl_sel_buf.constData();
}
extern "C" void owl_ui_selecteur_set_option_label(OwlControl* sel, int idx, const char* lbl) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->modifyLabel(idx - 1, QString::fromUtf8(lbl ? lbl : ""));
}
extern "C" void owl_ui_selecteur_set_option_sub_label(OwlControl* sel, int idx, const char* lbl) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->modifySubLabel(idx - 1, QString::fromUtf8(lbl ? lbl : ""));
}
extern "C" void owl_ui_selecteur_set_option_stored(OwlControl* sel, int idx, const char* v) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->modifyStored(idx - 1, QString::fromUtf8(v ? v : ""));
}
extern "C" int  owl_ui_selecteur_get_selection_index(OwlControl* sel) {
    OwlSelecteur* s = as_selecteur(sel);
    return s ? s->selectionIndex() : 0;
}
extern "C" void owl_ui_selecteur_set_selection_index(OwlControl* sel, int idx) {
    OwlSelecteur* s = as_selecteur(sel);
    if (s) s->setSelectionIndex(idx);
}
extern "C" void owl_ui_selecteur_on_change(OwlControl* sel, OwlCallback cb, void* d) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    s->m_cbChange = cb; s->m_dChange = d;
}
extern "C" void owl_ui_selecteur_on_entry(OwlControl* sel, OwlCallback cb, void* d) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    s->m_cbEntry = cb; s->m_dEntry = d;
}
extern "C" void owl_ui_selecteur_on_exit(OwlControl* sel, OwlCallback cb, void* d) {
    OwlSelecteur* s = as_selecteur(sel);
    if (!s) return;
    s->m_cbExit = cb; s->m_dExit = d;
}

/*  — style typé applique en UNE feuille QSS sur le QFrame. Pendant du
 * owl_ui_interrupteur_set_style, adapte radio. Le conteneur (CadreExtérieur) est
 * scope via #objectName pour ne pas cascader aux options ; TexteOptions cible
 * QRadioButton ; CadrePuce cible QRadioButton::indicator ; Puce (rond) approche
 * le fond de ::indicator:checked. */
extern "C" void owl_ui_selecteur_set_style(OwlControl* sel,
                                           const OwlSelecteurStyleSpec* s) {
    OwlSelecteur* se = as_selecteur(sel);
    if (!se || !s) return;

    /* Format espacé 'rgb(R, G, B)' — inspectable via __DebugFeuilleStyle, distinct
     * du owl_bg_css sans espace.  — alpha < 255 => 'rgba(R, G, B, A)' (même
     * format espacé) ; alpha=255 reste byte-identique à l'émetteur opaque d'origine. */
    auto rgb_css = [](int v, int alpha = 255) {
        int r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
        if (alpha >= 255)
            return QString("rgb(%1, %2, %3)").arg(r).arg(g).arg(b);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(r).arg(g).arg(b).arg(alpha / 255.0, 0, 'g', 4);
    };
    auto border_css = [&](const OwlCadreSpec* c) -> QString {
        if (!c) return QString();
        QString b;
        /*  — la bordure n'est émise que si l'épaisseur > 0, mais
         * l'arrondi est indépendant : un CadreExtérieur { Epaisseur=0, Arrondi>0 }
         * doit produire des coins ronds sans liseré (aligné owl_table_border_css). */
        if (c->thickness > 0)
            b += QString("border: %1px %2 %3; ")
                     .arg(c->thickness)
                     .arg(cadre_line_style_to_css(c->line_style))
                     .arg(rgb_css(c->color));
        if (c->corner_radius > 0)
            b += QString("border-radius: %1px; ").arg(c->corner_radius);
        return b;
    };

    /* Le conteneur doit porter un objectName pour le scope #id (sinon le
     * background/border du CadreExtérieur cascaderait sur les QRadioButton). */
    if (se->objectName().isEmpty())
        se->setObjectName(QString("owl_sel_%1").arg((quintptr)se, 0, 16));
    QString oname = se->objectName();

    QString css;

    /* --- CadreExtérieur : fond + bordure du conteneur (#id-scope). --- */
    {
        QString body;
        if (s->ext_bg >= 0) body += "background-color: " + rgb_css(s->ext_bg, s->ext_bg_alpha) + "; ";
        body += border_css(s->ext_border);
        if (!body.isEmpty())
            css += "QFrame#" + oname + " { " + body + "} ";
    }

    /* --- TexteOptions : couleur + police du texte des options. --- */
    {
        QString body;
        if (s->txt_fg > 0) body += "color: " + rgb_css(s->txt_fg, s->txt_fg_alpha) + "; ";
        if (s->txt_bg >= 0) body += "background-color: " + rgb_css(s->txt_bg, s->txt_bg_alpha) + "; ";
        if (s->txt_font_family && *s->txt_font_family)
            body += QString("font-family: \"%1\"; ").arg(QString::fromUtf8(s->txt_font_family));
        if (s->txt_font_size > 0)
            body += QString("font-size: %1pt; ").arg(s->txt_font_size);
        if (s->txt_bold == 1)      body += "font-weight: bold; ";
        else if (s->txt_bold == 0) body += "font-weight: normal; ";
        if (s->txt_italic == 1)      body += "font-style: italic; ";
        else if (s->txt_italic == 0) body += "font-style: normal; ";
        if (!body.isEmpty())
            css += "QRadioButton { " + body + "} ";
    }

    /* --- CadrePuce : fond + bordure de la boite du rond (::indicator). ---
     * Styler l'::indicator remplace le glyphe radio natif (rond) par une boîte
     * QSS carrée par défaut. Un Sélecteur est conceptuellement rond : dès qu'on
     * pose un style de puce, on impose une taille fixe et un border-radius = moitié
     * de la taille pour garder l'indicateur circulaire (sauf si le thème a défini
     * son propre Arrondi via CadrePuce..Cadre..Arrondi, respecté par border_css). */
    {
        QString body;
        if (s->puce_bg >= 0) body += "background-color: " + rgb_css(s->puce_bg, s->puce_bg_alpha) + "; ";
        body += border_css(s->puce_border);
        if (!body.isEmpty()) {
            bool hasRadius = s->puce_border && s->puce_border->corner_radius > 0;
            if (!hasRadius) {
                int th = s->puce_border ? s->puce_border->thickness : 0;
                if (th < 0) th = 0;
                int box = 16 + 2 * th;   /* contenu 16px + bordure des 2 côtés */
                body += QString("width: 16px; height: 16px; border-radius: %1px; ")
                            .arg(box / 2);
            }
            css += "QRadioButton::indicator { " + body + "} ";
        }
    }

    /* --- Puce : couleur du rond selectionne (approche : fond de
     * ::indicator:checked). Qt ne stylise pas le glyphe radio seul (image
     * thémée) ; colorer le fond de ::indicator:checked donne un rond visible. --- */
    if (s->puce_mark > 0) {
        css += "QRadioButton::indicator:checked { background-color: "
               + rgb_css(s->puce_mark, s->puce_mark_alpha) + "; } ";
    }

    se->setStyleSheet(css);
    se->refitAfterStyle();
}

/* ============================================================
 * Champ Interrupteur — V1 (checkbox group multi-select)
 *
 * QFrame custom + N QCheckBox INDEPENDANTES (pas QButtonGroup).
 * Mode tri-etat via QCheckBox::setTristate.
 * ============================================================ */

struct OwlIntOption {
    QString label;
    QString subLabel;
    QString imagePath;
    bool    tristate = false;   /* mode tri-etat PAR OPTION */
};

class OwlInterrupteur : public QFrame {
public:
    explicit OwlInterrupteur(QWidget* parent = nullptr) : QFrame(parent) {
        rebuildLayout();
    }
    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }

    OwlCallback m_cbChange = nullptr; void* m_dChange = nullptr;
    OwlCallback m_cbEntry  = nullptr; void* m_dEntry  = nullptr;
    OwlCallback m_cbExit   = nullptr; void* m_dExit   = nullptr;

    int  m_mode    = 0;
    int  m_columns = 1;
    int  m_optionHeight = 0;   /* 0 = hauteur naturelle (auto) */
    bool m_tristate = false;
    std::vector<OwlIntOption> m_options;

    void setMode(int m)    { m_mode = m;    rebuild(); }
    void setColumns(int c) { m_columns = c <= 0 ? 1 : c; rebuild(); }
    void setOptionHeight(int px) { m_optionHeight = px < 0 ? 0 : px; rebuild(); }
    int  optionHeight() const    { return m_optionHeight; }
    /*  — TroisEtats de champ = convenance « toutes les options » : pose
     * le mode sur chaque option, source de verite = m_options[i].tristate. */
    void setTristate(bool t) {
        m_tristate = t;
        for (auto& opt : m_options) opt.tristate = t;
        for (auto* cb : m_checks) if (cb) cb->setTristate(t);
    }

    /*  — mode tri-etat d'UNE option (1-based cote API, 0-based ici). */
    void setOptionTristate(int idx, bool t) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].tristate = t;
        if (idx < (int)m_checks.size() && m_checks[idx])
            m_checks[idx]->setTristate(t);
    }
    bool optionTristate(int idx) const {
        if (idx < 0 || idx >= (int)m_options.size()) return false;
        return m_options[idx].tristate;
    }

    void addOption(const OwlIntOption& opt)            { m_options.push_back(opt); rebuild(); }
    void insertOption(int idx, const OwlIntOption& opt) {
        if (idx < 0) idx = 0;
        if (idx > (int)m_options.size()) idx = (int)m_options.size();
        m_options.insert(m_options.begin() + idx, opt);
        rebuild();
    }
    void removeOption(int idx) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options.erase(m_options.begin() + idx);
        rebuild();
    }
    void clearOptions() { m_options.clear(); rebuild(); }
    int  count() const  { return (int)m_options.size(); }

    /*  — cible du focus pour le raccourci : la checkbox cochee, sinon la
     * premiere ; permet a l'espace de basculer l'etat. Repli sur le conteneur si
     * aucune option. */
    QWidget* focusTarget() {
        for (auto* c : m_checks) if (c && c->checkState() == Qt::Checked) return c;
        if (!m_checks.empty() && m_checks.front()) return m_checks.front();
        return this;
    }

    int getOptionState(int idx) const {
        if (idx < 0 || idx >= (int)m_checks.size() || !m_checks[idx]) return 0;
        Qt::CheckState st = m_checks[idx]->checkState();
        if (st == Qt::Checked)         return 1;
        if (st == Qt::PartiallyChecked) return -1;
        return 0;
    }
    void setOptionState(int idx, int state) {
        if (idx < 0 || idx >= (int)m_checks.size() || !m_checks[idx]) return;
        if (state == -1)
            m_checks[idx]->setCheckState(Qt::PartiallyChecked);
        else
            m_checks[idx]->setCheckState(state ? Qt::Checked : Qt::Unchecked);
    }

    int64_t getBitmask() const {
        int64_t mask = 0;
        for (size_t i = 0; i < m_checks.size(); i++) {
            if (m_checks[i] && m_checks[i]->isChecked())
                mask |= (int64_t)1 << i;
        }
        return mask;
    }
    void setBitmask(int64_t mask) {
        for (size_t i = 0; i < m_checks.size(); i++) {
            if (!m_checks[i]) continue;
            bool on = (mask & ((int64_t)1 << i)) != 0;
            m_checks[i]->setChecked(on);
        }
    }

    void modifyLabel(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].label = s;
        rebuild();
    }
    void modifySubLabel(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].subLabel = s;
        rebuild();
    }
    void modifyImage(int idx, const QString& s) {
        if (idx < 0 || idx >= (int)m_options.size()) return;
        m_options[idx].imagePath = s;
        rebuild();
    }

protected:
    void focusInEvent(QFocusEvent* e) override {
        QFrame::focusInEvent(e);
        if (m_cbEntry) m_cbEntry(asCtrl(), m_dEntry);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QFrame::focusOutEvent(e);
        if (m_cbExit) m_cbExit(asCtrl(), m_dExit);
    }

    /*  — pendant strict du Selecteur : la police ecrite directement
     * sur le champ doit rendre la meme geometrie que par la feuille de style. */
    void changeEvent(QEvent* e) override {
        QFrame::changeEvent(e);
        if (e->type() == QEvent::FontChange && m_layout) refitAfterStyle();
    }

private:
    std::vector<QCheckBox*> m_checks;
    QGridLayout*  m_layout = nullptr;

    void rebuildLayout() {
        if (m_layout) delete m_layout;
        m_layout = new QGridLayout(this);
        m_layout->setContentsMargins(4, 4, 4, 4);
        m_layout->setSpacing(2);
    }
    void rebuild() {
        /* Save current state. */
        int64_t prevMask = getBitmask();

        for (auto* c : m_checks) if (c) delete c;
        m_checks.clear();

        QLayoutItem* item;
        while ((item = m_layout->takeAt(0)) != nullptr) {
            if (item->widget()) delete item->widget();
            delete item;
        }

        bool hasSubLabel = (m_mode == 1 || m_mode == 3);
        bool hasIcon     = (m_mode == 2 || m_mode == 3);
        int rowsPerOption = hasSubLabel ? 2 : 1;
        int cols = m_columns > 0 ? m_columns : 1;

        for (size_t i = 0; i < m_options.size(); i++) {
            QCheckBox* c = new QCheckBox(m_options[i].label, this);
            c->setTristate(m_options[i].tristate);
            if (hasIcon && !m_options[i].imagePath.isEmpty()) {
                c->setIcon(QIcon(m_options[i].imagePath));
            }
            /* HauteurOption > 0 : hauteur fixe de la ligne d'option. 0 = naturelle. */
            if (m_optionHeight > 0)
                c->setFixedHeight(m_optionHeight);
            owl_track_conn(c,
                QObject::connect(c, &QCheckBox::checkStateChanged, this,
                                 [this](Qt::CheckState) {
                    if (m_cbChange) m_cbChange(asCtrl(), m_dChange);
                }));
            m_checks.push_back(c);
            int slotRow = (int)(i / cols) * rowsPerOption;
            int slotCol = (int)(i % cols);
            /* Aligner en haut : une cellule de grid plus haute que son widget
             * centre l'item par defaut. A hauteur fixe (m_optionHeight), ce
             * centrage repartit l'espace residuel autour de chaque ligne ->
             * gap parasite. AlignTop tasse chaque ligne au sommet de sa cellule. */
            m_layout->addWidget(c, slotRow, slotCol, Qt::AlignTop);
            if (hasSubLabel && !m_options[i].subLabel.isEmpty()) {
                QLabel* sub = new QLabel(m_options[i].subLabel, this);
                QFont f = sub->font();
                f.setPointSize(qMax(7, f.pointSize() - 2));
                sub->setFont(f);
                sub->setStyleSheet("color: #666; margin-left: 18px;");
                m_layout->addWidget(sub, slotRow + 1, slotCol, Qt::AlignTop);
            }
        }
        /* Restore selection state. */
        if (prevMask != 0) setBitmask(prevMask);

        /* Packer les options en haut : le QGridLayout distribue l'espace vertical
         * residuel sur ses rows quand le QFrame depasse la somme des lignes. On
         * neutralise ce reflux en donnant tout le stretch a une row sentinelle
         * placee sous la derniere option (les rows d'options gardent stretch 0
         * -> hauteur exacte demandee, la row sentinelle absorbe le surplus). */
        int usedRows = m_options.empty()
            ? 0
            : (((int)(m_options.size() - 1) / cols) + 1) * rowsPerOption;
        /* rowCount() ne decroit pas entre rebuilds : on reset tous les stretches
         * connus avant de poser la sentinelle, sinon une ancienne row-sentinelle
         * plus basse resterait a 1 apres suppression d'options.
         *
         * rowCount() capture UNE fois : setRowStretch(r, ...) agrandit la grille
         * des que r >= rowCount(). Une borne reevaluee a chaque tour ferait donc
         * croitre la grille en lockstep avec r -> boucle infinie jusqu'a
         * l'explosion du QVector interne (SIGSEGV). Borne fixe + '<' = sur. */
        int knownRows = m_layout->rowCount();
        for (int r = 0; r < knownRows; r++)
            m_layout->setRowStretch(r, 0);
        /* Top-pack uniquement a hauteur d'option fixe (HauteurOption > 0) : la row
         * sentinelle absorbe le surplus vertical. En hauteur naturelle (0), on
         * laisse le QGridLayout repartir l'espace uniformement (pas de sentinelle). */
        if (m_optionHeight > 0)
            m_layout->setRowStretch(usedRows, 1);

        applyCanonLineHeights();
        refitToContent();
    }

    /*  — la hauteur d'une ligne suit la meme loi, stylee ou non. */
    void applyCanonLineHeights() {
        if (m_optionHeight > 0) return;   /* HauteurOption : hauteur imposee */
        for (auto* c : m_checks) {
            if (c) c->setMinimumHeight(owl_option_canon_line_height(c, false));
        }
    }

    /* Auto-grow : le conteneur ne doit pas comprimer les options dans la
     * geometrie initiale. On borne sa hauteur minimale au contenu (N lignes
     * x hauteur de ligne + marges + espacements) et on rafraichit le
     * sizeHint pour les layouts parents. */
    void refitToContent() {
        m_layout->invalidate();
        int contentH = m_layout->sizeHint().height();
        setMinimumHeight(contentH);
        if (height() < contentH) resize(width(), contentH);
        updateGeometry();
    }

public:
    /* A rappeler apres avoir pose une feuille de style : elle peut changer la
     * police effective des options, donc la hauteur canonique de la ligne. */
    void refitAfterStyle() {
        applyCanonLineHeights();
        refitToContent();
    }

    QSize sizeHint() const override {
        QSize s = QFrame::sizeHint();
        if (m_layout) s.setHeight(qMax(s.height(), m_layout->sizeHint().height()));
        return s;
    }
    QSize minimumSizeHint() const override {
        QSize s = QFrame::minimumSizeHint();
        if (m_layout) s.setHeight(qMax(s.height(), m_layout->sizeHint().height()));
        return s;
    }
};

static OwlInterrupteur* as_interrupteur(OwlControl* w) {
    return dynamic_cast<OwlInterrupteur*>(reinterpret_cast<QWidget*>(w));
}

extern "C" OwlControl* owl_ui_interrupteur_create(OwlWindow* parent,
                                                   int x, int y, int w, int h) {
    if (!parent) return nullptr;
    OwlInterrupteur* it = new OwlInterrupteur(central_of(parent));
    if (x >= 0 && y >= 0) it->setGeometry(x, y, w, h);
    else                  it->resize(w > 0 ? w : 200, h > 0 ? h : 100);
    it->show();
    return reinterpret_cast<OwlControl*>(it);
}

extern "C" void owl_ui_interrupteur_set_display_mode(OwlControl* sel, int mode) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (!s) return;
    int idx = mode & 0xFF;
    if (idx < 0 || idx > 3) idx = 0;
    s->setMode(idx);
}
extern "C" int owl_ui_interrupteur_get_display_mode(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    return s ? s->m_mode : 0;
}
extern "C" void owl_ui_interrupteur_set_column_count(OwlControl* sel, int cols) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (s) s->setColumns(cols);
}
extern "C" int owl_ui_interrupteur_get_column_count(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    return s ? s->m_columns : 1;
}
extern "C" void owl_ui_interrupteur_set_option_height(OwlControl* sel, int px) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (s) s->setOptionHeight(px);
}
extern "C" int owl_ui_interrupteur_get_option_height(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    return s ? s->optionHeight() : 0;
}
extern "C" void owl_ui_interrupteur_set_three_states(OwlControl* sel, int enabled) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (s) s->setTristate(enabled ? true : false);
}
extern "C" int owl_ui_interrupteur_get_three_states(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    return (s && s->m_tristate) ? 1 : 0;
}

extern "C" void owl_ui_interrupteur_add_option(OwlControl* sel,
                                                const char* label,
                                                const char* sub_label,
                                                const char* image_path) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (!s) return;
    OwlIntOption opt;
    opt.label     = QString::fromUtf8(label ? label : "");
    opt.subLabel  = QString::fromUtf8(sub_label ? sub_label : "");
    opt.imagePath = QString::fromUtf8(image_path ? image_path : "");
    s->addOption(opt);
}

extern "C" void owl_ui_interrupteur_insert_option(OwlControl* sel, int idx,
                                                   const char* label,
                                                   const char* sub_label,
                                                   const char* image_path) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (!s) return;
    OwlIntOption opt;
    opt.label     = QString::fromUtf8(label ? label : "");
    opt.subLabel  = QString::fromUtf8(sub_label ? sub_label : "");
    opt.imagePath = QString::fromUtf8(image_path ? image_path : "");
    s->insertOption(idx - 1, opt);
}

extern "C" void owl_ui_interrupteur_remove_option(OwlControl* sel, int idx) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (s) s->removeOption(idx - 1);
}

extern "C" void owl_ui_interrupteur_clear(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (s) s->clearOptions();
}

extern "C" int owl_ui_interrupteur_count(OwlControl* sel) {
    OwlInterrupteur* s = as_interrupteur(sel);
    return s ? s->count() : 0;
}

static QByteArray g_owl_int_buf;

extern "C" const char* owl_ui_interrupteur_get_option_label(OwlControl* sel, int idx) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (!s) return nullptr;
    int z = idx - 1;
    if (z < 0 || z >= s->count()) return nullptr;
    g_owl_int_buf = s->m_options[z].label.toUtf8();
    return g_owl_int_buf.constData();
}
extern "C" const char* owl_ui_interrupteur_get_option_sub_label(OwlControl* sel, int idx) {
    OwlInterrupteur* s = as_interrupteur(sel);
    if (!s) return nullptr;
    int z = idx - 1;
    if (z < 0 || z >= s->count()) return nullptr;
    g_owl_int_buf = s->m_options[z].subLabel.toUtf8();
    return g_owl_int_buf.constData();
}
extern "C" void owl_ui_interrupteur_set_option_label(OwlControl* sel, int idx, const char* s) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (w) w->modifyLabel(idx - 1, QString::fromUtf8(s ? s : ""));
}
extern "C" void owl_ui_interrupteur_set_option_sub_label(OwlControl* sel, int idx, const char* s) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (w) w->modifySubLabel(idx - 1, QString::fromUtf8(s ? s : ""));
}
extern "C" int owl_ui_interrupteur_get_option_state(OwlControl* sel, int idx) {
    OwlInterrupteur* w = as_interrupteur(sel);
    return w ? w->getOptionState(idx - 1) : 0;
}
extern "C" void owl_ui_interrupteur_set_option_state(OwlControl* sel, int idx, int state) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (w) w->setOptionState(idx - 1, state);
}
extern "C" int owl_ui_interrupteur_get_option_three_states(OwlControl* sel, int idx) {
    OwlInterrupteur* w = as_interrupteur(sel);
    return (w && w->optionTristate(idx - 1)) ? 1 : 0;
}
extern "C" void owl_ui_interrupteur_set_option_three_states(OwlControl* sel, int idx, int enabled) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (w) w->setOptionTristate(idx - 1, enabled ? true : false);
}

extern "C" int64_t owl_ui_interrupteur_get_bitmask(OwlControl* sel) {
    OwlInterrupteur* w = as_interrupteur(sel);
    return w ? w->getBitmask() : 0;
}
extern "C" void owl_ui_interrupteur_set_bitmask(OwlControl* sel, int64_t mask) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (w) w->setBitmask(mask);
}

extern "C" void owl_ui_interrupteur_on_change(OwlControl* sel, OwlCallback cb, void* d) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (!w) return;
    w->m_cbChange = cb; w->m_dChange = d;
}
extern "C" void owl_ui_interrupteur_on_entry(OwlControl* sel, OwlCallback cb, void* d) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (!w) return;
    w->m_cbEntry = cb; w->m_dEntry = d;
}
extern "C" void owl_ui_interrupteur_on_exit(OwlControl* sel, OwlCallback cb, void* d) {
    OwlInterrupteur* w = as_interrupteur(sel);
    if (!w) return;
    w->m_cbExit = cb; w->m_dExit = d;
}

/*  — style typé applique en UNE feuille QSS sur le QFrame. Le conteneur
 * (CadreExtérieur) est scope via #objectName pour ne pas cascader aux cases ;
 * TexteOptions cible QCheckBox ; CadreCoche cible QCheckBox::indicator ; Coche
 * (marque) approche le fond de ::indicator:checked. */
extern "C" void owl_ui_interrupteur_set_style(OwlControl* sel,
                                              const OwlInterrupteurStyleSpec* s) {
    OwlInterrupteur* it = as_interrupteur(sel);
    if (!it || !s) return;

    /* Format espace 'rgb(R, G, B)' — lisible et inspectable via __DebugFeuilleStyle
     * (symetrique de owl_ui_control_set_state_style).  — alpha < 255 =>
     * 'rgba(R, G, B, A)' (même format espacé) ; alpha=255 reste byte-identique. */
    auto rgb_css = [](int v, int alpha = 255) {
        int r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
        if (alpha >= 255)
            return QString("rgb(%1, %2, %3)").arg(r).arg(g).arg(b);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(r).arg(g).arg(b).arg(alpha / 255.0, 0, 'g', 4);
    };
    /* Bordure shorthand depuis un OwlCadreSpec (scalaires : couleur/epaisseur/
     * trait + arrondi). Suffisant pour le Style typé V1 (pas de per-bord ici). */
    auto border_css = [&](const OwlCadreSpec* c) -> QString {
        if (!c) return QString();
        QString b;
        /*  — la bordure n'est émise que si l'épaisseur > 0, mais
         * l'arrondi est indépendant : un CadreExtérieur { Epaisseur=0, Arrondi>0 }
         * doit produire des coins ronds sans liseré (aligné owl_table_border_css). */
        if (c->thickness > 0)
            b += QString("border: %1px %2 %3; ")
                     .arg(c->thickness)
                     .arg(cadre_line_style_to_css(c->line_style))
                     .arg(rgb_css(c->color));
        if (c->corner_radius > 0)
            b += QString("border-radius: %1px; ").arg(c->corner_radius);
        return b;
    };

    /* Le conteneur doit porter un objectName pour le scope #id (sinon le
     * background/border du CadreExtérieur cascaderait sur les QCheckBox). */
    if (it->objectName().isEmpty())
        it->setObjectName(QString("owl_int_%1").arg((quintptr)it, 0, 16));
    QString oname = it->objectName();

    QString css;

    /* --- CadreExtérieur : fond + bordure du conteneur (#id-scope). --- */
    {
        QString body;
        if (s->ext_bg >= 0) body += "background-color: " + rgb_css(s->ext_bg, s->ext_bg_alpha) + "; ";
        body += border_css(s->ext_border);
        if (!body.isEmpty())
            css += "QFrame#" + oname + " { " + body + "} ";
    }

    /* --- TexteOptions : couleur + police du texte des cases. --- */
    {
        QString body;
        if (s->txt_fg > 0) body += "color: " + rgb_css(s->txt_fg, s->txt_fg_alpha) + "; ";
        if (s->txt_bg >= 0) body += "background-color: " + rgb_css(s->txt_bg, s->txt_bg_alpha) + "; ";
        if (s->txt_font_family && *s->txt_font_family)
            body += QString("font-family: \"%1\"; ").arg(QString::fromUtf8(s->txt_font_family));
        if (s->txt_font_size > 0)
            body += QString("font-size: %1pt; ").arg(s->txt_font_size);
        if (s->txt_bold == 1)      body += "font-weight: bold; ";
        else if (s->txt_bold == 0) body += "font-weight: normal; ";
        if (s->txt_italic == 1)      body += "font-style: italic; ";
        else if (s->txt_italic == 0) body += "font-style: normal; ";
        if (!body.isEmpty())
            css += "QCheckBox { " + body + "} ";
    }

    /* --- CadreCoche : fond + bordure de la boite de coche (::indicator). --- */
    {
        QString body;
        if (s->coche_bg >= 0) body += "background-color: " + rgb_css(s->coche_bg, s->coche_bg_alpha) + "; ";
        body += border_css(s->coche_border);
        if (!body.isEmpty())
            css += "QCheckBox::indicator { " + body + "} ";
    }

    /* --- Coche : couleur de la marque (approche : fond de l'indicator coché).
     * Qt ne stylise pas le glyphe de coche seul (image thémée) ; colorer le fond
     * de ::indicator:checked donne une marque visible a la couleur voulue. --- */
    if (s->coche_mark > 0) {
        css += "QCheckBox::indicator:checked { background-color: "
               + rgb_css(s->coche_mark, s->coche_mark_alpha) + "; } ";
    }

    it->setStyleSheet(css);
    it->refitAfterStyle();
}

/* ============================================================
 * Champ ZoneRepetee (Looper) — V1
 *
 * Layout :
 *   OwlZoneRepetee (QFrame)                  <- top-level widget
 *     QVBoxLayout (margins=0)
 *       QScrollArea
 *         QWidget m_content                  <- viewport content
 *           QVBoxLayout                      <- Sens vertical (unique)
 *             [rep1 QWidget][rep2 QWidget]…  <- chaque rep
 *
 * Selection : QPushButton-free, on tracke un currentRow et on emet le
 * callback row_select via mousePressEvent installe sur chaque rep widget
 * (eventFilter centralise).
 * ============================================================ */

#include <QScrollArea>
#include <QMouseEvent>

/*  — premier descendant focusable (profondeur) d'un sous-arbre, sinon
 * nullptr. Sert a poser le focus sur un controle interne reel plutot que sur un
 * conteneur inerte. */
static QWidget* owl_first_focusable(QWidget* root) {
    if (!root) return nullptr;
    const QList<QWidget*> kids = root->findChildren<QWidget*>();
    for (QWidget* w : kids) {
        if (w && w->isEnabled() && w->focusPolicy() != Qt::NoFocus)
            return w;
    }
    return nullptr;
}

class OwlZoneRepetee : public QFrame {
public:
    explicit OwlZoneRepetee(QWidget* parent = nullptr) : QFrame(parent) {
        m_outer = new QVBoxLayout(this);
        m_outer->setContentsMargins(0, 0, 0, 0);
        m_outer->setSpacing(0);
        m_scroll = new QScrollArea(this);
        m_scroll->setWidgetResizable(true);
        m_outer->addWidget(m_scroll);
        m_content = new QWidget(m_scroll);
        m_content->setObjectName(QString("owlZrContent_%1").arg((quintptr)m_content));
        m_inner = new QVBoxLayout(m_content);
        m_inner->setContentsMargins(2, 2, 2, 2);
        m_inner->setSpacing(0);
        m_inner->addStretch(1);  /* pousse les reps en haut */
        m_scroll->setWidget(m_content);
    }

    OwlControl* asCtrl() { return reinterpret_cast<OwlControl*>(this); }
    QWidget*    contentWidget() const { return m_content; }
    int         currentRow() const { return m_currentRow; }

    void setMultiSelect(bool m) { m_multi = m; }
    bool isMultiSelect() const  { return m_multi; }

    /* Installe l'eventFilter sur le rep + tous ses descendants. Sans ca,
     * un clic sur un sous-widget (Saisie, Bouton) ne remonte pas au rep
     * et la selection ne change pas. La recursion couvre tout le sous-arbre. */
    void installFilterRecursive(QWidget* w) {
        if (!w) return;
        w->installEventFilter(this);
        for (QObject* child : w->children()) {
            QWidget* cw = qobject_cast<QWidget*>(child);
            if (cw) installFilterRecursive(cw);
        }
    }
    int addRep(QWidget* rep) {
        if (!rep) return 0;
        rep->setParent(m_content);
        installFilterRecursive(rep);
        /* insere AVANT le stretch final. */
        int beforeStretch = m_inner->count() - 1;
        if (beforeStretch < 0) beforeStretch = 0;
        m_inner->insertWidget(beforeStretch, rep);
        m_reps.append(rep);
        rep->show();
        /* Style la nouvelle rep (fond parite/selection/survol + CadreInterne +
         * Séparateur). La parite des suivantes ne change pas a l'ajout en fin. */
        styleOneRep(rep, m_reps.size());
        return m_reps.size();
    }
    int insertRep(int idx_1based, QWidget* rep) {
        if (!rep) return 0;
        if (idx_1based < 1 || idx_1based > m_reps.size() + 1) return addRep(rep);
        rep->setParent(m_content);
        installFilterRecursive(rep);
        m_inner->insertWidget(idx_1based - 1, rep);
        m_reps.insert(idx_1based - 1, rep);
        rep->show();
        /* Insertion en milieu de liste : la parite de toutes les reps qui
         * suivent bascule -> re-style l'ensemble. */
        restyleReps();
        return idx_1based;
    }
    void removeRep(int idx_1based) {
        if (idx_1based < 1 || idx_1based > m_reps.size()) return;
        QWidget* rep = m_reps[idx_1based - 1];
        m_reps.removeAt(idx_1based - 1);
        m_inner->removeWidget(rep);
        rep->deleteLater();
        if (m_currentRow == idx_1based) m_currentRow = 0;
        else if (m_currentRow > idx_1based) m_currentRow--;
        /* Parite decalee -> re-style. */
        restyleReps();
    }
    void clearReps() {
        for (QWidget* w : m_reps) {
            m_inner->removeWidget(w);
            w->deleteLater();
        }
        m_reps.clear();
        m_currentRow = 0;
    }
    int countReps() const { return m_reps.size(); }
    QWidget* getRep(int idx_1based) const {
        if (idx_1based < 1 || idx_1based > m_reps.size()) return nullptr;
        return m_reps[idx_1based - 1];
    }

    /*  — cible du focus pour le raccourci : un controle focusable de la
     * rep courante (sinon la premiere rep), a defaut la QScrollArea (fleches ->
     * defilement des occurrences). Repli sur le conteneur si rien de focusable. */
    QWidget* focusTarget() {
        QWidget* rep = nullptr;
        if (m_currentRow >= 1 && m_currentRow <= m_reps.size())
            rep = m_reps[m_currentRow - 1];
        else if (!m_reps.isEmpty())
            rep = m_reps.first();
        if (rep) {
            QWidget* f = owl_first_focusable(rep);
            if (f) return f;
        }
        if (m_scroll) return m_scroll;
        return this;
    }

    void setSelected(int idx_1based) {
        if (idx_1based < 0 || idx_1based > m_reps.size()) return;
        if (m_currentRow == idx_1based) return;
        m_currentRow = idx_1based;
        restyleReps();   /* la selection change -> re-peint les fonds. */
        if (m_cbRowSelect) m_cbRowSelect(asCtrl(), m_dRowSelect, idx_1based, 0);
    }

    /*  — memorise la spec de style et re-style toutes les reps.
     *  — OwlZrStyleSpec porte des pointeurs OwlCadreSpec* (ext/inner)
     * que le caller alloue sur SA pile (stdlib_ui.c : extBorder/innerBorder
     * locaux). Une copie plate `m_style = s` recopierait ces pointeurs, qui
     * pendouillent des le retour du caller. Le restyle initial (synchrone ici)
     * lisait donc du bon, mais le restyle differe (showEvent -> singleShot(0),
     * apres le retour du caller) relisait une pile ecrasee -> CadreInterne a
     * 119px et fond uniforme a l'ouverture. On COPIE les OwlCadreSpec dans un
     * stockage possede par la ZR et on re-pointe la spec dessus. */
    void setStyleSpec(const OwlZrStyleSpec& s) {
        m_style = s;
        if (s.ext_border) { m_extBorder = *s.ext_border; m_style.ext_border = &m_extBorder; }
        else m_style.ext_border = nullptr;
        if (s.inner_border) { m_innerBorder = *s.inner_border; m_style.inner_border = &m_innerBorder; }
        else m_style.inner_border = nullptr;
        m_hasStyle = true;
        applyContentBackdrop();
        restyleReps();
    }

    /*  — les reps a fond translucide (`rgba(...)`) doivent se compositer
     * sur le CadreExtérieur (fond du widget). Or les reps sont enfantes de
     * m_content (QWidget du QScrollArea), dont le fond par defaut (palette Base,
     * gris) s'interpose entre le QFrame rouge et la rep : la rep se melange donc
     * au gris, pas au CadreExtérieur. On peint m_content avec ext_bg (opaque)
     * pour offrir le bon backdrop de compositing. Scope #objectName pour ne pas
     * cascader aux reps. */
    void applyContentBackdrop() {
        if (!m_content) return;
        QString css;
        if (m_hasStyle && m_style.ext_bg >= 0)
            css = "#" + m_content->objectName() + " { background-color: "
                  + owl_table_rgb_css(m_style.ext_bg) + "; } ";
        m_content->setStyleSheet(css);
    }

    /* Applique le style a UNE rep selon son index 1-based. Cascade fond :
     * survol > selection > parite. Un seul fond gagne (pas de superposition).
     * CadreInterne = bordure de la rep ; Séparateur = trait bas. */
    void styleOneRep(QWidget* rep, int idx_1based) {
        if (!rep || !m_hasStyle) return;
        const OwlZrStyleSpec& s = m_style;
        bool selected = (idx_1based == m_currentRow && m_currentRow > 0);

        /* Cascade fond (survol non exprimable en QWidget statique -> :hover QSS).
         * Ici on choisit le fond de repos : selection > parite. */
        int bg = -1, fg = -1, bgAlpha = 255, fgAlpha = 255;
        if (selected) {
            bg = s.selection_bg; bgAlpha = s.selection_bg_alpha;
            fg = s.selection_fg; fgAlpha = s.selection_fg_alpha;
        } else {
            /* Parite 1-based coherente avec la selection : rep #1 = impaire. */
            bool odd = (idx_1based % 2 == 1);
            bg      = odd ? s.impaires_bg       : s.paires_bg;
            bgAlpha = odd ? s.impaires_bg_alpha : s.paires_bg_alpha;
        }

        /* Nom d'objet stable pour scoper le QSS a la rep (pas aux enfants). */
        if (rep->objectName().isEmpty())
            rep->setObjectName(QString("owlZrRep_%1").arg((quintptr)rep));
        QString sel = "#" + rep->objectName();

        QString body;
        if (bg >= 0) body += "background-color: " + owl_table_rgb_css_a(bg, bgAlpha) + "; ";
        if (fg >= 0) body += "color: " + owl_table_rgb_css_a(fg, fgAlpha) + "; ";
        if (s.inner_bg >= 0 && bg < 0)
            body += "background-color: " + owl_table_rgb_css_a(s.inner_bg, s.inner_bg_alpha) + "; ";
        body += owl_table_border_css(s.inner_border);
        /* Séparateur : trait bas entre reps (n'ecrase pas un CadreInterne pose). */
        if (s.separateur >= 0 && (!s.inner_border || s.inner_border->thickness <= 0)) {
            body += "border-bottom: 1px solid "
                    + owl_table_rgb_css_a(s.separateur, s.separateur_alpha) + "; ";
            /*  — un séparateur translucide se melange au fond du CONTENEUR
             * (backdrop CadreExtérieur), pas au fond de la rep : clippe le fond de
             * la rep a la padding-box pour qu'il ne passe pas sous la bordure, la
             * bordure translucide se composite alors sur le backdrop. Opaque =>
             * pas de clip (rendu byte-identique). */
            if (s.separateur_alpha < 255)
                body += "background-clip: padding; ";
        }

        QString css;
        if (!body.isEmpty()) css += sel + " { " + body + "} ";
        /* Survol : QSS :hover sur la rep (le fond survol prime a l'affichage). */
        if (s.survol_bg >= 0 || s.survol_fg >= 0) {
            QString hb;
            if (s.survol_bg >= 0) hb += "background-color: " + owl_table_rgb_css_a(s.survol_bg, s.survol_bg_alpha) + "; ";
            if (s.survol_fg >= 0) hb += "color: " + owl_table_rgb_css_a(s.survol_fg, s.survol_fg_alpha) + "; ";
            css += sel + ":hover { " + hb + "} ";
        }
        rep->setStyleSheet(css);
        /* Force le moteur de style a re-evaluer immediatement la feuille posee.
         * Sans ca, un QSS scope par objectName pose avant le polish initial du
         * sous-arbre (cas de l'ouverture) n'est honore qu'au cycle d'evenement
         * suivant. unpolish+polish rend le fond par-rep visible sans attendre. */
        if (QStyle* st = rep->style()) {
            st->unpolish(rep);
            st->polish(rep);
        }
        /*  — le polish recalcule le style mais ne PLANIFIE pas de repaint.
         * A l'ouverture, styleOneRep est appele (via showEvent -> singleShot 0)
         * AVANT le premier paintEvent d'exposition de la fenetre : le fond par-rep
         * n'apparaissait qu'apres un clic (qui, lui, provoque un vrai repaint).
         * update() marque la rep dirty : le prochain paintEvent (l'exposition
         * elle-meme) la repeint deja avec le fond parite/selection a jour. */
        rep->update();
    }

    void restyleReps() {
        if (!m_hasStyle) return;
        for (int i = 0; i < m_reps.size(); i++)
            styleOneRep(m_reps[i], i + 1);
    }

    OwlEventCb2i m_cbRowSelect = nullptr; void* m_dRowSelect = nullptr;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::MouseButtonPress ||
            ev->type() == QEvent::FocusIn) {
            QWidget* w = qobject_cast<QWidget*>(obj);
            /* Walk up parents until on tombe sur un widget direct de m_content
             * (= un rep). Permet a un clic sur un sous-widget (Saisie/Bouton)
             * de selectionner la rep parente. */
            while (w && w != m_content) {
                int idx = m_reps.indexOf(w);
                if (idx >= 0) { setSelected(idx + 1); break; }
                w = w->parentWidget();
            }
        }
        return QFrame::eventFilter(obj, ev);
    }
    /*  — Re-style toutes les reps au PREMIER show. A l'ouverture, les
     * reps sont ensemencees (addRep -> styleOneRep) AVANT que la fenetre soit
     * affichee : leur feuille QSS est posee alors que le sous-arbre n'a pas
     * encore ete "poli" par le moteur de style de Qt. Le rendu initial ignore
     * donc les fonds par-rep (parite/selection) — ils n'apparaissaient qu'apres
     * le premier evenement declenchant restyleReps (focus/clic), qui re-posait
     * les feuilles et forcait un re-polish. Un restyle SYNCHRONE dans showEvent
     * ne suffit pas : Qt n'a pas encore fini de polir les reps enfants a cet
     * instant, donc le QSS par-rep reste ignore jusqu'au cycle suivant. On
     * differe donc le restyle d'un tick d'event-loop (singleShot(0)) : le lambda
     * s'execute apres le polish initial du sous-arbre, les fonds par-rep sont
     * alors honores immediatement — rendu d'ouverture == rendu apres focus,
     * sans clic. QPointer garde contre un teardown de la ZR avant le tick. */
    void showEvent(QShowEvent* e) override {
        QFrame::showEvent(e);
        if (!m_firstShownRestyled) {
            m_firstShownRestyled = true;
            QPointer<OwlZoneRepetee> self(this);
            QTimer::singleShot(0, this, [self]() {
                if (self) self->restyleReps();
            });
        }
    }
private:
    QVBoxLayout* m_outer = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget*     m_content = nullptr;
    QBoxLayout*  m_inner = nullptr;
    QList<QWidget*> m_reps;
    bool m_multi = false;
    int  m_currentRow = 0;
    OwlZrStyleSpec m_style = {};
    OwlCadreSpec m_extBorder = {};    /*  — copies possedees : la spec */
    OwlCadreSpec m_innerBorder = {};  /* pointe dessus, pas sur la pile caller. */
    bool m_hasStyle = false;
    bool m_firstShownRestyled = false;   /*  — re-polish unique au 1er show */
};

/* ============================================================
 * Champ Jauge — QProgressBar + rendu symetrique custom.
 * ============================================================ */

/* Dessine le texte d'une jauge, pivote -90deg quand elle est verticale (texte
 * lisible de bas en haut, aligne sur le rendu du designer). En horizontal :
 * drawText classique. Le pen/font doivent etre poses par l'appelant. */
static void owl_jauge_draw_text(QPainter& pnt, const QRect& r,
                                const QString& txt, bool vertical,
                                Qt::Alignment hAlign) {
    if (!vertical) {
        pnt.drawText(r, hAlign | Qt::AlignVCenter, txt);
        return;
    }
    /* Vertical : texte pivote -90deg. Le cadrage horizontal ne concerne que le
     * sens de lecture horizontal -> on garde le texte centre dans la barre. */
    pnt.save();
    pnt.translate(r.center());
    pnt.rotate(-90.0);
    /* Apres rotation, largeur/hauteur du rect de texte sont echangees. */
    QRect rr(-r.height() / 2, -r.width() / 2, r.height(), r.width());
    pnt.drawText(rr, Qt::AlignCenter, txt);
    pnt.restore();
}

/* Sous-classe QProgressBar : ajoute le mode symetrique (progression depuis le
 * centre) et une couleur de remplissage personnalisee. Pas de Q_OBJECT (juste
 * un paintEvent override) -> pas de moc, cf OwlImageLabel/OwlListWidget. */
class OwlJauge : public QProgressBar {
public:
    explicit OwlJauge(QWidget* parent) : QProgressBar(parent) {}
    bool m_symmetric = false;
    int  m_progressColor = -1;   /* Remplissage : BGR canon, -1 = defaut theme */
    int  m_progressColorAlpha = 255; /*  — alpha du remplissage, 255 = opaque */
    int  m_grooveColor   = -1;   /* CadreExtérieur..CouleurFond (piste), -1 = defaut */
    int  m_grooveColorAlpha = 255;   /*  — alpha de la piste, 255 = opaque */
    int  m_textColor     = -1;   /* Libellé..Couleur (texte %), -1 = defaut theme */
    int  m_textColorAlpha = 255; /*  — alpha du texte %, 255 = opaque */
    /* CadrageHorizontal du texte %. Defaut = centre (historique).
     * Le texte est peint par le paintEvent -> alignement porte ici. */
    Qt::Alignment m_textHAlign = Qt::AlignHCenter;
    OwlCadreSpec m_border{};
    bool m_hasBorder = false;    /* CadreExtérieur..Cadre visible */
    /* Libellé..Police ( lot4) : surcharges appliquees au texte % dans
     * paintEvent. Sentinelles alignees sur OwlJaugeStyleSpec : famille vide /
     * taille 0 = non pose, drapeaux -1 = non pose. */
    QString m_labelFontFamily;
    int m_labelFontSize = 0;
    int m_labelBold      = -1;
    int m_labelItalic    = -1;
    int m_labelUnderline = -1;
    int m_labelStrike    = -1;

    /* Construit la QFont du texte % : part de la police courante du widget et
     * n'applique que les surcharges posees (Libellé..Police). */
    QFont labelFont() const {
        QFont f = font();
        if (!m_labelFontFamily.isEmpty()) f.setFamily(m_labelFontFamily);
        if (m_labelFontSize > 0)  f.setPointSize(m_labelFontSize);
        if (m_labelBold >= 0)     f.setBold(m_labelBold == 1);
        if (m_labelItalic >= 0)   f.setItalic(m_labelItalic == 1);
        if (m_labelUnderline >= 0) f.setUnderline(m_labelUnderline == 1);
        if (m_labelStrike >= 0)    f.setStrikeOut(m_labelStrike == 1);
        return f;
    }

    /* Pen du texte % : couleur explicite (avec son alpha) ou palette Text.
     *  — alpha=255 rend un QColor opaque byte-identique. */
    QColor labelPenColor() const {
        if (m_textColor < 0) return palette().color(QPalette::Text);
        QColor c = owl_table_bgr_to_qcolor(m_textColor);
        c.setAlpha(m_textColorAlpha);
        return c;
    }

    /* Ré-applique le style HYBRIDE. Le rendu de la Jauge lit trois
     * canaux distincts, pas tous pilotables en QSS :
     *   - piste + bordure : QSS `QProgressBar {}` (mode classique) ET
     *     QPalette::Base (mode symetrique, peint la piste hors QSS).
     *   - remplissage : QSS `QProgressBar::chunk {}` (classique) ET
     *     m_progressColor (mode symetrique, chunk custom).
     *   - texte % : m_textColor lu directement par le paintEvent pour le pen.
     *     Poser un QSS (piste/chunk) bascule Qt sur QStyleSheetStyle qui ecrase
     *     la QPalette::Text effective ; le membre bypasse ce canal. */
    void applyStyle() {
        QPalette pal = palette();
        if (m_grooveColor >= 0) {
            QColor gc = owl_table_bgr_to_qcolor(m_grooveColor);
            gc.setAlpha(m_grooveColorAlpha);   /*  — piste translucide */
            pal.setColor(QPalette::Base, gc);
        }
        setPalette(pal);

        /* Mode symetrique : piste + chunk + texte peints dans paintEvent -> pas
         * de QSS (elle capturerait le rendu du groove classique). */
        if (m_symmetric) { setStyleSheet(QString()); return; }

        QString qss;
        if (m_grooveColor >= 0 || m_hasBorder) {
            QString body;
            if (m_grooveColor >= 0)
                body += "background-color: "
                        + owl_table_rgb_css_a(m_grooveColor, m_grooveColorAlpha) + "; ";
            if (m_hasBorder)
                body += owl_table_border_css(&m_border);
            qss += "QProgressBar { " + body + "} ";
        }
        if (m_progressColor >= 0) {
            /*  — chunk alpha-aware : un remplissage translucide se compose
             * sur la piste (background QProgressBar) déjà peinte. Opaque => #rrggbb
             * byte-identique à l'émetteur d'origine. */
            QColor c = owl_table_bgr_to_qcolor(m_progressColor);
            QString col = (m_progressColorAlpha >= 255)
                ? c.name()
                : owl_table_rgb_css_a(m_progressColor, m_progressColorAlpha);
            qss += QString("QProgressBar::chunk { background-color: %1; }").arg(col);
        }
        setStyleSheet(qss);
    }

protected:
    void paintEvent(QPaintEvent* ev) override {
        if (!m_symmetric) {
            /* Texte visible : on ne laisse jamais le style peindre le texte
             * lui-meme. En vertical, Fusion l'ecrirait a l'horizontale ; en
             * horizontal, la QSS ::chunk posee par CouleurJauge bascule sur
             * QStyleSheetStyle qui l'aligne a GAUCHE. Le designer le centre
             * dans les deux cas. On peint groove+chunk via le style SANS son
             * texte (opt.textVisible = false) -> la couleur du chunk (QSS ou
             * Fusion) est preservee, puis on sur-dessine le texte centre
             * (pivote -90deg si vertical). Perte assumee : le rendu bicolore
             * Fusion (texte clair sur le chunk) devient unicolore centre, ce
             * qui matche le texte unicolore centre du designer. */
            if (isTextVisible()) {
                bool vert = (orientation() == Qt::Vertical);
                QPainter pnt(this);
                QStyleOptionProgressBar opt;
                initStyleOption(&opt);
                opt.textVisible = false;
                style()->drawControl(QStyle::CE_ProgressBar, &opt, &pnt, this);
                pnt.setPen(labelPenColor());
                pnt.setFont(labelFont());
                owl_jauge_draw_text(pnt, rect(), text(), vert, m_textHAlign);
                return;
            }
            QProgressBar::paintEvent(ev);
            return;
        }
        QPainter pnt(this);
        QRect r = rect();
        pnt.fillRect(r, palette().color(QPalette::Base));
        int lo = minimum(), hi = maximum(), v = value();
        double span = (hi > lo) ? double(hi - lo) : 1.0;
        double mid  = (double(lo) + double(hi)) / 2.0;
        double frac = (double(v) - mid) / span;   /* -0.5 .. +0.5 */
        QColor chunk;
        if (m_progressColor >= 0) {
            chunk = owl_table_bgr_to_qcolor(m_progressColor);
            chunk.setAlpha(m_progressColorAlpha);   /*  */
        } else {
            chunk = palette().color(QPalette::Highlight);
        }
        if (orientation() == Qt::Vertical) {
            int cy = r.center().y();
            int fill = int(frac * r.height());
            QRect fr = (fill >= 0) ? QRect(r.left(), cy - fill, r.width(), fill)
                                   : QRect(r.left(), cy, r.width(), -fill);
            pnt.fillRect(fr, chunk);
        } else {
            int cx = r.center().x();
            int fill = int(frac * r.width());
            QRect fr = (fill >= 0) ? QRect(cx, r.top(), fill, r.height())
                                   : QRect(cx + fill, r.top(), -fill, r.height());
            pnt.fillRect(fr, chunk);
        }
        if (isTextVisible()) {
            pnt.setPen(labelPenColor());
            pnt.setFont(labelFont());
            owl_jauge_draw_text(pnt, r, text(), orientation() == Qt::Vertical,
                                m_textHAlign);
        }
        (void)ev;
    }
};

static OwlJauge* as_jauge(OwlControl* g) {
    return g ? dynamic_cast<OwlJauge*>(reinterpret_cast<QWidget*>(g)) : nullptr;
}

extern "C" OwlControl* owl_ui_jauge_create(OwlWindow* parent, int x, int y,
                                            int w, int h) {
    if (!parent) return nullptr;
    OwlJauge* g = new OwlJauge(central_of(parent));
    g->setRange(0, 100);
    g->setValue(0);
    g->setTextVisible(true);
    if (x >= 0 && y >= 0) g->setGeometry(x, y, w, h);
    else                  g->resize(w > 0 ? w : 150, h > 0 ? h : 24);
    g->show();
    return reinterpret_cast<OwlControl*>(g);
}

extern "C" void owl_ui_jauge_set_range(OwlControl* g, int min_val, int max_val) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    if (max_val < min_val) max_val = min_val;
    j->setRange(min_val, max_val);
}
extern "C" int owl_ui_jauge_get_min(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return j ? j->minimum() : 0;
}
extern "C" int owl_ui_jauge_get_max(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return j ? j->maximum() : 0;
}
extern "C" void owl_ui_jauge_set_value(OwlControl* g, int value) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    /* QProgressBar::setValue IGNORE une valeur hors plage (pas de clamp natif).
     * On borne explicitement a [min, max] pour la semantique Jauge. */
    if (value < j->minimum()) value = j->minimum();
    if (value > j->maximum()) value = j->maximum();
    j->setValue(value);
}
extern "C" int owl_ui_jauge_get_value(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return j ? j->value() : 0;
}
extern "C" void owl_ui_jauge_set_vertical(OwlControl* g, int vertical) {
    OwlJauge* j = as_jauge(g);
    if (j) j->setOrientation(vertical ? Qt::Vertical : Qt::Horizontal);
}
extern "C" int owl_ui_jauge_get_vertical(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return (j && j->orientation() == Qt::Vertical) ? 1 : 0;
}
extern "C" void owl_ui_jauge_set_h_align(OwlControl* g, int h) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    j->m_textHAlign = owl_h_to_qt(h);
    j->update();
}
extern "C" int owl_ui_jauge_get_h_align(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    if (!j) return OWL_CH_CENTRE;
    return qt_h_to_owl(j->m_textHAlign);
}
extern "C" void owl_ui_jauge_set_symmetric(OwlControl* g, int symmetric) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    j->m_symmetric = (symmetric != 0);
    j->applyStyle();   /* bascule QSS <-> paint custom */
    j->update();
}
extern "C" int owl_ui_jauge_get_symmetric(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return (j && j->m_symmetric) ? 1 : 0;
}
extern "C" void owl_ui_jauge_set_progress_color(OwlControl* g, int bgr) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    j->m_progressColor = bgr;
    j->applyStyle();
    j->update();
}
extern "C" int owl_ui_jauge_get_progress_color(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    return j ? j->m_progressColor : -1;
}
extern "C" void owl_ui_jauge_set_style(OwlControl* g, const OwlJaugeStyleSpec* s) {
    OwlJauge* j = as_jauge(g);
    if (!j || !s) return;
    j->m_grooveColor = s->ext_bg;
    j->m_grooveColorAlpha = s->ext_bg_alpha;   /*  */
    j->m_textColor   = s->label_fg;
    j->m_textColorAlpha = s->label_fg_alpha;   /*  */
    /* Remplissage ecrit dans m_progressColor : meme membre que le raccourci
     * ..CouleurJauge -> equivalence pixel garantie. */
    if (s->fill >= 0) { j->m_progressColor = s->fill; j->m_progressColorAlpha = s->fill_alpha; }
    if (s->ext_border) { j->m_border = *s->ext_border; j->m_hasBorder = true; }
    else               { j->m_hasBorder = false; }
    /* Libellé..Police ( lot4) : surcharges du texte %. */
    j->m_labelFontFamily = (s->label_font_family && *s->label_font_family)
                               ? QString::fromUtf8(s->label_font_family) : QString();
    j->m_labelFontSize = s->label_font_size;
    j->m_labelBold      = s->label_bold;
    j->m_labelItalic    = s->label_italic;
    j->m_labelUnderline = s->label_underline;
    j->m_labelStrike    = s->label_strike;
    j->applyStyle();
    j->update();
}
extern "C" void owl_ui_jauge_set_text(OwlControl* g, const char* text_utf8) {
    OwlJauge* j = as_jauge(g);
    if (!j) return;
    /* Trois cas canon :
     *   - NULL (jamais posé / réinitialisation) -> pourcentage automatique %p%.
     *     Le chemin d'ingestion ne pose PAS la propriété quand elle est absente
     *     (garde `if (txtJauge)` init, prop_str -> NULL) : NULL = non posé.
     *   - "" (chaîne vide explicite) -> texte masqué (textVisible=false), rien
     *     n'est peint. C'est le cœur du fix : distinguer non-posé de posé-vide.
     *   - "xxx" -> texte personnalisé. */
    if (!text_utf8) {
        j->setFormat(QStringLiteral("%p%"));
        j->setTextVisible(true);
    } else if (*text_utf8) {
        j->setFormat(QString::fromUtf8(text_utf8));
        j->setTextVisible(true);
    } else {
        j->setTextVisible(false);   /* "" -> masqué */
    }
    j->update();
}
extern "C" const char* owl_ui_jauge_get_text(OwlControl* g) {
    OwlJauge* j = as_jauge(g);
    if (!j) return "";
    /* Buffer statique thread-local : le C-side copie immediatement. */
    static thread_local QByteArray s_buf;
    /* Texte masqué ("" explicite) : round-trip la chaîne vide, pas le format
     * résiduel. */
    s_buf = j->isTextVisible() ? j->format().toUtf8() : QByteArray();
    return s_buf.constData();
}

static OwlZoneRepetee* as_zr(OwlControl* c) {
    return c ? dynamic_cast<OwlZoneRepetee*>(reinterpret_cast<QWidget*>(c)) : nullptr;
}

/*  — voir la declaration anticipee pres de owl_ui_set_focus_shortcut. */
static QWidget* owl_focus_target_for(QWidget* w) {
    if (!w) return w;
    if (auto* s = dynamic_cast<OwlSelecteur*>(w))    return s->focusTarget();
    if (auto* i = dynamic_cast<OwlInterrupteur*>(w)) return i->focusTarget();
    if (auto* z = dynamic_cast<OwlZoneRepetee*>(w))  return z->focusTarget();
    return w;
}

extern "C" OwlControl* owl_ui_zr_create(OwlWindow* parent, int x, int y, int w, int h) {
    OwlZoneRepetee* zr = new OwlZoneRepetee(central_of(parent));
    if (w > 0 && h > 0) zr->setGeometry(x, y, w, h);
    zr->show();
    return zr->asCtrl();
}
extern "C" void owl_ui_zr_set_multi_select(OwlControl* c, int multi) {
    OwlZoneRepetee* z = as_zr(c); if (z) z->setMultiSelect(multi != 0);
}
extern "C" int owl_ui_zr_get_multi_select(OwlControl* c) {
    OwlZoneRepetee* z = as_zr(c); return z && z->isMultiSelect() ? 1 : 0;
}
extern "C" int owl_ui_zr_add_rep(OwlControl* c, OwlControl* rep) {
    OwlZoneRepetee* z = as_zr(c); QWidget* rw = reinterpret_cast<QWidget*>(rep);
    return z ? z->addRep(rw) : 0;
}
extern "C" int owl_ui_zr_insert_rep(OwlControl* c, int idx, OwlControl* rep) {
    OwlZoneRepetee* z = as_zr(c); QWidget* rw = reinterpret_cast<QWidget*>(rep);
    return z ? z->insertRep(idx, rw) : 0;
}
extern "C" void owl_ui_zr_remove_rep(OwlControl* c, int idx) {
    OwlZoneRepetee* z = as_zr(c); if (z) z->removeRep(idx);
}
extern "C" void owl_ui_zr_clear(OwlControl* c) {
    OwlZoneRepetee* z = as_zr(c); if (z) z->clearReps();
}
extern "C" int owl_ui_zr_count(OwlControl* c) {
    OwlZoneRepetee* z = as_zr(c); return z ? z->countReps() : 0;
}
extern "C" OwlControl* owl_ui_zr_get_rep(OwlControl* c, int idx) {
    OwlZoneRepetee* z = as_zr(c);
    return z ? reinterpret_cast<OwlControl*>(z->getRep(idx)) : nullptr;
}
extern "C" OwlWindow* owl_ui_zr_rep_as_parent(OwlControl* rep) {
    /* Le rep est un QWidget direct (typiquement un OwlContainer). */
    return reinterpret_cast<OwlWindow*>(rep);
}
extern "C" int owl_ui_zr_get_selected(OwlControl* c) {
    OwlZoneRepetee* z = as_zr(c); return z ? z->currentRow() : 0;
}
extern "C" void owl_ui_zr_set_selected(OwlControl* c, int idx) {
    OwlZoneRepetee* z = as_zr(c); if (z) z->setSelected(idx);
}
extern "C" void owl_ui_zr_on_row_select(OwlControl* c, OwlEventCb2i cb, void* d) {
    OwlZoneRepetee* z = as_zr(c);
    if (!z) return;
    z->m_cbRowSelect = cb; z->m_dRowSelect = d;
}

extern "C" void owl_ui_zr_set_style(OwlControl* c, const OwlZrStyleSpec* s) {
    OwlZoneRepetee* z = as_zr(c);
    if (!z || !s) return;

    /* CadreExtérieur : fond + bordure du widget entier (QFrame). Scope
     * #objectName pour ne pas cascader le QSS aux reps enfantes. */
    QFrame* frame = static_cast<QFrame*>(reinterpret_cast<QWidget*>(c));
    if (frame->objectName().isEmpty())
        frame->setObjectName(QString("owlZr_%1").arg((quintptr)frame));
    QString body;
    if (s->ext_bg >= 0)
        body += "background-color: " + owl_table_rgb_css_a(s->ext_bg, s->ext_bg_alpha) + "; ";
    body += owl_table_border_css(s->ext_border);
    QString css;
    if (!body.isEmpty())
        css += "#" + frame->objectName() + " { " + body + "} ";
    frame->setStyleSheet(css);

    /* Memorise + re-style toutes les reps existantes (les futures reps sont
     * stylees a l'ajout via addRep -> styleOneRep). */
    z->setStyleSpec(*s);
}
/* ===== Helpers transverses pour ZR (et reutilisables ailleurs) ===== */

extern "C" void owl_ui_control_set_object_name(OwlControl* w, const char* name) {
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    if (qw && name) qw->setObjectName(QString::fromUtf8(name));
}

extern "C" const char* owl_ui_control_get_object_name(OwlControl* w) {
    static thread_local QByteArray buf;
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    if (!qw) return "";
    buf = qw->objectName().toUtf8();
    return buf.constData();
}

extern "C" void owl_ui_control_set_widget_kind(OwlControl* w, int kind) {
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    if (qw) qw->setProperty("__owl_kind__", kind);
}

extern "C" int owl_ui_control_get_widget_kind(OwlControl* w) {
    QWidget* qw = reinterpret_cast<QWidget*>(w);
    if (!qw) return -1;
    QVariant v = qw->property("__owl_kind__");
    return v.isValid() ? v.toInt() : -1;
}

/* Plancher irreductible d'un champ.
 *
 * QWidgetItem::minimumSize est LA mesure qu'un QBoxLayout / QGridLayout
 * interroge pour savoir jusqu'ou il a le droit de retrecir un enfant : elle
 * compose minimumSizeHint, un minimumSize pose explicitement et la politique
 * de taille du widget. Interroger la meme chose ici, c'est publier ce que le
 * moteur applique — pas une re-derivation qui pourrait diverger.
 *
 * ensurePolished d'abord : une feuille de style ne touche la police effective
 * du widget qu'au polish, et plusieurs planchers en derivent. */
extern "C" void owl_ui_control_min_floor(OwlControl* c, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    if (!w) return;
    w->ensurePolished();
    QWidgetItem item(w);
    const QSize m = item.minimumSize();
    if (out_w) *out_w = m.width();
    if (out_h) *out_h = m.height();
}

extern "C" int owl_ui_control_layout_spacing(OwlControl* c) {
    QWidget* w = reinterpret_cast<QWidget*>(c);
    if (!w || !w->layout()) return -1;
    return w->layout()->spacing();
}

/* Taille en points de la police effective d'un champ.
 *
 * C'est la police A LAQUELLE son plancher a ete mesure. L'editeur en a besoin
 * pour deriver la croissance d'un plancher qui suit sa police au-dela d'un
 * seuil (le chrome d'un Onglet, par exemple) : sans elle, il extrapole a partir
 * d'une reference inconnue.
 *
 * pointSize() rend -1 quand la police a ete definie en pixels ; on repasse alors
 * par QFontInfo qui resout la taille reellement appliquee. ensurePolished pour
 * que la feuille de style ait pose sa police avant la lecture. Rend -1 sur
 * widget nul. */
extern "C" int owl_ui_control_font_point_size(OwlControl* c) {
    QWidget* w = reinterpret_cast<QWidget*>(c);
    if (!w) return -1;
    w->ensurePolished();
    const QFont f = w->font();
    int pt = f.pointSize();
    if (pt <= 0) pt = QFontInfo(f).pointSize();
    return pt;
}

/* Plancher irreductible du meme champ, mais mesure APRES avoir gonfle sa police
 * a `point_size`.
 *
 * Sert a etablir, par la mesure et non par une liste ecrite a la main, si le
 * plancher d'une famille SUIT son contenu : on compare ce plancher-ci au
 * plancher a la police par defaut. Egaux => le plancher est insensible a la
 * police (Table, Liste, ZoneRepetee) ; differents => il la suit (Libelle,
 * Onglet, ...). setFont se propage aux enfants sans police propre — la bande de
 * languettes d'un Onglet grossit donc avec, ce qu'on veut justement detecter.
 * La police est restauree ensuite : ce temoin peut resservir. */
extern "C" void owl_ui_control_min_floor_at_font(OwlControl* c, int point_size,
                                                 int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    QWidget* w = reinterpret_cast<QWidget*>(c);
    if (!w) return;
    const QFont saved = w->font();
    QFont f = saved;
    f.setPointSize(point_size);
    w->setFont(f);
    w->ensurePolished();
    QWidgetItem item(w);
    const QSize m = item.minimumSize();
    if (out_w) *out_w = m.width();
    if (out_h) *out_h = m.height();
    w->setFont(saved);
    w->ensurePolished();
}

extern "C" OwlControl* owl_ui_control_find_child(OwlControl* parent, const char* name) {
    QWidget* qw = reinterpret_cast<QWidget*>(parent);
    if (!qw || !name) return nullptr;
    /* Cherche directement parmi les enfants directs ; si ZR rep contient un
     * Conteneur intermediaire, ce sera couvert par recursion via Qt::FindChildrenRecursively. */
    QWidget* found = qw->findChild<QWidget*>(QString::fromUtf8(name),
                                              Qt::FindChildrenRecursively);
    return reinterpret_cast<OwlControl*>(found);
}
