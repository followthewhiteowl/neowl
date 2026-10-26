// SPDX-License-Identifier: AGPL-3.0-only
#include "common.h"
#include "scanner.h"
#include "vm.h"
#include "owl_new.h"
#include "table.h"
#include "object.h"
#include "project.h"
#include "prescan.h"
#include "testing.h"
#include "formatter.h"
#include "compiler.h"
#include "memory.h"
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_limits.h"
#include "owl_errors.h"
#include "owl_operator_catalog.h"
#include "owl_globalvar_catalog.h"
#include "owl_methods.h"
#include "registry_self_test.h"
#include "owl_keyword_catalog.h"
#include "owl_categories.h"
#include "debug_stdio.h"
#include "ui_catalog.h"
#include "ui_presentation.h"
#include "ui_decl.h"
#ifdef OWL_HAS_GUI
#include "stdlib_ui.h"   /*  : planchers mesures pour --vscode-data */
#endif
#include "owl_event_catalog.h"
#include "cli_doc.h"
#include "embedded_docs.h"
#include "owl_serve.h"
#include "service_decl.h"
#include "http_service_runtime.h"
#include "http_openapi.h"
#include "startup_window.h"

#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  include <fcntl.h>          /* _O_BINARY (bascule de mode sur fd 1) */
#  define owl_getcwd(buf, sz)  _getcwd((buf), (sz))
#  define owl_chdir(path)      _chdir((path))
#else
#  include <unistd.h>
#  include <sys/select.h>
#  define owl_getcwd(buf, sz)  getcwd((buf), (sz))
#  define owl_chdir(path)      chdir((path))
#endif

/* ================================================================
 * LEXER TEST MODE
 * ================================================================ */

/* ================================================================
 * RUN PROJECT
 * ================================================================ */

/* Recherche récursive d'un fichier .owl dont le basename matche `name` ou
 * `name.owl`. Retourne le chemin alloué (free par le caller), ou NULL si
 * non trouvé. Si plusieurs matches, prend le PREMIER trouvé (DFS) — c'est
 * un raccourci CLI, on n'essaie pas d'être exhaustif. */
static char* find_owl_file_recursive(const char* root, const char* name) {
    DIR* dir = opendir(root);
    if (dir == NULL) return NULL;
    char* result = NULL;
    int nameLen = (int)strlen(name);
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* en = entry->d_name;
        if (en[0] == '.') continue;
        size_t rootLen = strlen(root);
        size_t enLen = strlen(en);
        char* fullPath = malloc(rootLen + 1 + enLen + 1);
        sprintf(fullPath, "%s/%s", root, en);
        if (is_directory(fullPath)) {
            result = find_owl_file_recursive(fullPath, name);
            free(fullPath);
            if (result != NULL) break;
        } else if (enLen > 4 && strcmp(en + enLen - 4, ".owl") == 0) {
            /* Match si basename == name OU basename == name.owl */
            bool match = false;
            if ((int)enLen - 4 == nameLen &&
                strncmp(en, name, (size_t)nameLen) == 0) {
                match = true; /* basename without .owl == name */
            } else if ((int)enLen == nameLen &&
                       strncmp(en, name, (size_t)nameLen) == 0) {
                match = true; /* user gave full filename with .owl */
            }
            if (match) {
                result = fullPath;
                break;
            }
            free(fullPath);
        } else {
            free(fullPath);
        }
    }
    closedir(dir);
    return result;
}

/* Extract directory part from a path. Returns "." if no directory. */
static char* extract_directory(const char* path) {
    /* Find last / or \ */
    const char* lastSep = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    if (lastSep == NULL) return safe_strdup(".");
    size_t len = lastSep - path;
    if (len == 0) return safe_strdup(".");
    char* dir = malloc(len + 1);
    if (dir == NULL) { fprintf(stderr, "owl: out of memory\n"); exit(74); }
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

/* Extract filename part from a path. */
static char* extract_filename(const char* path) {
    const char* lastSep = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    if (lastSep == NULL) return safe_strdup(path);
    return safe_strdup(lastSep + 1);
}

/*  — lance un projet/script scope.
 *  - entryFile != NULL : entree = ce fichier (nomme explicitement).
 *  - entryFile == NULL : entree = projet.owl (lancement projet) ; erreur si
 *    absent.
 *  projectMode : true => boot i18n depuis projet.owl + teardown Fermeture(). */
static int run_project_scoped(const char* dir, const char* entryFile,
                              bool recursive, bool projectMode) {
    Project project;
    project_init(&project);

    if (!project_discover_scoped(&project, dir, true, recursive)) {
        project_free(&project);
        return 65;
    }

    project.isProjectMode = projectMode;

    /*  — lancement PROJET : on se place dans le dossier du projet.owl
     * (frontiere de projet, cf. /) pour que les chemins relatifs
     * (fOuvre, fCharge, dChargeImage...) resolvent depuis le projet et non depuis
     * le CWD du terminal. `dir` est ici la racine absolue produite par
     * resolve_project_root (_fullpath/realpath). Garde sur projectMode : en mode
     * script, on conserve la semantique  (dossier courant du terminal) et
     * `dir` peut d'ailleurs etre relatif. owl test ne passe pas par ici. */
    if (projectMode) {
        if (owl_chdir(dir) != 0) {
            fprintf(stderr, "owl: impossible de se placer dans le dossier du "
                            "projet '%s'\n", dir);
            project_free(&project);
            return 74;
        }
    }

    if (entryFile != NULL) {
        project.entryIndex = project_find_file(&project, entryFile);
        if (project.entryIndex < 0) {
            fprintf(stderr, "owl: '%s' introuvable dans le projet\n", entryFile);
            project_free(&project);
            return 65;
        }
    } else {
        /* Lancement projet : l'entree est projet.owl (son top-level = le
         * programme). Marqueur bilingue FR/EN, casse-insensible. */
        project.entryIndex = -1;
        for (int i = 0; i < project.fileCount; i++) {
            if (is_projet_marker_name(project.files[i].name)) {
                project.entryIndex = i;
                break;
            }
        }
        if (project.entryIndex < 0) {
            fprintf(stderr,
                    "owl: aucun point d'entree : nomme un fichier "
                    "(owl <fichier>.owl) ou cree un projet.owl\n");
            project_free(&project);
            return 65;
        }
    }

    InterpretResult result = project_run(&project);

    /*  — lancement PROJET (owl <dir> / owl projet.owl) : apres le body
     * + les compagnons, ouvre la fenetre de demarrage (metadonnee typee
     * FenetreDemarrage / StartupWindow) si renseignee. En lancement fichier
     * (F9 : owl <FEN>), l'entree n'est pas le marqueur projet -> pas d'ouverture
     * ici, c'est l'auto-open GO-mode qui ouvre la fenetre CIBLE (plus de
     * double-ouverture). La designation est un const char* pointant une
     * constante du chunk : on la copie AVANT project_free et avant toute alloc
     * (owl_run_startup_window alloue), sinon un GC pourrait liberer l'ObjString. */
    char startupWin[128] = "";
    bool projectLaunch = projectMode
        && (entryFile == NULL || is_projet_marker_name(entryFile));
    if (result == INTERPRET_OK && projectLaunch) {
        const char* sw = compiler_projet_designated_startup_window();
        if (sw != NULL && sw[0] != '\0') {
            strncpy(startupWin, sw, sizeof(startupWin) - 1);
            startupWin[sizeof(startupWin) - 1] = '\0';
        }
    }

    project_free(&project);

    if (result == INTERPRET_COMPILE_ERROR) return 65;
    if (result == INTERPRET_RUNTIME_ERROR) return 70;
    if (startupWin[0] != '\0') return owl_run_startup_window(startupWin);
    return 0;
}

/* Dispatch `owl <dossier>` aligne sur la frontiere de projet (,
 * modele a 2 niveaux — ).
 *  - projet.owl dans `dir` (ou un ancetre) -> lance le PROJET : entree =
 *    projet.owl (son top-level = le programme), scope arbre recursif.
 *  - aucun projet.owl -> erreur : plus d'auto-run implicite d'un fichier.
 *    L'utilisateur doit nommer un fichier (`owl <fichier>.owl`) ou creer un
 *    projet.owl. */
static int run_directory(const char* dir) {
    /* Resout la frontiere en remontant depuis `dir` (sentinelle bidon : seul
     * son dossier parent — donc `dir` — sert de point de depart a la
     * remontee). */
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/_", dir);
    char root[1024];
    ProjectMode mode = resolve_project_root(probe, root, sizeof(root));

    if (mode == PROJECT_MODE_PROJECT) {
        return run_project_scoped(root, NULL, true, true);
    }

    fprintf(stderr,
            "owl: aucun point d'entree : nomme un fichier (owl <fichier>.owl) "
            "ou cree un projet.owl dans '%s'\n", dir);
    return 65;
}

/* ================================================================
 * GO LOCAL — auto-Ouvre pour fichier-fenetre
 * ================================================================
 *
 * `owl FEN_X.owl` ouvre implicitement la fenetre comme si un main.owl
 * voisin avait fait `Ouvre(FEN_X)`. Heuristique stricte : le fichier
 * doit etre un "fichier-fenetre pur" — exactement une declaration
 * top-level `IDENT est une Fenetre ... FIN` et rien d'autre que des
 * commentaires/lignes vides autour.
 *
 * Implementation purement token-based via le Scanner : aucune VM ni
 * compile n'est invoquee, donc 0 effet de bord sur la suite normale.
 * Si la detection echoue (cas negatif), on rend la main au flow
 * standard sans rien changer.
 *
 * Restriction connue : les fenetres avec PROCEDURE Initialisation(p1,
 * p2) parametree echoueront runtime cote Ouvre. Hors-scope, cf.
 * ticket pour un suivi dedie. */

/* Liste des tokens qui ouvrent un bloc terminé par FIN dans le DSL
 * fenetre. Le tracker de profondeur les incremente pour balancer les
 * FIN imbriques (PROPRIETES, CONSTANTE, PROCEDURE/FONCTION handlers,
 * controles de flot embarques dans les handlers, sous-widgets a
 * keyword). Les sous-controles `IDENT est un <type>` sont traites
 * separement (cf go_local_detect_fenetre) car seuls certains types
 * ouvrent un bloc.
 *
 * Aligne sur le pre-pass du compiler (compiler.c window_skip_nested_block
 * + branche EST de la passe DSL) : c'est la source de verite. Tout ajout
 * d'ouvreur de bloc DSL cote compiler doit etre reporte ici.
 *
 * TOKEN_ETAT n'est PAS un ouvreur : dans un corps de Fenetre, `Etat` est
 * une propriete scalaire (`Etat = Actif`), pas un bloc DSL. Le compilo ne
 * le compte pas non plus (absent de window_skip_nested_block). Les etats
 * de style vivent en chaine de membre `Style..X..Etats[etat]..Y`, deja
 * neutralisee par le skip prev==DOUBLE_DOT dans go_local_detect_fenetre
 *.
 *
 * TOKEN_AVEC est absent lui aussi : le meme mot-cle sert de MODIFICATEUR sans
 * FIN au sein d'autres instructions (`POUR TOUT v DE t AVEC <cond>`,
 * `Position(...) AVEC <options>`). Son discriminant n'est ni le token suivant
 * ni la copule mais la POSITION dans la ligne — traite au site d'appel.
 *
 * TOKEN_SI et TOKEN_CONSTANTE sont absents de cette liste : ce sont les deux
 * seuls ouvreurs a avoir AUSSI une forme sans FIN legale (`SI c ALORS instr`,
 * `CONSTANTE K = 3`), donc leur comptage demande un peek — cf.
 * go_local_opens_block_peek (delegue a prescan_depth_delta), applique au site
 * d'appel. TOKEN_PROCEDURE et TOKEN_FONCTION restent ici (le handler
 * `PROCEDURE Nom() ... FIN` est toujours un bloc) mais sont neutralises au
 * site d'appel quand ils suivent la copule (`p est une Procedure = ...` =
 * annotation de type, pas un handler). */
static bool go_local_token_opens_block(TokenType t) {
    switch (t) {
        case TOKEN_FENETRE:
        case TOKEN_PROPRIETES:
        case TOKEN_PROCEDURE:
        case TOKEN_FONCTION:
        case TOKEN_POUR:
        case TOKEN_TANTQUE:
        case TOKEN_BOUCLE:
        case TOKEN_SELON:
        case TOKEN_QUAND:
        case TOKEN_CLASSE:
        case TOKEN_STRUCTURE:
        case TOKEN_ENUMERATION:
        case TOKEN_COMBINAISON:
        case TOKEN_INTERFACE:
            return true;
        default:
            return false;
    }
}

/* Widget keyword token = type UI built-in (Libelle/Bouton/Saisie/...).
 * N'ouvre un bloc QUE dans le contexte `IDENT est un <keyword>` ; comme
 * nom de propriete dans un bloc PROPRIETES (`Libelle = "x"`) il ne doit
 * surtout PAS compter. La distinction se fait au site d'appel (cf.
 * go_local_detect_fenetre, branche prev==EST). */
static bool go_local_token_is_widget_keyword(TokenType t) {
    switch (t) {
        case TOKEN_LIBELLE:
        case TOKEN_BOUTON:
        case TOKEN_SAISIE:
        case TOKEN_TABLE:
        case TOKEN_CONTENEUR:
        case TOKEN_LISTE:
        case TOKEN_COLONNE:
            return true;
        default:
            return false;
    }
}

/* Prefixe widget des declarations (`LIB_X est un Champ Libelle`).
 * Le prefixe n'est PAS un keyword scanner (conflit avec `champ est une
 * chaine`, cf. scanner.c) -> c'est un TOKEN_IDENTIFIER. On le reconnait donc
 * par comparaison du lexeme. Casse-insensible (invariant runtime) ; pas
 * d'accent a gerer (formes ASCII pures). Trois formes acceptees, exactement
 * celles que le compilo/plugin reconnaissent (cf  `(Champ|Control|
 * Field)`) : `Champ` (FR), `Control` (EN canon) et `Field` (synonyme EN).
 * Toutes les variantes (`Champ Libelle`, `Control Label`, `Field de Saisie`,
 * `Champ Conteneur`...) ouvrent un corps + FIN. */
static bool go_local_token_is_champ_prefix(Token t) {
    static const char* const prefixes[] = {"champ", "control", "field"};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t len = strlen(prefixes[p]);
        if ((size_t)t.length != len) continue;
        bool match = true;
        for (size_t i = 0; i < len; i++) {
            char c = t.start[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != prefixes[p][i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/* Mots-cles de type d'un bloc menu classique (`MNU est un MenuPrincipal`,
 * `SM est un SousMenu`, `OPT est une OptionMenu`, `SEP est un MenuSeparateur`)
 * ainsi que le menu contextuel (`MNU est un MenuContextuel`, declare dans la
 * Fenetre en sibling des champs). Comme le prefixe `Champ`, ce ne sont PAS des
 * keywords scanner ( : is_widget=false) : ils arrivent en
 * TOKEN_IDENTIFIER et se reconnaissent par lexeme. Chacun ouvre un corps + FIN
 * -> depth++ inconditionnel dans le tracker (le peek-PROPRIETES echoue pour
 * MenuPrincipal/MenuContextuel, dont la ligne suivante est un bloc Style ou une
 * declaration enfant, et pour MenuSeparateur, sans PROPRIETES du tout).
 * Formes FR+EN canon, casse + accent tolerant (scanner_strip_accents comme
 * menu_keyword_of_token cote compiler.c). */
static bool go_local_token_is_menu_keyword(Token t) {
    if (t.type != TOKEN_IDENTIFIER || t.start == NULL) return false;
    char buf[64];
    int n = scanner_strip_accents(t.start, t.length, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    buf[n] = '\0';
    static const char* const kws[] = {
        "MenuPrincipal",   "MainMenu",
        "SousMenu",        "Submenu",
        "OptionMenu",      "MenuOption",
        "MenuSeparateur",  "MenuSeparator",
        "MenuContextuel",  "ContextMenu",
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if (strcasecmp(buf, kws[i]) == 0) return true;
    }
    return false;
}

/* Mot-cle `Style` du bloc de style menu : `Style` seul sur sa
 * ligne ouvre un corps + FIN a l'interieur d'un MenuPrincipal (miroir de
 * menu_parse_style_block cote compiler.c, qui matche le lexeme "Style").
 * Ce n'est PAS un keyword scanner -> arrive en TOKEN_IDENTIFIER, reconnu
 * par lexeme. Forme FR=EN (pas de variante). Le meme mot sert AUSSI de
 * propriete widget (`Style = STY_X`, `Style..CouleurFond`) : la
 * disambiguation forme-bloc vs propriete se fait au site d'appel via le
 * token brut suivant (NEWLINE = bloc). */
static bool go_local_token_is_style_kw(Token t) {
    if (t.type != TOKEN_IDENTIFIER || t.start == NULL) return false;
    char buf[16];
    int n = scanner_strip_accents(t.start, t.length, buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    buf[n] = '\0';
    return strcasecmp(buf, "Style") == 0;
}

/* Le SI / CONSTANTE qui vient d'etre lu ouvre-t-il un bloc terminé par FIN ?
 *   `SI cond ALORS` + NEWLINE      -> forme bloc, un FIN viendra    -> true
 *   `SI cond ALORS <instruction>`  -> forme mono-ligne, aucun FIN   -> false
 *   `CONSTANTE` + NEWLINE          -> forme bloc, les decls suivent -> true
 *   `CONSTANTE K = 3`              -> forme inline, aucun FIN       -> false
 * Compter une forme sans FIN fait consommer le FIN de la Fenetre pour "fermer"
 * ce SI : la fenetre reste ouverte aux yeux du detecteur, l'auto-open echoue et
 * le filet anti-silence accuse a tort un contenu supplementaire (,
 * repro support).
 *
 *  : la regle n'est plus reimplementee ici. Le detecteur consomme la
 * source de verite unique `prescan_depth_delta` (prescan.h), qui applique le
 * meme peek et le meme defaut conservateur (source malformee -> forme bloc :
 * mieux vaut rater une auto-ouverture que relacher le garde anti-silence).
 * `sc` est copie : le scan reel n'avance pas. */
static bool go_local_opens_block_peek(const Scanner* sc, Token t) {
    Scanner peek = *sc;
    return prescan_depth_delta(t, &peek) > 0;
}

bool go_local_detect_fenetre(const char* source, char* outName,
                             int outCapacity) {
    if (source == NULL || outName == NULL || outCapacity <= 0) return false;
    outName[0] = '\0';

    Scanner sc;
    scanner_init(&sc, source);

    /* Phase 1 : skip NEWLINEs jusqu'au premier vrai token. */
    Token t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);

    /* Doit etre : IDENT (le nom du FEN). */
    if (t.type != TOKEN_IDENTIFIER) return false;
    int nameLen = t.length;
    if (nameLen >= outCapacity) return false;
    const char* nameStart = t.start;

    /* Suivant : EST (eventuels NEWLINEs entre — le DSL canon strict
     * impose tout sur une seule ligne mais robustesse defensive). */
    t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    if (t.type != TOKEN_EST) return false;

    /* Suivant : FENETRE (les articles "un"/"une" sont consommes par le
     * scanner en position d'article, cf. scan_identifier). En EN l'article `a`/`an`
     * n'est PAS un filler scanner -> le sauter ici comme dans la boucle
     * de profondeur ( l'avait fait dans la boucle mais pas a ce
     * check initial -> `IDENT is a Window` ratait l'auto-open, ). */
    t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    if (t.type == TOKEN_A ||
        (t.type == TOKEN_IDENTIFIER && t.length == 2 &&
         (t.start[0] == 'a' || t.start[0] == 'A') &&
         (t.start[1] == 'n' || t.start[1] == 'N'))) {
        t = scanner_scan_token(&sc);
        while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    }
    if (t.type != TOKEN_FENETRE) return false;

    /* OK, on est dans un bloc Fenetre. Tracker la profondeur jusqu'au
     * FIN qui ramene a 0.
     *
     * Regle d'ouverture de sous-bloc (alignee sur le pre-pass compiler,
     * cf. compiler.c branche EST de window_skip_nested_block) :
     *   - keyword ouvreur (PROPRIETES, PROCEDURE, sous-widget keyword
     *     Libelle/Bouton/..., flot POUR/TANTQUE/...) -> depth++ direct via
     *     go_local_token_opens_block.
     *   - SI : depth++ SEULEMENT sous la forme bloc (`ALORS` en fin de
     *     ligne), cf. go_local_opens_block_peek.
     *   - CONSTANTE : depth++ SEULEMENT sous la forme bloc (mot-cle seul en
     *     fin de ligne), cf. go_local_opens_block_peek.
     *   - PROCEDURE/FONCTION apres la copule (`p est une Procedure`) :
     *     annotation de type, jamais un bloc.
     *   - `IDENT est un <IDENT>` : ouvre un bloc SEULEMENT si le type est
     *     un widget DSL a corps, signale par un PROPRIETES sur la ligne
     *     significative suivante (Image/Combo/Selecteur/...). Une variable
     *     type-sans-bloc (`tmr est un Timer`, `n est un entier`) n'ouvre
     *     PAS de bloc et ne doit pas incrementer depth (bug ). */
    int depth = 1;
    TokenType prev = TOKEN_FENETRE;
    TokenType prev2 = TOKEN_EOF;  /* sentinelle */
    int prevLine = t.line;        /* ligne du token significatif precedent */
    for (;;) {
        t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) {
            /* Bloc malforme — laisse le compilo normal lever l'erreur. */
            return false;
        }
        if (t.type == TOKEN_NEWLINE) continue;

        /* Premier token significatif de sa ligne : seul discriminant possible
         * pour AVEC (cf. sa branche plus bas). */
        bool firstOnLine = (t.line != prevLine);
        prevLine = t.line;

        /*  — article EN entre `is` et le type (`IDENT is a <Type>`).
         * En FR `un/une` est un filler jete ; en EN `a`->TOKEN_A,
         * `an`->IDENT 2 chars. On le saute sans toucher prev/prev2 pour que
         * <Type> voie prev==EST. */
        if (prev == TOKEN_EST &&
            (t.type == TOKEN_A ||
             (t.type == TOKEN_IDENTIFIER && t.length == 2 &&
              (t.start[0] == 'a' || t.start[0] == 'A') &&
              (t.start[1] == 'n' || t.start[1] == 'N')))) {
            continue;
        }

        /* Acces membre : tout token suivant `.` ou `..` est un nom de
         * membre (`tmr..Procedure`, `obj.Champ`), jamais un ouvreur de
         * bloc ni un debut de decl. Sans ca, `tmr..Procedure` compte
         * TOKEN_PROCEDURE -> depth++ parasite sans FIN (, repro
         * Snake FinInitialisation). Aligne sur le pre-pass compiler
         * (afterDot dans window_skip_nested_block). */
        if (prev == TOKEN_DOT || prev == TOKEN_DOUBLE_DOT) {
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* Declaration de sous-controle : `IDENT est un <type>`, ou
         * prev==EST et prev2==IDENT (le nom du controle). Deux familles :
         *   - widget a keyword (Libelle/Bouton/...) -> ouvre toujours un
         *     bloc (corps PROPRIETES + FIN), depth++.
         *   - type IDENT (Image, Combo... = widget DSL IDENT, ou Timer,
         *     entier, type user) -> ouvre un bloc SSI un PROPRIETES suit
         *     sur la ligne significative suivante. Sinon decl scalaire
         *     sans FIN, pas de depth++ ( : Timer ne doit pas
         *     incrementer). */
        if (prev == TOKEN_EST && prev2 == TOKEN_IDENTIFIER &&
            (t.type == TOKEN_IDENTIFIER ||
             go_local_token_is_widget_keyword(t.type))) {
            if (go_local_token_is_widget_keyword(t.type)) {
                depth++;
            } else if (go_local_token_is_champ_prefix(t)) {
                /* `<NOM> est un Champ <Type>` : le prefixe Champ garantit un
                 * corps + FIN, quel que soit <Type> (Libelle/Saisie/Combo/
                 * Conteneur...). depth++ inconditionnel sans peek.
                 * Le <Type> suivant ne re-compte pas : prev devient IDENT(champ)
                 * != EST, et les widget keywords ne sont pas dans
                 * go_local_token_opens_block. */
                depth++;
            } else if (go_local_token_is_menu_keyword(t)) {
                /* `<NOM> est un <MenuPrincipal|SousMenu|OptionMenu|
                 * MenuSeparateur>` : chaque noeud de menu ouvre un corps + FIN.
                 * depth++ inconditionnel. Le peek-PROPRIETES ci-
                 * dessous ne suffit pas : MenuPrincipal enchaine directement une
                 * declaration enfant (pas de PROPRIETES) et MenuSeparateur n'a
                 * aucune PROPRIETES -> leur FIN sous-decrementait depth et
                 * desynchronisait la detection (exit 65 anti-silence). */
                depth++;
            } else {
                Scanner peek = sc;
                Token after = scanner_scan_token(&peek);
                while (after.type == TOKEN_NEWLINE) after = scanner_scan_token(&peek);
                if (after.type == TOKEN_PROPRIETES) depth++;
            }
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* `SINON SI` (else if) ne redemarre PAS un bloc : la chaine
         * SI/SINON SI/SINON est fermee par un unique FIN. Compter le SI
         * d'un `SINON SI` sur-incremente depth (1 de plus par branche,
         * 1 seul FIN) -> detection cassee sur tout handler a multi-branches
         * (, repro Snake ChangeDirection). */
        if (t.type == TOKEN_SI && prev == TOKEN_SINON) {
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* SI hors chaine SINON : ne compte que sous sa forme bloc. La forme
         * mono-ligne `SI cond ALORS <instr>` (canon, sans FIN) ne doit pas
         * incrementer depth. */
        if (t.type == TOKEN_SI) {
            if (go_local_opens_block_peek(&sc, t)) depth++;
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* CONSTANTE : ne compte que sous sa forme bloc (`CONSTANTE` seul en fin
         * de ligne). La forme inline `CONSTANTE K = 3` (canon, sans FIN) ne
         * doit pas incrementer depth. */
        if (t.type == TOKEN_CONSTANTE) {
            if (go_local_opens_block_peek(&sc, t)) depth++;
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* `IDENT est une Procedure|Fonction` : annotation de TYPE d'une variable
         * porteuse de code (`p est une Procedure = Compile(...)`, ou sans
         * initialisation), pas un handler `PROCEDURE Nom() ... FIN` -> aucun FIN
         * ne suit, donc aucun depth++. Ici le peek de fin de ligne ne peut pas
         * trancher (la forme sans initialisation se termine par un NEWLINE) :
         * c'est la copule qui distingue. Miroir du guard afterEst du pre-pass
         * compiler (prepass_block_collect_procs, ). Les autres mots-cles
         * structurels post-copule (`E est une Enumeration`, `st est une
         * Structure`) ouvrent au contraire un vrai bloc ferme par FIN dans un
         * corps de fenetre : ils continuent de compter. */
        if (prev == TOKEN_EST && prev2 == TOKEN_IDENTIFIER &&
            (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION)) {
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* AVEC : un seul mot-cle, deux emplois. En tete de ligne c'est
         * l'instruction `AVEC <cible>` … `FIN` -> depth++. Au milieu d'une
         * ligne c'est un MODIFICATEUR d'une autre instruction (`POUR TOUT v DE
         * t AVEC <cond>`, `Position(...) AVEC <options>`) et aucun FIN ne
         * suit. Sans ce filtre, `POUR TOUT … AVEC …` comptait +2 pour un seul
         * FIN : la fenetre n'etait jamais refermee aux yeux du detecteur et
         * l'auto-ouverture etait refusee sur un source parfaitement legal —
         * meme symptome que  (mesure ). */
        if (t.type == TOKEN_AVEC) {
            if (firstOnLine) depth++;
            prev2 = prev;
            prev = t.type;
            continue;
        }

        /* Bloc `Style` de menu : `Style` seul sur sa ligne ouvre
         * un corps + FIN dans un MenuPrincipal. Le meme mot sert de propriete
         * widget (`Style = STY_X`, `Style..CouleurFond = X`) qui NE doit PAS
         * compter. On distingue par le token brut suivant (sans skip NEWLINE) :
         * NEWLINE -> forme bloc -> depth++ ; =/../.// [ -> propriete -> rien.
         * Sans ce depth++, le FIN du bloc Style sous-decremente depth et
         * desynchronise la detection (exit 65 anti-silence). */
        if (go_local_token_is_style_kw(t)) {
            Scanner peek = sc;
            Token after = scanner_scan_token(&peek);
            if (after.type == TOKEN_NEWLINE) depth++;
            prev2 = prev;
            prev = t.type;
            continue;
        }

        if (go_local_token_opens_block(t.type)) {
            depth++;
        } else if (t.type == TOKEN_FIN) {
            depth--;
            if (depth == 0) break;  /* FIN qui ferme le bloc Fenetre */
        }

        prev2 = prev;
        prev = t.type;
    }

    /* Phase 3 : apres le FIN qui ferme la fenetre, seuls NEWLINE et
     * EOF sont autorises. Tout autre token signale du code top-level
     * supplementaire -> rejet (cas negatif : variable globale,
     * deuxieme fenetre, classe, PROCEDURE Main, etc.). */
    for (;;) {
        t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        if (t.type != TOKEN_NEWLINE) return false;
    }

    /* Tout est ok — copie le nom dans le buffer du caller. */
    memcpy(outName, nameStart, (size_t)nameLen);
    outName[nameLen] = '\0';
    return true;
}

/* Filet anti-silence : un fichier dont la tete RESSEMBLE a une
 * declaration de fenetre top-level (`IDENT est une Fenetre`) mais que
 * go_local_detect_fenetre n'a pas su auto-ouvrir (detection plus stricte
 * qui rate un cas) ne doit jamais sortir en exit 0 muet. On detecte
 * uniquement les 3 premiers tokens significatifs — volontairement laxiste,
 * c'est un garde-fou, pas un parser. */
static bool go_local_looks_like_fenetre(const char* source) {
    if (source == NULL) return false;
    Scanner sc;
    scanner_init(&sc, source);

    Token t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    if (t.type != TOKEN_IDENTIFIER) return false;

    t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    if (t.type != TOKEN_EST) return false;

    t = scanner_scan_token(&sc);
    while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    /* Article EN `a`/`an` entre la copule et le type — meme
     * skip que le detecteur principal, sinon `IDENT is a Window` echappe
     * au filet et sort en exit 0 muet. */
    if (t.type == TOKEN_A ||
        (t.type == TOKEN_IDENTIFIER && t.length == 2 &&
         (t.start[0] == 'a' || t.start[0] == 'A') &&
         (t.start[1] == 'n' || t.start[1] == 'N'))) {
        t = scanner_scan_token(&sc);
        while (t.type == TOKEN_NEWLINE) t = scanner_scan_token(&sc);
    }
    return t.type == TOKEN_FENETRE;
}

/* Wrapper non-static pour le self-test C. La fonction interne
 * reste static ; ce point d'entree permet a go_local_test.c de verifier
 * que le filet anti-silence reconnait la meme tete que le detecteur. */
bool go_local_looks_like_fenetre_test(const char* source) {
    return go_local_looks_like_fenetre(source);
}

/* ----  — inspection des parametres d'une fenetre lancee en GO LOCAL.
 *
 * Un point d'entree `owl FEN_X.owl` sur une fenetre parametree ne doit plus
 * poser silencieusement Null : soit on refuse (type avance, ou requis sans
 * valeur en contexte non interactif), soit on collecte les valeurs (TTY :
 * invite ; pipe : consomme) et on ouvre normalement. La decision se prend
 * ICI, avant tout appel a Ouvre (donc avant tout montage Qt). */

/* Rendu d'un message d'erreur registry selon la langue active (FR/EN). */
static const char* go_local_render(OwlErrorCode code, ...) {
    va_list ap;
    va_start(ap, code);
    const char* s = owl_error_render_active_va(code, ap);
    va_end(ap);
    return s;
}

/*  — libelle humain du kind de bloc DSL refuse au lancement direct,
 * dans la langue active (FR/EN). Sert a interpoler {kind} dans le message
 * E_RT_ENTRY_NOT_LAUNCHABLE. */
static const char* entry_kind_label(PrescanEntryKind kind) {
    int en = owl_error_render_lang_is_en();
    switch (kind) {
        case PRESCAN_ENTRY_ETAT:        return en ? "report" : "état";
        case PRESCAN_ENTRY_CLASSE:      return en ? "class" : "classe";
        case PRESCAN_ENTRY_COLLECTION:  return en ? "procedure collection"
                                                  : "collection de procédures";
        case PRESCAN_ENTRY_THEME:       return en ? "theme" : "thème";
        case PRESCAN_ENTRY_STRUCTURE:   return en ? "structure" : "structure";
        case PRESCAN_ENTRY_ENUM:        return en ? "enumeration" : "énumération";
        case PRESCAN_ENTRY_COMBINAISON: return en ? "combination" : "combinaison";
        case PRESCAN_ENTRY_INTERFACE:   return en ? "interface" : "interface";
        default:                        return en ? "type" : "type";
    }
}

/* Detection GO-mode pre-run (token-based, 0 effet de bord) : remplit
 * fenName/autoOpen/looksLikeFenetre depuis la source. */
static void go_local_prerun(const char* src, char* fenName, int fenCap,
                            bool* autoOpen, bool* looksLikeFenetre) {
    *autoOpen = false;
    *looksLikeFenetre = false;
    if (src == NULL) return;
    *autoOpen = go_local_detect_fenetre(src, fenName, fenCap);
    *looksLikeFenetre = go_local_looks_like_fenetre(src);
}

/* Epilogue GO-mode post-run : auto-Ouvre la fenetre si detectee, ou
 * sort en erreur anti-silence si la tete ressemble a une fenetre sans
 * auto-ouverture possible. Retourne le code final. */
static int go_local_postrun(int code, bool autoOpen, const char* fenName,
                            bool looksLikeFenetre) {
    if (code == 0 && autoOpen) {
        return owl_run_startup_window(fenName);
    }
    if (code == 0 && !autoOpen && looksLikeFenetre) {
        /* Filet anti-silence (doctrine OWL, ) : la tete ressemble
         * a une fenetre top-level mais la detection d'auto-ouverture a
         * echoue. Sans ce garde, le process sortirait en exit 0 muet —
         * le pire diagnostic possible. On signale et on sort non-zero.
         *
         *  — le message n'enumere plus de causes. Les trois qu'il
         * citait (2e fenetre, variable globale, instruction au meme niveau)
         * sont desormais toutes interceptees en amont par une erreur de
         * compilation dediee ("Un fichier-fenetre ne peut contenir que le
         * bloc Fenetre..."), donc avec code != 0 : elles n'atteignent jamais
         * cette branche. Ce qui reste joignable ici, c'est un faux negatif du
         * detecteur (source valide, --check clean, detection ratee) — cas du
         * signalement support. Enumerer des causes fausses envoyait
         * l'utilisateur chercher un probleme inexistant. */
        fprintf(stderr,
                "owl: ce fichier declare une fenetre mais l'ouverture "
                "automatique n'a pas pu etre determinee.\n"
                "     Contournement : ouvrir la fenetre depuis un fichier "
                "voisin, avec Ouvre(<NomDeLaFenetre>).\n"
                "     Ce cas n'est pas attendu sur un fichier valide : merci "
                "de le signaler.\n");
        return 65;
    }
    return code;
}

/* Dispatch `owl <fichier.owl>` aligne sur la frontiere :
 *  - projet.owl reserve -> erreur, non executable.
 *  - frontiere PROJECT (projet.owl ancetre) -> scope arbre raciné.
 *  - frontiere SCRIPT/ENTRYPOINT -> dossier du fichier seul.
 *
 *  : si le fichier est un "fichier-fenetre pur" (un seul bloc
 * Fenetre top-level, rien d'autre), wrappe implicitement Ouvre(FEN_X)
 * apres l'execution du script. */
static int run_owl_file(const char* path) {
    char fenName[128];
    bool autoOpen, looksLikeFenetre;
    char* src = read_file(path);
    go_local_prerun(src, fenName, (int)sizeof(fenName),
                    &autoOpen, &looksLikeFenetre);

    /*  — refus AVANT toute execution (donc avant la resolution/execution
     * d'un projet.owl en contexte F9) : un fichier dont le top-level est un bloc
     * DSL non lancable (ni Projet, ni Fenetre) sort non-zero avec un message
     * clair, au lieu d'un exit 0 muet. Le check porte sur le fichier d'ENTREE. */
    char entryName[128];
    PrescanEntryKind entryKind =
        prescan_source_entry_block_kind(src, entryName, (int)sizeof(entryName));
    free(src);
    if (entryKind != PRESCAN_ENTRY_NONE &&
        entryKind != PRESCAN_ENTRY_PROJET &&
        entryKind != PRESCAN_ENTRY_FENETRE) {
        fprintf(stderr, "%s\n",
                go_local_render(E_RT_ENTRY_NOT_LAUNCHABLE,
                                "name", entryName,
                                "kind", entry_kind_label(entryKind), NULL));
        return 65;
    }

    char root[1024];
    ProjectMode mode = resolve_project_root(path, root, sizeof(root));
    char* filename = extract_filename(path);
    int code;
    if (mode == PROJECT_MODE_PROJECT) {
        /* Fichier DANS un projet (ou projet.owl lui-meme) : entree = ce
         * fichier, projet.owl + compagnons charges (defs + top-levels avant),
         * i18n boot + teardown Fermeture.  : `owl projet.owl` lance
         * ainsi le projet (son top-level = le programme). */
        code = run_project_scoped(root, filename, true, true);
    } else {
        /* Mode script pur : dossier du fichier seul, aucun teardown projet. */
        char* dir = extract_directory(path);
        code = run_project_scoped(dir, filename, false, false);
        free(dir);
    }
    free(filename);

    return go_local_postrun(code, autoOpen, fenName, looksLikeFenetre);
}

/* ================================================================
 * REPL
 * ================================================================ */

#include "owl_readline.h"

/* Profondeur des blocs encore ouverts dans le tampon du REPL : > 0 laisse
 * l'invite de continuation affichee, 0 declenche l'interpretation.
 *
 *  — le comptage delegue a la source unique (prescan_depth_delta) au
 * lieu de porter sa propre liste d'ouvreurs. Ce clone-la avait quatre trous, et
 * un sur-comptage n'est PAS cosmetique ici : l'invite ne retombe jamais, tout
 * ce qui suit — `quitter` compris — est avale dans le tampon, et la session
 * sort avec le code 0. Etaient casses :
 *   `SI c ALORS instr`        forme mono-ligne comptee comme bloc
 *   `SINON SI`               +1 pour une chaine fermee par un seul FIN
 *   `POUR TOUT … AVEC cond`   modificateur compte comme instruction a bloc
 *   `CONSTANTE` en bloc       jamais compte -> interpretation prematuree
 * PROPRIETES et FENETRE manquaient aussi (meme classe : sous-comptage).
 *
 * Le reste du discriminant n'est pas exprimable par la table (il demande un
 * lookbehind ou la position dans la ligne) et vit ici, comme dans
 * go_local_detect_fenetre. Non-static : exerce par --self-test-block-openers. */
int repl_count_block_depth(const char* source) {
    Scanner sc;
    scanner_init(&sc, source);
    int depth = 0;
    TokenType prev  = TOKEN_NEWLINE;
    TokenType prev2 = TOKEN_EOF;   /* sentinelle */
    int prevLine = 0;
    for (;;) {
        Token tok = scanner_scan_token(&sc);
        if (tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_NEWLINE) continue;

        /* Seul discriminant possible pour AVEC (cf. sa branche plus bas). */
        bool firstOnLine = (tok.line != prevLine);
        prevLine = tok.line;

        /* Acces membre : tout token suivant `.`/`..` est un nom de membre
         * (`tmr..Procedure`), jamais un ouvreur. */
        if (prev == TOKEN_DOT || prev == TOKEN_DOUBLE_DOT) {
            prev2 = prev; prev = tok.type; continue;
        }

        /* `SINON SI` : continuation de la meme conditionnelle, un seul FIN. */
        if (tok.type == TOKEN_SI && prev == TOKEN_SINON) {
            prev2 = prev; prev = tok.type; continue;
        }

        /* `p est une Procedure|Fonction` : annotation de type, aucun FIN. Les
         * autres mots-cles structurels post-copule ouvrent au contraire un
         * vrai bloc (cf. la branche des extras plus bas). */
        if (prev == TOKEN_EST && prev2 == TOKEN_IDENTIFIER &&
            (tok.type == TOKEN_PROCEDURE || tok.type == TOKEN_FONCTION)) {
            prev2 = prev; prev = tok.type; continue;
        }

        /* AVEC : instruction a bloc en tete de ligne, modificateur au milieu. */
        if (tok.type == TOKEN_AVEC) {
            if (prescan_avec_opens_block(firstOnLine)) depth++;
            prev2 = prev; prev = tok.type; continue;
        }

        if (prescan_token_in_depth_table(tok.type)) {
            Scanner peek = sc;
            depth += prescan_depth_delta(tok, &peek);
        } else {
            switch (tok.type) {
                /* Hors table : n'arrivent qu'apres la copule (`c est une
                 * Classe`), ou ils ouvrent un corps ferme par FIN. */
                case TOKEN_CLASSE:
                case TOKEN_STRUCTURE:
                case TOKEN_ENUMERATION:
                case TOKEN_COMBINAISON:
                case TOKEN_INTERFACE:
                case TOKEN_FENETRE:
                    depth++;
                    break;
                default: break;
            }
        }
        prev2 = prev; prev = tok.type;
    }
    return depth;
}

/* Show globals defined by the user (skip __ internals and natives) */
static void repl_show_globals(void) {
    printf("  Variables globales:\n");
    Table* t = &vm.globals;
    int shown = 0;
    for (int i = 0; i < t->capacity; i++) {
        Entry* e = &t->entries[i];
        if (e->key == NULL) continue;
        /* Skip internal names and native functions */
        if (e->key->chars[0] == '_') continue;
        if (IS_NATIVE(e->value)) continue;
        if (IS_FUNCTION(e->value)) {
            printf("    %s()\n", e->key->chars);
        } else {
            printf("    %s = ", e->key->chars);
            value_print(e->value);
            printf("\n");
        }
        shown++;
    }
    if (shown == 0) printf("    (aucune)\n");
}

/* Completion candidates */
static const char* repl_keywords[] = {
    "SI", "ALORS", "SINON", "FIN", "POUR", "TOUT", "TANTQUE",
    "BOUCLE", "FAIRE", "SELON", "CAS", "AUTRE", "SORTIR",
    "CONTINUER", "RENVOYER", "RETOUR", "AVEC", "QUAND", "EXCEPTION",
    "PROCEDURE", "FONCTION", "CLASSE", "STRUCTURE", "ENUMERATION",
    "COMBINAISON", "INTERFACE", "HERITE", "IMPLEMENTE", "PROPRIETE",
    "PUBLIC", "PRIVE", "PROTEGE", "GLOBALE", "LOCAL", "CONSTANTE",
    "EST", "SONT", "DE", "DANS", "VRAI", "FAUX", "ET", "OU", "PAS",
    NULL
};

/* Sink de candidats de complétion : découple la production des candidats
 * (registre vivant) de leur consommation (owl_completion_add en REPL, capture
 * en self-test). */
typedef void (*ReplCompletionSink)(const char* candidate, void* ctx);

/* Un octet fait-il partie d'un identifiant OWL ? Inclut les octets UTF-8
 * (>= 0x80) pour qu'une saisie accentuée (« Décl ») soit traitée comme un seul
 * mot et matche la forme canonique accentuée du registre. */
static int repl_ident_byte(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

/* Émet `cand` (+ `suffix`) en réinjectant la partie de `buf` qui précède le
 * mot courant (préserve le contexte : `x = Decl` -> `x = DeclencheEvenement(`). */
static void repl_emit(const char* buf, const char* word, const char* cand,
                      const char* suffix, ReplCompletionSink sink, void* ctx) {
    char full[512];
    snprintf(full, sizeof(full), "%.*s%s%s",
             (int)(word - buf), buf, cand, suffix);
    sink(full, ctx);
}

/* Un candidat matche-t-il le mot courant (préfixe casse-insensible, wlen octets)
 * ET diffère-t-il des formes déjà émises pour cette entrée (anti-doublon) ? */
static void repl_try(const char* buf, const char* word, int wlen,
                     const char* cand, const char* skipA, const char* skipB,
                     const char* suffix, ReplCompletionSink sink, void* ctx) {
    if (!cand) return;
    if (skipA && strcasecmp(cand, skipA) == 0) return;
    if (skipB && strcasecmp(cand, skipB) == 0) return;
    if (strncasecmp(word, cand, (size_t)wlen) != 0) return;
    repl_emit(buf, word, cand, suffix, sink, ctx);
}

/* Enumère les candidats de complétion pour buf[0..bufLen) contre le REGISTRE
 * VIVANT : natives (formes FR canon + ASCII + EN, hors OWL_NAT_INTERNAL), types
 * composés + primitifs, constantes (dont valeurs d'enum), noms d'enum, plus les
 * mots-clés et les symboles définis par l'utilisateur dans la session REPL.
 * Exposé (non-static) pour le self-test interne. */
void repl_completion_enumerate(const char* buf, int bufLen,
                               ReplCompletionSink sink, void* ctx) {
    const char* word = buf + bufLen;
    while (word > buf && repl_ident_byte((unsigned char)word[-1]))
        word--;
    int wlen = (int)((buf + bufLen) - word);
    if (wlen == 0) return;

    /* Mots-clés : source figée légitime (absents du registre natives). */
    for (int i = 0; repl_keywords[i]; i++)
        repl_try(buf, word, wlen, repl_keywords[i], NULL, NULL, "", sink, ctx);

    /* Natives — registre vivant, natives internes masquées. */
    for (int i = 0, n = owl_registry_count(); i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        repl_try(buf, word, wlen, d->canonical,      NULL,          NULL,             "(", sink, ctx);
        repl_try(buf, word, wlen, d->canonicalAscii, d->canonical,  NULL,             "(", sink, ctx);
        repl_try(buf, word, wlen, d->nameEN,         d->canonical,  d->canonicalAscii,"(", sink, ctx);
    }

    /* Types composés (registre) + primitifs (table cli_doc). */
    for (int i = 0, nt = owl_registry_types_count(); i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t) continue;
        repl_try(buf, word, wlen, t->canonical,      NULL,         NULL,             "", sink, ctx);
        repl_try(buf, word, wlen, t->canonicalAscii, t->canonical, NULL,             "", sink, ctx);
        repl_try(buf, word, wlen, t->nameEN,         t->canonical, t->canonicalAscii,"", sink, ctx);
    }
    for (int i = 0, np = cli_doc_primitive_count(); i < np; i++) {
        const char* pn = NULL; const char* pa = NULL; const char* pe = NULL;
        if (!cli_doc_primitive_at(i, &pn, &pa, &pe)) continue;
        repl_try(buf, word, wlen, pn, NULL, NULL, "", sink, ctx);
        repl_try(buf, word, wlen, pa, pn,   NULL, "", sink, ctx);
        repl_try(buf, word, wlen, pe, pn,   pa,   "", sink, ctx);
    }

    /* Constantes (registre) — inclut les valeurs d'enum. */
    for (int i = 0, nc = owl_registry_consts_count(); i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c) continue;
        repl_try(buf, word, wlen, c->name,   NULL,    NULL, "", sink, ctx);
        repl_try(buf, word, wlen, c->nameEN, c->name, NULL, "", sink, ctx);
    }

    /* Noms d'enum eux-mêmes (registre). */
    for (int i = 0, ne = owl_registry_enums_count(); i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e) continue;
        repl_try(buf, word, wlen, e->name,      NULL,      NULL,        "", sink, ctx);
        repl_try(buf, word, wlen, e->nameAscii, e->name,   NULL,        "", sink, ctx);
        repl_try(buf, word, wlen, e->nameEN,    e->name,   e->nameAscii,"", sink, ctx);
    }

    /* Symboles définis par l'utilisateur dans la session (variables, procédures). */
    Table* t = &vm.globals;
    for (int i = 0; i < t->capacity; i++) {
        Entry* e = &t->entries[i];
        if (e->key == NULL || e->key->chars[0] == '_') continue;
        if (IS_NATIVE(e->value)) continue;
        if (strncasecmp(word, e->key->chars, (size_t)wlen) == 0)
            repl_emit(buf, word, e->key->chars,
                      IS_FUNCTION(e->value) ? "(" : "", sink, ctx);
    }
}

static void repl_completion_add_sink(const char* candidate, void* ctx) {
    (void)ctx;
    owl_completion_add(candidate);
}

static void repl_completion_cb(const char* buf, int bufLen) {
    repl_completion_enumerate(buf, bufLen, repl_completion_add_sink, NULL);
}

/* Session flags populated from CLI. Keep as simple globals — main() is the
 * only mutator. */
bool g_repl_quiet = false;        /* --quiet: no banner, no prompt */
bool g_repl_auto_print = false;   /* --repl: auto-print bare expressions */
bool g_json_errors = false;       /* --json-errors: structured error output */

static void repl(void) {
    if (!g_repl_quiet) {
        /* Banner on stderr so stdout stays clean for piped consumers
         * (Jupyter kernel, pipe-through tools, test harnesses). */
        fprintf(stderr, "OWL %s - langage de programmation productif\n", OWL_VERSION);
        fprintf(stderr, "Tapez 'aide' pour les commandes, 'quitter' pour sortir.\n\n");
    }

    owl_completion_set_callback(repl_completion_cb);
    owl_history_load(".owl_history");

    char buffer[65536];
    buffer[0] = '\0';
    int blockDepth = 0;
    bool first_line = true; /*  : strip BOM UTF-8 sur la 1re ligne du flux */

    for (;;) {
        const char* prompt = g_repl_quiet ? "" :
                             (blockDepth > 0 ? "...> " : "owl> ");
        char* line = owl_readline(prompt);
        if (!line) { printf("\n"); break; } /* EOF */

        /*  : PowerShell 5.1 (et autres) pipe parfois avec un BOM
         * UTF-8 en tete du flux. Le chemin fichier le strippe deja
         * (project.c via owl_strip_utf8_bom) ; on s'aligne ici pour
         * stdin/REPL. Uniquement sur la PREMIERE ligne du flux — un BOM
         * apparaissant au milieu reste intact (coherent file-side, et un
         * user TTY ne tape pas BOM au clavier). */
        if (first_line) {
            owl_strip_utf8_bom(line);
            first_line = false;
        }

        /* Special commands (only at top level, not inside a block) */
        if (blockDepth == 0 && line[0] != '\0') {
            if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0 ||
                strcmp(line, "quitter") == 0) {
                free(line); break;
            }
            if (strcmp(line, "clear") == 0 || strcmp(line, "effacer") == 0) {
                owl_clear_screen(); free(line); continue;
            }
            if (strcmp(line, "globals") == 0 || strcmp(line, "globales") == 0) {
                repl_show_globals(); free(line); continue;
            }
            if (strcmp(line, "help") == 0 || strcmp(line, "aide") == 0) {
                printf("  Commandes REPL:\n");
                printf("    aide / help          Afficher cette aide\n");
                printf("    quitter / quit       Quitter\n");
                printf("    effacer / clear      Effacer l'ecran\n");
                printf("    globales / globals   Lister les variables globales\n");
                printf("  Tab                    Completion automatique\n");
                printf("  Fleches                Historique (haut/bas) et edition (gauche/droite)\n\n");
                free(line); continue;
            }
        }

        /* Skip empty lines at top level */
        if (blockDepth == 0 && line[0] == '\0') {
            free(line); continue;
        }

        /* Accumulate safely with bounds check */
        {
            size_t bufLen = strlen(buffer);
            size_t lineLen = strlen(line);
            if (bufLen + lineLen + 2 < sizeof(buffer)) {
                memcpy(buffer + bufLen, line, lineLen);
                buffer[bufLen + lineLen] = '\n';
                buffer[bufLen + lineLen + 1] = '\0';
            } else {
                fprintf(stderr, "owl: REPL buffer full, input truncated\n");
            }
        }
        owl_history_add(line);
        free(line);

        /* Check block depth */
        blockDepth = repl_count_block_depth(buffer);

        if (blockDepth <= 0) {
            vm_interpret(buffer);
            buffer[0] = '\0';
            blockDepth = 0;
        }
    }

    owl_history_save(".owl_history");
    owl_history_free();
}

/* ================================================================
 * MAIN
 * ================================================================ */

/* Alias commun pour `--help` / `-h` / fallback usage : delegue a
 * `cli_doc_help` (source de verite unique). */
static int call_help(void) {
    char* help_argv[] = {"help"};
    return cli_doc_help(1, help_argv);
}

/* ================================================================
 * DOCUMENTATION GENERATORS
 *
 * All native metadata comes from the runtime registry (owl_registry.c),
 * populated by each module's stdlib_*_init() via
 * owl_registry_register_module(). The old static OWL_NATIVE_CATALOG[]
 * has been eliminated after the Phase 4 migration.
 * ================================================================ */

/* Build a "Name(p1, p2, ...)" display signature for a native.
 * name lets us swap in the English alias when generating EN docs. */
static const char* def_signature_display_named(const OwlNativeDef* d,
                                               const char* name,
                                               char* buf, int bufSize) {
    const OwlSignature* sig = d->signatures;
    if (sig == NULL || sig[0].returnType == NULL) {
        snprintf(buf, bufSize, "%s()", name);
        return buf;
    }
    int off = snprintf(buf, bufSize, "%s(", name);
    const OwlParam* p = sig[0].params;
    bool first = true;
    if (p) {
        for (; p->name; p++) {
            if (!first && off < bufSize - 1) {
                off += snprintf(buf + off, bufSize - off, ", ");
            }
            off += snprintf(buf + off, bufSize - off, "%s", p->name);
            first = false;
        }
    }
    if (off < bufSize - 1) snprintf(buf + off, bufSize - off, ")");
    return buf;
}

static const char* def_signature_display(const OwlNativeDef* d, char* buf, int bufSize) {
    return def_signature_display_named(d, d->canonical, buf, bufSize);
}





/* ================================================================
 * VSCODE DATA -- structured JSON for plugin generation
 * ================================================================ */

/* Check if a token type belongs to a category */
static bool is_control_token(TokenType t) {
    return t == TOKEN_SI || t == TOKEN_ALORS || t == TOKEN_SINON ||
           t == TOKEN_FIN || t == TOKEN_POUR || t == TOKEN_A ||
           t == TOKEN_TOUT || t == TOKEN_TANTQUE || t == TOKEN_BOUCLE ||
           t == TOKEN_FAIRE || t == TOKEN_SELON || t == TOKEN_CAS ||
           t == TOKEN_AUTRE || t == TOKEN_SORTIR || t == TOKEN_CONTINUER ||
           t == TOKEN_RENVOYER || t == TOKEN_RETOUR ||
           t == TOKEN_AVEC || t == TOKEN_QUAND || t == TOKEN_EXCEPTION ||
           t == TOKEN_PAS;
}

static bool is_declaration_token(TokenType t) {
    return t == TOKEN_EST || t == TOKEN_SONT || t == TOKEN_DE ||
           t == TOKEN_DANS || t == TOKEN_SUR || t == TOKEN_CONSTANTE ||
           t == TOKEN_LOCAL || t == TOKEN_GLOBAL ||
           t == TOKEN_LIBERER || t == TOKEN_ELEMENT || t == TOKEN_SEPAREE ||
           t == TOKEN_PROCEDURE || t == TOKEN_FONCTION ||
           t == TOKEN_CLASSE || t == TOKEN_STRUCTURE || t == TOKEN_ENUMERATION ||
           t == TOKEN_COMBINAISON || t == TOKEN_INTERFACE ||
           /* DSL Fenetre v1 (Phase F) : Fenetre/Libelle/Bouton/Saisie sont
            * des keywords de declaration UI. Aliases EN inclus via la table
            * scanner (Window/Label/Button/Input). */
           t == TOKEN_FENETRE || t == TOKEN_LIBELLE ||
           t == TOKEN_BOUTON  || t == TOKEN_SAISIE;
}

static bool is_oop_token(TokenType t) {
    return t == TOKEN_HERITE ||
           t == TOKEN_IMPLEMENTE ||
           t == TOKEN_CONSTRUCTEUR || t == TOKEN_DESTRUCTEUR ||
           t == TOKEN_OBJET || t == TOKEN_ANCETRE ||
           t == TOKEN_DYNAMIQUE;
}

static bool is_visibility_token(TokenType t) {
    return t == TOKEN_PUBLIC || t == TOKEN_PRIVE || t == TOKEN_PROTEGE ||
           t == TOKEN_GLOBALE;
}

static bool is_logical_token(TokenType t) {
    return t == TOKEN_ET || t == TOKEN_OU || t == TOKEN_ET_SC ||
           t == TOKEN_OU_SC || t == TOKEN_MODULO || t == TOKEN_DANS_SC;
}

static bool is_type_token_vscode(TokenType t) {
    return t == TOKEN_BOOLEEN || t == TOKEN_ENTIER || t == TOKEN_REEL ||
           t == TOKEN_MONETAIRE || t == TOKEN_NUMERIQUE || t == TOKEN_CHAINE ||
           t == TOKEN_CARACTERE || t == TOKEN_DATE || t == TOKEN_HEURE ||
           t == TOKEN_DATEHEURE || t == TOKEN_DUREE || t == TOKEN_BUFFER ||
           t == TOKEN_JSON_KW || t == TOKEN_UUID ||
           t == TOKEN_SANS ||
           t == TOKEN_SIGNE || t == TOKEN_TABLEAU || t == TOKEN_FILE ||
           t == TOKEN_PILE || t == TOKEN_LISTE || t == TOKEN_ASSOCIATIF ||
           t == TOKEN_FIXE || t == TOKEN_PAR;
}

static bool is_constant_token(TokenType t) {
    return t == TOKEN_VRAI || t == TOKEN_FAUX || t == TOKEN_NUL;
}

/* Print a JSON array of strings from keyword table filtered by predicate.
 * Emet a la fois les formes d'entree (cle table scan, ASCII) ET la forme
 * canon FR (token_type_name) — cette derniere porte les accents
 * (« Énumération », « PRIVÉ », « PROTÉGÉ »). La grammar VSCode est
 * insensible a la casse mais PAS aux accents : sans la forme canon
 * accentuee, les mots-cles accentues ne sont pas colores (bug 0156).
 * Dedup case-sensitive intra-appel pour ne pas dupliquer une forme deja
 * emise (ex: cle « public » + canon « PUBLIC » coexistent ; « PUBLIC »
 * n'est pas reemis deux fois). */
static void print_filtered_keywords(bool (*predicate)(TokenType)) {
    /* Buffer de dedup : pointeurs vers les strings deja emises (cles de la
     * table scan, statiques) + formes canon (statiques aussi). Compare par
     * contenu. Taille large : la plus grosse categorie reste < 64 entrees. */
    const char* emitted[256];
    int emittedCount = 0;
    printf("[");
    bool first = true;
    for (int i = 0; ; i++) {
        TokenType type;
        const char* name = scanner_keyword_at(i, &type);
        if (name == NULL) break;
        if (!predicate(type)) continue;

        /* Forme d'entree (cle scan). */
        if (!first) printf(",");
        printf("\"%s\"", name);
        first = false;
        if (emittedCount < 256) emitted[emittedCount++] = name;

        /* Forme canon FR (token_type_name) — emise si distincte des formes
         * deja sorties (porte les accents pour la grammar). */
        const char* canon = token_type_name(type);
        if (canon != NULL) {
            bool seen = false;
            for (int k = 0; k < emittedCount; k++) {
                if (strcmp(emitted[k], canon) == 0) { seen = true; break; }
            }
            if (!seen) {
                printf(",\"%s\"", canon);
                if (emittedCount < 256) emitted[emittedCount++] = canon;
            }
        }
    }
    printf("]");
}

static void json_escape(FILE* f, const char* s);  /* forward — defini plus bas */

/* Mapping OwlPropType → string JSON-friendly pour --vscode-data. */
static const char* prop_type_json(OwlPropType t) {
    switch (t) {
        case OWL_PROP_TYPE_INT:      return "int";
        case OWL_PROP_TYPE_STRING:   return "string";
        case OWL_PROP_TYPE_BOOL:     return "bool";
        case OWL_PROP_TYPE_VARIANT:  return "tout";
        case OWL_PROP_TYPE_POSITION: return "position";
        case OWL_PROP_TYPE_COLOR:    return "color";
        case OWL_PROP_TYPE_ENUM:     return "enum";
        case OWL_PROP_TYPE_MULTILINGUE: return "multilingue";
        case OWL_PROP_TYPE_OBJECT:   return "object";
    }
    return "unknown";
}

/* Emet la paire JSON "defaultValue":<v> + "defaultKind":"<k>" pour une
 * prop DSL ( Veine Y). La valeur suit le kind :
 *   UNSET / NULL -> null      (pas de valeur connue)
 *   AUTO         -> "auto"    (sentinelle derive-du-moteur)
 *   BOOL         -> true|false
 *   INT          -> nombre
 *   ENUM / STRING-> chaine
 * defaultKind preserve la distinction unset/null/auto pour les
 * consommateurs qui en ont besoin (l'expansion JSON les confondrait). */
static void emit_prop_default_json(const OwlPropDefault* d) {
    printf(",\"defaultValue\":");
    switch (d->kind) {
        case OWL_PROP_DEFAULT_BOOL:   printf(d->i ? "true" : "false"); break;
        case OWL_PROP_DEFAULT_INT:    printf("%d", d->i); break;
        case OWL_PROP_DEFAULT_AUTO:   printf("\"auto\""); break;
        case OWL_PROP_DEFAULT_ENUM:
        case OWL_PROP_DEFAULT_STRING: json_escape(stdout, d->s ? d->s : ""); break;
        case OWL_PROP_DEFAULT_NULL:
        case OWL_PROP_DEFAULT_UNSET:
        default:                      printf("null"); break;
    }
    printf(",\"defaultKind\":");
    json_escape(stdout, owl_prop_default_kind_name(d->kind));
}

/* ============================================================
 * --credits : attribution des composants tiers affichee par le CLI.
 *
 * Affiche ici l'attribution CC-BY-3.0 de Game-Icons, requise par sa
 * licence. Les notices completes des bibliotheques tierces (Qt6, qpdf,
 * libs MIT/BSD/Zlib/CC0, autres jeux d'icones) vivent dans le fichier
 * THIRD-PARTY-NOTICES.
 * ============================================================ */
static void print_credits(void) {
    printf("OWL — Credits tiers\n");
    printf("\n");
    printf("Game-Icons   CC-BY-3.0   Lorc, Delapouite et autres contributeurs.\n");
}

/* Non-static : le garde bilingue des catalogues (,
 * internal_tests/catalog_bilingual_test.c) capture la sortie de cet export
 * in-process pour la confronter au registre. Idem dump_lsp_catalog. */
void print_vscode_data(void) {
    printf("{\n");
    printf("  \"version\": \"%s\",\n", OWL_VERSION);

    /* Keywords by category */
    printf("  \"keywords_control\": ");
    print_filtered_keywords(is_control_token);
    printf(",\n");

    printf("  \"keywords_declaration\": ");
    print_filtered_keywords(is_declaration_token);
    printf(",\n");

    printf("  \"keywords_oop\": ");
    print_filtered_keywords(is_oop_token);
    printf(",\n");

    printf("  \"keywords_visibility\": ");
    print_filtered_keywords(is_visibility_token);
    printf(",\n");

    printf("  \"keywords_logical\": ");
    print_filtered_keywords(is_logical_token);
    printf(",\n");

    printf("  \"types\": ");
    print_filtered_keywords(is_type_token_vscode);
    printf(",\n");

    /* Constants (Vrai/Faux/Null from keywords + psd* from runtime) */
    printf("  \"constants_keywords\": ");
    print_filtered_keywords(is_constant_token);
    printf(",\n");

    /* Elision types (d'entiers, d'entieres, etc.) */
    printf("  \"elision_types\": [\"entiers\",\"entiere\",\"entieres\",\"chaines\",\"booleens\",\"booleennes\",\"reels\",\"monetaires\",\"numeriques\",\"caracteres\",\"dates\",\"heures\",\"durees\",\"buffers\",\"variants\",\"uuids\",\"octets\",\"jsons\"],\n");

    /* Boot the VM first — this populates the registry AND vm.globals. */
    vm_init();

    /*  — police de reference des planchers (section ui_min_floors plus
     * bas). Capturee ICI, tot : elle est lue sur le template Police de vm.types,
     * qui n'est pas racine du GC ; les grosses sections JSON qui suivent
     * declenchent une collecte qui le libere, et un clone tardif rendrait NIL. */
    int floor_font_pt = -1;
#ifdef OWL_HAS_GUI
    floor_font_pt = owl_ui_default_police_taille();
#endif

    /* constants_runtime is now derived from the registry (OwlConstDef entries
     * registered by stdlib_builtin_types.c and stdlib_json.c) + all enum values. */
    printf("  \"constants_runtime\": [");
    {
        int nc = owl_registry_consts_count();
        for (int i = 0; i < nc; i++) {
            const OwlConstDef* c = owl_registry_const_at(i);
            if (i > 0) printf(",");
            json_escape(stdout, c->name);
            /* Emit the EN canon alias too (invariant bilingue n°3) so the
             * regenerated TextMate grammar colours EN forms (dispFree, …) as
             * constants. Enum members live here as OwlConstDef entries, so
             * their EN form is reachable without a valuesEN field on OwlEnum. */
            if (c->nameEN) {
                printf(",");
                json_escape(stdout, c->nameEN);
            }
        }
        /* Add all enum values (owlTypes values, httpMéthodes values, etc.) */
        int ne = owl_registry_enums_count();
        for (int i = 0; i < ne; i++) {
            const OwlEnum* e = owl_registry_enum_at(i);
            if (e->values) {
                for (const char* const* v = e->values; *v != NULL; v++) {
                    printf(",");
                    json_escape(stdout, *v);
                }
            }
        }
    }
    printf("],\n");

    /* builtin_types is derived from OwlTypeDef entries (registered by
     * stdlib_builtin_types.c) — canonical FR name, accented, with field list. */
    printf("  \"builtin_types\": [\n");
    {
        int nt = owl_registry_types_count();
        for (int i = 0; i < nt; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (i > 0) printf(",\n");
            printf("    {\"name\":");
            json_escape(stdout, t->canonical);
            /* Alias EN canon du type (chaîneMultilingue -> multilingualString,
             * httpRequête -> httpRequest). Sans lui le plugin ne peut colorer
             * que la forme FR : un projet ecrit en anglais n'a aucun type
             * surligne. Emis seulement s'il differe du canon FR, comme les
             * autres exports (feuilles composees, natives). */
            if (t->nameEN && *t->nameEN && strcmp(t->nameEN, t->canonical) != 0) {
                printf(",\"nameEN\":");
                json_escape(stdout, t->nameEN);
            }
            printf(",\"description\":");
            json_escape(stdout, t->descFR ? t->descFR : "");
            printf(",\"fields\":[");
            bool firstF = true;
            if (t->fields) {
                for (const OwlField* f = t->fields; f->name != NULL; f++) {
                    if (!firstF) printf(",");
                    firstF = false;
                    /* Per-leaf metadata (type/default/enum) so designers can
                     * build dropdowns for constrained leaves (ex. Cadre.Trait
                     * -> traitCadre). Same shape as dump_doc_html. The display
                     * form is the accented variant when set — what the
                     * formatter and external tools (audit, doc gen) expect. */
                    printf("{\"name\":");
                    json_escape(stdout, f->nameAccented ? f->nameAccented : f->name);
                    /* Alias EN canon de la feuille (Libellé->Caption pour un
                     * bouton, Cadre->Border, Souligné->Underlined). Le designer
                     * emet la cle cascade en EN quand owl.language = en. Emis
                     * uniquement si present (les feuilles sans alias EN sont
                     * rares mais possibles), meme forme que le dump props. */
                    if (f->nameEN && *f->nameEN) {
                        printf(",\"nameEN\":");
                        json_escape(stdout, f->nameEN);
                    }
                    printf(",\"type\":");
                    json_escape(stdout, f->type ? f->type : "");
                    /*  — signature contractuelle d'un field callback
                     * (type:"procédure") : le designer/IDE l'affiche pour guider
                     * l'ecriture de la procedure. Emis pour tout field qui la
                     * porte (NULL = field non-callback, cle omise). */
                    if (f->callbackSig) {
                        printf(",\"callbackSig\":");
                        json_escape(stdout, f->callbackSig);
                    }
                    if (f->defaultValue) {
                        printf(",\"defaultValue\":");
                        json_escape(stdout, f->defaultValue);
                    }
                    const OwlEnum* en = f->allowedBase ? f->allowedBase : f->allowedOptions;
                    if (en) {
                        printf(",\"enum\":");
                        json_escape(stdout, en->name);
                        if (en->values) {
                            printf(",\"enum_values\":[");
                            bool firstV = true;
                            for (const char* const* v = en->values; *v; v++) {
                                if (!firstV) printf(",");
                                firstV = false;
                                json_escape(stdout, *v);
                            }
                            printf("]");
                            /* Formes EN paralleles (meme ordre/longueur) : le
                             * designer affiche l'EN des valeurs d'enum quand
                             * owl.language = en. nameEN de la constante membre,
                             * fallback sur la valeur FR si absent. */
                            printf(",\"enum_values_en\":[");
                            bool firstVE = true;
                            for (const char* const* v = en->values; *v; v++) {
                                if (!firstVE) printf(",");
                                firstVE = false;
                                const OwlConstDef* cst = owl_registry_const_find(*v);
                                json_escape(stdout, (cst && cst->nameEN && *cst->nameEN)
                                                        ? cst->nameEN : *v);
                            }
                            printf("]");
                        }
                    }
                    printf("}");
                }
            }
            printf("]");
            if (t->literalKeysEnum) {
                printf(",\"literalKeysEnum\":");
                json_escape(stdout, t->literalKeysEnum->name);
            }
            printf("}");
        }
    }
    printf("\n  ],\n");

    /* "natives" section — documented functions. Sourced from the registry
     * first, then falls back to the legacy OWL_NATIVE_CATALOG[] for any
     * function not yet migrated. During the migration this keeps the
     * output byte-identical for catalog-only functions while exposing
     * registry-only ones (richer metadata) additively. */
    printf("  \"natives\": [\n");
    {
        bool firstEntry = true;
        int rn = owl_registry_count();
        for (int i = 0; i < rn; i++) {
            const OwlNativeDef* d = owl_registry_at(i);
            if (d->flags & OWL_NAT_INTERNAL) continue;
            char sig[128];
            def_signature_display(d, sig, sizeof sig);
            if (!firstEntry) printf(",\n");
            printf("    {\"category\":");
            json_escape(stdout, d->category ? d->category : "Divers");
            printf(",\"nameFR\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            json_escape(stdout, d->nameEN ? d->nameEN : d->canonical);
            printf(",\"signature\":");
            json_escape(stdout, sig);
            printf("}");
            firstEntry = false;
        }
    }
    printf("\n  ],\n");

    /* Extension methods — sourced from owl_methods.c via method_catalog_*. */
    printf("  \"methods\": [\n");
    {
        bool firstM = true;
        int total = method_catalog_count();
        for (int i = 0; i < total; i++) {
            const char* typeName = NULL;
            const MethodEntry* m = method_catalog_at(i, &typeName);
            if (!m || !m->name || !typeName) continue;
            if (!firstM) printf(",\n");
            printf("    {\"type\":");
            json_escape(stdout, typeName);
            printf(",\"name\":");
            json_escape(stdout, m->name);
            printf(",\"arity\":%d}", m->arity);
            firstM = false;
        }
    }
    printf("\n  ],\n");

    printf("  \"natives_runtime\": [");
    {
        bool firstN = true;
        for (int i = 0; i < vm.globals.capacity; i++) {
            Entry* entry = &vm.globals.entries[i];
            if (entry->key == NULL) continue;
            if (!IS_NATIVE(entry->value)) continue;
            /* Skip internal names starting with __ */
            if (entry->key->length >= 2 &&
                entry->key->chars[0] == '_' && entry->key->chars[1] == '_') continue;
            if (!firstN) printf(",");
            json_escape(stdout, entry->key->chars);
            firstN = false;
        }
    }
    printf("],\n");

    /* Runtime-registered builtin types (ObjInstance templates registered by
     * register_builtin_types(): httpRequete, httpReponse, Chrono, Definition).
     * Must be highlighted as TYPES (storage.type), not as functions.
     *  : les templates vivent dans vm.types (namespace separe). */
    printf("  \"builtin_types_runtime\": [");
    {
        bool firstT = true;
        for (int i = 0; i < vm.types.capacity; i++) {
            Entry* entry = &vm.types.entries[i];
            if (entry->key == NULL) continue;
            if (!IS_INSTANCE(entry->value)) continue;
            if (entry->key->length >= 2 &&
                entry->key->chars[0] == '_' && entry->key->chars[1] == '_') continue;
            if (!firstT) printf(",");
            json_escape(stdout, entry->key->chars);
            firstT = false;
        }
    }
    printf("],\n");

    /* Special global variables (not natives, not types) — tracked by the
     * runtime for user code to read. Highlight them distinctly. */
    printf("  \"erreur_globals\": [\"ErreurDetectee\"],\n");

    /* Phase B : section dsl_properties.
     *
     * Pour chaque proprietee DSL (Largeur, PoliceNom, Format, etc.) emet :
     *   - kind        : "Fenetre" / "Libelle" / "Bouton" / "Saisie" / "Table" /
     *                   "Etat" / "blocEtat" / "LibelleEtat"
     *   - name        : nom canon FR (avec accents)
     *   - nameAscii   : alias sans accent (si different)
     *   - nameEN      : nom EN canon
     *   - type        : "int" / "string" / "bool" / "color" / "enum" / "tout"
     *   - enum_name   : nom de l'OwlEnum (si enum_name fourni, meme pour
     *                   type=string avec hint d'autocomplete)
     *   - enum_values : tableau resolu des valeurs (si l'enum existe)
     *   - enum_combinable : true si combinable bitwise
     *   - descFR / descEN : descriptions
     *
     * Consume par : generate_vscode.py (regex dsl-properties dynamique +
     * generation prop_registry_gen.js pour designers JS). */
    printf("  \"dsl_properties\": [\n");
    {
        int n = owl_dsl_props_count();
        for (int i = 0; i < n; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (!e || !e->def || !e->def->nameFR) continue;
            if (i > 0) printf(",\n");
            printf("    {");
            printf("\"kind\":");        json_escape(stdout, e->kind);
            printf(",\"name\":");       json_escape(stdout, e->def->nameFR);
            if (e->def->nameAscii) {
                printf(",\"nameAscii\":"); json_escape(stdout, e->def->nameAscii);
            }
            printf(",\"nameEN\":");
            json_escape(stdout, e->def->nameEN ? e->def->nameEN : "");
            printf(",\"type\":");       json_escape(stdout, prop_type_json(e->def->type));
            if (e->def->enum_name) {
                printf(",\"enum_name\":");
                json_escape(stdout, e->def->enum_name);
                const OwlEnum* en = owl_registry_enum_find(e->def->enum_name);
                if (en && en->values) {
                    printf(",\"enum_values\":[");
                    bool firstV = true;
                    for (const char* const* v = en->values; *v; v++) {
                        if (!firstV) printf(",");
                        json_escape(stdout, *v);
                        firstV = false;
                    }
                    printf("]");
                    /* Formes EN paralleles (meme ordre/longueur) : le designer
                     * affiche l'EN des valeurs de combo quand owl.language = en.
                     * nameEN de la constante membre, fallback FR si absent. */
                    printf(",\"enum_values_en\":[");
                    bool firstVE = true;
                    for (const char* const* v = en->values; *v; v++) {
                        if (!firstVE) printf(",");
                        firstVE = false;
                        const OwlConstDef* cst = owl_registry_const_find(*v);
                        json_escape(stdout, (cst && cst->nameEN && *cst->nameEN)
                                                ? cst->nameEN : *v);
                    }
                    printf("]");
                    if (en->combinable) printf(",\"enum_combinable\":true");
                }
            }
            /*  : prop composee (OWL_PROP_TYPE_OBJECT) — expose le
             * type-cible pour l'autocomplete cascade cote VSCode. */
            if (e->def->object_type) {
                printf(",\"object_type\":");
                json_escape(stdout, e->def->object_type);
            }
            /*  : etat runtime (lu/ecrit a l'execution, pas une option
             * de design). Le designer exclut ces props de son inspector. */
            if (e->def->runtime_only) printf(",\"runtime_only\":true");
            printf(",\"descFR\":");
            json_escape(stdout, e->def->descFR ? e->def->descFR : "");
            printf(",\"descEN\":");
            json_escape(stdout, e->def->descEN ? e->def->descEN : "");
            emit_prop_default_json(&e->def->dft);
            printf("}");
        }
    }
    printf("\n  ],\n");

    /*  : section presentation (couche 2).
     *
     * A cote de dsl_properties (semantique pure), expose l'intention de
     * presentation du designer pour les widgets couverts (pilote : Saisie).
     * Chaque entree (cle-valeur reutilisable cote front) porte :
     *   - kind        : widget ("Saisie")
     *   - name        : nom de la prop (cle vers dsl_properties)
     *   - labelFR/EN  : libelle court d'affichage bilingue (entrees designer)
     *   - group       : regroupement UX abstrait (identite/contenu/ui/
     *                   disposition/apparence/comportement/aide/avance)
     *   - importance  : "courant" | "avance"
     *   - editor      : type d'editeur abstrait (text/int/bool/enum/color/font/
     *                   image/multilangue/composite/position/variant)
     *   - order       : ordre d'affichage dans le groupe
     *   - visibility  : "designer" | "cache"
     *   - enum_subset : sous-ensemble de valeurs d'enum (optionnel, ex Etat)
     *
     * Le mapping group -> onglet concret (Aurora) vit cote front (couche 3),
     * pas ici. Source : src/ui_presentation.c. */
    printf("  \"presentation\": [\n");
    owl_presentation_emit_json(stdout);
    printf("\n  ],\n");

    /*  (etage 2) — feuilles des types composites exposes. Objet cle
     * par type composite : pour chaque feuille, son editeur abstrait. Le front
     * lit ce bloc pour rendre l'accordeon d'un composite (ex Police) sans
     * deviner l'editeur d'une feuille (Police.Couleur -> color, non inferable
     * depuis le stockage RVB entier). Source : src/ui_presentation.c. */
    printf("  \"composite_leaves\": {\n");
    owl_presentation_leaves_emit_json(stdout);
    printf("\n  },\n");

    /*  — modele d'etats du designer (refonte ). Le style est
     * un struct de sous-elements ; le crochet [etat] vit sur le membre d'etats
     * d'un sous-element, plus a la racine. Deux sections derivees :
     *   - style_elements : par type de style RACINE -> ses sous-elements
     *                      (ZoneSaisie/Libellé/CadreExtérieur pour styleSaisie).
     *   - element_states : par sous-element -> sa palette d'etats (vide si le
     *                      sous-element n'en porte pas ; seule ZoneSaisie en a).
     * L'iteration part de compiler_style_model_* (types racine, grossit avec un
     * futur styleBouton), derive les sous-elements depuis le registre de types,
     * et relie chaque sous-element a sa palette via le type de son membre
     * collection porteur d'etats (STYLE_PALETTES, compiler.c). Les slots template
     * internes (_Survol...) restent prives. */
    printf("  \"style_elements\": {\n");
    {
        int nm = compiler_style_model_count();
        bool firstModel = true;
        for (int i = 0; i < nm; i++) {
            const OwlTypeDef* root = owl_registry_type_find(compiler_style_model_type_at(i));
            if (root == NULL || root->fields == NULL) continue;
            if (!firstModel) printf(",\n");
            firstModel = false;
            printf("    ");
            json_escape(stdout, root->canonical);
            printf(": [");
            bool firstSub = true;
            for (const OwlField* f = root->fields; f->name; f++) {
                if (!firstSub) printf(", ");
                firstSub = false;
                json_escape(stdout, f->nameAccented ? f->nameAccented : f->name);
            }
            printf("]");
        }
    }
    printf("\n  },\n");

    /* element_states (legacy transition) : keye par NOM d'affichage du
     * sous-element. Collision possible quand deux widgets partagent un nom de
     * sous-element aux etats divergents (ex "Libellé" : stateful sur le Bouton,
     * statique sur la Saisie). Le front migre vers element_states_by_type puis
     * ce bloc disparait ( volet 2). */
    printf("  \"element_states\": {\n");
    owl_presentation_element_states_emit_json(stdout, false);
    printf("\n  },\n");

    /* element_states_by_type : meme contenu keye par TYPE composite
     * du sous-element (ex "styleZoneBouton"). Cle non-collisionnante : deux
     * sous-elements homonymes ont des types distincts, donc leurs palettes ne
     * s'ecrasent plus. C'est la source que le front doit consommer (le pont
     * ELEMENT_STATES_BY_TYPE cote front devient inutile). Valeur = tableau
     * [ {fr,en}, ... ] des etats (vide si le sous-element n'en porte pas). */
    printf("  \"element_states_by_type\": {\n");
    owl_presentation_element_states_emit_json(stdout, true);
    printf("\n  },\n");

    /*  — menu_style_roots : mapping racine de menu -> type de style.
     * Les racines MenuPrincipal/MenuContextuel ne sont PAS des kinds DSL (blocs
     * non-materialises, pas d'ObjUiControl, donc pas de prop Style qui porterait
     * l'object_type comme sur un widget). Le front derive quand meme leur
     * structure de style depuis le TYPE (styleMenuPrincipal / styleMenuContextuel,
     * exposes dans style_elements ci-dessus) ; il lui manquait juste le pont
     * racine -> type. Cette section le fournit. Source unique : accesseur couche 2
     * owl_presentation_menu_style_root_* (ui_presentation.c), aucun hardcode ici.
     * Forme FR canon de la racine -> nom du type de style. */
    printf("  \"menu_style_roots\": {");
    {
        int nr = owl_presentation_menu_style_root_count();
        const char* rootFR = NULL;
        const char* rootEN = NULL;
        const char* styleType = NULL;
        for (int i = 0; i < nr; i++) {
            if (!owl_presentation_menu_style_root_at(i, &rootFR, &rootEN, &styleType))
                continue;
            if (i > 0) printf(",");
            printf(" ");
            json_escape(stdout, rootFR);
            printf(": ");
            json_escape(stdout, styleType);
        }
    }
    printf(" },\n");

    /*  : section languages.
     *
     * Expose le catalogue des constantes langueXxx (34 entrées) pour
     * l'éditeur multilingue du designer, qui listait jusqu'ici une poignée
     * de langues hardcodées. Pour chaque langue :
     *   - name      : nom canon de la constante ("langueFrançais")
     *   - nameAscii : alias ASCII sans accent (omis si identique au canon)
     *   - nameEN    : alias EN canon ("languageFrench"), omis si absent
     *   - tag       : tag BCP 47 / RFC 5646 ("fr-FR")
     *   - label     : libellé lisible dérivé du nom canon (préfixe "langue"
     *                 retiré, accents conservés — "Français")
     *
     * Source unique : table statique de stdlib_i18n.c (rien d'inventé ici).
     * Consommé par generate_vscode.py → window.OWL_LANGUAGES (côté plugin). */
    printf("  \"languages\": [\n");
    {
        int nl = owl_i18n_langue_count();
        for (int i = 0; i < nl; i++) {
            const OwlLangueInfo* l = owl_i18n_langue_at(i);
            if (!l || !l->name) continue;
            if (i > 0) printf(",\n");
            printf("    {\"name\":");
            json_escape(stdout, l->name);
            if (l->nameAscii) {
                printf(",\"nameAscii\":");
                json_escape(stdout, l->nameAscii);
            }
            if (l->nameEN) {
                printf(",\"nameEN\":");
                json_escape(stdout, l->nameEN);
            }
            printf(",\"tag\":");
            json_escape(stdout, l->bcp47 ? l->bcp47 : "");
            /* label = nom canon sans le préfixe "langue" (accents conservés). */
            const char* label = l->name;
            if (strncmp(label, "langue", 6) == 0) label += 6;
            printf(",\"label\":");
            json_escape(stdout, label);
            printf("}");
        }
    }
    printf("\n  ],\n");

    /*  — section colors.
     *
     * Expose le catalogue des couleurs prédéfinies (membres de l'enum
     * couleur) sous forme nom FR/EN → hex CSS "#RRGGBB", pour que le picker
     * du designer arrête de recopier la table à la main (dérive nom→hex).
     * Pour chaque couleur :
     *   - fr       : nom canon FR accentué ("Rouge", "RougeFoncé")
     *   - en       : nom canon EN ("Red"), omis si absent
     *   - hex      : "#RRGGBB" (conversion BGR→RGB via owl_color_bgr_to_html,
     *                partagée avec RVBVersHTML), ou null si sentinelle
     *   - sentinel : true pour les couleurs sans RGB fixe (Transparent,
     *                CouleurDéfaut, iSystème) — le front décide de l'affichage
     * Source unique : catalogue de stdlib_color.c (rien d'inventé ici). */
    printf("  \"colors\": [\n");
    {
        int ncol = owl_color_catalog_count();
        for (int i = 0; i < ncol; i++) {
            const OwlColorInfo* col = owl_color_catalog_at(i);
            if (!col || !col->nameFR) continue;
            if (i > 0) printf(",\n");
            printf("    {\"fr\":");
            json_escape(stdout, col->nameFR);
            if (col->nameEN) {
                printf(",\"en\":");
                json_escape(stdout, col->nameEN);
            }
            char hex[16];
            if (owl_color_bgr_to_html(col->value, hex, sizeof hex)) {
                printf(",\"hex\":");
                json_escape(stdout, hex);
            } else {
                printf(",\"hex\":null,\"sentinel\":true");
            }
            printf("}");
        }
    }
    printf("\n  ],\n");

    vm_free();

    /* Accented type forms for TextMate grammar.
     * Includes both unaccented (e.g. "chaine") and accented canonical
     * ("chaîne") so both are highlighted as types. UTF-8 bytes are emitted
     * directly — callers must read stdin as UTF-8. */
    /* Niveau 2 : events DSL widget — source unique src/event_catalog_widgets.c.
     * Dumpe l'union des canonical/nameEN de tous les widget kinds connus. La
     * regex TextMate `procedure-declaration` consomme cette liste pour colorer
     * 'PROCEDURE surXxx' en bleu (support.function.event), idempotent quand
     * on ajoute un event au catalogue. */
    printf("  \"events_widget\": [\n");
    {
        OwlControlKind kinds[] = {
            OWL_CONTROL_LABEL, OWL_CONTROL_BUTTON, OWL_CONTROL_INPUT,
            OWL_CONTROL_TABLE, OWL_CONTROL_CONTAINER, OWL_CONTROL_IMAGE,
            OWL_CONTROL_LISTE, OWL_CONTROL_COMBO, OWL_CONTROL_SELECTEUR,
            OWL_CONTROL_INTERRUPTEUR, OWL_CONTROL_ZONE_REPETEE,
            OWL_CONTROL_ONGLET, OWL_CONTROL_WINDOW,
        };
        bool first = true;
        /* Dedup via comparaison string. Pour simplifier on accepte qq doublons :
         * generate_vscode.py les filtre via set(). */
        for (size_t kk = 0; kk < sizeof(kinds)/sizeof(kinds[0]); kk++) {
            int n = owl_event_catalog_count(kinds[kk]);
            for (int i = 0; i < n; i++) {
                const OwlEventDef* d = owl_event_catalog_at(kinds[kk], i);
                if (!d || !d->canonical) continue;
                if (!first) printf(",\n");
                first = false;
                printf("    {\"canonical\":");
                json_escape(stdout, d->canonical);
                printf(",\"nameEN\":");
                if (d->nameEN) json_escape(stdout, d->nameEN);
                else           printf("null");
                printf("}");
            }
        }
        printf("\n  ],\n");
    }

    /*  — events lifecycle Fenetre (catalogue C dedie, hors enum
     * OwlControlKind car sans payload type). Source unique consommee par
     * compiler.c::is_predefined_window_block + LSP autocomplete. Expose
     * ici pour que generate_vscode.py (cf ) puisse construire la
     * regex TextMate de coloration `support.function.event.owl` sans
     * hardcoder les noms. */
    printf("  \"events_window_lifecycle\": [\n");
    {
        int n = owl_window_lifecycle_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_window_lifecycle_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    /*  — events ETAT (top + sous-controle). Memes catalogues C que
     * compiler.c + cli_doc.c. Exposes ici pour generate_vscode.py : coloration
     * bleue (support.function.event.owl) de Filtre/Lecture/Fermeture (top) et
     * AvantImpression (sous-controle) sans hardcode cote generateur. */
    printf("  \"events_etat\": [\n");
    {
        int n = owl_etat_event_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_etat_event_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    printf("  \"events_etat_subcontrol\": [\n");
    {
        int n = owl_etat_subcontrol_event_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_etat_subcontrol_event_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    /*  — event lifecycle de la CollectionDeProcédures (Terminaison/
     * Termination). Meme catalogue C que le LSP ; expose ici pour que
     * generate_vscode.py colore l'event en bleu (support.function.event.owl),
     * a parite avec Fermeture/FinInitialisation, sans hardcode. */
    printf("  \"events_collection\": [\n");
    {
        int n = owl_collection_event_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_collection_event_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    /*  — events de niveau ServiceREST / OpérationHTTP.
     * Memes catalogues C que le compilateur + le LSP ; exposes ici pour que
     * generate_vscode.py colore surAuthentification/surErreur/... (service) et
     * surRequête (operation) en bleu (support.function.event.owl), a parite
     * avec Fermeture/FinInitialisation, sans hardcode cote generateur. */
    printf("  \"events_service\": [\n");
    {
        int n = owl_service_event_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_service_event_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    printf("  \"events_operation\": [\n");
    {
        int n = owl_operation_event_catalog_count();
        for (int i = 0; i < n; i++) {
            const OwlEventDef* d = owl_operation_event_catalog_at(i);
            if (!d || !d->canonical) continue;
            if (i > 0) printf(",\n");
            printf("    {\"canonical\":");
            json_escape(stdout, d->canonical);
            printf(",\"nameEN\":");
            if (d->nameEN) json_escape(stdout, d->nameEN);
            else           printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    /* Lot F : widget kinds + sous-blocs DSL (FR / ASCII / EN). Source unique
     * src/ui_decl.c::CONTROL_KINDS. Consomme par generate_vscode.py pour
     * la regex TextMate de coloration et l'auto-indent (au lieu de hardcoder
     * la liste cote Python). */
    printf("  \"widget_kinds\": [\n");
    {
        const OwlControlKindInfo* tbl = owl_control_kinds_table();
        bool first = true;
        for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
            if (!first) printf(",\n");
            first = false;
            printf("    {");
            printf("\"nameFR\":");    json_escape(stdout, k->nameFR);
            printf(",\"nameAscii\":");
            if (k->nameAscii) json_escape(stdout, k->nameAscii);
            else              printf("null");
            printf(",\"nameEN\":");   json_escape(stdout, k->nameEN ? k->nameEN : "");
            printf(",\"is_widget\":%s", k->is_widget ? "true" : "false");
            /*  — prefixe de nommage canon (.canonicalPrefix,
             *  : "TABLE_", "BTN_"...). Source unique cote owl ;
             * le designer (owl-integration) le lit ici au lieu de recopier
             * le canon a la main (drift ferme). null pour les sortes sans
             * convention (Fenetre, CodeBarres, sous-blocs structurels). */
            printf(",\"prefix\":");
            if (k->canonicalPrefix) json_escape(stdout, k->canonicalPrefix);
            else                    printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    /*  — plancher irreductible de chaque famille de champ : la taille
     * en dessous de laquelle un layout ne la descend jamais.
     *
     * Une fenetre declaree a 560 s'ouvre a 582 quand un champ impose plus que
     * ce qui est ecrit ; l'apercu de l'editeur doit dessiner la taille reelle,
     * pas celle du fichier. Les LOIS de composition (somme, max, espacement,
     * hauteur de texte derivee de la police) restent cote editeur : il sait les
     * appliquer et elles ne derivent pas. Ce qui derive, ce sont ces planchers
     * constants — d'ou leur mesure ici, a chaque build, plutot qu'un nombre
     * grave a la main dans le front (defaut vivant dix mois sur ).
     *
     * `optionSpacing` n'est renseigne que pour les familles qui empilent des
     * lignes d'option : l'editeur en a besoin pour composer leur hauteur.
     * `contentAgnostic` et `floorFontPt` publient deux choses que
     * l'editeur ecrivait encore a la main : la classification des familles dont
     * le plancher ne suit pas le contenu, et la police de mesure du plancher.
     *
     * Le garde-fou tests/ui_min_floors/run.sh confronte cette section a ce que
     * le moteur applique par le chemin complet. Sans lui, publier ne vaudrait
     * pas mieux que graver. */
    printf("  \"ui_min_floors\": [\n");
    {
        const OwlControlKindInfo* tbl = owl_control_kinds_table();
        bool first = true;
        for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
            if (!k->is_widget) continue;
            OwlControlKind kind;
            if (!owl_control_kind_of_info(k, &kind)) continue;
            int mw = 0, mh = 0, spacing = -1, agnostic = 0;
#ifdef OWL_HAS_GUI
            if (!owl_ui_min_floor_for_kind((int)kind, &mw, &mh, &spacing,
                                           &agnostic)) continue;
#else
            /* Build sans backend graphique (owl-cli) : aucun widget a mesurer,
             * la section reste vide plutot que de publier un chiffre invente. */
            continue;
#endif
            if (!first) printf(",\n");
            first = false;
            printf("    {");
            printf("\"kind\":");   json_escape(stdout, k->nameFR);
            printf(",\"kindEN\":"); json_escape(stdout, k->nameEN ? k->nameEN : "");
            printf(",\"minWidth\":%d,\"minHeight\":%d", mw, mh);
            printf(",\"optionSpacing\":");
            if (spacing >= 0) printf("%d", spacing); else printf("null");
            /*  — contentAgnostic remplace la liste ecrite a la main de
             * l'editeur (les familles dont le plancher ne suit pas le contenu) ;
             * floorFontPt est la police a laquelle le plancher a ete mesure, de
             * quoi extrapoler un plancher qui suit sa police. */
            printf(",\"contentAgnostic\":%s", agnostic ? "true" : "false");
            printf(",\"floorFontPt\":");
            if (floor_font_pt > 0) printf("%d", floor_font_pt); else printf("null");
            printf("}");
        }
        printf("\n  ],\n");
    }

    printf("  \"types_accented\": ["
           "\"chaine\",\"chaines\","
           "\"cha\xc3\xaene\",\"cha\xc3\xaenes\","                   /* chaîne / chaînes */
           "\"booleen\",\"booleenne\",\"booleens\",\"booleennes\","
           "\"bool\xc3\xa9""en\",\"bool\xc3\xa9""enne\",\"bool\xc3\xa9""ens\",\"bool\xc3\xa9""ennes\"," /* booléen... */
           "\"entiere\",\"entieres\","
           "\"enti\xc3\xa8re\",\"enti\xc3\xa8res\","                 /* entière / entières */
           "\"monetaire\",\"mon\xc3\xa9taire\","
           "\"numerique\",\"num\xc3\xa9rique\","
           "\"caractere\",\"caract\xc3\xa8re\","                     /* caractère */
           "\"duree\",\"dur\xc3\xa9""e\","                           /* durée */
           "\"reel\",\"reels\","
           "\"r\xc3\xa9""el\",\"r\xc3\xa9""els\""                    /* réel / réels */
           "],\n");

    /* Limites compile-time du compilateur OWL — single source owl_limits.def.
     * Expose le catalogue pour outils externes (doc generation, audit, plugins
     * VSCode/IDE). Cf. . */
    printf("  \"limits\": [\n");
    {
        int nl = owl_registry_limit_count();
        for (int i = 0; i < nl; i++) {
            const OwlLimitDef* L = owl_registry_limit_at(i);
            if (!L) continue;
            if (i > 0) printf(",\n");
            printf("    {\"name\":");
            json_escape(stdout, L->name);
            printf(",\"value\":%d", L->value);
            printf(",\"category\":");
            json_escape(stdout, L->category ? L->category : "");
            printf(",\"descFR\":");
            json_escape(stdout, L->descFR ? L->descFR : "");
            printf(",\"descEN\":");
            json_escape(stdout, L->descEN ? L->descEN : "");
            printf("}");
        }
    }
    printf("\n  ]\n");

    printf("}\n");
}

/* Emit a C-string literal, escaping quotes and backslashes. */
static void emit_c_string(const char* s) {
    if (s == NULL) { printf("NULL"); return; }
    putchar('"');
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { putchar('\\'); putchar('"'); }
        else if (c == '\\') { putchar('\\'); putchar('\\'); }
        else if (c == '\n') { putchar('\\'); putchar('n'); }
        else if (c == '\r') { putchar('\\'); putchar('r'); }
        else if (c == '\t') { putchar('\\'); putchar('t'); }
        else if (c < 0x20)  { printf("\\x%02x\"\"", c); }
        else                { putchar(*p); }
    }
    putchar('"');
}

/*  (L3.5) — un champ typé couleur (registry : type "couleur", ex
 * Image.CouleurRemplissage ; OU allowedBase == enum "couleur", ex CouleurFond
 * qui est un entier BGR) est exposé au LSP comme sous-type composé Couleur.
 * Le template runtime ne stocke qu'un entier (fieldType "int" par défaut) donc
 * l'autocomplete du littéral inline `CouleurFond = { <curseur> }` ne verrait
 * pas les champs Couleur ; on résout via la métadonnée registry du champ (même
 * source que la validation compile-time côté compiler.c). Renvoie "Couleur" si
 * le champ nommé est une couleur, sinon NULL. */
static const char* composed_field_color_subtype(const OwlTypeDef* td,
                                                 const char* fieldName) {
    if (td == NULL || fieldName == NULL) return NULL;
    for (const OwlField* f = td->fields; f && f->name; f++) {
        bool nameMatch = strcasecmp(f->name, fieldName) == 0
            || (f->nameAccented && strcasecmp(f->nameAccented, fieldName) == 0)
            || (f->nameEN && strcasecmp(f->nameEN, fieldName) == 0);
        if (!nameMatch) continue;
        if (f->type && strcasecmp(f->type, "couleur") == 0) return "Couleur";
        if (f->allowedBase && f->allowedBase->name
            && strcasecmp(f->allowedBase->name, "couleur") == 0) return "Couleur";
        return NULL;
    }
    return NULL;
}

/*  — forme EN canon d'un champ de type composé. Le template runtime ne
 * porte que le storage FR ; l'alias EN vit sur la déclaration OwlField du type
 * (source unique, la même qui sert la résolution EN->FR au runtime). Renvoie
 * NULL si le champ n'a pas de déclaration (template construit à la volée) ou
 * pas d'alias EN — l'appelant retombe alors sur la forme FR. */
static const char* composed_field_name_en(const OwlTypeDef* td,
                                          const char* fieldName) {
    if (td == NULL || fieldName == NULL) return NULL;
    for (const OwlField* f = td->fields; f && f->name; f++) {
        bool nameMatch = strcasecmp(f->name, fieldName) == 0
            || (f->nameAccented && strcasecmp(f->nameAccented, fieldName) == 0);
        if (!nameMatch) continue;
        if (f->nameEN && f->nameEN[0] && strcasecmp(f->nameEN, f->name) != 0)
            return f->nameEN;
        return NULL;
    }
    return NULL;
}

/* ================================================================
 * DUMP LSP CATALOG — generate a C source file with OWL_NATIVE_CATALOG[]
 *
 * The LSP executable is standalone and doesn't link the OWL runtime, so
 * it can't read the registry directly. Instead, we snapshot the registry
 * at build-time into a static C file the LSP compiles against.
 *
 * Usage:  owl --dump-lsp-catalog > lsp/lsp_catalog_gen.c
 *
 * Each OwlNativeDef in the registry becomes an OwlNativeInfo row (old
 * shape preserved for LSP compat).
 * ================================================================ */

/*  — table d'appartenance nom de propriete -> kinds accepteurs,
 * remplie avant l'emission par compiler_iter_builtin_props_kinds. Le dump
 * des lignes dedupe par nameFR : sans cette table le kind d'origine est
 * perdu et le LSP sert les 17 lignes a n'importe quel receveur primitif. */
typedef struct {
    char nameFR[64];
    char kinds[128];   /* " date dateheure " — ASCII minuscule, borne d'espaces */
} BuiltinPropKinds;
static BuiltinPropKinds g_bpKinds[64];
static int              g_bpKindsCount = 0;

/* Normalise un nom de kind en ASCII minuscule ("durée" -> "duree"), forme
 * sous laquelle le LSP compare son receveur (cf. is_primitive_prop_type). */
static void bp_kind_ascii(const char* kindName, char* out, size_t outSize) {
    int n = scanner_strip_accents(kindName, (int)strlen(kindName),
                                  out, (int)outSize);
    for (int i = 0; i < n; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
}

static void collect_builtin_prop_kind_cb(const char* kindName,
                                         const char* propNameFR, void* ctx) {
    (void)ctx;
    char kAscii[64];
    bp_kind_ascii(kindName, kAscii, sizeof(kAscii));
    char needle[68];
    snprintf(needle, sizeof(needle), " %s ", kAscii);

    BuiltinPropKinds* slot = NULL;
    for (int i = 0; i < g_bpKindsCount; i++) {
        if (strcmp(g_bpKinds[i].nameFR, propNameFR) == 0) { slot = &g_bpKinds[i]; break; }
    }
    if (!slot) {
        if (g_bpKindsCount >= (int)(sizeof(g_bpKinds)/sizeof(g_bpKinds[0]))) return;
        slot = &g_bpKinds[g_bpKindsCount++];
        snprintf(slot->nameFR, sizeof(slot->nameFR), "%s", propNameFR);
        snprintf(slot->kinds, sizeof(slot->kinds), " ");
    }
    if (strstr(slot->kinds, needle) != NULL) return;   /* deja note */
    size_t used = strlen(slot->kinds);
    snprintf(slot->kinds + used, sizeof(slot->kinds) - used, "%s ", kAscii);
}

static const char* builtin_prop_kinds_for(const char* nameFR) {
    for (int i = 0; i < g_bpKindsCount; i++) {
        if (strcmp(g_bpKinds[i].nameFR, nameFR) == 0) return g_bpKinds[i].kinds;
    }
    return " ";
}

/* Callback pour dump_lsp_catalog : emet une entree OwlBuiltinPropInfo. */
static void emit_builtin_prop_cb(const BuiltinKindProp* p, void* ctx) {
    int* count = (int*)ctx;
    printf("    {");
    emit_c_string(p->nameFR); printf(", ");
    emit_c_string(p->nameEN ? p->nameEN : p->nameFR); printf(", ");
    emit_c_string(p->descFR ? p->descFR : ""); printf(", ");
    emit_c_string(p->descEN ? p->descEN : (p->descFR ? p->descFR : "")); printf(", ");
    emit_c_string(p->valueType ? p->valueType : ""); printf(", ");
    emit_c_string(builtin_prop_kinds_for(p->nameFR));
    printf("},\n");
    (*count)++;
}

/* Resout le nom EN canonique d'un nom de type FR pour le catalogue LSP
 * (invariant bilingue 3). La donnee EN n'existe qu'au registry ; le LSP
 * standalone ne le linke pas, on la fige donc ici au dump.
 *
 *   - "rien" n'est PAS un type registry (`owl explain rien` = inconnu),
 *     mapping minimal explicite -> "nothing".
 *   - Les primitifs (entier/durée/booléen/chaîne/...) et les types
 *     composes sont resolus par owl_registry_type_find -> nameEN.
 *   - Fallback : le nom FR tel quel (jamais NULL en sortie). */
static const char* lsp_type_name_en(const char* frType) {
    if (!frType || !*frType) return frType;
    if (strcmp(frType, "rien") == 0) return "nothing";
    const OwlTypeDef* td = owl_registry_type_find(frType);
    if (td && td->nameEN && *td->nameEN) return td->nameEN;
    return frType;
}

/* Emet une signature individuelle au format OwlNativeSigInfo (struct
 * literal C99). Le caller a deja imprime "{" si necessaire ; ici on
 * imprime "{signature, returnType, returnTypeEN, paramCount, {params...}}".
 *
 * sig->arity dicte combien de params de sig->params[] sont actifs :
 *   - arity == -1 : variadique, tous les params declares
 *   - arity >= 0  : N params (utile pour les overloads ou un cb
 *                   distinct par arite — ex. Agrege 2/3 args partage
 *                   le meme params[] mais avec arite differente). */
static void emit_native_sig_struct(const char* nativeName,
                                    const OwlSignature* sig) {
    const OwlParam* params = sig->params;
    const char* retType    = sig->returnType ? sig->returnType : "tout";

    int declaredCount = 0;
    if (params) for (const OwlParam* p = params; p->name; p++) declaredCount++;
    int paramCount = (sig->arity == -1 || sig->arity > declaredCount)
                     ? declaredCount : sig->arity;

    /* Build display signature "Name(p1, p2, ...)" — uniquement les
     * paramCount premiers params (les "actifs" pour cette arite). */
    char sigbuf[256];
    int off = snprintf(sigbuf, sizeof sigbuf, "%s(", nativeName);
    if (params) {
        int idx = 0;
        for (const OwlParam* p = params; p->name && idx < paramCount; p++, idx++) {
            if (idx > 0 && off < (int)sizeof sigbuf - 2)
                off += snprintf(sigbuf + off, sizeof sigbuf - off, ", ");
            off += snprintf(sigbuf + off, sizeof sigbuf - off, "%s", p->name);
        }
    }
    if (off < (int)sizeof sigbuf - 1)
        snprintf(sigbuf + off, sizeof sigbuf - off, ")");

    printf("{");
    emit_c_string(sigbuf); printf(", ");
    emit_c_string(retType); printf(", ");
    emit_c_string(lsp_type_name_en(retType)); printf(", ");
    printf("%d, {", paramCount);
    if (params) {
        int idx = 0;
        for (const OwlParam* p = params; p->name && idx < paramCount; p++, idx++) {
            if (idx > 0) printf(", ");
            printf("{");
            emit_c_string(p->name); printf(", ");
            emit_c_string(p->type ? p->type : ""); printf(", ");
            emit_c_string(lsp_type_name_en(p->type ? p->type : "")); printf(", ");
            emit_c_string(p->desc ? p->desc : ""); printf(", ");
            if (p->allowedBase)    emit_c_string(p->allowedBase->name);
            else                   printf("0");
            printf(", ");
            if (p->allowedOptions) emit_c_string(p->allowedOptions->name);
            else                   printf("0");
            printf(", %d", p->acceptsLiterals ? 1 : 0);
            printf("}");
        }
    }
    printf("}}");
}

/* ================================================================
 * DOC HTML — navigateur de doc auto-contenu
 *
 * Lit un template HTML committé et injecte les données registry au
 * placeholder `//__OWL_DOC_DATA__` : un tableau JS `DATA` (natives,
 * types, enums, constantes + relations enum/param/type) + les listes
 * de mots pour la coloration. Sortie sur stdout, redirigée par le
 * build vers docs/reference/owl-doc.html.
 * ================================================================ */

/* Bloc DSL structurel scope keyword.control.dsl-properties cote grammaire
 * TextMate : PROPRIETES/PROPRIÉTÉS/PROPERTIES. Aligne le doc-browser sur
 * l'editeur (mot-cle orange). NE PAS y mettre les widget-kinds (Fenetre/
 * Groupe/Saisie) : ils restent type/violet via WIDGET_KIND_SET cote hlLine. */
static bool is_dsl_block_token(TokenType t) {
    return t == TOKEN_PROPRIETES;
}

static bool doc_kw_predicate(TokenType t) {
    return is_control_token(t) || is_declaration_token(t) || is_oop_token(t)
        || is_visibility_token(t) || is_logical_token(t)
        || is_dsl_block_token(t);
}

/* Émet la liste JS des mots-clés (formes scan + canon accentué) filtrés. */
static void doc_emit_keyword_array(bool (*predicate)(TokenType)) {
    printf("[");
    bool first = true;
    for (int i = 0; ; i++) {
        TokenType type;
        const char* name = scanner_keyword_at(i, &type);
        if (name == NULL) break;
        if (!predicate(type)) continue;
        if (!first) printf(",");
        json_escape(stdout, name);
        first = false;
        const char* canon = token_type_name(type);
        if (canon && strcmp(canon, name) != 0) { printf(","); json_escape(stdout, canon); }
    }
    printf("]");
}

/* Émet UN event dans le tableau JS en cours : formes FR (canonical), EN
 * (nameEN si présent), et ascii (canonical accent-strip si différent). `first`
 * suit l'état virgule à travers les appels. */
static void doc_emit_event_one(const char* fr, const char* en, bool* first) {
    if (!fr || !*fr) return;
    if (!*first) printf(",");
    json_escape(stdout, fr);
    *first = false;
    if (en && *en && strcmp(en, fr) != 0) { printf(","); json_escape(stdout, en); }
    char ascii[256];
    scanner_strip_accents(fr, (int)strlen(fr), ascii, (int)sizeof(ascii));
    if (strcmp(ascii, fr) != 0) { printf(","); json_escape(stdout, ascii); }
}

/* HL_EVENT_RAW : union des noms d'event/hook colorés en bleu (support.function
 * .event) après PROCEDURE/FONCTION par la grammaire TextMate. 4 sources owl.exe :
 *   1. events widgets (owl_event_catalog, par OwlControlKind)
 *   2. lifecycle fenêtre (owl_window_lifecycle_catalog)
 *   2bis. events État + sous-contrôles d'État (Filtre/Lecture/AvantImpression…)
 *   3. hooks de test (liste fixe avantChaqueTest/…)
 *   4. points d'entrée (Main, Fermeture/Closing, Lecture/Read)
 * Le Set JS côté template dédoublonne ; on émet toutes les formes utiles. */
static void doc_emit_event_array(void) {
    printf("[");
    bool first = true;

    /* 1. events widgets : itère la table CONTROL_KINDS, résout le kind, dump. */
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        OwlControlKind kind;
        if (!owl_control_kind_of_info(k, &kind)) continue;
        int ne = owl_event_catalog_count(kind);
        for (int i = 0; i < ne; i++) {
            const OwlEventDef* e = owl_event_catalog_at(kind, i);
            if (e) doc_emit_event_one(e->canonical, e->nameEN, &first);
        }
    }

    /* 2. lifecycle fenêtre (FinInitialisation, Fermeture, …). */
    for (int i = 0, n = owl_window_lifecycle_catalog_count(); i < n; i++) {
        const OwlEventDef* e = owl_window_lifecycle_catalog_at(i);
        if (e) doc_emit_event_one(e->canonical, e->nameEN, &first);
    }

    /* 2bis. events niveau État (Filtre/Lecture/Fermeture) + sous-contrôles
     *       d'État (Initialisation/AvantImpression). */
    for (int i = 0, n = owl_etat_event_catalog_count(); i < n; i++) {
        const OwlEventDef* e = owl_etat_event_catalog_at(i);
        if (e) doc_emit_event_one(e->canonical, e->nameEN, &first);
    }
    for (int i = 0, n = owl_etat_subcontrol_event_catalog_count(); i < n; i++) {
        const OwlEventDef* e = owl_etat_subcontrol_event_catalog_at(i);
        if (e) doc_emit_event_one(e->canonical, e->nameEN, &first);
    }

    /* 3. hooks de test (liste fixe — cf cli_doc.c is_fixture_name). */
    static const struct { const char* fr; const char* en; } HOOKS[] = {
        {"avantToutTest",   "beforeAllTests"},
        {"avantChaqueTest", "beforeEachTest"},
        {"apresChaqueTest", "afterEachTest"},
        {"apresToutTest",   "afterAllTests"},
        {NULL, NULL}
    };
    for (int i = 0; HOOKS[i].fr; i++)
        doc_emit_event_one(HOOKS[i].fr, HOOKS[i].en, &first);

    /* 4. points d'entrée (convention OWL — hooks runtime câblés). */
    static const struct { const char* fr; const char* en; } ENTRIES[] = {
        {"Main",      NULL},
        {"Fermeture", "Closing"},
        {"Lecture",   "Read"},
        {NULL, NULL}
    };
    for (int i = 0; ENTRIES[i].fr; i++)
        doc_emit_event_one(ENTRIES[i].fr, ENTRIES[i].en, &first);

    printf("]");
}

static void doc_emit_desc(const char* fr, const char* en) {
    printf(",\"descFR\":"); json_escape(stdout, fr ? fr : "");
    printf(",\"descEN\":"); json_escape(stdout, en ? en : (fr ? fr : ""));
}

/* Émet ,"catEN":<forme EN> en miroir du ,"cat":<fr> déjà écrit. Résout la
 * forme anglaise via la table bilingue (owl_categories.def) ; repli sur le FR
 * si la catégorie n'a pas de forme EN connue (jamais vide). Le viewer choisit
 * cat ou catEN selon la langue courante (helper catLabel). */
static void doc_emit_cat_en(const char* fr) {
    const OwlCategoryPair* pair = owl_category_resolve(fr);
    const char* en = (pair && pair->en && *pair->en) ? pair->en : fr;
    printf(",\"catEN\":"); json_escape(stdout, en ? en : "");
}

/* Clé de comparaison membre : accent-strip + minuscule, comme la résolution
 * runtime (casse + accent insensibles). Base commune au cross-ref des
 * collisions (fiche existante ⇄ nom de membre). */
static void doc_norm_key(const char* s, char* out, size_t cap) {
    char stripped[128];
    scanner_strip_accents(s ? s : "", (int)strlen(s ? s : ""), stripped, (int)sizeof(stripped));
    size_t j = 0;
    for (const char* p = stripped; *p && j + 1 < cap; p++)
        out[j++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
    out[j] = '\0';
}

/* Cross-ref des collisions : si `fname` (nom d'une fiche existante
 * native/type/enum/const/widget) coïncide, au sens de la résolution runtime,
 * avec un membre imbriqué — propriété DSL de widget OU champ de type composite
 * (Projet inclus) — on émet un renvoi textuel bilingue DANS l'objet JSON de la
 * fiche. Le viewer le rend comme lien vers la fiche « membre » agrégée. Compte
 * les porteurs distincts (widgets vs types) pour la formulation. */
static void doc_emit_member_xref(const char* fname) {
    if (!fname || !*fname) return;
    char key[128];
    doc_norm_key(fname, key, sizeof(key));
    if (!key[0]) return;

    int widgetCount = 0, typeCount = 0;

    /* porteurs widget : propriétés DSL (owl_dsl_prop_at). */
    const char* wseen[128]; int wn = 0;
    for (int i = 0, np = owl_dsl_props_count(); i < np; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR || !e->kind) continue;
        char nk[128], ek[128]; ek[0] = '\0';
        doc_norm_key(e->def->nameFR, nk, sizeof(nk));
        if (e->def->nameEN) doc_norm_key(e->def->nameEN, ek, sizeof(ek));
        if (strcmp(nk, key) != 0 && (ek[0] == '\0' || strcmp(ek, key) != 0)) continue;
        int dup = 0;
        for (int c = 0; c < wn; c++) if (strcmp(wseen[c], e->kind) == 0) { dup = 1; break; }
        if (!dup && wn < 128) wseen[wn++] = e->kind;
    }
    widgetCount = wn;

    /* porteurs type composite : champs de tous les types du registre (Projet
     * compris, il est aussi un membre côté DSLPROPS). */
    const char* tseen[256]; int tn = 0;
    for (int i = 0, nt = owl_registry_types_count(); i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->fields || !t->canonical) continue;
        for (const OwlField* f = t->fields; f->name; f++) {
            char nk[128], ek[128]; ek[0] = '\0';
            doc_norm_key(f->nameAccented ? f->nameAccented : f->name, nk, sizeof(nk));
            if (f->nameEN) doc_norm_key(f->nameEN, ek, sizeof(ek));
            if (strcmp(nk, key) != 0 && (ek[0] == '\0' || strcmp(ek, key) != 0)) continue;
            int dup = 0;
            for (int c = 0; c < tn; c++) if (strcmp(tseen[c], t->canonical) == 0) { dup = 1; break; }
            if (!dup && tn < 256) tseen[tn++] = t->canonical;
            break;  /* un type ne compte qu'une fois même s'il a 2 champs homonymes */
        }
    }
    typeCount = tn;

    if (widgetCount == 0 && typeCount == 0) return;

    /* Accord singulier/pluriel : les libellés portent le compte réel. */
    const char* wFR = (widgetCount > 1) ? "widgets" : "widget";
    const char* tFR = (typeCount   > 1) ? "types composites" : "type composite";
    const char* wEN = (widgetCount > 1) ? "controls" : "control";
    const char* tEN = (typeCount   > 1) ? "composite types" : "composite type";

    char fr[256], en[256];
    if (widgetCount > 0 && typeCount > 0) {
        snprintf(fr, sizeof(fr),
                 "Également une propriété d'interface (%d %s) et un champ de %d %s",
                 widgetCount, wFR, typeCount, tFR);
        snprintf(en, sizeof(en),
                 "Also a member: interface property of %d %s and field of %d %s",
                 widgetCount, wEN, typeCount, tEN);
    } else if (widgetCount > 0) {
        snprintf(fr, sizeof(fr), "Également une propriété d'interface (%d %s)", widgetCount, wFR);
        snprintf(en, sizeof(en), "Also a member: interface property of %d %s", widgetCount, wEN);
    } else {
        snprintf(fr, sizeof(fr), "Également un champ de %d %s", typeCount, tFR);
        snprintf(en, sizeof(en), "Also a member: field of %d %s", typeCount, tEN);
    }
    printf(",\"memberXref\":");   json_escape(stdout, fr);
    printf(",\"memberXrefEN\":"); json_escape(stdout, en);
}

/* Type lisible d'une propriété DSL (enum/object portés à part dans le JSON). */
static const char* doc_prop_type_label(const OwlControlPropDef* p) {
    switch (p->type) {
        case OWL_PROP_TYPE_INT:         return "entier";
        case OWL_PROP_TYPE_STRING:      return "chaîne";
        case OWL_PROP_TYPE_BOOL:        return "booléen";
        case OWL_PROP_TYPE_VARIANT:     return "tout";
        case OWL_PROP_TYPE_POSITION:    return "position";
        case OWL_PROP_TYPE_COLOR:       return "couleur";
        case OWL_PROP_TYPE_MULTILINGUE: return "chaîneMultilingue";
        case OWL_PROP_TYPE_ENUM:        return p->enum_name ? p->enum_name : "enum";
        case OWL_PROP_TYPE_OBJECT:      return p->object_type ? p->object_type : "objet";
    }
    return "tout";
}

/* json_escape variante bornee par longueur : les blobs de doc embarquee
 * (embedded_docs) ne sont PAS zero-terminees, on escape exactement `len`
 * octets. Meme table d'echappement que json_escape. */
static void doc_json_escape_n(FILE* f, const unsigned char* s, long len) {
    fputc('"', f);
    for (long i = 0; i < len; i++) {
        unsigned char c = s[i];
        if (c == '"')       fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20)  fprintf(f, "\\u%04x", c);
        else                fputc(c, f);
    }
    fputc('"', f);
}

/* Titre d'un chapitre de doc : 1er titre H1 du markdown ("# Texte"), sinon
 * humanisation du stem (retire le prefixe NNN_, remplace '_' par espace,
 * capitalise le 1er caractere). Ecrit dans out (zero-terminee).
 * Le H1 est emis json-escape-bound directement (peut contenir de l'UTF-8) ;
 * le fallback humanise est ASCII-only sur le stem. */
static void doc_emit_guide_title(FILE* f, const char* name,
                                 const unsigned char* data, long len) {
    /* cherche la 1re ligne commencant par "# " (H1). Tolere whitespace
     * de tete sur la ligne ; s'arrete au 1er '\n'/'\r'. */
    long i = 0;
    while (i < len) {
        long ls = i;
        /* skip indentation eventuelle */
        long p = ls;
        while (p < len && (data[p] == ' ' || data[p] == '\t')) p++;
        if (p < len && data[p] == '#') {
            long h = p;
            int hashes = 0;
            while (h < len && data[h] == '#') { hashes++; h++; }
            if (hashes == 1 && h < len && data[h] == ' ') {
                /* H1 trouve : contenu jusqu'a fin de ligne */
                long ts = h + 1;
                long te = ts;
                while (te < len && data[te] != '\n' && data[te] != '\r') te++;
                /* trim trailing spaces */
                while (te > ts && (data[te-1] == ' ' || data[te-1] == '\t')) te--;
                if (te > ts) { doc_json_escape_n(f, data + ts, te - ts); return; }
            }
        }
        /* avance a la ligne suivante */
        while (i < len && data[i] != '\n') i++;
        if (i < len) i++;
    }
    /* fallback : humanise le stem (ex "00_apercu_du_langage" -> "Apercu du langage") */
    const char* s = name;
    /* saute un prefixe numerique NNN_ */
    const char* q = s;
    while (*q >= '0' && *q <= '9') q++;
    if (q != s && *q == '_') s = q + 1;
    fputc('"', f);
    bool firstAlpha = true;
    for (const char* c = s; *c; c++) {
        char ch = *c;
        if (ch == '_') { fputc(' ', f); continue; }
        if (firstAlpha && ch >= 'a' && ch <= 'z') { ch = (char)(ch - 'a' + 'A'); }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) firstAlpha = false;
        if (ch == '"' || ch == '\\') fputc('\\', f);
        fputc(ch, f);
    }
    fputc('"', f);
}

/* ------------------------------------------------------------------
 *  — page "Nouveautés" : ensemble des fiches navigables + diff
 * contre la baseline figée à la release précédente.
 *
 * Le type + la fonction de diff vivent dans cli_doc.c (testés en
 * in-process). Ici on ne fait que déclarer la vue attendue : un typedef
 * identique dans cli_doc.h collisionnerait avec celui du self-test (qui
 * inclut le header ET redéfinit le type). Deux typedefs identiques dans
 * des TU distinctes sont sans conséquence au link (C : liaison par nom).
 * ------------------------------------------------------------------ */
typedef struct {
    const char* kind;
    const char* name;
} OwlDocFicheId;
extern int owl_doc_novelties_diff(const OwlDocFicheId* baseline, int nb,
                                  const OwlDocFicheId* current, int nc,
                                  OwlDocFicheId* out, int out_cap);

/* Énumère l'ensemble des fiches navigables (mêmes entités et MÊMES chaînes
 * `kind` que le DATA de dump_doc_html) : natives, types (composés +
 * primitifs), enums, constantes, widgets, chapitres publics. Les chaînes
 * pointées sont statiques (registry / docs embarqués) — aliasées, pas
 * copiées. Retourne le nombre de fiches ; *out est malloc'd (à free). */
static int collect_doc_fiches(OwlDocFicheId** out) {
    int cap = 64, cnt = 0;
    OwlDocFicheId* arr = (OwlDocFicheId*)malloc((size_t)cap * sizeof(OwlDocFicheId));
    if (!arr) { *out = NULL; return 0; }
    #define FICHE_PUSH(K, N) do {                                          \
        if (cnt >= cap) { cap *= 2;                                        \
            OwlDocFicheId* g = (OwlDocFicheId*)realloc(arr,                \
                (size_t)cap * sizeof(OwlDocFicheId));                      \
            if (!g) { free(arr); *out = NULL; return 0; } arr = g; }       \
        arr[cnt].kind = (K); arr[cnt].name = (N); cnt++;                   \
    } while (0)

    for (int i = 0, n = owl_registry_count(); i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        FICHE_PUSH("native", d->canonical);
    }
    for (int i = 0, nt = owl_registry_types_count(); i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (t) FICHE_PUSH("type", t->canonical);
    }
    for (int i = 0, np = cli_doc_primitive_count(); i < np; i++) {
        const char* pn = NULL;
        if (cli_doc_primitive_at(i, &pn, NULL, NULL) && pn) FICHE_PUSH("type", pn);
    }
    for (int i = 0, ne = owl_registry_enums_count(); i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (e) FICHE_PUSH("enum", e->name);
    }
    for (int i = 0, ncst = owl_registry_consts_count(); i < ncst; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (c && c->name) FICHE_PUSH("const", c->name);
    }
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        FICHE_PUSH("widget", k->nameFR);
    }
    for (const OwlEmbeddedDoc* g = EMBEDDED_PUBLIC_DOCS; g->name; g++) {
        FICHE_PUSH("reference", g->name);
    }
    #undef FICHE_PUSH
    *out = arr;
    return cnt;
}

/* Lit la baseline `tools/doc_fiches_baseline.tsv` (chemin dérivé du
 * template : <racine>/docs/reference/<tpl> → <racine>/tools/...). Lignes
 * `kind<TAB>name` ; `#version<TAB>V` renseigne *out_version ; autres `#`
 * ignorés. Baseline absente → 0 fiche (bootstrap : page vide, pas d'erreur).
 * *out est malloc'd (à free), ainsi que le tampon de chaînes possédé. */
typedef struct {
    OwlDocFicheId* items;
    int            count;
    char*          strpool;   /* possède toutes les chaînes kind/name */
    char           version[32];
} DocBaseline;

static void doc_baseline_load(const char* tplPath, DocBaseline* bl) {
    memset(bl, 0, sizeof(*bl));
    /* Dérive la racine du dépôt depuis le chemin du template. */
    char path[1024];
    snprintf(path, sizeof(path), "%s", tplPath ? tplPath : "");
    /* remonte 3 segments : _owl-doc-template.html / reference / docs */
    for (int seg = 0; seg < 3; seg++) {
        char* slash = NULL;
        for (char* p = path; *p; p++) if (*p == '/' || *p == '\\') slash = p;
        if (!slash) { path[0] = '\0'; break; }
        *slash = '\0';
    }
    char full[1100];
    if (path[0]) snprintf(full, sizeof(full), "%s/tools/doc_fiches_baseline.tsv", path);
    else         snprintf(full, sizeof(full), "tools/doc_fiches_baseline.tsv");

    FILE* f = fopen(full, "rb");
    if (!f) return;   /* bootstrap : pas de baseline */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    bl->strpool = (char*)malloc((size_t)sz + 1);
    if (!bl->strpool) { fclose(f); return; }
    size_t rd = fread(bl->strpool, 1, (size_t)sz, f);
    bl->strpool[rd] = '\0';
    fclose(f);

    /* Compte les lignes pour dimensionner le tableau. */
    int lines = 0;
    for (size_t i = 0; i < rd; i++) if (bl->strpool[i] == '\n') lines++;
    bl->items = (OwlDocFicheId*)malloc((size_t)(lines + 1) * sizeof(OwlDocFicheId));
    if (!bl->items) { free(bl->strpool); bl->strpool = NULL; return; }

    char* p = bl->strpool;
    while (*p) {
        char* line = p;
        char* nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; } else { p = line + strlen(line); }
        /* strip CR */
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';
        if (line[0] == '\0') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        char* val = tab + 1;
        if (line[0] == '#') {
            if (strcmp(line, "#version") == 0)
                snprintf(bl->version, sizeof(bl->version), "%s", val);
            continue;
        }
        bl->items[bl->count].kind = line;
        bl->items[bl->count].name = val;
        bl->count++;
    }
}

static void doc_baseline_free(DocBaseline* bl) {
    free(bl->items);
    free(bl->strpool);
    bl->items = NULL; bl->strpool = NULL;
}

/* Résout la desc FR/EN d'une nouveauté (note affichée). Les chapitres
 * (kind reference) n'ont pas de desc registry : note laissée nulle (le
 * viewer retombe sur le titre via resolve()). */
static void novelty_note(const char* kind, const char* name,
                         const char** frOut, const char** enOut) {
    *frOut = NULL; *enOut = NULL;
    if (strcmp(kind, "native") == 0) {
        const OwlNativeDef* d = owl_registry_find(name);
        if (d) { *frOut = d->descFR; *enOut = d->descEN; }
    } else if (strcmp(kind, "type") == 0) {
        const OwlTypeDef* t = owl_registry_type_find(name);
        if (t) { *frOut = t->descFR; *enOut = t->descEN; }
    } else if (strcmp(kind, "enum") == 0) {
        const OwlEnum* e = owl_registry_enum_find(name);
        if (e) { *frOut = e->descFR; *enOut = e->descEN; }
    } else if (strcmp(kind, "const") == 0) {
        const OwlConstDef* c = owl_registry_const_find(name);
        if (c) { *frOut = c->descFR; *enOut = c->descEN; }
    } else if (strcmp(kind, "widget") == 0) {
        for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++)
            if (strcmp(k->nameFR, name) == 0) { *frOut = k->descFR; *enOut = k->descEN; break; }
    }
}

static int novelty_cmp_ids(const void* a, const void* b) {
    const OwlDocFicheId* fa = (const OwlDocFicheId*)a;
    const OwlDocFicheId* fb = (const OwlDocFicheId*)b;
    int c = strcmp(fa->kind, fb->kind);
    if (c != 0) return c;
    return strcmp(fa->name, fb->name);
}

/* owl --dump-doc-fiches — émet l'ensemble des fiches en TSV trié
 * (kind<TAB>name), précédé d'un en-tête `#version`. Consommé par
 * `make snapshot` pour figer tools/doc_fiches_baseline.tsv. */
static void dump_doc_fiches(void) {
    vm_init();
    OwlDocFicheId* arr = NULL;
    int n = collect_doc_fiches(&arr);
    if (n > 1) qsort(arr, (size_t)n, sizeof(OwlDocFicheId), novelty_cmp_ids);
    printf("# owl doc fiches baseline — regenere par 'make snapshot'\n");
    printf("#version\t%s\n", OWL_VERSION);
    for (int i = 0; i < n; i++) printf("%s\t%s\n", arr[i].kind, arr[i].name);
    free(arr);
    vm_free();
}

static void dump_doc_html(const char* tplPath) {
    FILE* tf = fopen(tplPath, "rb");
    if (!tf) {
        fprintf(stderr, "owl --dump-doc-html: template introuvable: %s\n", tplPath);
        return;
    }
    fseek(tf, 0, SEEK_END);
    long sz = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    char* tpl = (char*)malloc((size_t)sz + 1);
    if (!tpl) { fclose(tf); return; }
    size_t rd = fread(tpl, 1, (size_t)sz, tf);
    tpl[rd] = '\0';
    fclose(tf);

    const char* MARK = "//__OWL_DOC_DATA__";
    char* at = strstr(tpl, MARK);
    if (!at) {
        fprintf(stderr, "owl --dump-doc-html: placeholder %s absent du template\n", MARK);
        free(tpl);
        return;
    }

    fwrite(tpl, 1, (size_t)(at - tpl), stdout);

    vm_init();

    printf("\nconst OWL_VERSION = ");
    json_escape(stdout, OWL_VERSION);
    printf(";\n");

    printf("\nconst DATA = [\n");
    bool firstObj = true;

    /* ---- natives ---- */
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"native\",\"name\":"); json_escape(stdout, d->canonical);
        printf(",\"ascii\":"); json_escape(stdout, d->canonicalAscii ? d->canonicalAscii : d->canonical);
        printf(",\"en\":"); json_escape(stdout, d->nameEN ? d->nameEN : "");
        printf(",\"cat\":"); json_escape(stdout, d->category ? d->category : "Divers");
        doc_emit_cat_en(d->category ? d->category : "Divers");
        doc_emit_desc(d->descFR, d->descEN);
        if (d->signatures && d->signatures->returnType) {
            printf(",\"signatures\":[");
            bool firstSig = true;
            for (const OwlSignature* s = d->signatures; s->returnType; s++) {
                if (!firstSig) printf(",");
                firstSig = false;
                printf("{\"params\":[");
                bool fp = true;
                /*  : borner a l'arite (prefixe partage : plusieurs
                 * signatures peuvent referencer le meme tableau de params).
                 * Variadique (arity<0) : pas de borne. */
                if (s->params) for (const OwlParam* p = s->params;
                     p->name && (s->arity < 0 || (p - s->params) < s->arity);
                     p++) {
                    if (!fp) printf(",");
                    fp = false;
                    printf("{\"n\":"); json_escape(stdout, p->name);
                    if (p->type) { printf(",\"type\":"); json_escape(stdout, p->type); }
                    if (p->desc) { printf(",\"d\":");    json_escape(stdout, p->desc); }
                    const OwlEnum* en = p->allowedOptions ? p->allowedOptions : p->allowedBase;
                    if (en) { printf(",\"enum\":"); json_escape(stdout, en->name); }
                    else if (p->type && owl_registry_type_find(p->type)) {
                        printf(",\"xref\":"); json_escape(stdout, p->type);
                    }
                    printf("}");
                }
                /* rt = type de retour brut (le viewer bâtit la signature,
                 * bilingue, via la map TYPE_EN) ; ret = description du retour. */
                printf("],\"rt\":");
                json_escape(stdout, s->returnType ? s->returnType : "rien");
                printf(",\"ret\":");
                json_escape(stdout, s->returnDesc ? s->returnDesc : "");
                printf("}");
            }
            printf("]");
        }
        /* Formes fluides : entrées du method catalog pointant vers l'impl de
         * cette native (ex: Cherche → <tableau>.Cherche()/.Seek()). Groupées
         * par receiver, paire FR/EN (nom == nameEN → EN, sinon FR) pour un
         * affichage lang-aware côté viewer. */
        {
            int mc = method_catalog_count();
            const char* recvs[24]; const char* frN[24]; const char* enN[24]; int nr = 0;
            for (int j = 0; j < mc; j++) {
                const char* tn = NULL;
                const MethodEntry* m = method_catalog_at(j, &tn);
                if (!m || !m->name || !tn || m->function != d->impl) continue;
                int idx = -1;
                for (int k = 0; k < nr; k++) if (strcmp(recvs[k], tn) == 0) { idx = k; break; }
                if (idx < 0) { if (nr >= 24) continue; idx = nr++; recvs[idx] = tn; frN[idx] = NULL; enN[idx] = NULL; }
                bool isEn = d->nameEN && strcmp(m->name, d->nameEN) == 0;
                if (isEn) { if (!enN[idx]) enN[idx] = m->name; }
                else      { if (!frN[idx]) frN[idx] = m->name; }
            }
            if (nr > 0) {
                printf(",\"methods\":[");
                for (int k = 0; k < nr; k++) {
                    if (k) printf(",");
                    const char* fr = frN[k] ? frN[k] : enN[k];
                    const char* en = enN[k] ? enN[k] : frN[k];
                    printf("{\"recv\":"); json_escape(stdout, recvs[k]);
                    printf(",\"fr\":"); json_escape(stdout, fr ? fr : "");
                    printf(",\"en\":"); json_escape(stdout, en ? en : "");
                    printf("}");
                }
                printf("]");
            }
        }
        cli_doc_emit_examples_json(d->canonical, d->canonicalAscii);
        cli_doc_emit_neighbors_json(d);
        cli_doc_emit_public_docs_json(d->canonical, d->canonicalAscii);
        doc_emit_member_xref(d->canonical);
        printf("}");
    }

    /* ---- types ---- */
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t) continue;
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"type\",\"name\":"); json_escape(stdout, t->canonical);
        printf(",\"ascii\":"); json_escape(stdout, t->canonicalAscii ? t->canonicalAscii : t->canonical);
        if (t->nameEN) { printf(",\"en\":"); json_escape(stdout, t->nameEN); }
        printf(",\"cat\":"); json_escape(stdout, t->category ? t->category : "Divers");
        doc_emit_cat_en(t->category ? t->category : "Divers");
        doc_emit_desc(t->descFR, t->descEN);
        if (t->fields) {
            printf(",\"fields\":[");
            bool ff = true;
            for (const OwlField* f = t->fields; f->name; f++) {
                if (!ff) printf(",");
                ff = false;
                printf("{\"n\":"); json_escape(stdout, f->nameAccented ? f->nameAccented : f->name);
                if (f->nameEN) { printf(",\"en\":"); json_escape(stdout, f->nameEN); }
                printf(",\"t\":"); json_escape(stdout, f->type ? f->type : "");
                if (f->descFR) { printf(",\"d\":"); json_escape(stdout, f->descFR); }
                if (f->descEN && f->descEN[0]) { printf(",\"dEN\":"); json_escape(stdout, f->descEN); }
                const OwlEnum* en = f->allowedBase ? f->allowedBase : f->allowedOptions;
                if (en) { printf(",\"enum\":"); json_escape(stdout, en->name); }
                printf("}");
            }
            printf("]");
        }
        cli_doc_emit_examples_json(t->canonical, t->canonicalAscii);
        cli_doc_emit_usedby_json(t);
        cli_doc_emit_public_docs_json(t->canonical, t->canonicalAscii);
        doc_emit_member_xref(t->canonical);
        printf("}");
    }

    /* ---- types primitifs (builtins VM, hors registry OwlTypeDef) ----
     * Émis avec kind "type" pour tomber sous le même filtre que les types
     * composés. Catégorie dédiée pour les regrouper dans le navigateur. */
    {
        int np = cli_doc_primitive_count();
        for (int i = 0; i < np; i++) {
            const char *pn = NULL, *pa = NULL, *pen = NULL, *pfr = NULL, *pden = NULL;
            if (!cli_doc_primitive_doc_at(i, &pn, &pa, &pen, &pfr, &pden)) continue;
            if (!firstObj) printf(",\n");
            firstObj = false;
            printf("{\"kind\":\"type\",\"name\":"); json_escape(stdout, pn);
            printf(",\"ascii\":"); json_escape(stdout, pa ? pa : pn);
            if (pen) { printf(",\"en\":"); json_escape(stdout, pen); }
            printf(",\"cat\":\"Primitifs\"");
            doc_emit_cat_en("Primitifs");
            doc_emit_desc(pfr, pden);
            doc_emit_member_xref(pn);
            printf("}");
        }
    }

    /* ---- enums ---- */
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e) continue;
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"enum\",\"name\":"); json_escape(stdout, e->name);
        printf(",\"ascii\":"); json_escape(stdout, e->nameAscii ? e->nameAscii : e->name);
        if (e->nameEN) { printf(",\"en\":"); json_escape(stdout, e->nameEN); }
        printf(",\"cat\":"); json_escape(stdout, e->category ? e->category : "Divers");
        doc_emit_cat_en(e->category ? e->category : "Divers");
        doc_emit_desc(e->descFR, e->descEN);
        printf(",\"combinable\":%s,\"values\":[", e->combinable ? "true" : "false");
        if (e->values) {
            bool fv = true;
            for (const char* const* v = e->values; *v; v++) {
                if (!fv) printf(",");
                fv = false;
                json_escape(stdout, *v);
            }
        }
        printf("]");
        cli_doc_emit_examples_json(e->name, e->nameAscii ? e->nameAscii : e->name);
        doc_emit_member_xref(e->name);
        printf("}");
    }

    /* ---- constantes (avec enum d'appartenance) ---- */
    int nc = owl_registry_consts_count();
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"const\",\"name\":"); json_escape(stdout, c->name);
        if (c->nameEN) { printf(",\"en\":"); json_escape(stdout, c->nameEN); }
        printf(",\"cat\":"); json_escape(stdout, c->category ? c->category : "Divers");
        doc_emit_cat_en(c->category ? c->category : "Divers");
        doc_emit_desc(c->descFR, c->descEN);
        const OwlEnum* owner = owl_registry_enum_owner_of_const(c->name);
        if (owner) { printf(",\"enum\":"); json_escape(stdout, owner->name); }
        cli_doc_emit_examples_json(c->name, c->name);
        doc_emit_member_xref(c->name);
        printf("}");
    }

    /* ---- mots-clés (TANTQUE, POUR, SI, SORTIR, SELON…) ----
     * Fiche searchable en miroir de `owl explain <kw>`. Sens primaire
     * (senses[0]) pour desc/syntaxe/exemple. Forme EN derivee des alias
     * scanner (l'alias dont la forme ascii-minuscule differe du canon, ex.
     * tantque/while -> WHILE) pour la recherche bilingue. Composites
     * (PAS DANS) et sens multiples : passes futures. */
    for (int i = 0, kwn = owl_keyword_count(); i < kwn; i++) {
        const OwlKeywordDef* k = owl_keyword_at(i);
        if (!k || k->senses_count < 1) continue;
        const OwlKeywordSense* s = &k->senses[0];
        const char* name = k->canon_display ? k->canon_display
                                            : token_type_name(k->token);
        if (!name || !*name) continue;

        char ascii[64];
        scanner_strip_accents(name, (int)strlen(name), ascii, (int)sizeof(ascii));

        /* canon en ascii-minuscule, base de comparaison aux alias. */
        char canonLow[64];
        size_t cl = 0;
        for (const char* p = ascii; *p && cl + 1 < sizeof(canonLow); p++)
            canonLow[cl++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
        canonLow[cl] = '\0';

        /* forme EN = 1er alias dont l'ascii-minuscule differe du canon,
         * remis en majuscules (les mots-cles s'affichent en majuscules). */
        char enForm[64];
        enForm[0] = '\0';

        /* Override curatif : pour ces tokens, le 1er alias different est une
         * FLEXION FRANCAISE (toute, publique, privee, protegee), pas la forme
         * anglaise. L'heuristique la choisirait a tort. La vraie forme EN est
         * un alias scanner existant (each/public/private/protected, cf.
         * src/scanner.c) — source de verite, pas une invention. Les autres
         * tokens restent servis par l'heuristique inchangee. */
        static const struct { TokenType tok; const char* en; } EN_OVERRIDE[] = {
            { TOKEN_TOUT,    "EACH"      },
            { TOKEN_PUBLIC,  "PUBLIC"    },
            { TOKEN_PRIVE,   "PRIVATE"   },
            { TOKEN_PROTEGE, "PROTECTED" },
        };
        for (size_t o = 0; o < sizeof(EN_OVERRIDE) / sizeof(EN_OVERRIDE[0]); o++) {
            if (EN_OVERRIDE[o].tok == k->token) {
                snprintf(enForm, sizeof(enForm), "%s", EN_OVERRIDE[o].en);
                break;
            }
        }

        for (int a = 0; enForm[0] == '\0'; a++) {
            TokenType at;
            const char* alias = scanner_keyword_at(a, &at);
            if (!alias) break;
            if (at != k->token) continue;
            char al[64];
            scanner_strip_accents(alias, (int)strlen(alias), al, (int)sizeof(al));
            char alLow[64];
            size_t l = 0;
            for (const char* p = al; *p && l + 1 < sizeof(alLow); p++)
                alLow[l++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
            alLow[l] = '\0';
            if (strcmp(alLow, canonLow) != 0) {
                size_t e = 0;
                for (const char* p = al; *p && e + 1 < sizeof(enForm); p++)
                    enForm[e++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
                enForm[e] = '\0';
                break;
            }
        }

        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"keyword\",\"name\":"); json_escape(stdout, name);
        printf(",\"ascii\":"); json_escape(stdout, ascii);
        if (enForm[0]) { printf(",\"en\":"); json_escape(stdout, enForm); }

        /* aliases = TOUTES les formes scanner du token (FR synonymes + EN),
         * pour que F1 sur `chaque`/`tous`/`each`/`all`/`every` resolve la
         * fiche du token (`TOUT`) au lieu d'un 404. `name`/`ascii`/`en`
         * seuls ne couvrent qu'une forme EN — les synonymes restaient muets.
         * On saute l'alias canon (ascii-minuscule == canonLow, deja indexe
         * via `ascii`) pour ne pas emettre le doublon evident. */
        {
            bool firstAlias = true;
            for (int a = 0; ; a++) {
                TokenType at;
                const char* alias = scanner_keyword_at(a, &at);
                if (!alias) break;
                if (at != k->token) continue;
                char al[64];
                scanner_strip_accents(alias, (int)strlen(alias), al, (int)sizeof(al));
                char alLow[64];
                size_t l = 0;
                for (const char* p = al; *p && l + 1 < sizeof(alLow); p++)
                    alLow[l++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
                alLow[l] = '\0';
                if (strcmp(alLow, canonLow) == 0) continue; /* = canon, deja via ascii */
                if (firstAlias) { printf(",\"aliases\":["); firstAlias = false; }
                else            { printf(","); }
                json_escape(stdout, alias);
            }
            if (!firstAlias) printf("]");
        }

        printf(",\"cat\":"); json_escape(stdout, k->category ? k->category : "Mot-clé");
        doc_emit_cat_en(k->category ? k->category : "Mot-clé");
        doc_emit_desc(s->descFR, s->descEN);
        if (s->syntax && *s->syntax)   { printf(",\"syntax\":");  json_escape(stdout, s->syntax); }
        if (s->example && *s->example) { printf(",\"example\":"); json_escape(stdout, s->example); }
        printf("}");
    }

    /* ---- widgets DSL (Fenêtre, Bouton, Libellé…) ---- */
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"widget\",\"name\":"); json_escape(stdout, k->nameFR);
        printf(",\"ascii\":"); json_escape(stdout, k->nameAscii ? k->nameAscii : k->nameFR);
        if (k->nameEN) { printf(",\"en\":"); json_escape(stdout, k->nameEN); }
        printf(",\"cat\":\"Interface\",\"widgetkind\":\"%s\"", k->is_widget ? "widget" : "bloc");
        doc_emit_cat_en("Interface");
        if (k->defaultProperty) { printf(",\"defprop\":"); json_escape(stdout, k->defaultProperty); }
        doc_emit_desc(k->descFR, k->descEN);
        doc_emit_member_xref(k->nameFR);
        printf("}");
    }

    /* ---- fiches de référence agrégées : entrées de navigation normales dans
     *      DATA (kind "reference"), filtrables par le chip "Référence" comme
     *      n'importe quel kind. Le contenu (table groupée) vit dans les consts
     *      ERRORS/LIMITS/OPERATORS/GLOBALVARS ; le viewer le retrouve par
     *      refkey au moment du rendu de la fiche. ---- */
    {
        struct { const char* fr; const char* en; const char* refkey; } refs[] = {
            {"Erreurs",            "Errors",          "errors"},
            {"Limites",            "Limits",          "limits"},
            {"Opérateurs (référence)", "Operators (reference)", "operators"},
            {"Variables globales", "Global variables","globalvars"},
        };
        for (int i = 0; i < 4; i++) {
            if (!firstObj) printf(",\n");
            firstObj = false;
            printf("{\"kind\":\"reference\",\"name\":"); json_escape(stdout, refs[i].fr);
            printf(",\"ascii\":"); json_escape(stdout, refs[i].refkey);
            printf(",\"en\":");    json_escape(stdout, refs[i].en);
            printf(",\"cat\":");   json_escape(stdout, "Référence");
            doc_emit_cat_en("Référence");
            printf(",\"refkey\":");json_escape(stdout, refs[i].refkey);
            printf("}");
        }
    }

    /* ---- chapitres de prose (docs/public_documentation) : memes entrees
     *      de navigation kind "reference", mais cat "Guide" (l'axe categorie
     *      sous-divise l'umbrella "Documentation"). Le markdown brut est
     *      porte par la const GUIDES (emise plus bas) et rendu par le viewer.
     *      Cle "doc" = stem (analogue a "refkey" des tables). On n'expose QUE
     *      EMBEDDED_PUBLIC_DOCS (public) ; jamais l'internal. ---- */
    for (const OwlEmbeddedDoc* g = EMBEDDED_PUBLIC_DOCS; g->name; g++) {
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"reference\",\"name\":");
        doc_emit_guide_title(stdout, g->name, g->data, g->len);
        /* titre EN cherchable : seulement si le guide a une variante EN
         * appariee par stem (meme source que GUIDES[stem].titleEN). Un guide
         * FR-only ne recoit aucune cle "en" (fallback recherche sur le nom FR). */
        for (const OwlEmbeddedDoc* e = EMBEDDED_PUBLIC_DOCS_EN; e->name; e++) {
            if (strcmp(e->name, g->name) == 0) {
                printf(",\"en\":"); doc_emit_guide_title(stdout, e->name, e->data, e->len);
                break;
            }
        }
        printf(",\"ascii\":"); json_escape(stdout, g->name);
        printf(",\"cat\":");   json_escape(stdout, "Guide");
        doc_emit_cat_en("Guide");
        printf(",\"doc\":");   json_escape(stdout, g->name);
        printf("}");
    }

    /* ---- fiche "Nouveautés" : diff de l'ensemble courant des
     *      fiches contre la baseline figée à la release précédente. Émise
     *      seulement s'il y a au moins un ajout (sinon page absente). ---- */
    OwlDocFicheId* nov = NULL;
    int novCount = 0;
    DocBaseline bl;
    doc_baseline_load(tplPath, &bl);
    {
        OwlDocFicheId* cur = NULL;
        int ncur = collect_doc_fiches(&cur);
        if (ncur > 0 && bl.count > 0) {
            nov = (OwlDocFicheId*)malloc((size_t)ncur * sizeof(OwlDocFicheId));
            if (nov) novCount = owl_doc_novelties_diff(bl.items, bl.count,
                                                       cur, ncur, nov, ncur);
        }
        /* nov aliase des chaînes statiques (registry/docs) — sûr après free(cur). */
        free(cur);
    }
    if (novCount > 0) {
        if (!firstObj) printf(",\n");
        firstObj = false;
        printf("{\"kind\":\"reference\",\"name\":");   json_escape(stdout, "Nouveautés");
        printf(",\"ascii\":");  json_escape(stdout, "novelties");
        printf(",\"en\":");     json_escape(stdout, "What's new");
        printf(",\"cat\":");    json_escape(stdout, "Référence");
        doc_emit_cat_en("Référence");
        printf(",\"refkey\":"); json_escape(stdout, "novelties");
        printf("}");
    }

    printf("\n];\n");

    /* NOVELTIES : liste des ajouts (kind+name = clé de résolution deep-link ;
     * le viewer retrouve nom affiché + desc via resolve()). note/noteEN =
     * desc de repli côté générateur. Émis même vide (guard typeof côté
     * template). NOVELTIES_BASE = version figée de la baseline (si connue). */
    printf("const NOVELTIES = [");
    for (int i = 0; i < novCount; i++) {
        if (i) printf(",");
        const char *fr = NULL, *en = NULL;
        novelty_note(nov[i].kind, nov[i].name, &fr, &en);
        printf("{\"k\":"); json_escape(stdout, nov[i].kind);
        printf(",\"n\":"); json_escape(stdout, nov[i].name);
        if (fr) { printf(",\"note\":");   json_escape(stdout, fr); }
        if (en) { printf(",\"noteEN\":"); json_escape(stdout, en); }
        printf("}");
    }
    printf("];\n");
    printf("const NOVELTIES_BASE = ");
    if (bl.version[0]) json_escape(stdout, bl.version);
    else printf("null");
    printf(";\n");
    free(nov);
    doc_baseline_free(&bl);

    /* ---- GUIDES : markdown BRUT de chaque chapitre public, indexe par stem.
     *      Le viewer rend GUIDES[doc].md via son mini-renderer markdown quand
     *      une entree kind:reference porte une cle "doc". ---- */
    printf("const GUIDES = {\n");
    {
        bool fg2 = true;
        for (const OwlEmbeddedDoc* g = EMBEDDED_PUBLIC_DOCS; g->name; g++) {
            if (!fg2) printf(",\n"); fg2 = false;
            json_escape(stdout, g->name);
            printf(":{\"name\":"); json_escape(stdout, g->name);
            printf(",\"title\":"); doc_emit_guide_title(stdout, g->name, g->data, g->len);
            printf(",\"md\":");    doc_json_escape_n(stdout, g->data, g->len);
            /* variante EN appariee par stem (table parallele) : alimente
             * titleEN/mdEN sur le stem FR, sans fiche autonome. Le viewer
             * choisit selon la langue (sinon bandeau de repli honnete). */
            for (const OwlEmbeddedDoc* e = EMBEDDED_PUBLIC_DOCS_EN; e->name; e++) {
                if (strcmp(e->name, g->name) == 0) {
                    printf(",\"titleEN\":"); doc_emit_guide_title(stdout, e->name, e->data, e->len);
                    printf(",\"mdEN\":");    doc_json_escape_n(stdout, e->data, e->len);
                    break;
                }
            }
            printf("}");
        }
    }
    printf("\n};\n");
    printf("const HL_KW_RAW = ");   doc_emit_keyword_array(doc_kw_predicate);     printf(";\n");
    printf("const HL_TYPE_RAW = "); doc_emit_keyword_array(is_type_token_vscode); printf(";\n");
    printf("const HL_EVENT_RAW = "); doc_emit_event_array();                       printf(";\n");

    /* ---- propriétés DSL (par widget) : le template dédup par nom + agrège
     *      les widgets ("accepté par"), comme owl list props / owl where. ---- */
    printf("const DSLPROPS = [\n");
    {
        int np = owl_dsl_props_count();
        bool fp = true;
        for (int i = 0; i < np; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (!e || !e->def || !e->def->nameFR) continue;
            const OwlControlPropDef* p = e->def;
            if (!fp) printf(",\n");
            fp = false;
            printf("{\"w\":"); json_escape(stdout, e->kind);
            printf(",\"n\":"); json_escape(stdout, p->nameFR);
            if (p->nameEN) { printf(",\"en\":"); json_escape(stdout, p->nameEN); }
            printf(",\"t\":"); json_escape(stdout, doc_prop_type_label(p));
            if (p->type == OWL_PROP_TYPE_ENUM && p->enum_name) { printf(",\"enm\":"); json_escape(stdout, p->enum_name); }
            if (p->type == OWL_PROP_TYPE_OBJECT && p->object_type) { printf(",\"obj\":"); json_escape(stdout, p->object_type); }
            emit_prop_default_json(&p->dft);
            doc_emit_desc(p->descFR, p->descEN);
            printf("}");
        }

        /*  — champs du type composite `Projet` (bloc `est un Projet`)
         * emis comme entrees DSLPROPS (w="Projet") : le template en synthetise
         * des fiches kind:"property" navigables via F1 sous leur nom FR ET EN
         * (StartupWindow, Theme…), au meme titre que les proprietes de widget.
         * Source unique : FIELDS_Projet (aucune desc inventee). */
        const OwlTypeDef* pj = owl_registry_type_find("Projet");
        if (pj && pj->fields) {
            for (const OwlField* f = pj->fields; f->name; f++) {
                if (!fp) printf(",\n");
                fp = false;
                printf("{\"w\":\"Projet\"");
                printf(",\"n\":"); json_escape(stdout, f->nameAccented ? f->nameAccented : f->name);
                if (f->nameEN) { printf(",\"en\":"); json_escape(stdout, f->nameEN); }
                printf(",\"t\":"); json_escape(stdout, f->type ? f->type : "");
                doc_emit_desc(f->descFR, f->descEN);
                cli_doc_emit_dsl_prop_public_docs_json(
                    f->nameAccented ? f->nameAccented : f->name, f->nameEN);
                printf("}");
            }
        }
    }
    printf("\n];\n");

    /* ---- champs des types composites : une entrée plate par
     *      (type porteur, champ). Le template les fond dans la même machinerie
     *      que DSLPROPS (byProp) pour synthétiser une fiche « membre » agrégée,
     *      navigable via F1 sous le nom FR ET EN du champ (MaxTokens, CodeÉtat,
     *      Endpoint…). Projet est SKIPPÉ ici : ses champs sont déjà émis dans
     *      DSLPROPS (w="Projet"). Source unique : les OwlField du registre
     *      (aucune desc inventée). ---- */
    printf("const TYPEFIELDS = [\n");
    {
        bool ft = true;
        for (int i = 0, nt = owl_registry_types_count(); i < nt; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (!t || !t->fields || !t->canonical) continue;
            if (strcmp(t->canonical, "Projet") == 0 ||
                (t->canonicalAscii && strcmp(t->canonicalAscii, "Projet") == 0)) continue;
            for (const OwlField* f = t->fields; f->name; f++) {
                if (!ft) printf(",\n");
                ft = false;
                printf("{\"tp\":"); json_escape(stdout, t->canonical);
                printf(",\"n\":"); json_escape(stdout, f->nameAccented ? f->nameAccented : f->name);
                if (f->nameEN) { printf(",\"en\":"); json_escape(stdout, f->nameEN); }
                printf(",\"t\":"); json_escape(stdout, f->type ? f->type : "");
                doc_emit_desc(f->descFR, f->descEN);
                printf("}");
            }
        }
    }
    printf("\n];\n");

    /* ---- events DSL (par widget + État) : surClic, FinInitialisation… Le
     *      template dédup par nom + agrège les widgets ("déclenché sur"). ---- */
    printf("const EVENTS = [\n");
    {
        bool fe = true;
        for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
            OwlControlKind kind;
            if (!owl_control_kind_from_token(k->nameFR, (int)strlen(k->nameFR), &kind, NULL)) continue;
            int ne = owl_event_catalog_count(kind);
            for (int j = 0; j < ne; j++) {
                const OwlEventDef* ev = owl_event_catalog_at(kind, j);
                if (!ev || !ev->canonical) continue;
                if (!fe) printf(",\n"); fe = false;
                printf("{\"w\":"); json_escape(stdout, k->nameAscii ? k->nameAscii : k->nameFR);
                printf(",\"n\":"); json_escape(stdout, ev->canonical);
                if (ev->nameEN) { printf(",\"en\":"); json_escape(stdout, ev->nameEN); }
                printf(",\"np\":%d", ev->paramCount);
                doc_emit_desc(ev->descFR, ev->descEN);
                printf("}");
            }
        }
        /* events catalogués à part : lifecycle Fenêtre + État (conteneur + sous-blocs) */
        struct { const char* w; int (*cnt)(void); const OwlEventDef* (*at)(int); } extra[] = {
            {"Fenetre",  owl_window_lifecycle_catalog_count,    owl_window_lifecycle_catalog_at},
            {"Etat",     owl_etat_event_catalog_count,          owl_etat_event_catalog_at},
            {"blocEtat", owl_etat_subcontrol_event_catalog_count, owl_etat_subcontrol_event_catalog_at},
        };
        for (int g = 0; g < 3; g++) {
            int ne = extra[g].cnt();
            for (int j = 0; j < ne; j++) {
                const OwlEventDef* ev = extra[g].at(j);
                if (!ev || !ev->canonical) continue;
                if (!fe) printf(",\n"); fe = false;
                printf("{\"w\":"); json_escape(stdout, extra[g].w);
                printf(",\"n\":"); json_escape(stdout, ev->canonical);
                if (ev->nameEN) { printf(",\"en\":"); json_escape(stdout, ev->nameEN); }
                printf(",\"np\":%d", ev->paramCount);
                doc_emit_desc(ev->descFR, ev->descEN);
                printf("}");
            }
        }
    }
    printf("\n];\n");

    /* ---- ERRORS : registre des messages d'erreur (owl_errors.def). Une
     *      fiche agrégée groupée par catégorie côté viewer. ---- */
    printf("const ERRORS = [\n");
    {
        int nerr = owl_registry_error_count();
        bool fr = true;
        for (int i = 0; i < nerr; i++) {
            const OwlErrorDef* e = owl_registry_error_at(i);
            if (!e || !e->name) continue;
            if (!fr) printf(",\n"); fr = false;
            const char* num = owl_registry_error_num(owl_registry_error_code_of(e));
            printf("{\"name\":"); json_escape(stdout, e->name);
            printf(",\"code\":"); json_escape(stdout, num ? num : "");
            printf(",\"cat\":"); json_escape(stdout, e->category ? e->category : "");
            doc_emit_cat_en(e->category ? e->category : "");
            doc_emit_desc(e->templateFR, e->templateEN);
            printf("}");
        }
    }
    printf("\n];\n");

    /* ---- LIMITS : limites compile-time (owl_limits.def). Fiche table. ---- */
    printf("const LIMITS = [\n");
    {
        int nlim = owl_registry_limit_count();
        bool fl = true;
        for (int i = 0; i < nlim; i++) {
            const OwlLimitDef* L = owl_registry_limit_at(i);
            if (!L || !L->name) continue;
            if (!fl) printf(",\n"); fl = false;
            printf("{\"name\":"); json_escape(stdout, L->name);
            printf(",\"value\":%d", L->value);
            printf(",\"cat\":"); json_escape(stdout, L->category ? L->category : "");
            doc_emit_cat_en(L->category ? L->category : "");
            doc_emit_desc(L->descFR, L->descEN);
            printf("}");
        }
    }
    printf("\n];\n");

    /* ---- OPERATORS : catalogue des opérateurs symboliques. Le viewer
     *      affiche une ligne par sens (un opérateur peut en avoir plusieurs). ---- */
    printf("const OPERATORS = [\n");
    {
        int nop = owl_operator_count();
        bool fo = true;
        for (int i = 0; i < nop; i++) {
            const OwlOperatorDef* o = owl_operator_at(i);
            if (!o || !o->display_form) continue;
            if (!fo) printf(",\n"); fo = false;
            printf("{\"form\":"); json_escape(stdout, o->display_form);
            printf(",\"cat\":"); json_escape(stdout, o->category ? o->category : "");
            doc_emit_cat_en(o->category ? o->category : "");
            printf(",\"senses\":[");
            bool fs = true;
            for (int s = 0; s < o->senses_count; s++) {
                const OwlKeywordSense* sn = &o->senses[s];
                if (!fs) printf(",");
                fs = false;
                printf("{\"labelFR\":"); json_escape(stdout, sn->label_fr ? sn->label_fr : "");
                printf(",\"labelEN\":"); json_escape(stdout, sn->label_en ? sn->label_en : "");
                printf(",\"syntax\":"); json_escape(stdout, sn->syntax ? sn->syntax : "");
                printf(",\"descFR\":"); json_escape(stdout, sn->descFR ? sn->descFR : "");
                printf(",\"descEN\":"); json_escape(stdout, sn->descEN ? sn->descEN : "");
                printf("}");
            }
            printf("]}");
        }
    }
    printf("\n];\n");

    /* ---- GLOBALVARS : variables globales prédéfinies. Fiche liste. ---- */
    printf("const GLOBALVARS = [\n");
    {
        int ng = owl_globalvar_catalog_count();
        bool fg = true;
        for (int i = 0; i < ng; i++) {
            const OwlGlobalVarDef* g = owl_globalvar_catalog_at(i);
            if (!g || !g->canonical) continue;
            if (!fg) printf(",\n"); fg = false;
            printf("{\"name\":"); json_escape(stdout, g->canonical);
            printf(",\"en\":"); json_escape(stdout, g->nameEN ? g->nameEN : "");
            printf(",\"type\":"); json_escape(stdout, g->type ? g->type : "");
            printf(",\"readonly\":%s", g->readOnly ? "true" : "false");
            doc_emit_desc(g->descFR, g->descEN);
            printf("}");
        }
    }
    printf("\n];\n");

    vm_free();

    fputs(at + strlen(MARK), stdout);
    free(tpl);
}

void dump_lsp_catalog(void) {
    vm_init();
    printf("/* AUTO-GENERATED by `owl --dump-lsp-catalog`. DO NOT EDIT BY HAND.\n");
    printf(" * Regenerate whenever the registry changes:\n");
    printf(" *   owl --dump-lsp-catalog > lsp/lsp_catalog_gen.c\n");
    printf(" */\n\n");
    printf("#include \"lsp_catalog.h\"\n\n");

    /* Pour chaque native, on emet d'abord son array OwlNativeSigInfo
     * en static const, puis on referencera ce tableau depuis l'entree
     * OWL_NATIVE_CATALOG. Le naming utilise l'index registry pour
     * eviter les collisions (les noms canon peuvent contenir des
     * accents incompatibles avec les identifiants C). */
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (!d->signatures) continue;

        /* Compte des sigs reelles (sentinel = returnType NULL). */
        int sigCount = 0;
        for (const OwlSignature* s = d->signatures; s->returnType; s++) sigCount++;
        if (sigCount == 0) continue;

        printf("static const OwlNativeSigInfo SIGS_%d[] = {\n", i);
        for (const OwlSignature* s = d->signatures; s->returnType; s++) {
            printf("    ");
            emit_native_sig_struct(d->canonical, s);
            printf(",\n");
        }
        printf("};\n");

        /* Aliases de la native (parite formatter : le matching native parcourt
         * canonical/ascii/EN/aliases — cf find_native_norm). */
        if (d->aliases && d->aliases[0]) {
            printf("static const char* const NAT_ALIASES_%d[] = {", i);
            for (const char* const* a = d->aliases; *a; a++) {
                emit_c_string(*a); printf(", ");
            }
            printf("0};\n");
        }
    }

    printf("\nconst OwlNativeInfo OWL_NATIVE_CATALOG[] = {\n");

    int emitted = 0;
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (!d->signatures) continue;

        int sigCount = 0;
        for (const OwlSignature* s = d->signatures; s->returnType; s++) sigCount++;
        if (sigCount == 0) continue;

        /* nameAscii: canonical with accents stripped (for LSP filterText) */
        char asciiName[256];
        scanner_strip_accents(d->canonical, (int)strlen(d->canonical),
                              asciiName, (int)sizeof(asciiName));

        printf("    {");
        emit_c_string(d->canonical); printf(", ");
        emit_c_string(d->nameEN ? d->nameEN : d->canonical); printf(", ");
        emit_c_string(asciiName); printf(",\n     ");
        emit_c_string(d->descFR ? d->descFR : ""); printf(", ");
        emit_c_string(d->descEN ? d->descEN : d->descFR ? d->descFR : ""); printf(",\n     ");
        emit_c_string(d->category ? d->category : "Divers");
        printf(", %d, SIGS_%d, ", sigCount, i);
        if (d->aliases && d->aliases[0]) printf("NAT_ALIASES_%d", i);
        else                             printf("0");
        printf("},\n");
        emitted++;
    }

    printf("};\n\n");
    printf("const int OWL_NATIVE_CATALOG_COUNT = %d;\n", emitted);

    /* ---- Constants ------------------------------------------------ */
    printf("\nconst OwlConstInfo OWL_CONST_CATALOG[] = {\n");
    {
        int nc = owl_registry_consts_count();
        int emittedC = 0;
        for (int i = 0; i < nc; i++) {
            const OwlConstDef* c = owl_registry_const_at(i);
            if (!c || !c->name) continue;
            printf("    {");
            emit_c_string(c->name); printf(", ");
            if (c->nameEN) emit_c_string(c->nameEN); else printf("0");
            printf(", ");
            emit_c_string(c->category ? c->category : "Divers"); printf(", ");
            emit_c_string(c->descFR ? c->descFR : "");
            printf("},\n");
            emittedC++;
        }
        printf("};\nconst int OWL_CONST_CATALOG_COUNT = %d;\n", emittedC);
    }

    /* ---- Types (with fields) -------------------------------------- */
    {
        int nt = owl_registry_types_count();

        /* First, emit a static field array per type (so we can point at
         * it from the main table). */
        for (int i = 0; i < nt; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (!t || !t->fields) continue;
            printf("\nstatic const OwlFieldInfo OWL_TYPE_FIELDS_%d[] = {\n", i);
            for (const OwlField* f = t->fields; f->name != NULL; f++) {
                const char* disp = f->nameAccented ? f->nameAccented : f->name;
                printf("    {");
                emit_c_string(disp); printf(", ");
                emit_c_string(f->type ? f->type : ""); printf(", ");
                emit_c_string(f->descFR ? f->descFR : ""); printf(", ");
                if (f->allowedBase)    emit_c_string(f->allowedBase->name);
                else                   printf("0");
                printf(", ");
                if (f->allowedOptions) emit_c_string(f->allowedOptions->name);
                else                   printf("0");
                printf(", ");
                emit_c_string(f->name); /* nameAscii : cle de match ASCII du formatter */
                printf(", ");
                /*  — corps hover bilingue : EN du champ (nom/desc) + EN
                 * du type du champ resolu via le registry. */
                emit_c_string(f->nameEN ? f->nameEN : disp); printf(", ");
                emit_c_string(f->descEN ? f->descEN : (f->descFR ? f->descFR : "")); printf(", ");
                emit_c_string(lsp_type_name_en(f->type ? f->type : "")); printf(", ");
                /*  — signature contractuelle d'un field callback
                 * (type:"procédure"), consommee par le hover LSP. 0 (NULL) pour
                 * tout field non-callback. */
                if (f->callbackSig) emit_c_string(f->callbackSig);
                else                printf("0");
                printf("},\n");
            }
            printf("};\n");
        }

        /*  — etats indexables via ..Etats[etat] par type collection
         * styleXxxEtats. Derives des tables compiler.c (STYLE_PALETTES widgets +
         * survol-only menus), jamais recopies : le catalogue LSP les lit pour la
         * completion/hover dans `Etats[`. Un array static par type porteur. */
        for (int i = 0; i < nt; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (!t) continue;
            int stateCount = compiler_etats_type_state_count(t->canonical);
            if (stateCount <= 0) continue;
            printf("\nstatic const OwlStateInfo OWL_TYPE_STATES_%d[] = {\n", i);
            for (int s = 0; s < stateCount; s++) {
                const char* fr = NULL; const char* en = NULL;
                compiler_etats_type_state_at(t->canonical, s, &fr, &en);
                printf("    {");
                emit_c_string(fr ? fr : ""); printf(", ");
                emit_c_string(en ? en : (fr ? fr : ""));
                printf("},\n");
            }
            printf("};\n");
        }

        printf("\nconst OwlTypeInfo OWL_TYPE_CATALOG[] = {\n");
        int emittedT = 0;
        for (int i = 0; i < nt; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (!t) continue;
            int fieldCount = 0;
            if (t->fields) for (const OwlField* f = t->fields; f->name; f++) fieldCount++;
            printf("    {");
            emit_c_string(t->canonical); printf(", ");
            emit_c_string(t->canonicalAscii ? t->canonicalAscii : t->canonical); printf(", ");
            if (t->nameEN) emit_c_string(t->nameEN); else printf("0");
            printf(", ");
            emit_c_string(t->category ? t->category : "Divers"); printf(", ");
            emit_c_string(t->descFR ? t->descFR : ""); printf(", ");
            /*  — descEN du type (fallback FR) pour hover bilingue. */
            emit_c_string(t->descEN ? t->descEN : (t->descFR ? t->descFR : "")); printf(", ");
            if (t->fields && fieldCount > 0)
                printf("OWL_TYPE_FIELDS_%d", i);
            else
                printf("0");
            printf(", %d, ", fieldCount);
            /* literalKeysEnum : nom de l'enum dont les valeurs sont les
             * cles legitimes d'un literal {} de ce type (autocomplete LSP). */
            if (t->literalKeysEnum) emit_c_string(t->literalKeysEnum->name);
            else                    printf("0");
            printf(", ");
            /*  — indexStates : etats valides via ..Etats[etat]. */
            int stateCount = compiler_etats_type_state_count(t->canonical);
            if (stateCount > 0) printf("OWL_TYPE_STATES_%d", i);
            else                printf("0");
            printf(", %d", stateCount);
            printf("},\n");
            emittedT++;
        }
        printf("};\nconst int OWL_TYPE_CATALOG_COUNT = %d;\n", emittedT);
    }

    /* ---- Enums (ordered sets of constant-identifier names) -------- */
    {
        int ne = owl_registry_enums_count();

        /* Emit a static values[] array per enum. */
        for (int i = 0; i < ne; i++) {
            const OwlEnum* e = owl_registry_enum_at(i);
            if (!e || !e->values) continue;
            printf("\nstatic const char* const OWL_ENUM_VALUES_%d[] = {\n", i);
            for (const char* const* v = e->values; *v; v++) {
                printf("    ");
                emit_c_string(*v);
                printf(",\n");
            }
            printf("    0\n};\n");
        }

        printf("\nconst OwlEnumInfo OWL_ENUM_CATALOG[] = {\n");
        int emittedE = 0;
        for (int i = 0; i < ne; i++) {
            const OwlEnum* e = owl_registry_enum_at(i);
            if (!e) continue;
            int vc = 0;
            if (e->values) for (const char* const* v = e->values; *v; v++) vc++;
            printf("    {");
            emit_c_string(e->name); printf(", ");
            emit_c_string(e->nameAscii ? e->nameAscii : e->name); printf(", ");
            if (e->nameEN) emit_c_string(e->nameEN); else printf("0");
            printf(", ");
            emit_c_string(e->category ? e->category : "Divers"); printf(", ");
            emit_c_string(e->descFR ? e->descFR : ""); printf(", ");
            if (e->values && vc > 0) printf("OWL_ENUM_VALUES_%d", i);
            else                     printf("0");
            printf(", %d, %d},\n", vc, e->combinable ? 1 : 0);
            emittedE++;
        }
        printf("};\nconst int OWL_ENUM_CATALOG_COUNT = %d;\n", emittedE);
    }

    /* ---- Extension methods (owl_methods.c tables) ---------------- */
    {
        printf("\nconst OwlMethodInfo OWL_METHOD_CATALOG[] = {\n");
        int total = method_catalog_count();
        int emittedM = 0;
        /* To detect aliases: remember (type, fn) pairs already seen.
         * If a new entry on the same type has the same fn pointer as a
         * previous one, it's an alias (e.g. "When" after "Quand"). */
        typedef struct { const char* type; NativeFn fn; } SeenEntry;
        SeenEntry seen[512];
        int seenCount = 0;
        for (int i = 0; i < total; i++) {
            const char* typeName = NULL;
            const MethodEntry* m = method_catalog_at(i, &typeName);
            if (!m || !m->name || !typeName) continue;
            int isAlias = 0;
            for (int k = 0; k < seenCount; k++) {
                if (seen[k].fn == m->function && strcmp(seen[k].type, typeName) == 0) {
                    isAlias = 1;
                    break;
                }
            }
            if (!isAlias && seenCount < 512) {
                seen[seenCount].type = typeName;
                seen[seenCount].fn = m->function;
                seenCount++;
            }
            /*  — résout la native sous-jacente par pointeur de
             * fonction pour exposer sa signature (params) au LSP. */
            const OwlNativeDef* nat = owl_registry_find_by_fn(m->function);
            printf("    {");
            emit_c_string(m->name); printf(", ");
            emit_c_string(typeName); printf(", %d, %d, ", m->arity, isAlias);
            if (nat && nat->canonical) emit_c_string(nat->canonical);
            else printf("0"); /* null const char* — convention du dump (cf params) */
            printf("},\n");
            emittedM++;
        }
        printf("};\nconst int OWL_METHOD_CATALOG_COUNT = %d;\n", emittedM);
    }

    /* ---- DSL Properties (Fenetre/Libelle/Bouton/Saisie/Table/colonne/
     *      Etat/blocEtat/LibelleEtat). Source unique : src/ui_catalog.c
     *      via owl_dsl_props_count()/owl_dsl_prop_at(). Le LSP autocomplete
     *      et le formatter consomment cette table. ------------------ */
    {
        printf("\nconst OwlDSLPropInfo OWL_DSL_PROP_CATALOG[] = {\n");
        int n = owl_dsl_props_count();
        int emittedP = 0;
        for (int i = 0; i < n; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (!e || !e->def || !e->def->nameFR) continue;
            const OwlControlPropDef* p = e->def;
            printf("    {");
            emit_c_string(e->kind); printf(", ");
            emit_c_string(p->nameFR); printf(", ");
            emit_c_string(p->nameAscii ? p->nameAscii : p->nameFR); printf(", ");
            emit_c_string(p->nameEN ? p->nameEN : p->nameFR); printf(", ");
            emit_c_string(prop_type_json(p->type)); printf(", ");
            if (p->enum_name) emit_c_string(p->enum_name);
            else              printf("0");
            printf(", ");
            emit_c_string(p->descFR ? p->descFR : ""); printf(", ");
            emit_c_string(p->descEN ? p->descEN : (p->descFR ? p->descFR : ""));
            printf(", ");
            /* object_type : type composé cible d'une prop OBJECT (Style→
             * styleSaisie). Le LSP l'utilise pour l'autocomplete des champs
             * dans `Style = { <cursor> }`. */
            if (p->object_type) emit_c_string(p->object_type);
            else                printf("0");
            printf("},\n");
            emittedP++;
        }
        printf("};\nconst int OWL_DSL_PROP_CATALOG_COUNT = %d;\n", emittedP);
    }

    /* V1.6 — section COMPOSED_TYPE_CATALOG. Itère le registry des types
     * réifiés (owl_registry_type_at) et dump les fields des templates
     * runtime correspondants (register_global_instance dans
     * stdlib_builtin_types.c). Le LSP s'en sert pour autocomplete dans
     * `var = { <cursor> }`. Ajouter un nouveau type composé : juste le
     * déclarer dans le registry — le LSP le voit automatiquement, aucun
     * filet manqué possible. */
    {
        /* Override pour les fields qui sont des constantes contraintes a un
         * enum mais stockees comme int dans le template. Le runtime ne sait
         * pas (l'OwlEnum est cote registry, pas cote ObjInstance), donc on
         * ajoute la map ici pour que l'autocomplete LSP connaisse l'enum. */
        static const struct {
            const char* typeName;
            const char* fieldName;
            const char* enumName;
        } ENUM_OVERRIDES[] = {
            {"Cadre", "Trait", "traitCadre"},
            {"Cadre", "Bords", "bordsCadre"},
            {"Trait", "Type",  "traitCadre"},
            /* Police.Nom : alignement avec PoliceNom flat shortcut
             * (allowedBase=iFamille). Liste de constantes iPolice* +
             * accepte string libre ("Arial"). */
            {"Police", "Nom",  "iFamille"},
            {NULL, NULL, NULL}
        };
        printf("\nconst OwlComposedTypeField OWL_COMPOSED_TYPE_CATALOG[] = {\n");
        int emittedCT = 0;
        char seen[256][64];
        int seenCount = 0;
        int reg_types_count = owl_registry_types_count();
        for (int ti = 0; ti < reg_types_count; ti++) {
            const OwlTypeDef* td = owl_registry_type_at(ti);
            if (!td || !td->canonical) continue;
            /* Essaie canonical (forme accentuée) puis canonicalAscii.
             *  : les templates vivent maintenant dans vm.types
             * (namespace separe). register_global_instance les y depose
             * sous la forme accentuée ou canonicalAscii selon les sites. */
            Value v;
            ObjString* tk = obj_copy_string(td->canonical, (int)strlen(td->canonical));
            bool found = table_get(&vm.types, tk, &v);
            if (!found && td->canonicalAscii
                       && strcmp(td->canonical, td->canonicalAscii) != 0) {
                tk = obj_copy_string(td->canonicalAscii, (int)strlen(td->canonicalAscii));
                found = table_get(&vm.types, tk, &v);
            }
            if (!found) continue;
            if (!IS_INSTANCE(v)) continue;
            ObjInstance* tpl = AS_INSTANCE(v);

            /* Skip si on a deja dump ce template (alias accentue/ASCII). */
            const char* tplKey = tpl->structName ? tpl->structName->chars : td->canonical;
            bool dup = false;
            for (int s = 0; s < seenCount; s++) {
                if (strcmp(seen[s], tplKey) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (seenCount < 256) {
                strncpy(seen[seenCount], tplKey, 63);
                seen[seenCount][63] = '\0';
                seenCount++;
            }

            /* Dump chaque field. Order : iterer fieldOrder si dispo, sinon
             * iterer la table fields directement. */
            if (tpl->fieldOrder && tpl->fieldOrder->elements.count > 0) {
                for (int fi = 0; fi < tpl->fieldOrder->elements.count; fi++) {
                    Value fnameVal = tpl->fieldOrder->elements.values[fi];
                    if (!IS_STRING(fnameVal)) continue;
                    ObjString* fname = AS_STRING(fnameVal);
                    Value fvalue;
                    if (!table_get(&tpl->fields, fname, &fvalue)) continue;
                    /* Skip les fields internes (commencent par __). */
                    if (fname->length >= 2 && fname->chars[0] == '_' && fname->chars[1] == '_')
                        continue;
                    const char* fieldType = "tout";
                    const char* subType = NULL;
                    if      (IS_INTEGER(fvalue)) fieldType = "int";
                    else if (IS_REAL(fvalue))    fieldType = "real";
                    else if (IS_STRING(fvalue))  fieldType = "string";
                    else if (IS_BOOL(fvalue))    fieldType = "bool";
                    else if (IS_INSTANCE(fvalue)) {
                        fieldType = "instance";
                        ObjInstance* sub = AS_INSTANCE(fvalue);
                        subType = sub->structName ? sub->structName->chars : NULL;
                    }
                    /* Override enum pour les fields scalaires contraints. */
                    for (int oi = 0; ENUM_OVERRIDES[oi].typeName; oi++) {
                        if (strcasecmp(ENUM_OVERRIDES[oi].typeName, tplKey) == 0
                            && strcasecmp(ENUM_OVERRIDES[oi].fieldName, fname->chars) == 0) {
                            fieldType = "enum";
                            subType = ENUM_OVERRIDES[oi].enumName;
                            break;
                        }
                    }
                    /* Override couleur : champ entier BGR exposé comme composé Couleur. */
                    {
                        const char* colorSub = composed_field_color_subtype(td, fname->chars);
                        if (colorSub) { fieldType = "instance"; subType = colorSub; }
                    }
                    const char* fnameEN = composed_field_name_en(td, fname->chars);
                    printf("    {");
                    emit_c_string(tplKey); printf(", ");
                    emit_c_string(fname->chars); printf(", ");
                    if (fnameEN) emit_c_string(fnameEN);
                    else         printf("0");
                    printf(", ");
                    emit_c_string(fieldType); printf(", ");
                    if (subType) emit_c_string(subType);
                    else         printf("0");
                    printf(", ");
                    emit_c_string("");  /* descFR vide pour V1.6 — V1.7 si besoin */
                    printf("},\n");
                    emittedCT++;
                }
            } else {
                /* Pas de fieldOrder : iter table fields directement. */
                for (int j = 0; j < tpl->fields.capacity; j++) {
                    Entry* e = &tpl->fields.entries[j];
                    if (!e->key) continue;
                    if (e->key->length >= 2 && e->key->chars[0] == '_' && e->key->chars[1] == '_')
                        continue;
                    const char* fieldType = "tout";
                    const char* subType = NULL;
                    if      (IS_INTEGER(e->value)) fieldType = "int";
                    else if (IS_REAL(e->value))    fieldType = "real";
                    else if (IS_STRING(e->value))  fieldType = "string";
                    else if (IS_BOOL(e->value))    fieldType = "bool";
                    else if (IS_INSTANCE(e->value)) {
                        fieldType = "instance";
                        ObjInstance* sub = AS_INSTANCE(e->value);
                        subType = sub->structName ? sub->structName->chars : NULL;
                    }
                    for (int oi = 0; ENUM_OVERRIDES[oi].typeName; oi++) {
                        if (strcasecmp(ENUM_OVERRIDES[oi].typeName, tplKey) == 0
                            && strcasecmp(ENUM_OVERRIDES[oi].fieldName, e->key->chars) == 0) {
                            fieldType = "enum";
                            subType = ENUM_OVERRIDES[oi].enumName;
                            break;
                        }
                    }
                    /* Override couleur : champ entier BGR exposé comme composé Couleur. */
                    {
                        const char* colorSub = composed_field_color_subtype(td, e->key->chars);
                        if (colorSub) { fieldType = "instance"; subType = colorSub; }
                    }
                    const char* fnameEN = composed_field_name_en(td, e->key->chars);
                    printf("    {");
                    emit_c_string(tplKey); printf(", ");
                    emit_c_string(e->key->chars); printf(", ");
                    if (fnameEN) emit_c_string(fnameEN);
                    else         printf("0");
                    printf(", ");
                    emit_c_string(fieldType); printf(", ");
                    if (subType) emit_c_string(subType);
                    else         printf("0");
                    printf(", ");
                    emit_c_string("");
                    printf("},\n");
                    emittedCT++;
                }
            }
        }
        printf("};\nconst int OWL_COMPOSED_TYPE_CATALOG_COUNT = %d;\n", emittedCT);
    }

    /* ================================================================
     * BUILTIN PROPERTY CATALOG — proprietes des types primitifs (Date,
     * Heure, DateHeure, Duree, Tableau, JSON, file/pile/liste). Source
     * canon : compiler.c::PROPS_*, iteres via compiler_iter_builtin_props
     * pour dedup et skip alias.
     * ================================================================ */
    {
        int bpcount = 0;
        g_bpKindsCount = 0;
        compiler_iter_builtin_props_kinds(collect_builtin_prop_kind_cb, NULL);
        printf("\nconst OwlBuiltinPropInfo OWL_BUILTIN_PROP_CATALOG[] = {\n");
        compiler_iter_builtin_props(emit_builtin_prop_cb, &bpcount);
        printf("};\nconst int OWL_BUILTIN_PROP_CATALOG_COUNT = %d;\n", bpcount);
    }

    vm_free();
}

/* owl.exe reste runtime-only (REPL, run, test, fmt, lsp, GUI). Le packaging
 * de projet vit dans le binaire dedie owl-build (src/build_main.c). */

/* ================================================================
 * FMT COMMAND
 * ================================================================ */

/* ================================================================
 * CHECK COMMAND -- compile-only, output errors as JSON
 * ================================================================ */

static void print_json_escaped(const char* s) {
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if ((unsigned char)*s < 0x20) {
                    printf("\\u%04x", (unsigned char)*s);
                } else {
                    fputc(*s, stdout);
                }
        }
    }
}

/* Canonise un chemin pour comparaison robuste entre `activeFile` (cote CLI =
 * argv, cote LSP = filepath complet) et `fileDiags[i].path` (issu du walk
 * projet). Les deux peuvent differer en forme : separateurs `/` vs `\`,
 * relatif vs absolu, `./` ou `..` parasites. On resout en absolu canonique
 * (_fullpath/realpath) puis on uniformise les separateurs en `/`. Sous
 * Windows on minuscule aussi (FS insensible a la casse) pour que la
 * comparaison soit coherente avec le systeme de fichiers reel.
 * `out` doit faire au moins 1024 octets. Renvoie `out` (jamais NULL : si la
 * resolution absolue echoue, on normalise le chemin tel quel). */
static char* path_canonicalize(const char* in, char* out, size_t outSize) {
    char resolved[1024];
#ifdef _WIN32
    if (_fullpath(resolved, in, sizeof(resolved)) == NULL) {
        snprintf(resolved, sizeof(resolved), "%s", in);
    }
#else
    if (realpath(in, resolved) == NULL) {
        snprintf(resolved, sizeof(resolved), "%s", in);
    }
#endif
    size_t n = 0;
    for (const char* p = resolved; *p && n + 1 < outSize; p++, n++) {
        char c = *p;
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
        out[n] = c;
    }
    out[n] = '\0';
    return out;
}

/* Chemin d'un fichier du perimetre tel qu'on le NOMME a l'utilisateur dans un
 * diagnostic : relatif a la racine de perimetre — donc le basename en mode
 * script, `sous/dossier/x.owl` en mode projet. Ni le cwd du process (un serveur
 * de controle resident n'a pas celui de l'utilisateur), ni un absolu (qui
 * rendrait chaque snapshot dependant de la machine, les corpus de fixtures
 * etant recopies dans un dossier de travail horodate). La racine est
 * reconstructible par le consommateur : c'est celle d' pour le fichier
 * controle. `path` et `root` viennent de la meme decouverte (path = root + '/' +
 * nom), le prefixe est donc litteral ; sinon on retombe sur le basename. */
static const char* path_relative_to_root(const char* path, const char* root) {
    size_t rlen = (root != NULL) ? strlen(root) : 0;
    if (rlen > 0 && strncmp(path, root, rlen) == 0
        && (path[rlen] == '/' || path[rlen] == '\\')) {
        return path + rlen + 1;
    }
    return owl_format_path_basename(path);
}

/* --check sur un dossier OU sur un fichier avec contexte projet.
 *
 * `dir` est interprete par la regle de frontiere ( + ) :
 * resolve_project_root remonte depuis <dir> et determine la racine reelle +
 * le scope (recursif ssi un `projet.owl` ancetre existe, sinon dossier seul).
 *
 * Deux modes selon activeFile :
 *  - activeFile == NULL  : check projet complet, sortie multi-fichier
 *    {"files":[{"path":"...","errors":[...],"warnings":[...]}, ...]}
 *  - activeFile != NULL  : check projet pour le CONTEXTE, mais ne rapporte
 *    QUE les diagnostics du fichier cible, au format mono-fichier
 *    {"errors":[...],"warnings":[...]} consomme par le LSP du save.
 *    (`owl --check <fichier> --project <dir>`) */
int handle_check_command(const char* filepath); /* fallback mono (defini plus bas) */

/* Parser JSON interne (stdlib_json.c) reutilise pour lire le petit JSON de
 * controle des overlays sur stdin ( Lot 1). */
extern ObjJson* json_parse_cstr(const char* src, int len);

/* Slurp complet de stdin dans un buffer malloc'd (NUL-termine). Renvoie NULL
 * si vide. `outLen` recoit la taille (hors NUL). Utilise pour --overlay-stdin :
 * le parent (LSP) ecrit un JSON puis ferme le pipe. */
static char* slurp_stdin(int* outLen) {
    size_t cap = 8192, len = 0;
    char* buf = malloc(cap);
    if (buf == NULL) return NULL;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char* grown = realloc(buf, cap);
            if (grown == NULL) { free(buf); return NULL; }
            buf = grown;
        }
    }
    buf[len] = '\0';
    if (outLen != NULL) *outLen = (int)len;
    if (len == 0) { free(buf); return NULL; }
    return buf;
}

/*  Lot 2a — en mode resident (`--check-server`), le JSON a deja ete lu
 * et deframe sur stdin avant le lancement du check : il est fourni ici au lieu
 * d'etre slurpe. La requete resident porte la cle "overlays" A SA RACINE, donc
 * exactement le meme parseur sert aux deux chemins — c'est ce qui garantit que
 * le resident applique les memes overlays que le CLI, sans code parallele.
 * NULL = chemin CLI historique (slurp de stdin). */
static const char* g_check_request_json = NULL;

/* Lit le JSON overlay sur stdin ({"overlays":[{"path":..,"content":..}]}) et
 * enregistre chaque entree sur `project`. Silencieux si stdin vide / JSON
 * invalide (le check retombe alors sur le disque, comportement sans overlay).
 * A appeler apres project_init, avant project_discover_scoped, avec la VM
 * initialisee (le parser alloue des ObjJson via le GC). */
static void load_overlays_from_stdin(Project* project) {
    int len = 0;
    char* buf;
    if (g_check_request_json != NULL) {
        len = (int)strlen(g_check_request_json);
        buf = safe_strdup(g_check_request_json);
    } else {
        buf = slurp_stdin(&len);
    }
    if (buf == NULL) return;

    ObjJson* root = json_parse_cstr(buf, len);
    free(buf);
    if (root == NULL || root->jtype != JSON_T_OBJECT) return;

    vm_push(OBJ_VAL(root)); /* root pendant les obj_copy_string des cles */
    ObjString* kOverlays = obj_copy_string("overlays", 8);
    ObjJson* arr = obj_json_object_get(root, kOverlays);
    if (arr != NULL && arr->jtype == JSON_T_ARRAY) {
        ObjString* kPath = obj_copy_string("path", 4);
        ObjString* kContent = obj_copy_string("content", 7);
        for (int i = 0; i < arr->as.arrayVal.count; i++) {
            ObjJson* item = arr->as.arrayVal.items[i];
            if (item == NULL || item->jtype != JSON_T_OBJECT) continue;
            ObjJson* pathNode = obj_json_object_get(item, kPath);
            ObjJson* contentNode = obj_json_object_get(item, kContent);
            if (pathNode == NULL || pathNode->jtype != JSON_T_STRING) continue;
            if (contentNode == NULL || contentNode->jtype != JSON_T_STRING) continue;
            project_set_overlay(project, pathNode->as.strVal->chars,
                                contentNode->as.strVal->chars);
        }
    }
    vm_pop();
}

/* Non-static : drive in-process par cli_ops_test.c (self-test-cli-ops) pour
 * couvrir le check en mode DOSSIER (deux passes contexte + capture), que le
 * shim mono-fichier ne peut pas exercer. */
int handle_check_project_command(const char* dir, const char* activeFile,
                                 bool overlayStdin) {
    /*  : si la cible (`--check <fichier> --project`) est un `test_*.owl`,
     * son contexte doit inclure les autres tests (et les sous-dossiers test/
     * ne doivent pas etre sautes a la decouverte). Cible prod ou mode dossier
     * (`activeFile == NULL`) : comportement inchange (tests exclus). */
    bool targetIsTest = (activeFile != NULL &&
                         strncmp(owl_format_path_basename(activeFile), "test_", 5) == 0);

    /*  : `<dir>` n'est plus une racine recursive forcee —
     * c'est un *point de remontee*. On le passe a resolve_project_root via une
     * sonde fictive : si un `projet.owl` ancetre existe, racine = ce projet
     * (scan arbre raciné) ; sinon racine = <dir> lui-meme (mode SCRIPT/ENTRY,
     * scope dossier seul, pas de scan recursif). Symetrie avec la resolution
     * implicite (`--check <fichier>` sans --project, ) qui passe
     * directement par resolve_project_root sur le fichier cible.
     *
     * Quand un projet.owl ancetre est trouve, on REDIRIGE vers cette racine
     * (resolvedRoot). Sans projet.owl, on garde `dir` brut pour preserver le
     * format des paths sortie (compat mode dossier `owl --check .` qui emet
     * `./file.owl` plutot qu'une normalisation absolue). */
    char resolvedRoot[1024];
    const char* scanRoot = dir;
    bool recursive;
    {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/__owl_probe.owl", dir);
        ProjectMode mode = resolve_project_root(probe, resolvedRoot,
                                                sizeof(resolvedRoot));
        recursive = (mode == PROJECT_MODE_PROJECT);
        if (recursive) scanRoot = resolvedRoot;
    }

    /* Mode mono-fichier sans projet.owl ancetre : le scope est le dossier seul
     * de <dir>. Si la cible vit ailleurs (sous-dossier hors scope du mode
     * SCRIPT), le prescan projet n'apporte rien ; on retombe directement sur
     * le check mono qui rapportera les erreurs intra-fichier du fichier cible.
     * Evite aussi le stderr "no .owl files found" quand le dossier scope est
     * vide.
     *
     *  : exception quand la cible est un VOISIN direct de <scanRoot>
     * (meme dossier). En mode SCRIPT le scope EST le dossier courant + ses
     * voisins ; le check mono ignore ces voisins (il prescanne mais
     * ne compile pas), donc les champs/super d'une classe de base voisine ne
     * peuplent pas g_class_field_vis -> faux positif "membre inconnu" sur un
     * membre herite. On laisse alors le flow passer par la boucle projet (qui
     * compile les voisins, cf. passe de contexte plus bas) pour resoudre
     * l'heritage cross-fichier comme run/test. La cible dans un SOUS-dossier de
     * <scanRoot> (hors scope SCRIPT, cf. case_017) garde l'early return mono. */
    if (activeFile != NULL && !recursive) {
        char afDir[1024], afCanonEarly[1024], rootCanonEarly[1024];
        path_canonicalize(activeFile, afCanonEarly, sizeof(afCanonEarly));
        /* dossier parent de la cible canonique */
        afDir[0] = '\0';
        {
            const char* sep = strrchr(afCanonEarly, '/');
            if (sep != NULL) {
                size_t n = (size_t)(sep - afCanonEarly);
                if (n >= sizeof(afDir)) n = sizeof(afDir) - 1;
                memcpy(afDir, afCanonEarly, n);
                afDir[n] = '\0';
            }
        }
        path_canonicalize(scanRoot, rootCanonEarly, sizeof(rootCanonEarly));
        bool targetIsNeighbor = (afDir[0] != '\0' &&
                                 strcmp(afDir, rootCanonEarly) == 0);
        if (!targetIsNeighbor) {
            return handle_check_command(activeFile);
        }
    }

    vm_init();

    Project project;
    project_init(&project);
    if (overlayStdin) load_overlays_from_stdin(&project);
    if (!project_discover_scoped(&project, scanRoot, !targetIsTest, recursive)) {
        project_free(&project);
        printf("{\"errors\":[{\"line\":1,\"column\":1,\"message\":\"Cannot discover project\"}]}\n");
        return 65;
    }

    SymbolTable* symbols = symtable_create();

    /*  — en mode projet (recursive), restreint la decouverte aux
     * fichiers a bloc DSL + la cible (activeFile) : coherence avec le modele
     * d'execution (project_run). En mode dossier (activeFile==NULL), aucune
     * entree forcee — projet.owl participe via son bloc DSL. En mode SCRIPT
     * (recursive==false), pas de filtre. */
    int checkEntryIndex = -1;
    if (activeFile != NULL) {
        char afCanonEntry[1024];
        path_canonicalize(activeFile, afCanonEntry, sizeof(afCanonEntry));
        for (int i = 0; i < project.fileCount; i++) {
            char fdCanon[1024];
            path_canonicalize(project.files[i].path, fdCanon, sizeof(fdCanon));
            if (strcmp(fdCanon, afCanonEntry) == 0) { checkEntryIndex = i; break; }
        }
    }

    /* Prescan : on saute les tests SAUF si la cible est elle-meme un test
     * (alors le contexte inclut les autres tests). La cible n'est jamais
     * sautee : prod => non-isTest ; test => targetIsTest vrai. */
    prescan_set_project_scope(recursive, checkEntryIndex);
    compiler_usage_scan_sources_clear();
    for (int i = 0; i < project.fileCount; i++) {
        if (!targetIsTest && project.files[i].isTest) {
            /*  — non indexe, mais compte comme usage : cf. le meme
             * couple dans le controle mono. C'est ce chemin (cible dossier)
             * que l'editeur relance en continu. */
            compiler_usage_scan_source_add(project.files[i].source, i);
            continue;
        }
        prescan_file(symbols, project.files[i].source, i);
    }
    prescan_set_project_scope(false, -1);

    /* Collect per-file diagnostics, then emit JSON with file attribution. */
    typedef struct { int line; int col; char msg[256]; int sev; } Diag;
    typedef struct { char path[512]; Diag diags[128]; int diagCount; } FileDiag;
    /* Un slot par fichier decouvert (jusqu'a MAX_PROJECT_FILES). sizeof(FileDiag)
     * ~35 Ko => ~9 Mo : sur le tas plutot qu'en BSS static, libere a chaque
     * sortie de la fonction. Le cap historique de 64 avalait silencieusement les
     * diagnostics des fichiers 65+. */
    FileDiag* fileDiags = (FileDiag*)malloc(sizeof(FileDiag) * MAX_PROJECT_FILES);
    int fileDiagCount = 0;

    /* En mode activeFile, seul le fichier cible doit etre compile : les autres
     * fichiers du projet n'ont servi qu'au contexte (prescan). On resout la
     * cible par chemin canonique (meme matching que la phase de report plus
     * bas, ). En mode dossier, on compile tous les fichiers prod. */
    char afCanonCompile[1024];
    if (activeFile != NULL) path_canonicalize(activeFile, afCanonCompile, sizeof(afCanonCompile));

    /*  +  : contexte POO cross-fichier, dans LES DEUX modes.
     *
     * Le prescan (SymbolTable) ne peuple PAS les registres persistants du
     * compilateur (g_class_super / g_class_field_vis / g_class_constructor_arity) ;
     * ceux-ci ne se remplissent qu'au compile_in_project d'un fichier. La
     * resolution d'un membre herite (colon_member_cross_check_global walk la
     * chaine de super) a donc besoin de l'etat POO de TOUS les fichiers prod
     * AVANT la capture, sinon une classe derivee checkee avant sa classe de base
     * (ordre de decouverte : cBiere < cBoisson) ne voit pas les champs herites
     * -> faux positif "membre inconnu".
     *
     *  : au lieu de full-compiler chaque voisin ( originel — le
     * commentaire disait explicitement que SEULS super/field_vis etaient
     * preserves, le reste des side-effects etant vide juste apres), on HYDRATE
     * ces registres directement depuis le prescan (super + arite cstr + vis des
     * champs). Sur un gros projet combine, le full-compile des voisins coutait
     * ~8 s par frappe editeur quel que soit le fichier edite ; l'hydratation est
     * quasi nulle. Recompute-from-source preserve (sources relues fraiches a
     * chaque check) -> compatible . Plus de side-effect fenetres/Etats/
     * services a nettoyer : l'hydratation ne touche que les tables POO, donc les
     * resets  (vm_windows_clear / vm_etats_clear / owl_service_*_reset)
     * qui n'existaient que pour defaire la pollution du full-compile sont retires.
     *
     * N'affecte ni la decouverte (resolvedRoot / project_discover_scoped /
     * recursive) ni le perimetre rapporte. */

    /*  — precalcule l'etat voisins (widgets/globales/collections) une
     * fois pour tout le check : la capture rejoue cet etat par fichier au lieu de
     * re-tokeniser tous les voisins (O(N^2) -> O(N)). */
    compiler_neighbor_prebuild(symbols);

    /*  — hydratation POO cross-fichier depuis le prescan (remplace le
     * full-compile des voisins de la passe de contexte ). */
    compiler_hydrate_constructor_arity_from_symbols(symbols);  /* super + arite cstr */
    compiler_hydrate_field_vis_from_symbols(symbols);          /* visibilite champs */
    compiler_hydrate_method_vis_from_symbols(symbols);         /* visibilite methodes */

    for (int i = 0; i < project.fileCount && fileDiagCount < MAX_PROJECT_FILES; i++) {
        if (activeFile != NULL) {
            char fdCanon[1024];
            path_canonicalize(project.files[i].path, fdCanon, sizeof(fdCanon));
            if (strcmp(fdCanon, afCanonCompile) != 0) continue;
        } else if (project.files[i].isTest) {
            continue;
        }

        CompileDiagnostic errors[128];
        int errorCount = 0;
        compile_set_error_capture(errors, 128, &errorCount);
        CompileDiagnostic warnings[128];
        int warningCount = 0;
        compile_set_warning_capture(warnings, 128, &warningCount);

        /*  : passer le path complet (pas le stem .name) pour que
         * l'exemption path-based du verrou DSL (cf. ) match aussi
         * en mode dossier. Diags JSON pointeront vers le path reel. */
        compile_in_project(project.files[i].source, project.files[i].path, symbols, i);

        compile_set_error_capture(NULL, 0, NULL);
        compile_set_warning_capture(NULL, 0, NULL);

        FileDiag* fd = &fileDiags[fileDiagCount++];
        strncpy(fd->path, project.files[i].path, 511);
        fd->path[511] = '\0';
        fd->diagCount = 0;

        for (int j = 0; j < errorCount && fd->diagCount < 128; j++) {
            Diag* d = &fd->diags[fd->diagCount++];
            d->line = errors[j].line;
            d->col  = errors[j].column;
            d->sev  = 1;
            strncpy(d->msg, errors[j].message, 255);
            d->msg[255] = '\0';
        }
        for (int j = 0; j < warningCount && fd->diagCount < 128; j++) {
            Diag* d = &fd->diags[fd->diagCount++];
            d->line = warnings[j].line;
            d->col  = warnings[j].column;
            d->sev  = 2;
            strncpy(d->msg, warnings[j].message, 255);
            d->msg[255] = '\0';
        }
    }

    /*  — l'etat voisins precalcule tient des pointeurs sur symbols->
     * sources[] : le liberer avant toute destruction du symtable et avant tout
     * chemin de sortie ulterieur (fallback handle_check_command inclus). */
    compiler_neighbor_prebuild_reset();

    /* Mode mono-fichier filtre : ne rapporte que le fichier cible, au format
     * {"errors":[...],"warnings":[...]}. Le contexte projet (prescan ci-dessus)
     * a supprime les faux positifs cross-fichier, mais on n'expose que les
     * diagnostics du fichier que l'editeur vient de sauver. */
    if (activeFile != NULL) {
        /* Matching par chemin complet canonique (pas basename) : deux fichiers
         * de meme nom dans des sous-dossiers distincts (models/widget.owl vs
         * core/widget.owl) ne doivent pas se confondre. */
        char afCanon[1024];
        path_canonicalize(activeFile, afCanon, sizeof(afCanon));
        FileDiag* target = NULL;
        for (int i = 0; i < fileDiagCount; i++) {
            char fdCanon[1024];
            path_canonicalize(fileDiags[i].path, fdCanon, sizeof(fdCanon));
            if (strcmp(fdCanon, afCanon) == 0) {
                target = &fileDiags[i];
                break;
            }
        }

        /* Fallback : aucun FileDiag ne matche le fichier cible (fichier neuf
         * pas encore dans le projet scanne, hors arborescence, mismatch de
         * forme non resolu...). On NE renvoie PAS {"errors":[],"warnings":[]}
         * silencieux : ce serait un faux negatif masquant un fichier
         * potentiellement casse. On retombe sur le check mono historique
         * (`owl --check <fichier>` sans contexte projet) pour rapporter au
         * moins les erreurs intra-fichier du fichier cible. */
        if (target == NULL) {
            free(fileDiags);
            compiler_usage_scan_sources_clear();
            symtable_destroy(symbols);
            project_free(&project);
            vm_free();
            return handle_check_command(activeFile);
        }

        printf("{\"errors\":[");
        bool first = true;
        int targetErrors = 0;
        for (int j = 0; j < target->diagCount; j++) {
            if (target->diags[j].sev != 1) continue;
            if (!first) printf(",");
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   target->diags[j].line, target->diags[j].col);
            print_json_escaped(target->diags[j].msg);
            printf("\"}");
            first = false;
            targetErrors++;
        }
        printf("],\"warnings\":[");
        first = true;
        for (int j = 0; j < target->diagCount; j++) {
            if (target->diags[j].sev != 2) continue;
            if (!first) printf(",");
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   target->diags[j].line, target->diags[j].col);
            print_json_escaped(target->diags[j].msg);
            printf("\"}");
            first = false;
        }
        printf("]}\n");

        free(fileDiags);
        compiler_usage_scan_sources_clear();
        symtable_destroy(symbols);
        project_free(&project);
        vm_free();
        return targetErrors > 0 ? 65 : 0;
    }

    /* Emit JSON: per-file format so the LSP can attribute each diagnostic correctly */
    printf("{\"files\":[");
    for (int i = 0; i < fileDiagCount; i++) {
        FileDiag* fd = &fileDiags[i];
        if (i > 0) printf(",");
        printf("{\"path\":\"");
        print_json_escaped(fd->path);
        printf("\",\"errors\":[");
        bool first = true;
        for (int j = 0; j < fd->diagCount; j++) {
            if (fd->diags[j].sev != 1) continue;
            if (!first) printf(",");
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   fd->diags[j].line, fd->diags[j].col);
            print_json_escaped(fd->diags[j].msg);
            printf("\"}");
            first = false;
        }
        printf("],\"warnings\":[");
        first = true;
        for (int j = 0; j < fd->diagCount; j++) {
            if (fd->diags[j].sev != 2) continue;
            if (!first) printf(",");
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   fd->diags[j].line, fd->diags[j].col);
            print_json_escaped(fd->diags[j].msg);
            printf("\"}");
            first = false;
        }
        printf("]}");
    }
    printf("]}\n");

    int totalErrors = 0;
    for (int i = 0; i < fileDiagCount; i++)
        for (int j = 0; j < fileDiags[i].diagCount; j++)
            if (fileDiags[i].diags[j].sev == 1) totalErrors++;

    free(fileDiags);
    compiler_usage_scan_sources_clear();
    symtable_destroy(symbols);
    project_free(&project);
    vm_free();
    return totalErrors > 0 ? 65 : 0;
}

/* Lot V — owl preview <file.owl> : ouvre la 1ere fenetre du source,
 * recharge automatiquement a chaque modification du fichier. Necessite
 * un build avec Qt (OWL_HAS_GUI). */
#ifdef OWL_HAS_GUI
extern int stdlib_ui_preview_run(const char* filepath);
static int handle_preview_command(const char* filepath) {
    vm_init();
    int rc = stdlib_ui_preview_run(filepath);
    vm_free();
    return rc;
}
#else
static int handle_preview_command(const char* filepath) {
    (void)filepath;
    fprintf(stderr, "owl preview: requiert un build avec Qt. "
            "Utiliser owl.exe a la place.\n");
    return 64;
}
#endif

/* Variante interne : suppose vm_init() déjà fait par le caller, ne fait
 * PAS vm_free() à la fin. Utilisé par le serveur MCP (cli_mcp.c) qui ne
 * peut pas se permettre de détruire le registry entre deux tool calls. */
int handle_check_command_no_vm_lifecycle_ex(const char* filepath,
                                            const char* overrideSource);

int handle_check_command_no_vm_lifecycle(const char* filepath) {
    return handle_check_command_no_vm_lifecycle_ex(filepath, NULL);
}

/* Wrapper public qui gère le lifecycle VM — pour `owl --check <file>` en
 * standalone. */
int handle_check_command(const char* filepath) {
    vm_init();
    int rc = handle_check_command_no_vm_lifecycle(filepath);
    vm_free();
    return rc;
}

/*  — `owl --check <fichier> --overlay-stdin` en mode SCRIPT : mêmes
 * diagnostics que `owl --check <fichier>` (format mono {"errors":[...]}) mais
 * le contenu du fichier cible provient du buffer non-sauvé injecté sur stdin
 * plutôt que du disque. Le scope reste strictement mono (dossier du fichier +
 * voisins prescannés depuis le disque, exactement comme sans flag) : on ne
 * bascule PAS vers un scan projet. */
int handle_check_command_overlay(const char* filepath) {
    vm_init();
    Project overlayProj;
    project_init(&overlayProj);
    load_overlays_from_stdin(&overlayProj);
    const char* overlaySrc = project_get_overlay(&overlayProj, filepath);
    /* Copie locale : project_free libère la table overlay juste après. */
    char* overrideSource = (overlaySrc != NULL) ? safe_strdup(overlaySrc) : NULL;
    project_free(&overlayProj);
    int rc = handle_check_command_no_vm_lifecycle_ex(filepath, overrideSource);
    free(overrideSource);
    vm_free();
    return rc;
}

/* ================================================================
 *  Lot 2a — `owl --check-server` : le check en process RESIDENT
 *
 * Pourquoi : le seul chargement du binaire (~100 Mo) coute ~150 ms a chaud et
 * jusqu'a ~5 s a froid (scan antivirus), AVANT le moindre travail utile —
 * `--version`, qui ne fait qu'un printf, se mesure au meme prix. Le LSP relance
 * un check debounce toutes les 400 ms pendant la frappe : ce plancher etait
 * repaye a chaque fois. En resident, il est paye UNE FOIS par session LSP.
 *
 * PROTOCOLE — framing par prefixe de longueur, dans les DEUX sens :
 *
 *     Content-Length: <n>\r\n\r\n<n octets>
 *
 * Le meme que la couche transport LSP, deja maitrisee des deux cotes. Un
 * delimiteur textuel (ligne vide, sentinelle) serait ambigu : les contenus
 * overlay portent des sauts de ligne et du texte utilisateur arbitraire. Le
 * prefixe de longueur rend aussi le CORPS DE REPONSE totalement opaque — c'est
 * exactement l'octet-pour-octet qu'aurait ecrit `owl --check`, sans reencodage,
 * ce qui est la garantie d'identite recherchee.
 *
 * REQUETE (JSON, cles dans cet ordre) :
 *     {"target":"<fichier ou dossier>","lang":"fr|en","overlays":[{path,content}]}
 *   - `target` : la cible du check, exactement l'argument de `owl --check`.
 *   - `lang`   : equivalent de `--lang en`.
 *   - `overlays` (optionnel) : presence = equivalent de `--overlay-stdin`.
 *
 * REPONSE : les octets stdout+stderr du check. Les deux flux sont fusionnes
 * parce que le spawn par check les fusionnait deja dans un pipe unique — le
 * verdict consomme par le LSP est donc identique. Corollaire utile : plus rien
 * ne peut fuir sur le canal de reponse et desynchroniser le framing.
 *
 * ETAT ENTRE DEUX REQUETES : chaque handler de check fait deja son propre
 * vm_init/vm_free ; on y ajoute compiler_reset_poo_state(), exactement la
 * recette du self-test compile-error qui rejoue 137 fixtures dans un process
 * unique sans fuite d'etat compile-time.
 * ================================================================ */

#ifdef _WIN32
#  include <process.h>          /* _getpid : nom de fichier de capture unique */
#  define owl_srv_fileno  _fileno
#  define owl_srv_dup     _dup
#  define owl_srv_dup2    _dup2
#  define owl_srv_close   _close
#  define owl_srv_open    _open
#  define owl_srv_unlink  _unlink
#  define owl_srv_getpid  _getpid
#  ifndef O_BINARY
#    define O_BINARY _O_BINARY
#  endif
#else
#  include <fcntl.h>            /* open, O_WRONLY/O_CREAT/O_TRUNC : musl a des includes stricts */
#  include <unistd.h>           /* close, dup, dup2, unlink, getpid */
#  define owl_srv_fileno  fileno
#  define owl_srv_dup     dup
#  define owl_srv_dup2    dup2
#  define owl_srv_close   close
#  define owl_srv_open    open
#  define owl_srv_unlink  unlink
#  define owl_srv_getpid  getpid
#  ifndef O_BINARY
#    define O_BINARY 0
#  endif
#endif

/* Garde-fou : une requete plus grosse que ca n'est pas un buffer d'editeur mais
 * un flux corrompu (desynchronisation du framing). On coupe la session. */
#define CHECK_SERVER_MAX_REQUEST (64 * 1024 * 1024)

/* Extrait la valeur chaine de `key` dans la requete. Deliberement sans le
 * parseur JSON du runtime : cette couche tourne ENTRE deux checks, quand aucune
 * VM n'existe (le parseur alloue des ObjJson via le GC).
 *
 * La recherche est bornee a la tete de la requete, avant `"overlays"` : les
 * contenus overlay sont du texte utilisateur arbitraire et pourraient contenir
 * la sequence `"target":`. C'est pourquoi le protocole impose target/lang AVANT
 * overlays. Renvoie 1 si trouve. */
static int check_server_get_str(const char* json, const char* key,
                                char* out, int cap) {
    out[0] = '\0';
    char pat[32];
    int patLen = snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* limit = strstr(json, "\"overlays\"");
    const char* p = strstr(json, pat);
    if (p == NULL || (limit != NULL && p > limit)) return 0;
    p += patLen;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int n = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p != '\0') {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    /* Aucun chemin realiste ne passe par la (le LSP serialise
                     * les paths en UTF-8 brut) : on garde l'octet tel quel pour
                     * ne pas produire un chemin silencieusement faux. */
                    if (n < cap - 1) out[n++] = '\\';
                    c = 'u';
                    break;
                }
                default: c = e; break;   /* \\ et \" */
            }
        }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = '\0';
    return (*p == '"');
}

/* Lit une requete framee sur stdin. Renvoie le corps malloc'd (NUL-termine) ou
 * NULL sur EOF propre / frame invalide — dans les deux cas le serveur s'arrete
 * (le LSP est parti, ou le canal est desynchronise et plus rien n'est fiable). */
static char* check_server_read_request(void) {
    long contentLength = -1;
    for (;;) {
        char line[512];
        int n = 0, c;
        while ((c = getchar()) != EOF && c != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
        }
        if (c == EOF && n == 0) return NULL;      /* EOF entre deux requetes */
        line[n] = '\0';
        while (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
        if (n == 0) break;                        /* ligne vide = fin d'en-tete */
        if (c == EOF) return NULL;                /* en-tete tronque */
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            contentLength = strtol(line + 15, NULL, 10);
        }
    }
    if (contentLength < 0 || contentLength > CHECK_SERVER_MAX_REQUEST) return NULL;
    char* body = malloc((size_t)contentLength + 1);
    if (body == NULL) return NULL;
    size_t got = fread(body, 1, (size_t)contentLength, stdin);
    body[got] = '\0';
    if ((long)got != contentLength) { free(body); return NULL; }
    return body;
}

int handle_check_server_command(void) {
#ifdef _WIN32
    /* Binaire des DEUX cotes : en mode texte, la traduction CRLF du CRT
     * decalerait les octets par rapport au Content-Length annonce. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Capture du verdict par fichier temporaire plutot que par pipe : un pipe
     * se remplit (~64 Ko) et, comme le serveur est mono-thread, il se
     * bloquerait lui-meme sur un gros projet. Meme recette que le self-test
     * compile-error. */
    const char* tmpDir = getenv("TMP");
    if (tmpDir == NULL || tmpDir[0] == '\0') tmpDir = getenv("TEMP");
    if (tmpDir == NULL || tmpDir[0] == '\0') tmpDir = getenv("TMPDIR");
    if (tmpDir == NULL || tmpDir[0] == '\0') tmpDir = ".";
    char capturePath[1024];
    snprintf(capturePath, sizeof(capturePath), "%s/owl_check_server_%lu.out",
             tmpDir, (unsigned long)owl_srv_getpid());

    for (;;) {
        char* request = check_server_read_request();
        if (request == NULL) break;   /* EOF : on meurt avec le client */

        char target[1024], lang[16], project[1024];
        check_server_get_str(request, "target", target, sizeof(target));
        check_server_get_str(request, "lang", lang, sizeof(lang));
        check_server_get_str(request, "project", project, sizeof(project));
        bool hasOverlays = (strstr(request, "\"overlays\"") != NULL);

        owl_error_render_set_lang_en(strcmp(lang, "en") == 0);
        compiler_reset_poo_state();
        /*  : le serveur resident sert plusieurs projets d'un workspace
         * dans le MEME process. Reset l'etat format projet compile-time
         * (FormatDate/Heure/Duree + FormatNombre/Monetaire) a chaque requete,
         * sinon un fichier du projet B se verrait diagnostique avec le format du
         * projet A (fuite d'isolation multi-projet). Miroir de project_run.c et
         * du test runner. */
        compiler_projet_boot_langue_reset();
        g_check_request_json = hasOverlays ? request : NULL;

        fflush(stdout);
        fflush(stderr);
        int savedOut = owl_srv_dup(owl_srv_fileno(stdout));
        int savedErr = owl_srv_dup(owl_srv_fileno(stderr));
        int capFd = owl_srv_open(capturePath,
                                 O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
        if (savedOut < 0 || savedErr < 0 || capFd < 0) {
            if (savedOut >= 0) owl_srv_close(savedOut);
            if (savedErr >= 0) owl_srv_close(savedErr);
            if (capFd >= 0) owl_srv_close(capFd);
            g_check_request_json = NULL;
            free(request);
            break;
        }
        owl_srv_dup2(capFd, owl_srv_fileno(stdout));
        owl_srv_dup2(capFd, owl_srv_fileno(stderr));
        owl_srv_close(capFd);

        /* Dispatch STRICTEMENT calque sur celui de main() pour `--check` :
         * meme fonction, memes arguments, donc meme verdict. */
        if (target[0] == '\0') {
            printf("{\"errors\":[{\"line\":1,\"column\":1,"
                   "\"message\":\"Cannot read file\"}]}\n");
        } else if (project[0] != '\0' && !is_directory(target)) {
            /* Mode fichier actif avec contexte projet (check par-frappe rapide,
             * ) : hydrate le projet <project> mais ne rapporte QUE les
             * diagnostics de <target>. Cle `project` absente/vide => on retombe
             * sur le dispatch legacy ci-dessous (comportement inchange). */
            handle_check_project_command(project, target, hasOverlays);
        } else if (is_directory(target)) {
            handle_check_project_command(target, NULL, hasOverlays);
        } else if (hasOverlays) {
            handle_check_command_overlay(target);
        } else {
            handle_check_command(target);
        }

        fflush(stdout);
        fflush(stderr);
        owl_srv_dup2(savedOut, owl_srv_fileno(stdout));
        owl_srv_dup2(savedErr, owl_srv_fileno(stderr));
        owl_srv_close(savedOut);
        owl_srv_close(savedErr);
        g_check_request_json = NULL;
        free(request);

        long capLen = 0;
        char* verdict = NULL;
        FILE* capFile = fopen(capturePath, "rb");
        if (capFile != NULL) {
            fseek(capFile, 0, SEEK_END);
            capLen = ftell(capFile);
            fseek(capFile, 0, SEEK_SET);
            if (capLen < 0) capLen = 0;
            verdict = malloc((size_t)capLen + 1);
            if (verdict != NULL) {
                size_t got = fread(verdict, 1, (size_t)capLen, capFile);
                capLen = (long)got;
                verdict[capLen] = '\0';
            } else {
                capLen = 0;
            }
            fclose(capFile);
        }
        printf("Content-Length: %ld\r\n\r\n", capLen);
        if (verdict != NULL && capLen > 0) fwrite(verdict, 1, (size_t)capLen, stdout);
        fflush(stdout);
        free(verdict);
    }

    owl_srv_unlink(capturePath);
    return 0;
}

int handle_check_command_no_vm_lifecycle_ex(const char* filepath,
                                            const char* overrideSource) {
    /*  — les registres de DECLARATION Fenetre/Etat (vm.windows /
     * vm.etats, alimentes par vm_window_register / vm_etat_register depuis
     * window_declaration / etat_declaration) vivent dans la VM, pas dans
     * l'unite de compilation. Ce handler ne fait volontairement PAS de
     * vm_init/vm_free : sans nettoyage, un 2e check de la meme source dans le
     * meme process retrouve le nom deja enregistre et emet un vrai
     * E_SEM_WINDOW_DUPLICATE (+ cascade "bloc ferme trop tot" quand le corps
     * n'est plus parse) -> resultat de compilation different du 1er check.
     * Un check part donc d'un registre de declaration vide, comme le fait
     * deja la passe de capture du check dossier. La resolution
     * cross-fichier reste intacte : elle est re-accumulee par ce check. */
    vm_windows_clear();
    vm_etats_clear();

    char* source = overrideSource != NULL ? safe_strdup(overrideSource)
                                          : read_file(filepath);
    if (!source) {
        printf("{\"errors\":[{\"line\":1,\"column\":1,\"message\":\"Cannot read file\"}]}\n");
        return 65;
    }

    /* Prescan le dossier parent pour cross-file symbols (PROCEDURE,
     * CLASSE, STRUCTURE, FENETRE, ETAT...). Sans ca, le validation
     * compile-time `is_known_global_name` rejette les references a
     * un Etat declare dans un autre fichier du meme projet. */
    Project project;
    project_init(&project);
    SymbolTable* symbols = symtable_create();
    bool hasProject = false;
    char scopeRoot[1024];
    scopeRoot[0] = '\0';
    {
        /*  : on resout la frontiere du fichier (script / point
         * d'entree / projet) plutot que de scanner aveuglement le dossier
         * parent en recursif. Seul un `projet.owl` ancetre etend le perimetre
         * aux sous-dossiers ; script et point d'entree restent confines au
         * dossier du fichier. */
        char* root = scopeRoot;
        ProjectMode mode = resolve_project_root(filepath, root, sizeof(scopeRoot));
        bool recursive = (mode == PROJECT_MODE_PROJECT);

        if (project_discover_scoped(&project, root, false, recursive)) {
            /*  — parite stricte avec run/serve : une frontiere projet
             * (projet.owl ancetre, mode == PROJECT_MODE_PROJECT) active le mode
             * projet pour que project_companion_in_compile_scope applique le
             * filtre de participation (compagnons PLATS exclus). Sans ca, --check
             * compilait les voisins non-participants et remontait leurs erreurs. */
            project.isProjectMode = recursive;
            /*  — meme filtre de perimetre que run/--check dossier : en
             * mode projet, seuls les fichiers a bloc DSL + la cible sont
             * decouverts. Entree = la cible (filepath), retrouvee par basename
             * (un seul path passe a --check, pas de collision). */
            int monoEntryIndex = -1;
            {
                const char* fpBase = filepath;
                const char* s1 = strrchr(filepath, '/');
                const char* s2 = strrchr(filepath, '\\');
                if (s1 && s1 >= fpBase) fpBase = s1 + 1;
                if (s2 && s2 >= fpBase) fpBase = s2 + 1;
                for (int i = 0; i < project.fileCount; i++) {
                    const char* pBase = project.files[i].path;
                    const char* p1 = strrchr(pBase, '/');
                    const char* p2 = strrchr(pBase, '\\');
                    if (p1 && p1 >= pBase) pBase = p1 + 1;
                    if (p2 && p2 >= pBase) pBase = p2 + 1;
                    if (strcmp(pBase, fpBase) == 0) { monoEntryIndex = i; break; }
                }
            }
            prescan_set_project_scope(recursive, monoEntryIndex);
            compiler_usage_scan_sources_clear();
            for (int i = 0; i < project.fileCount; i++) {
                /*  — la CIBLE du controle est toujours indexee, meme
                 * quand c'est un test : a l'execution, le fichier lance EST
                 * l'entree, donc ses procedures/globales top-level sont visibles
                 * des compagnons qu'il ouvre (pattern callback injecte : la FEN
                 * voisine appelle depuis FinInitialisation une procedure declaree
                 * dans le test). Sans ca, la passe perimetre recompile
                 * le compagnon sans voir le callback -> faux positif
                 * "fonction/procedure inconnue". L'exclusion  ne vise que
                 * les AUTRES tests du perimetre (un test voisin ne doit rien
                 * rendre resoluble a la cible), pas la cible elle-meme. */
                if (!project.files[i].isTest || i == monoEntryIndex) {
                    prescan_file(symbols, project.files[i].source, i);
                } else {
                    /*  — un test du perimetre n'est PAS indexe (il ne
                     * doit rien rendre resoluble a un fichier de production),
                     * mais ce qu'il nomme compte comme usage : sans ca, une
                     * variable dont le seul utilisateur est un test etait
                     * annoncee "jamais utilisee" par le controle alors que
                     * `owl test` sur le meme dossier ne disait rien. */
                    compiler_usage_scan_source_add(project.files[i].source, i);
                }
            }
            prescan_set_project_scope(false, -1);
            /*  : hydrate les tables compile-time POO (arite cstr,
             * heritage) depuis le prescan pour que les references cross-file
             * a une classe avec cstr a args requis remontent l'erreur
             * E_SEM_INSTANCE_NEEDS_ARGS au lieu de l'auto-instancier a tort. */
            compiler_hydrate_constructor_arity_from_symbols(symbols);
            hasProject = true;
        }
    }

    CompileDiagnostic errors[128];
    int errorCount = 0;
    compile_set_error_capture(errors, 128, &errorCount);

    CompileDiagnostic warnings[128];
    int warningCount = 0;
    compile_set_warning_capture(warnings, 128, &warningCount);

    /* Retrouve l'index du fichier courant dans le projet prescanne.
     * Sans ca, currentFileIndex=-1 a la compile : tous les symboles
     * prives du PROJET (y compris ceux du fichier qu'on est en train
     * de checker) seraient consideres "from another file" et bloqueraient
     * l'auto-reference. Bug recurrent signale T5 du QA.
     * Match par basename, suffisant pour `--check <file>` car on
     * passe un seul path et project_discover_ex scanne le dossier
     * parent — pas de collision de basename a craindre. */
    int currentFileIndex = -1;
    if (hasProject) {
        const char* fpBase = filepath;
        const char* fpSepFwd = strrchr(filepath, '/');
        const char* fpSepBwd = strrchr(filepath, '\\');
        if (fpSepFwd && fpSepFwd >= fpBase) fpBase = fpSepFwd + 1;
        if (fpSepBwd && fpSepBwd >= fpBase) fpBase = fpSepBwd + 1;
        for (int i = 0; i < project.fileCount; i++) {
            const char* pBase = project.files[i].path;
            const char* pSepFwd = strrchr(pBase, '/');
            const char* pSepBwd = strrchr(pBase, '\\');
            if (pSepFwd && pSepFwd >= pBase) pBase = pSepFwd + 1;
            if (pSepBwd && pSepBwd >= pBase) pBase = pSepBwd + 1;
            if (strcmp(pBase, fpBase) == 0) {
                currentFileIndex = i;
                break;
            }
        }
    }

    ObjFunction* fn;
    if (hasProject) {
        fn = compile_in_project(source, filepath, symbols, currentFileIndex);
    } else {
        fn = compile(source);
    }
    (void)fn;

    compile_set_error_capture(NULL, 0, NULL);
    compile_set_warning_capture(NULL, 0, NULL);

    /*  — PERIMETRE DU CONTROLE = PERIMETRE DE L'EXECUTION.
     *
     * Le prescan ci-dessus resout les SYMBOLES des voisins mais ne les COMPILE
     * pas : un voisin qui ne compile pas restait invisible et le controle
     * rendait CLEAN sur un dossier incapable de tourner. Faux vert sur la
     * commande que l'utilisateur lance naturellement, et que l'editeur relance
     * en continu (`--check-server`).
     *
     * Le perimetre vient du meme predicat que l'execution
     * (project_companion_in_compile_scope) et l'ordre du meme tri topologique :
     * egalite par construction, pas par recopie. Arret au PREMIER compagnon
     * fautif, comme project_run : ca borne le cas casse sans cascade de bruit.
     *
     * Attribution : diagnostic ancre sur la ligne 1 du fichier CONTROLE, message
     * prefixe du compagnon et de sa ligne. On ne deplace pas le diagnostic vers
     * l'URI du compagnon — l'editeur poserait un marqueur dans un document qu'il
     * n'a pas ouvert, sans savoir quand l'effacer. Le fichier controle est
     * legitimement rouge : il ne peut pas tourner.
     *
     * Cout assume, aucun seuil de garde : cf.  (justesse > latence — un
     * controle qui ment est exactement ce que cet ADR refuse). Le mode projet
     * payait deja ce prix ; l'anomalie etait le mono. Mesure sur un dossier de
     * 280 voisins tous valides (pire cas, aucun arret anticipe) : 1,4 s contre
     * 0,6 s pour le meme fichier seul — dont ~0,6 s de simple chargement du
     * binaire. Sur les dossiers reels les plus denses du depot : 0,1 a 0,45 s.
     *
     * La passe tourne APRES la capture de la cible : les diagnostics propres au
     * fichier controle gardent leur ordre, et le compile de la cible a deja
     * peuple les registres POO (donc moins de faux positifs sur un compagnon
     * derive que dans l'ordre d'execution).
     *
     * currentFileIndex < 0 : la cible n'a pas ete retrouvee dans la decouverte
     * (fichier hors arborescence scannee). Les autres fichiers ne sont alors pas
     * son perimetre, et la cible serait comptee comme son propre compagnon —
     * erreur rapportee deux fois. */
    if (hasProject && currentFileIndex >= 0 && errorCount < 128) {
        int order[MAX_PROJECT_FILES];
        int orderCount = 0;
        int projetIdx = -1;
        for (int i = 0; i < project.fileCount && orderCount < MAX_PROJECT_FILES; i++) {
            if (!project_companion_in_compile_scope(&project, i, currentFileIndex))
                continue;
            if (is_projet_marker_name(project.files[i].name)) { projetIdx = i; continue; }
            order[orderCount++] = i;
        }
        /* Parent avant enfant : un compagnon qui herite d'une classe
         * declaree dans un autre compagnon doit compiler apres elle, sinon
         * `Herite de` ne resout pas -> faux positif. */
        topo_sort_companions_by_inheritance(symbols, order, orderCount);
        /* projet.owl d'abord : ses metadonnees sont lues en cascade par les
         * autres compagnons, comme en phase 1.a de l'execution. */
        if (projetIdx >= 0 && orderCount < MAX_PROJECT_FILES) {
            for (int k = orderCount; k > 0; k--) order[k] = order[k - 1];
            order[0] = projetIdx;
            orderCount++;
        }

        CompileDiagnostic compErrors[8];
        CompileDiagnostic compWarnings[8];
        for (int oc = 0; oc < orderCount; oc++) {
            int i = order[oc];
            int compErrorCount = 0, compWarningCount = 0;
            /* Capture aussi les warnings : sans buffer ils partiraient sur
             * stderr, polluant la sortie d'un controle par ailleurs propre. */
            compile_set_error_capture(compErrors, 8, &compErrorCount);
            compile_set_warning_capture(compWarnings, 8, &compWarningCount);
            compile_in_project(project.files[i].source, project.files[i].path,
                               symbols, i);
            compile_set_error_capture(NULL, 0, NULL);
            compile_set_warning_capture(NULL, 0, NULL);
            if (compErrorCount == 0) continue;

            /* Un diagnostic de niveau DOSSIER (deux marqueurs de projet, par
             * exemple) est leve a l'identique par chaque fichier du perimetre :
             * le repeter n'apprend rien. On passe au compagnon suivant plutot
             * que de s'arreter — si un autre porte une vraie erreur distincte,
             * elle doit sortir. Aucun risque de faux vert : le message est deja
             * rapporte, donc le fichier est deja rouge.
             *
             * Egalite STRICTE du message, pas une inclusion : un message court
             * ("Expression attendue.") est sous-chaine de messages plus longs,
             * et un test d'inclusion avalerait alors une vraie erreur de
             * compagnon — le faux vert par la porte de derriere. */
            bool alreadyReported = false;
            for (int j = 0; j < errorCount; j++) {
                if (strcmp(errors[j].message, compErrors[0].message) == 0) {
                    alreadyReported = true;
                    break;
                }
            }
            if (alreadyReported) continue;

            const char* rel = path_relative_to_root(project.files[i].path, scopeRoot);
            CompileDiagnostic* d = &errors[errorCount++];
            d->line = 1;
            d->column = 1;
            snprintf(d->message, sizeof(d->message), "%s:%d : %s",
                     rel, compErrors[0].line, compErrors[0].message);
            snprintf(d->file, sizeof(d->file), "%s", rel);
            break;
        }
    }

    /*  — la pass cross-file shadow "champ de classe vs variable
     * globale top-level d'un autre fichier" introduite par  a ete
     * supprimee. Les classes ont un namespace strict d'acces (`obj.champ`
     * post-instanciation, `:champ` dans methode) qui ecarte toute ambiguite
     * de lecture, donc le pilier 2 (determinisme) n'est pas viole. Aligne
     * sur canon qui ne leve ni erreur ni warning sur ce cas. Les autres
     * types user (Structure / Enum) etaient deja exclus de cette pass et
     * `ProjectMember` ne capture pas les champs Fenetre/Etat -> apres revert,
     * la pass n'a plus rien a traiter, suppression nette. Le check shadow
     * scope-different reste actif pour : var locale, parametre procedure,
     * variable de boucle POUR, champ Fenetre/Etat (geres dans compiler.c).
     * Cf.  section "Exception : champs de classe". */

    /* Pointeurs non-owning sur les sources du Project : a vider AVANT sa
     * liberation. */
    compiler_usage_scan_sources_clear();
    symtable_destroy(symbols);
    project_free(&project);

    /* Output JSON */
    printf("{\"errors\":[");
    for (int i = 0; i < errorCount; i++) {
        if (i > 0) printf(",");
        /*  : champ file present uniquement quand non-vide (erreur
         * issue d'un fichier different du fichier-cible). Preserve le format
         * historique pour 100% des fixtures existantes. */
        if (errors[i].file[0] != '\0') {
            printf("{\"file\":\"");
            print_json_escaped(errors[i].file);
            printf("\",\"line\":%d,\"column\":%d,\"message\":\"",
                   errors[i].line, errors[i].column);
        } else {
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   errors[i].line, errors[i].column);
        }
        print_json_escaped(errors[i].message);
        printf("\"}");
    }
    printf("],\"warnings\":[");
    for (int i = 0; i < warningCount; i++) {
        if (i > 0) printf(",");
        if (warnings[i].file[0] != '\0') {
            printf("{\"file\":\"");
            print_json_escaped(warnings[i].file);
            printf("\",\"line\":%d,\"column\":%d,\"message\":\"",
                   warnings[i].line, warnings[i].column);
        } else {
            printf("{\"line\":%d,\"column\":%d,\"message\":\"",
                   warnings[i].line, warnings[i].column);
        }
        print_json_escaped(warnings[i].message);
        printf("\"}");
    }
    printf("]}\n");

    free(source);
    return errorCount > 0 ? 65 : 0;
}

/* Variante "isolee" : compile un seul fichier sans scan du dossier parent.
 * Reserve aux usages snippet-inline (MCP owl_check) ou la source est un
 * fragment auto-suffisant fourni par le LLM, qui ne reference aucun
 * symbole cross-fichier. Evite le cout (~25s) du project_discover_ex sur
 * un repo qui contient des centaines de .owl. */
int handle_check_command_isolated(const char* filepath) {
    /*  — meme fuite que le handler scope-projet : le serveur MCP
     * partage un seul VM sur toute la session, donc deux owl_check du meme
     * snippet a bloc Fenetre/Etat rendaient un faux doublon au 2e appel. */
    vm_windows_clear();
    vm_etats_clear();

    char* source = read_file(filepath);
    if (!source) {
        printf("{\"errors\":[{\"line\":1,\"column\":1,\"message\":\"Cannot read file\"}]}\n");
        return 65;
    }

    CompileDiagnostic errors[128];
    int errorCount = 0;
    compile_set_error_capture(errors, 128, &errorCount);

    CompileDiagnostic warnings[128];
    int warningCount = 0;
    compile_set_warning_capture(warnings, 128, &warningCount);

    ObjFunction* fn = compile(source);
    (void)fn;

    compile_set_error_capture(NULL, 0, NULL);
    compile_set_warning_capture(NULL, 0, NULL);

    printf("{\"errors\":[");
    for (int i = 0; i < errorCount; i++) {
        if (i > 0) printf(",");
        printf("{\"line\":%d,\"column\":%d,\"message\":\"",
               errors[i].line, errors[i].column);
        print_json_escaped(errors[i].message);
        printf("\"}");
    }
    printf("],\"warnings\":[");
    for (int i = 0; i < warningCount; i++) {
        if (i > 0) printf(",");
        printf("{\"line\":%d,\"column\":%d,\"message\":\"",
               warnings[i].line, warnings[i].column);
        print_json_escaped(warnings[i].message);
        printf("\"}");
    }
    printf("]}\n");

    free(source);
    return errorCount > 0 ? 65 : 0;
}

/* ================================================================
 * FMT COMMAND
 * ================================================================ */

/* Minimal JSON string escaper: quotes " and \, escapes control chars as
 * \uXXXX. Doesn't cover every corner but is sufficient for registry data
 * (ASCII descriptions + UTF-8 accented text). */
static void json_escape(FILE* f, const char* s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"')       fputs("\\\"", f);
        else if (*p == '\\') fputs("\\\\", f);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') fputs("\\r", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20)  fprintf(f, "\\u%04x", *p);
        else                 fputc(*p, f);
    }
    fputc('"', f);
}

/* `owl new <type> <Nom>` imprime un squelette générique sur stdout.
 * Différent de `owl template <nom>` qui imprime des exemples métier
 * complets ; ici on génère la déclaration minimale + props courantes
 * + event(s) lifecycle de base, pour redirection libre par l'utilisateur :
 *
 *   owl new fenetre MaFen      > MaFen.owl
 *   owl new champ table TBL_X  >> page.owl
 *
 * Si tu cherches un exemple métier riche : `owl template <nom>`.
 *
 * IMPORTANT : tous les squelettes générés doivent être conformes au
 * DSL v2 (PROPRIETES ... FIN obligatoire, canons canon des constantes).
 * La rule self-test 37 (registry_self_test) compile chaque variante
 * et casse le build si un squelette devient invalide. */

/* Canons FR/EN des symboles utilisés par les squelettes. Tous proviennent
 * du registry (invariant bilingue 3) : aucune forme EN inventée ici. Le
 * sélecteur `L(fr,en)` choisit selon la langue demandée. */
typedef struct {
    int en;  /* 0 = FR (défaut), 1 = EN */
} OwlNewLang;

/* Constantes de disposition (enum `disposition`) canon FR -> EN. */
static const char* disp_const_canon(int disp, int en) {
    /* disp : 0=verticale 1=horizontale 2=grille 3=libre */
    switch (disp) {
        case 1: return en ? "dispHorizontal" : "dispHorizontale";
        case 2: return en ? "dispGrid"       : "dispGrille";
        case 3: return en ? "dispFree"       : "dispLibre";
        default: return en ? "dispVertical"  : "dispVerticale";
    }
}

/* Renvoie la forme canon (FR ou EN) d'une constante du registry sans
 * inventer le nom EN (règle 1) : on lit le nameEN porté par l'OwlConstDef.
 * Si en=0, ou si le nameEN est absent, renvoie le nom FR tel quel. */
static const char* new_const_canon(const char* fr_name, int en) {
    if (!en) return fr_name;
    const OwlConstDef* c = owl_registry_const_find(fr_name);
    if (c && c->nameEN) return c->nameEN;
    return fr_name;  /* fallback : pas de forme EN distincte (ex: formatA4) */
}

/* Constantes de format de page (enum `etatFormat`) FR canon -> forme demandée. */
static const char* fmt_const_canon(int fmt, int en) {
    /* fmt : 0=A4(defaut) 1=A3 2=A5 3=Letter 4=Legal */
    const char* fr;
    switch (fmt) {
        case 1:  fr = "formatA3";     break;
        case 2:  fr = "formatA5";     break;
        case 3:  fr = "formatLetter"; break;
        case 4:  fr = "formatLegal";  break;
        default: fr = "formatA4";     break;
    }
    return new_const_canon(fr, en);
}

/* Constantes d'orientation (enum `etatOrientation`) FR canon -> forme demandée.
 * orientPaysage porte nameEN=orientLandscape ; orientPortrait identique. */
static const char* orient_const_canon(int orient, int en) {
    /* orient : 0=portrait(defaut) 1=paysage */
    const char* fr = (orient == 1) ? "orientPaysage" : "orientPortrait";
    return new_const_canon(fr, en);
}

static int owl_new_generate_champ(const char* sub, const char* name,
                                  int en, FILE* out);
static int owl_new_generate_menu(const char* kind, const char* name,
                                 int en, FILE* out);

/* Émet `src` sur `out` après passage par le formateur.
 *
 * Tout contenu généré par OWL doit être un point fixe de son propre
 * formateur : sinon le premier save de l'utilisateur réindente le fichier
 * qu'on vient de lui produire. Formater la sortie plutôt que
 * coder tabulations et alignements en dur dans les format strings rend le
 * résultat canonique par construction ET bilingue gratuitement — la largeur
 * de la colonne du '=' dépend du nom dans la langue émise (`Disposition` vs
 * `Layout`), un padding en dur serait faux dans une des deux langues.
 *
 * Requiert le registre peuplé : le provider de canon posé par vm_init est ce
 * qui donne accents et casse canon des natives. Repli sur `src` tel quel si
 * le formateur refuse la source — un générateur ne rend jamais du vide, et
 * le contrôle de canonicité du self-test (règles 37/38) attrape le cas. */
static void owl_emit_canonical(const char* src, FILE* out) {
    char* canon = owl_format(src);
    fputs(canon ? canon : src, out);
    free(canon);
}

/* Générateur brut : écrit les squelettes tels que codés, sans canonisation.
 * Passer par owl_new_generate_ex, qui formate la sortie. */
static int owl_new_generate_raw_ex(const char* kind, const char* sub,
                                   const char* name, int disp, int fmt,
                                   int orient, int en, FILE* out) {
    /* Raccourcis de sélection FR/EN. */
    #define L(fr, eng) (en ? (eng) : (fr))

    if (strcmp(kind, "fenetre") == 0 || strcmp(kind, "window") == 0) {
        fprintf(out,
            "%s %s %s\n"
            "    %s\n"
            "        %s = \"%s\"\n"
            "        %s = 400\n"
            "        %s = 300\n"
            "        %s = %s\n"
            "    %s\n"
            "\n"
            "    BTN_OK %s %s %s\n"
            "        %s\n"
            "            %s = \"OK\"\n"
            "            %s = %s\n"
            "            %s = 100\n"
            "        %s\n"
            "\n"
            "        PROCEDURE %s()\n"
            "            %s()\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "    %s\n"
            "%s\n",
            name, L("est une", "is a"), L("Fen\xc3\xaatre", "Window"),
            L("PROPRIETES", "PROPERTIES"),
            L("Titre", "Title"), name,
            L("Largeur", "Width"),
            L("Hauteur", "Height"),
            L("Disposition", "Layout"), disp_const_canon(disp, en),
            L("FIN", "END"),
            L("est un", "is a"), L("Champ", "Control"), L("Bouton", "Button"),
            L("PROPRIETES", "PROPERTIES"),
            L("Libell\xc3\xa9", "Caption"),
            L("TypeBouton", "ButtonType"), L("btnValider", "btnValidate"),
            L("Largeur", "Width"),
            L("FIN", "END"),
            L("surClic", "onClick"),
            L("Ferme", "Close"),
            L("FIN", "END"),
            L("FIN", "END"),
            L("FinInitialisation", "EndInitialization"),
            L("FIN", "END"),
            L("FIN", "END"));
        return 0;
    }

    if (strcmp(kind, "etat") == 0 || strcmp(kind, "report") == 0) {
        /* État : 5 sous-blocs canon nommés (Corps obligatoire au runtime,
         * les 4 autres montrent la structure complète) + PROPRIETES racine
         * (Format/Orientation/4 marges) + hook Lecture (boucle d'impression). */
        fprintf(out,
            "%s %s %s\n"
            "    %s\n"
            "        %s      = %s\n"
            "        %s = %s\n"
            "        %s   = 10\n"
            "        %s    = 10\n"
            "        %s = 15\n"
            "        %s = 15\n"
            "    %s\n"
            "\n"
            "    intro %s %s\n"
            "        %s\n"
            "            %s = 25\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    hautpage %s %s\n"
            "        %s\n"
            "            %s = 12\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    corps %s %s\n"
            "        %s\n"
            "            %s = 30\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    piedpage %s %s\n"
            "        %s\n"
            "            %s = 12\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    findoc %s %s\n"
            "        %s\n"
            "            %s = 20\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "    %s\n"
            "%s\n",
            name, L("est un", "is a"), L("Etat", "Report"),
            L("PROPRIETES", "PROPERTIES"),
            L("Format", "Format"), fmt_const_canon(fmt, en),
            L("Orientation", "Orientation"), orient_const_canon(orient, en),
            L("MargeHaut", "TopMargin"),
            L("MargeBas", "BottomMargin"),
            L("MargeGauche", "LeftMargin"),
            L("MargeDroite", "RightMargin"),
            L("FIN", "END"),
            L("est un", "is a"), L("EnTeteDocument", "DocumentHeader"),
            L("PROPRIETES", "PROPERTIES"), L("Hauteur", "Height"), L("FIN", "END"),
            L("FIN", "END"),
            L("est un", "is a"), L("HautDePage", "PageHeader"),
            L("PROPRIETES", "PROPERTIES"), L("Hauteur", "Height"), L("FIN", "END"),
            L("FIN", "END"),
            L("est un", "is a"), L("Corps", "Body"),
            L("PROPRIETES", "PROPERTIES"), L("Hauteur", "Height"), L("FIN", "END"),
            L("FIN", "END"),
            L("est un", "is a"), L("BasDePage", "PageFooter"),
            L("PROPRIETES", "PROPERTIES"), L("Hauteur", "Height"), L("FIN", "END"),
            L("FIN", "END"),
            L("est un", "is a"), L("FinDeDocument", "DocumentFooter"),
            L("PROPRIETES", "PROPERTIES"), L("Hauteur", "Height"), L("FIN", "END"),
            L("FIN", "END"),
            L("Lecture", "Read"),
            L("FIN", "END"),
            L("FIN", "END"));
        return 0;
    }

    if (strcmp(kind, "classe") == 0 || strcmp(kind, "class") == 0) {
        fprintf(out,
            "%s %s %s\n"
            "PUBLIC\n"
            "    sNom %s %s\n"
            "\n"
            "    PROCEDURE %s(sNom %s %s)\n"
            "        :sNom = sNom\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "        %s\n"
            "    %s\n"
            "%s\n",
            name, L("est une", "is a"), L("Classe", "Class"),
            L("est une", "is a"), L("cha\xc3\xaene", "string"),
            L("Constructeur", "constructor"),
            L("est une", "is a"), L("cha\xc3\xaene", "string"),
            L("FIN", "END"),
            L("Destructeur", "Destructor"),
            L("// lib\xc3\xa9rer les ressources de l'instance",
              "// release the instance resources"),
            L("FIN", "END"),
            L("FIN", "END"));
        return 0;
    }

    if (strcmp(kind, "collection") == 0) {
        /* Collection de procédures (, ) : bloc DSL explicite
         * `NOM est une CollectionDeProcédures ... FIN`. Le corps est la zone de
         * déclaration (globales projet en nom nu, procédures publiques, énums,
         * structures) exécutée au lancement du projet ; `Terminaison()` est le
         * hook de fin de vie, reconnu UNIQUEMENT dans ce bloc. Indentation tabs
         * (canon formatter). */
        (void)disp; (void)fmt; (void)orient; (void)sub;
        fprintf(out,
            "%s %s %s\n"
            "\t%s %s %s = %s\n"
            "\t\n"
            "\tPROCEDURE %s(%s %s %s)\n"
            "\t\t%s(%s)\n"
            "\t%s\n"
            "\t\n"
            "\tPROCEDURE %s()\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n",
            name, L("est une", "is a"),
            L("CollectionDeProc\xc3\xa9""dures", "ProcedureCollection"),
            L("gbEstActif", "gbIsActive"), L("est un", "is a"),
            L("bool\xc3\xa9""en", "boolean"), L("Vrai", "True"),
            L("LogEcrit", "LogWrite"),
            L("sLog", "sLog"), L("est une", "is a"), L("cha\xc3\xaene", "string"),
            L("Trace", "Trace"),
            L("sLog", "sLog"),
            L("FIN", "END"),
            /* Zone de terminaison : appelee a la fermeture du projet. */
            L("Terminaison", "Termination"),
            L("// lib\xc3\xa9rer les ressources \xc3\xa0 la fermeture du projet",
              "// release the resources at project close"),
            L("FIN", "END"),
            L("FIN", "END"));
        return 0;
    }

    if (strcmp(kind, "theme") == 0) {
        /* Thème : bloc de styles nommés en littéral d'instance `= { ... }`.
         * Forme ancrée sur la fixture fmt-stability theme_bloc_styles_nommes :
         * sortie déjà canonique (tabs + alignement sur largeur d'affichage),
         * donc `owl fmt --check` du fichier généré donne zéro diff. Les
         * sous-propriétés (CadreExtérieur/Libellé/ZoneSaisie, CouleurFond/
         * Couleur) proviennent du registry (styleBouton/styleSaisie). */
        (void)disp; (void)fmt; (void)orient; (void)sub;
        if (en) {
            fprintf(out,
                "%s is a Theme\n"
                "\tprimaryButton is a styleButton = {\n"
                "\t\tOuterFrame = { BackgroundColor = Blue }\n"
                "\t\tCaption    = { Color = White }\n"
                "\t}\n"
                "\n"
                "\tcompactInput is a styleInput = {\n"
                "\t\tInputZone = { BackgroundColor = LightYellow }\n"
                "\t}\n"
                "END\n",
                name);
        } else {
            fprintf(out,
                "%s est un Th\xc3\xa8me\n"
                "\tboutonPrimaire est un styleBouton = {\n"
                "\t\tCadreExt\xc3\xa9rieur = { CouleurFond = Bleu }\n"
                "\t\tLibell\xc3\xa9        = { Couleur = Blanc }\n"
                "\t}\n"
                "\n"
                "\tsaisieCompacte est un styleSaisie = {\n"
                "\t\tZoneSaisie = { CouleurFond = JauneClair }\n"
                "\t}\n"
                "FIN\n",
                name);
        }
        return 0;
    }

    if (strcmp(kind, "service") == 0) {
        /* Service REST (DSL `ServiceREST` / `OpérationHTTP`) : squelette
         * représentatif minimal. Un préfixe de chemin (UrlBase) + une
         * opération de lecture avec paramètre de chemin (`id`) lié dans le
         * handler, réponse via `MoiMême..Réponse..CodeÉtat`. Service public : PAS de
         * bloc Authentification (l'auth runtime n'existe pas encore et
         * déclencherait un avertissement — un squelette neuf naît sans
         * warning). Indentation tabs (canon formatter). Bilingue via L(). */
        (void)disp; (void)fmt; (void)orient; (void)sub;
        fprintf(out,
            "%s %s %s\n"
            "\t%s\n"
            "\t\t%s = [\"api\", \"v1\"]\n"
            "\t%s\n"
            "\n"
            "\t%s %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = %s\n"
            "\t\t\t%s = [\"%s\", id]\n"
            "\t\t\t%s = [%s]\n"
            "\t\t\t%s = %s\n"
            "\t\t%s\n"
            "\t\tPROCEDURE %s(id %s %s) : %s\n"
            "\t\t\t%s\n"
            "\t\t\t%s..%s..%s = %s\n"
            "\t\t\t%s \"...\"\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n",
            name, L("est un", "is a"), "ServiceREST",
            L("PROPRIETES", "PROPERTIES"),
            L("UrlBase", "BaseUrl"),
            L("FIN", "END"),
            L("LireRessource", "ReadResource"), L("est une", "is an"),
            L("Op\xc3\xa9rationHTTP", "HTTPOperation"),
            L("PROPRIETES", "PROPERTIES"),
            L("M\xc3\xa9thode", "Method"), L("httpGet", "httpGet"),
            L("Chemin", "Path"), L("ressources", "resources"),
            L("R\xc3\xa9ponses", "Responses"), L("httpOK", "httpOK"),
            L("Code\xc3\x89tat", "StatusCode"), L("httpOK", "httpOK"),
            L("FIN", "END"),
            L("surRequ\xc3\xaate", "onRequest"), L("est un", "is an"),
            L("entier", "integer"), L("cha\xc3\xaene", "string"),
            L("// remplacer par votre logique : charger la ressource `id`",
              "// replace with your logic: load resource `id`"),
            L("MoiM\xc3\xaame", "MySelf"), L("R\xc3\xa9ponse", "Response"),
            L("Code\xc3\x89tat", "StatusCode"),
            L("httpOK", "httpOK"),
            L("RENVOYER", "RETURN"),
            L("FIN", "END"),
            L("FIN", "END"),
            L("FIN", "END"));
        return 0;
    }

    /* Constructs menu (kinds top-level) : tentés avant les champs. Le
     * générateur renvoie -1 si `kind` n'est pas un menu, on enchaîne alors. */
    {
        (void)disp; (void)fmt; (void)orient; (void)sub;
        int mrc = owl_new_generate_menu(kind, name, en, out);
        if (mrc == 0) return 0;
    }

    if (strcmp(kind, "champ") == 0 || strcmp(kind, "control") == 0) {
        (void)disp;
        return owl_new_generate_champ(sub, name, en, out);
    }
    #undef L
    return -1;
}

/* Génération étendue : `disp` (0..3) ne s'applique qu'à fenetre,
 * `en` bascule sur le canon EN du registry. Renvoie 0 si la combinaison
 * (kind, sub) est connue ; -1 sinon. owl_new_generate délègue ici.
 *
 * Point d'émission unique de `owl new` : génère en mémoire (EOL '\n' nu,
 * tmpfile est binaire) puis canonise via le formateur. L'écriture finale
 * reste au caller — stdout ou fichier en mode texte, donc EOL natif
 * plateforme (acquis ). */
int owl_new_generate_ex(const char* kind, const char* sub, const char* name,
                        int disp, int fmt, int orient, int en, FILE* out) {
    FILE* tmp = tmpfile();
    if (tmp == NULL) {
        /* Pas de scratch disponible : mieux vaut un squelette non canonique
         * qu'aucun squelette. */
        return owl_new_generate_raw_ex(kind, sub, name, disp, fmt, orient, en,
                                       out);
    }
    int rc = owl_new_generate_raw_ex(kind, sub, name, disp, fmt, orient, en,
                                     tmp);
    if (rc != 0) { fclose(tmp); return rc; }

    long size = ftell(tmp);
    if (size <= 0) { fclose(tmp); return 0; }
    rewind(tmp);
    char* raw = (char*)malloc((size_t)size + 1);
    if (raw == NULL) { fclose(tmp); return -1; }
    size_t got = fread(raw, 1, (size_t)size, tmp);
    raw[got] = '\0';
    fclose(tmp);

    owl_emit_canonical(raw, out);
    free(raw);
    return 0;
}

/* Cœur de génération exposé pour les self-tests et le tool MCP. Conserve
 * la signature historique (4 args) ; délègue à _ex en FR / dispVerticale. */
int owl_new_generate(const char* kind, const char* sub, const char* name,
                     FILE* out) {
    return owl_new_generate_ex(kind, sub, name, 0, 0, 0, 0, out);
}

/* Sous-générateur des champs (kind=="champ"). Extrait pour garder _ex
 * lisible. `en` bascule sur le canon EN du registry. */
static int owl_new_generate_champ(const char* sub, const char* name,
                                  int en, FILE* out) {
    #define L(fr, eng) (en ? (eng) : (fr))
    const char* est  = L("est un", "is a");
    const char* chp  = L("Champ", "Control");
    const char* prop = L("PROPRIETES", "PROPERTIES");
    const char* fin  = L("FIN", "END");
    const char* larg = L("Largeur", "Width");
    const char* haut = L("Hauteur", "Height");

    if (!sub) return -1;

    if (strcmp(sub, "bouton") == 0 || strcmp(sub, "button") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = \"OK\"\n"
            "        %s = btnNormal\n"
            "        %s = 100\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Bouton", "Button"),
            prop, L("Libell\xc3\xa9", "Caption"),
            L("TypeBouton", "ButtonType"), larg, fin,
            L("surClic", "onClick"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "libelle") == 0 || strcmp(sub, "label") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = \"...\"\n"
            "        %s = 200\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Libell\xc3\xa9", "Label"),
            prop, L("Libell\xc3\xa9", "Caption"), larg, fin, fin);
        return 0;
    }
    if (strcmp(sub, "saisie") == 0 || strcmp(sub, "input") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = %s\n"
            "        %s = \"...\"\n"
            "        %s = 200\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Saisie", "Input"),
            prop, L("TypeSaisie", "InputType"), L("typSaisieTexte", "typeInputText"),
            L("Indication", "Placeholder"), larg, fin,
            L("surValidation", "onValidation"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "table") == 0) {
        fprintf(out,
            "%s %s %s Table\n"
            "    %s\n"
            "        %s = 400\n"
            "        %s = 200\n"
            "    %s\n"
            "\n"
            "    COL_Nom %s %s\n"
            "        %s\n"
            "            %s = \"%s\"\n"
            "            %s = 200\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    COL_Valeur %s %s\n"
            "        %s\n"
            "            %s = \"%s\"\n"
            "            %s = 150\n"
            "        %s\n"
            "    %s\n"
            "\n"
            "    PROCEDURE %s()\n"
            "    %s\n"
            "%s\n",
            name, est, chp,
            prop, larg, haut, fin,
            L("est une", "is a"), L("colonne", "Column"),
            prop, L("Libellé", "Caption"), L("Nom", "Name"), larg, fin, fin,
            L("est une", "is a"), L("colonne", "Column"),
            prop, L("Libellé", "Caption"), L("Valeur", "Value"), larg, fin, fin,
            L("surChangementSelection", "onSelectionChange"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "conteneur") == 0 || strcmp(sub, "container") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 400\n"
            "        %s = 300\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Conteneur", "Container"),
            prop, larg, haut, fin, fin);
        return 0;
    }
    if (strcmp(sub, "image") == 0) {
        fprintf(out,
            "%s %s %s Image\n"
            "    %s\n"
            "        %s = 200\n"
            "        %s = 200\n"
            "        Image = \"chemin/image.png\"\n"
            "        %s = cadHomothetique\n"
            "    %s\n"
            "%s\n",
            name, est, chp,
            prop, larg, haut, L("Cadrage", "Framing"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "liste") == 0 || strcmp(sub, "list") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 200\n"
            "        %s = 200\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Liste", "List"),
            prop, larg, haut, fin, fin);
        return 0;
    }
    if (strcmp(sub, "combo") == 0 || strcmp(sub, "combobox") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 200\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Combo", "ComboBox"),
            prop, larg, fin, fin);
        return 0;
    }
    if (strcmp(sub, "selecteur") == 0 || strcmp(sub, "radiogroup") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 200\n"
            "        %s = 100\n"
            "        %s = selStandard\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("S\xc3\xa9lecteur", "RadioGroup"),
            prop, larg, haut, L("ModeAffichage", "DisplayMode"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "interrupteur") == 0 || strcmp(sub, "checkbox") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 200\n"
            "        %s = 100\n"
            "        %s = selStandard\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("Interrupteur", "CheckBox"),
            prop, larg, haut, L("ModeAffichage", "DisplayMode"), fin, fin);
        return 0;
    }
    if (strcmp(sub, "zonerepetee") == 0 ||
        strcmp(sub, "zoner\xc3\xa9p\xc3\xa9t\xc3\xa9""e") == 0 ||
        strcmp(sub, "looper") == 0) {
        fprintf(out,
            "%s %s %s %s\n"
            "    %s\n"
            "        %s = 400\n"
            "        %s = 300\n"
            "        %s = zrVertical\n"
            "    %s\n"
            "%s\n",
            name, est, chp, L("ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", "Looper"),
            prop, larg, haut, L("Sens", "Direction"), fin, fin);
        return 0;
    }
    return -1;
    #undef L
}

/* Sous-générateur des constructs menu (kinds top-level menuprincipal /
 * menucontextuel / optionmenu / sousmenu / menuseparateur). Casse-insensible
 * sur le kind, alias EN acceptés. `en` bascule sur le canon EN du registry
 * (MainMenu / ContextMenu / MenuOption / Submenu / MenuSeparator, Caption,
 * onSelection). Renvoie 0 si émis, -1 si `kind` n'est pas un construct menu
 * (le dispatch enchaîne alors sur les champs). Les squelettes sont
 * minimalistes façon champ : déclaration + une option enfant d'exemple pour
 * montrer la nidification. Les blocs sont autonomes (colonne 0) ; le contexte
 * porteur (Fenêtre pour un menu racine, menu pour un élément) est fourni par
 * l'appelant / le site d'insertion. */
static int owl_new_generate_menu(const char* kind, const char* name,
                                 int en, FILE* out) {
    #define L(fr, eng) (en ? (eng) : (fr))
    if (!kind || !name) return -1;

    int is_main = 0, is_ctx = 0, is_opt = 0, is_sub = 0, is_sep = 0, is_loupe = 0,
        is_corps = 0;
    if (strcasecmp(kind, "menuprincipal") == 0 ||
        strcasecmp(kind, "mainmenu") == 0) is_main = 1;
    else if (strcasecmp(kind, "menuloupe") == 0 ||
             strcasecmp(kind, "magnifiermenu") == 0 ||
             strcasecmp(kind, "searchmenu") == 0) is_loupe = 1;
    else if (strcasecmp(kind, "menucorps") == 0 ||
             strcasecmp(kind, "bodymenu") == 0) is_corps = 1;
    else if (strcasecmp(kind, "menucontextuel") == 0 ||
             strcasecmp(kind, "contextmenu") == 0 ||
             strcasecmp(kind, "popupmenu") == 0) is_ctx = 1;
    else if (strcasecmp(kind, "optionmenu") == 0 ||
             strcasecmp(kind, "menuoption") == 0 ||
             strcasecmp(kind, "menuitem") == 0) is_opt = 1;
    else if (strcasecmp(kind, "sousmenu") == 0 ||
             strcasecmp(kind, "submenu") == 0) is_sub = 1;
    else if (strcasecmp(kind, "menuseparateur") == 0 ||
             strcasecmp(kind, "menus\xc3\xa9parateur") == 0 ||
             strcasecmp(kind, "menuseparator") == 0) is_sep = 1;
    else return -1;

    const char* fin  = L("FIN", "END");
    const char* prop = L("PROPRIETES", "PROPERTIES");
    const char* lib  = L("Libell\xc3\xa9", "Caption");
    const char* sel  = L("surS\xc3\xa9lection", "onSelection");
    const char* estM = L("est un", "is a");     /* menu masculin */
    const char* estF = L("est une", "is a");    /* une OptionMenu */
    const char* opt  = L("OptionMenu", "MenuOption");

    /* MenuSéparateur : bloc nu, pas de PROPRIETES. */
    if (is_sep) {
        fprintf(out, "%s %s %s\n%s\n",
            name, estM, L("MenuS\xc3\xa9parateur", "MenuSeparator"), fin);
        return 0;
    }

    /* OptionMenu : Libellé + surSélection. */
    if (is_opt) {
        fprintf(out,
            "%s %s %s\n"
            "\t%s\n"
            "\t\t%s = \"...\"\n"
            "\t%s\n"
            "\n"
            "\tPROCEDURE %s()\n"
            "\t%s\n"
            "%s\n",
            name, estF, opt, prop, lib, fin, sel, fin, fin);
        return 0;
    }

    /* SousMenu : Libellé + une OptionMenu enfant (montre la nidification). */
    if (is_sub) {
        fprintf(out,
            "%s %s %s\n"
            "\t%s\n"
            "\t\t%s = \"...\"\n"
            "\t%s\n"
            "\n"
            "\tOPT_Option %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"...\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n",
            name, estM, L("SousMenu", "Submenu"),
            prop, lib, fin,
            estF, opt, prop, lib, fin, sel, fin, fin, fin);
        return 0;
    }

    /* MenuPrincipal : une barre de menu a des entrées de 1er niveau qui sont
     * des SousMenu déroulants (« Fichier », « Édition »…) contenant les
     * OptionMenu. Le squelette par défaut reflète cette arborescence :
     * MenuPrincipal → SousMenu (Libellé) → OptionMenu enfant. */
    if (is_main) {
        fprintf(out,
            "%s %s %s\n"
            "\tSM_SousMenu %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"...\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tOPT_Option %s %s\n"
            "\t\t\t%s\n"
            "\t\t\t\t%s = \"...\"\n"
            "\t\t\t%s\n"
            "\n"
            "\t\t\tPROCEDURE %s()\n"
            "\t\t\t%s\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n",
            name, estM, L("MenuPrincipal", "MainMenu"),
            estM, L("SousMenu", "Submenu"),
            prop, lib, fin,
            estF, opt, prop, lib, fin, sel, fin, fin, fin, fin);
        return 0;
    }

    /* MenuLoupe : menu contextuel prêt à attacher à un Champ Table via la
     * propriété MenuLoupe. Les options sont déjà câblées sur
     * MoiMême = la colonne cliquée dans le surSélection. Rechercher lance la
     * saisie loupe, Filtrer applique un filtre « commence par », Effacer le
     * retire. Un commentaire final montre l'attache sur la Table cible. */
    if (is_loupe) {
        const char* moi  = L("MoiM\xc3\xaame", "MySelf");
        const char* nSai = L("TableSaisieLoupe", "TableInputSearch");
        const char* nAct = L("TableActiveFiltre", "TableEnableFilter");
        const char* nDes = L("TableD\xc3\xa9sactiveFiltre", "TableDisableFilter");
        const char* fCom = L("filtreCommencePar", "filterStartsWith");
        const char* propMenu = L("MenuLoupe", "MagnifierMenu");
        fprintf(out,
            "%s %s %s\n"
            "\tOPT_Rechercher %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"%s\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t\t%s(%s)\n"
            "\t\t%s\n"
            "\t%s\n"
            "\n"
            "\tOPT_Filtrer %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"%s\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t\t%s(%s, %s, \"A\")\n"
            "\t\t%s\n"
            "\t%s\n"
            "\n"
            "\tOPT_Effacer %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"%s\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t\t%s(%s)\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n"
            "\n"
            "%s\n"
            "// TABLE_X %s %s  %s = %s  %s\n",
            name, estM, L("MenuContextuel", "ContextMenu"),
            estF, opt, prop, lib, L("Rechercher", "Search"), fin,
            sel, nSai, moi, fin, fin,
            estF, opt, prop, lib, L("Filtrer : commence par", "Filter: starts with"), fin,
            sel, nAct, moi, fCom, fin, fin,
            estF, opt, prop, lib, L("Effacer le filtre", "Clear filter"), fin,
            sel, nDes, moi, fin, fin, fin,
            L("// Attache : sur le Champ Table cible, ajouter la propriété "
              "MenuLoupe (MoiMême = la colonne cliquée).",
              "// Attach: add the MenuLoupe property on the target Table control "
              "(MySelf = the clicked column)."),
            L("est un Champ Table", "is a Table Control"), prop,
            propMenu, name, fin);
        return 0;
    }

    /* MenuCorps : menu contextuel prêt à attacher au corps d'un Champ Table
     * via la propriété canon MenuContextuel. Les options sont déjà
     * câblées sur MoiMême = la Table dans surSélection : « Tout copier » copie
     * la table dans le presse-papier, « Exporter vers Excel… » demande un
     * chemin via fSélecteur en mode sauvegarde (fselCrée) puis écrit le .xlsx.
     * Un commentaire final montre l'attache sur la Table cible. */
    if (is_corps) {
        const char* moi   = L("MoiM\xc3\xaame", "MySelf");
        const char* nClip = L("TableVersPressePapier", "TableToClipboard");
        const char* nXls  = L("TableVersExcel", "TableToExcel");
        const char* nSel  = L("fS\xc3\xa9lecteur", "fSelect");
        const char* kCree = L("fselCr\xc3\xa9""e", "fselCreate");
        const char* estCh = L("est une cha\xc3\xaene", "is a string");
        const char* chem  = L("chemin", "path");
        const char* kSi   = L("SI", "IF");
        const char* kAl   = L("ALORS", "THEN");
        const char* propMenu = L("MenuContextuel", "ContextMenu");
        fprintf(out,
            "%s %s %s\n"
            "\tOPT_ToutCopier %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"%s\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t\t%s(%s)\n"
            "\t\t%s\n"
            "\t%s\n"
            "\n"
            "\tOPT_ExporterExcel %s %s\n"
            "\t\t%s\n"
            "\t\t\t%s = \"%s\"\n"
            "\t\t%s\n"
            "\n"
            "\t\tPROCEDURE %s()\n"
            "\t\t\t%s %s = %s(\"\", \"export.xlsx\", \"%s\", \"%s\" + TAB + \"*.xlsx\", \"xlsx\", %s)\n"
            "\t\t\t%s %s <> \"\" %s\n"
            "\t\t\t\t%s(%s, %s)\n"
            "\t\t\t%s\n"
            "\t\t%s\n"
            "\t%s\n"
            "%s\n"
            "\n"
            "%s\n"
            "// TABLE_X %s %s  %s = %s  %s\n",
            name, estM, L("MenuContextuel", "ContextMenu"),
            estF, opt, prop, lib, L("Tout copier", "Copy all"), fin,
            sel, nClip, moi, fin, fin,
            estF, opt, prop, lib, L("Exporter vers Excel\xe2\x80\xa6", "Export to Excel\xe2\x80\xa6"), fin,
            sel, chem, estCh, nSel,
            L("Exporter vers Excel", "Export to Excel"),
            L("Classeur Excel (*.xlsx)", "Excel workbook (*.xlsx)"), kCree,
            kSi, chem, kAl, nXls, moi, chem, fin, fin, fin, fin,
            L("// Attache : sur le Champ Table cible, ajouter la propriété "
              "MenuContextuel (MoiMême = la Table).",
              "// Attach: add the MenuContextuel property on the target Table "
              "control (MySelf = the Table)."),
            L("est un Champ Table", "is a Table Control"), prop,
            propMenu, name, fin);
        return 0;
    }

    /* MenuContextuel : liste plate d'actions (pas de sous-menu par défaut) :
     * menu racine + une OptionMenu enfant directe. */
    fprintf(out,
        "%s %s %s\n"
        "\tOPT_Option %s %s\n"
        "\t\t%s\n"
        "\t\t\t%s = \"...\"\n"
        "\t\t%s\n"
        "\n"
        "\t\tPROCEDURE %s()\n"
        "\t\t%s\n"
        "\t%s\n"
        "%s\n",
        name, estM, L("MenuContextuel", "ContextMenu"),
        estF, opt, prop, lib, fin, sel, fin, fin, fin);
    return 0;
    #undef L
}

/* Liste exposée pour le self-test : toutes les combinaisons (kind, sub)
 * supportées par owl_new_generate. Le terminateur est {NULL, NULL}. */
const OwlNewVariant OWL_NEW_VARIANTS[] = {
    {"fenetre",    NULL},
    {"etat",       NULL},
    {"classe",     NULL},
    {"collection", NULL},
    {"theme",      NULL},
    {"service",    NULL},
    {"champ",    "bouton"},
    {"champ",    "libelle"},
    {"champ",    "saisie"},
    {"champ",    "table"},
    {"champ",    "conteneur"},
    {"champ",    "image"},
    {"champ",    "liste"},
    {"champ",    "combo"},
    {"champ",    "selecteur"},
    {"champ",    "interrupteur"},
    {"champ",    "zonerepetee"},
    {"menuprincipal",  NULL},
    {"menucontextuel", NULL},
    {"menuloupe",      NULL},
    {"menucorps",      NULL},
    {"optionmenu",     NULL},
    {"sousmenu",       NULL},
    {"menuseparateur", NULL},
    {NULL, NULL}
};

static void new_usage(FILE* out) {
    fprintf(out, "Usage : owl new <type> <Nom> [options]\n");
    fprintf(out, "        types : fenetre, etat, classe, collection, theme,\n");
    fprintf(out, "                service, champ <kind> <Nom>, projet|project [Nom],\n");
    fprintf(out, "                menuprincipal <Nom>, menucontextuel <Nom>,\n");
    fprintf(out, "                menuloupe <Nom>, menucorps <Nom>,\n");
    fprintf(out, "                optionmenu <Nom>, sousmenu <Nom>,\n");
    fprintf(out, "                menuseparateur <Nom>\n");
    fprintf(out, "        menuloupe : menu de loupe pret a attacher a un Champ\n");
    fprintf(out, "                Table (propriete MenuLoupe) ; options deja cablees\n");
    fprintf(out, "                sur MoiMeme (Rechercher / Filtrer / Effacer).\n");
    fprintf(out, "        menucorps : menu du corps d'un Champ Table (propriete\n");
    fprintf(out, "                MenuContextuel) ; options deja cablees sur MoiMeme\n");
    fprintf(out, "                (Tout copier -> presse-papier / Export Excel).\n");
    fprintf(out, "        theme : bloc `<Nom> est un Theme` avec styles nommes d'exemple.\n");
    fprintf(out, "        service : squelette `<Nom> est un ServiceREST` (service HTTP\n");
    fprintf(out, "                public, une operation de lecture parametree).\n");
    fprintf(out, "        projet|project : ecrit un marqueur projet.owl|project.owl\n");
    fprintf(out, "                dans le dossier courant (refuse si un marqueur\n");
    fprintf(out, "                existe deja) ; Nom par defaut = nom du dossier.\n");
    fprintf(out, "                option --theme <Nom> : ajoute une ligne Theme\n");
    fprintf(out, "                (reference vers une instance `est un Theme`).\n");
    fprintf(out, "        kind  : bouton | libelle | saisie | table | conteneur |\n");
    fprintf(out, "                image | liste | combo | selecteur |\n");
    fprintf(out, "                interrupteur | zonerepetee\n");
    fprintf(out, "        options (valeurs acceptees en FR et EN) :\n");
    fprintf(out, "          --disposition verticale|horizontale|grille|libre\n");
    fprintf(out, "                        (alias EN : vertical|horizontal|grid|free)\n");
    fprintf(out, "                        (fenetre ; defaut verticale)\n");
    fprintf(out, "          --format a3|a4|a5|letter|legal\n");
    fprintf(out, "                        (etat ; defaut a4)\n");
    fprintf(out, "          --orientation portrait|paysage\n");
    fprintf(out, "                        (alias EN : landscape)\n");
    fprintf(out, "                        (etat ; defaut portrait)\n");
    fprintf(out, "          --lang fr|en  (defaut fr ; en = formes canon anglaises)\n");
}

/* Mappe le libellé de disposition CLI vers l'index interne (0..3).
 * Renvoie -1 si inconnu. */
static int new_disp_index(const char* s) {
    if (strcmp(s, "verticale")   == 0 || strcmp(s, "vertical")   == 0) return 0;
    if (strcmp(s, "horizontale") == 0 || strcmp(s, "horizontal") == 0) return 1;
    if (strcmp(s, "grille")      == 0 || strcmp(s, "grid")       == 0) return 2;
    if (strcmp(s, "libre")       == 0 || strcmp(s, "free")       == 0) return 3;
    return -1;
}

/* Mappe le format de page CLI (a3..legal) vers l'index interne (0..4).
 * 0 = A4 (defaut). Renvoie -1 si inconnu. */
static int new_fmt_index(const char* s) {
    if (strcmp(s, "a4")     == 0) return 0;
    if (strcmp(s, "a3")     == 0) return 1;
    if (strcmp(s, "a5")     == 0) return 2;
    if (strcmp(s, "letter") == 0) return 3;
    if (strcmp(s, "legal")  == 0) return 4;
    return -1;
}

/* Mappe l'orientation CLI vers l'index interne (0..1).
 * 0 = portrait (defaut). Renvoie -1 si inconnu. */
static int new_orient_index(const char* s) {
    if (strcmp(s, "portrait") == 0) return 0;
    if (strcmp(s, "paysage")  == 0 || strcmp(s, "landscape") == 0) return 1;
    return -1;
}

/*  — sanitize `raw` en identifiant OWL valide dans `out` (taille
 * `outSize`). Tout octet hors [A-Za-z0-9_] devient '_'. Si le resultat est
 * vide ou ne commence pas par une lettre/underscore, fallback "Projet". */
static void owl_new_projet_sanitize_ident(const char* raw, char* out, size_t outSize) {
    size_t j = 0;
    for (const char* p = raw; *p && j + 1 < outSize; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_') {
            out[j++] = (char)c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';

    /* Un identifiant OWL ne peut pas etre vide ni debuter par un chiffre. */
    bool valid = (j > 0);
    if (valid) {
        char f = out[0];
        if (!((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z') || f == '_')) {
            valid = false;
        }
    }
    if (!valid) snprintf(out, outSize, "Projet");
}

/*  — vrai si `name` (dans le dossier courant) existe deja. */
static bool owl_new_cwd_has_file(const char* name) {
    FILE* f = fopen(name, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

/*  — `owl new projet|project [Nom]`. Diverge des autres types `new` :
 * au lieu d'imprimer un squelette sur stdout, pose le fichier marqueur de
 * projet (projet.owl / project.owl, graphie de l'argument) dans le dossier
 * courant. Refuse si un marqueur (l'une ou l'autre graphie) existe deja :
 * une frontiere de projet est unique par dossier. Nom optionnel : derive du
 * dossier courant (sanitize) si absent. Le contenu suit --lang (FR defaut). */
static int handle_new_projet(int argc, char* argv[]) {
    int en = owl_error_render_lang_is_en();
    bool graphieEn = (strcasecmp(argv[2], "project") == 0);
    const char* fileName = graphieEn ? "project.owl" : "projet.owl";

    /* Refus si un marqueur de projet existe deja (les deux graphies sont
     * mutuellement exclusives, cf. ). Pas d'ecrasement. */
    const char* existing = NULL;
    if (owl_new_cwd_has_file("projet.owl"))       existing = "projet.owl";
    else if (owl_new_cwd_has_file("project.owl"))  existing = "project.owl";
    if (existing != NULL) {
        if (en) {
            fprintf(stderr,
                "owl new : a project marker already exists here (%s); "
                "edit it or remove it first.\n", existing);
        } else {
            fprintf(stderr,
                "owl new : un marqueur de projet existe déjà ici (%s) ; "
                "éditez-le ou supprimez-le d'abord.\n", existing);
        }
        return 1;
    }

    /* `--theme <Nom>` (optionnel) : ajoute une ligne `Thème = <Nom>` dans le
     * bloc PROPRIETES (reference typee vers une instance `est un Thème`, sans
     * guillemets) ET genere le voisin theme.owl qui DECLARE cette instance
     *. Sans ce voisin la reference etait pendante -> projet.owl ne
     * compilait pas. Sanitize comme le nom de projet (identifiant valide). */
    const char* themeRaw = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            themeRaw = argv[++i];
        }
    }
    char themeIdent[512];
    themeIdent[0] = '\0';
    if (themeRaw != NULL) {
        owl_new_projet_sanitize_ident(themeRaw, themeIdent, sizeof(themeIdent));
    }

    /*  — refus si theme.owl existe deja : pas d'ecrasement (meme
     * contrat que le marqueur projet plus haut). Verifie AVANT toute ecriture
     * pour rester atomique (on ne veut pas de projet.owl orphelin ecrit alors
     * que le voisin theme.owl ne peut pas l'etre). */
    if (themeIdent[0] != '\0' && owl_new_cwd_has_file("theme.owl")) {
        if (en) {
            fprintf(stderr,
                "owl new : a theme file already exists here (theme.owl); "
                "edit it or remove it first.\n");
        } else {
            fprintf(stderr,
                "owl new : un fichier de thème existe déjà ici (theme.owl) ; "
                "éditez-le ou supprimez-le d'abord.\n");
        }
        return 1;
    }

    /* Nom : premier argument positionnel apres le type, sinon derive du nom
     * du dossier courant sanitize. */
    char ident[512];
    const char* explicitName = (argc >= 4 && argv[3][0] != '-') ? argv[3] : NULL;
    if (explicitName != NULL) {
        /* Meme sanitisation que le nom derive du dossier : un nom explicite
         * peut contenir espaces/accents/caracteres non-identifiant (cas
         * banal "Mon Projet"), le fichier genere doit rester compilable. */
        owl_new_projet_sanitize_ident(explicitName, ident, sizeof(ident));
    } else {
        char cwd[1024];
        if (owl_getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
        /* Uniformise les separateurs puis prend le dernier segment. */
        for (char* p = cwd; *p; p++) if (*p == '\\') *p = '/';
        const char* base = strrchr(cwd, '/');
        base = (base != NULL) ? base + 1 : cwd;
        owl_new_projet_sanitize_ident(base, ident, sizeof(ident));
    }

    /*  — EOL natif plateforme : ce fichier est cree de zero et ne sera
     * jamais reecrit par OWL, donc pas de churn de diff a craindre. Le helper
     * est le point de decision unique (cf. common.h). */
    FILE* out = owl_fopen_generated_source(fileName);
    if (out == NULL) {
        if (en) fprintf(stderr, "owl new : cannot write '%s'.\n", fileName);
        else    fprintf(stderr, "owl new : impossible d'écrire '%s'.\n", fileName);
        return 1;
    }
    /* Template complet : toutes les proprietes du catalogue Projet, meme
     * vides (Description/Auteur/Licence = "" sont des valeurs constantes
     * valides). Langues en tete (obligatoire) ; forme liste imposee
     *, valeur par defaut selon --lang : [langueFrançais] (fr)
     * ou [langueEnglish] (en).
     *
     * L'alignement du '=' et l'indentation sont laisses au formateur
     * (owl_emit_canonical, ) : ils dependent de la largeur
     * d'AFFICHAGE du nom de champ dans la langue emise, que du padding en dur
     * ne peut pas suivre sans se retromper au premier renommage. */
    char body[1024];
    int n = snprintf(body, sizeof body,
        "%s %s %s\n"
        "\t%s\n"
        "\t\t%s = %s\n"
        "\t\tDescription = \"\"\n"
        "\t\t%s = \"\"\n"
        "\t\t%s = \"\"\n",
        ident, en ? "is a" : "est un", en ? "Project" : "Projet",
        en ? "PROPERTIES" : "PROPRIETES",
        en ? "Languages" : "Langues",
        en ? "[languageEnglish]" : "[langueFran\xc3\xa7" "ais]",
        en ? "Author" : "Auteur",
        en ? "License" : "Licence");
    /* Ligne Thème optionnelle : reference typee vers une instance
     * `est un Thème` (sans guillemets). */
    if (n > 0 && (size_t)n < sizeof body && themeIdent[0] != '\0') {
        n += snprintf(body + n, sizeof body - (size_t)n,
                      en ? "\t\tTheme = %s\n" : "\t\tTh\xc3\xa8me = %s\n",
                      themeIdent);
    }
    if (n > 0 && (size_t)n < sizeof body) {
        snprintf(body + n, sizeof body - (size_t)n,
                 "\t%s\n%s\n", en ? "END" : "FIN", en ? "END" : "FIN");
    }
    owl_emit_canonical(body, out);
    fclose(out);

    /*  — voisin theme.owl : declare l'instance `<X> est un Thème`
     * referencee par le champ Theme de projet.owl. Reutilise le generateur de
     * squelette theme (source unique, sortie deja canonique) ; ecrit en mode
     * texte -> EOL natif plateforme (acquis ), comme projet.owl. */
    if (themeIdent[0] != '\0') {
        FILE* themeOut = owl_fopen_generated_source("theme.owl");
        if (themeOut == NULL) {
            if (en) fprintf(stderr, "owl new : cannot write 'theme.owl'.\n");
            else    fprintf(stderr, "owl new : impossible d'écrire 'theme.owl'.\n");
            return 1;
        }
        owl_new_generate_ex("theme", NULL, themeIdent, 0, 0, 0, en, themeOut);
        fclose(themeOut);
    }

    if (en) fprintf(stdout, "owl new : created '%s' (project '%s').\n", fileName, ident);
    else    fprintf(stdout, "owl new : '%s' créé (projet '%s').\n", fileName, ident);
    if (themeIdent[0] != '\0') {
        if (en) fprintf(stdout,
            "owl new : created 'theme.owl' (theme '%s') — fill in its styles.\n",
            themeIdent);
        else    fprintf(stdout,
            "owl new : 'theme.owl' créé (thème '%s') — complétez ses styles.\n",
            themeIdent);
    }
    return 0;
}

/* Non-static : drive par le harnais cli_ops_test (--self-test-cli-ops) avec
 * un argv synthetique. 'main' n'est plus un nom reserve : le
 * harnais verrouille qu'il est traite comme un nom de fichier ordinaire. */
int handle_new_command(int argc, char* argv[]) {
    /*  — `projet`/`project` a une semantique fichier (pose un
     * marqueur, refuse si existe), incompatible avec le pattern stdout des
     * autres types. Chemin dedie, autorise l'absence de nom (derive du
     * dossier), donc avant la garde argc < 4. */
    if (argc >= 3
        && (strcasecmp(argv[2], "projet") == 0 || strcasecmp(argv[2], "project") == 0)) {
        return handle_new_projet(argc, argv);
    }

    if (argc < 4) {
        new_usage(stderr);
        return 1;
    }
    const char* kind = argv[2];
    const char* sub  = NULL;
    const char* name = NULL;
    int disp = 0;     /* dispVerticale par défaut (fenetre) */
    int fmt = 0;      /* formatA4 par défaut (etat) */
    int orient = 0;   /* orientPortrait par défaut (etat) */
    /* `--lang fr|en` est un flag de session global (parse_session_flags) :
     * on lit l'état déjà capturé plutôt que de le re-parser ici. Le squelette
     * émis suit la même langue que les diagnostics. */
    int en = owl_error_render_lang_is_en();

    bool is_champ = (strcmp(kind, "champ") == 0 || strcmp(kind, "control") == 0);

    /* Indices positionnels : champ => [sub, name] à partir d'argv[3] ;
     * autres => [name] à partir d'argv[3]. Les `--flag` peuvent suivre.
     * (--lang est déjà retiré d'argv par parse_session_flags.) */
    int pos_seen = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--disposition") == 0) {
            if (i + 1 >= argc) { new_usage(stderr); return 1; }
            disp = new_disp_index(argv[++i]);
            if (disp < 0) {
                fprintf(stderr, "owl new : disposition '%s' inconnue.\n", argv[i]);
                new_usage(stderr);
                return 1;
            }
        } else if (strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc) { new_usage(stderr); return 1; }
            fmt = new_fmt_index(argv[++i]);
            if (fmt < 0) {
                fprintf(stderr, "owl new : format '%s' inconnu.\n", argv[i]);
                new_usage(stderr);
                return 1;
            }
        } else if (strcmp(argv[i], "--orientation") == 0) {
            if (i + 1 >= argc) { new_usage(stderr); return 1; }
            orient = new_orient_index(argv[++i]);
            if (orient < 0) {
                fprintf(stderr, "owl new : orientation '%s' inconnue.\n", argv[i]);
                new_usage(stderr);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "owl new : option inconnue '%s'.\n", argv[i]);
            new_usage(stderr);
            return 1;
        } else {
            if (is_champ && pos_seen == 0)      sub  = argv[i];
            else if (name == NULL)              name = argv[i];
            pos_seen++;
        }
    }

    if (name == NULL || (is_champ && sub == NULL)) {
        new_usage(stderr);
        return 1;
    }

    /*  — le nom doit etre un identifiant OWL legal AVANT d'emettre
     * le moindre squelette. Sinon `owl new classe cl-testCompil` produit du
     * code casse (`cl - testCompil`, le tiret etant l'operateur soustraction)
     * avec exit 0, que le plugin ecrit sans avertir. On refuse tot, message
     * clair sur stderr, rien sur stdout. Le sous-nom `champ` (type de widget)
     * a sa propre resolution -> pas concerne par cette regle. */
    if (!owl_is_valid_identifier(name)) {
        if (en) {
            fprintf(stderr,
                "owl new: invalid name '%s' — an identifier may contain only "
                "letters, digits, '_' and accented characters (a hyphen, space "
                "or punctuation is not allowed, nor a digit in first position).\n",
                name);
        } else {
            fprintf(stderr,
                "owl new : nom invalide '%s' — un identifiant ne peut contenir "
                "que des lettres, chiffres, '_' et accents (le tiret, l'espace "
                "et la ponctuation ne sont pas autorisés, ni un chiffre en "
                "première position).\n",
                name);
        }
        return 1;
    }

    /* La résolution des formes EN canon (orient_const_canon, disp_const_canon
     * via registry) lit le registre vivant : il faut vm_init pour le peupler.
     * Inutile en FR pur (formes littérales), on évite le coût. */
    if (en) vm_init();
    int rc = owl_new_generate_ex(kind, sub, name, disp, fmt, orient, en, stdout);
    if (en) vm_free();
    if (rc != 0) {
        if (sub) fprintf(stderr, "owl new champ : kind '%s' inconnu.\n", sub);
        else     fprintf(stderr, "owl new : type '%s' inconnu.\n", kind);
        new_usage(stderr);
        return 1;
    }
    return 0;
}

/* Non-static : le harnais cli_ops_test (--self-test-cli-ops) le drive
 * directement avec un argv synthetique pour verrouiller le dispatch
 * (no-arg -> exit 64, cible valide -> exit 0). */
/* fmt_path_basename / fmt_basename_is_test / fmt_build_context : extraits dans
 * formatter_context.c (owl_format_path_basename / _basename_is_test /
 * _build_context), partages avec le handler de format du LSP. */

/* Forward decl : defini plus bas. Utilise par handle_fmt_command pour
 * rejeter toute cible non-.owl (, CLAUDE.md massacre evite). */
static bool has_owl_extension(const char* path);

int handle_fmt_command(int argc, char* argv[]) {
    bool dryRun = false;
    const char* target = NULL;
    const char* projectDir = NULL;  /* point de remontee pour resolve_project_root */

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            dryRun = true;
        } else if (strcmp(argv[i], "--project") == 0) {
            /*  +  : `--project <dir>` est un *point de
             * remontee* pour resoudre la frontiere de projet. Cas VSCode :
             * l'extension passe `path.dirname(document.uri)` pour qu'un buffer
             * temp hors-projet beneficie quand meme du contexte cross-fichier
             * du dossier reel. Plus de scan recursif inconditionnel. */
            if (i + 1 < argc) projectDir = argv[++i];
        } else if (argv[i][0] == '-') {
            /* Flag inconnu rejete explicitement (alpha break-net : `--strict`
             * a ete retire, la normalisation whitespace est desormais le
             * comportement unique). Sans ce garde, un `--xxx` glisserait en
             * cible et produirait un "cannot open" trompeur. */
            fprintf(stderr,
                "owl fmt: option inconnue '%s'.\n"
                "  options : --check (dry-run), --project <dir>\n", argv[i]);
            return 64;
        } else if (target != NULL) {
            /*  — plusieurs cibles : refus explicite. Avant ce garde,
             * `target` etait ecrase a chaque positionnel (mesure : rc=0, le
             * DERNIER chemin gagnait, rien n'etait dit du premier). Meme
             * defaut, meme correctif et meme code de sortie que `owl test`
             * : aucune commande de ce depot ne cumule de cibles. */
            fprintf(stderr,
                "owl fmt: une seule cible a la fois (recu aussi '%s' apres "
                "'%s').\n"
                "  owl fmt <fichier.owl>   formate le fichier\n"
                "  options : --check (dry-run), --project <dir>\n",
                argv[i], target);
            return 64;
        } else {
            target = argv[i];
        }
    }

    /* Pas de cible explicite : on NE formate RIEN. owl fmt ne traite qu'un
     * fichier unique a la fois (le mode dossier recursif a ete retire).
     * L'utilisateur doit nommer explicitement un fichier .owl. */
    if (target == NULL) {
        fprintf(stderr,
            "owl fmt: une cible est requise.\n"
            "  owl fmt <fichier.owl>   formate le fichier\n"
            "  options : --check (dry-run), --project <dir>\n");
        return 64;
    }

    /* Initialise the VM so the registry is populated — le formatter lit les
     * formes canon (accents/casse) via le provider que vm_init pose sur le
     * backend registry vivant. Sans ca, les natives migrees perdent leur
     * normalisation d'accent dans owl fmt. */
    vm_init();

    if (is_directory(target)) {
        fprintf(stderr,
            "owl fmt ne formate qu'un fichier a la fois "
            "(le mode dossier a ete retire).\n");
        return 64;
    }

    /*  : rejet strict des extensions non-.owl. Le formatter
     * applique la casse/accent canonique du registry — applique a un
     * .md, .txt, .json ou un fichier sans extension, il massacre le
     * contenu silencieusement (cas reel : CLAUDE.md ecrase). L'extension
     * est l'unique critere fiable (heuristique de contenu trop fragile).
     * Pas de --force / bypass : alpha break-net, le caller passe le bon
     * chemin ou rien. */
    if (!has_owl_extension(target)) {
        fprintf(stderr,
            "owl fmt: cible '%s' n'est pas un fichier .owl "
            "(le formatter ne traite que cette extension).\n", target);
        return 64;
    }

    /* Construit le contexte de casse cross-fichier. Racine resolue par la
     * frontiere de projet : script/point d'entree = dossier seul,
     * projet (projet.owl ancetre) = arbre raciné. */
    const char* targetBasename = owl_format_path_basename(target);
    bool targetIsTest = owl_format_basename_is_test(targetBasename);

    /*  +  : `--project <dir>` est un *point de remontee* (pas
     * une racine forcee recursive). Sans `--project`, on remonte depuis le
     * fichier cible ; avec `--project`, depuis <dir>. Dans les deux cas on
     * passe par resolve_project_root et le mode resultant determine racine +
     * scope (recursif ssi projet.owl ancetre trouve). Symetrie totale : un
     * `--project <dirname du fichier>` reproduit la resolution implicite.
     *
     * Sans projet.owl ancetre, on prefere garder `projectDir` brut comme
     * racine de scan (preserve le format des chemins relatifs cote sortie) ;
     * avec projet.owl, on redirige vers la racine resolue (absolue). */
    char resolvedRoot[1024];
    const char* root;
    bool recursive;
    {
        char probe[1024];
        if (projectDir) {
            snprintf(probe, sizeof(probe), "%s/__owl_probe.owl", projectDir);
        } else {
            snprintf(probe, sizeof(probe), "%s", target);
        }
        ProjectMode mode = resolve_project_root(probe, resolvedRoot,
                                                sizeof(resolvedRoot));
        recursive = (mode == PROJECT_MODE_PROJECT);
        if (recursive) {
            root = resolvedRoot;
        } else if (projectDir) {
            root = projectDir;
        } else {
            root = resolvedRoot;  /* implicite : dossier du fichier cible */
        }
    }

    OwlFormatContext* ctx = owl_format_build_context(root, targetBasename,
                                              targetIsTest, recursive);

    int r = owl_format_file_ctx(target, dryRun, ctx);
    owl_format_context_free(ctx);
    if (dryRun && r == 2) return 1;
    return r;
}

/* Check if the given path has a .owl extension (casse-insensible :
 * MAIN.OWL / file.Owl restent reconnus, coherent avec l'invariant
 * casse du langage). */
static bool has_owl_extension(const char* path) {
    size_t len = strlen(path);
    return len > 4 && strcasecmp(path + len - 4, ".owl") == 0;
}

/*  — `owl translate --to en|fr <fichier.owl> [-o <out>]`
 *
 * Reecrit la surface langage du fichier dans la langue cible. Sortie sur
 * stdout par defaut (jamais in-place silencieux, coherent ) ; `-o`
 * / `--output` ecrit dans un fichier. Erreurs propres : --to absent ou
 * invalide -> exit 64, cible non-.owl -> 64, intokenizable -> 1.
 *
 * Non-static : drive par le harnais translate_test (--self-test-translate)
 * avec un argv synthetique pour verrouiller le dispatch. */
int handle_translate_command(int argc, char* argv[]) {
    OwlTranslateLang target = OWL_TRANSLATE_NONE;
    const char* path = NULL;
    const char* outPath = NULL;
    const char* projectDir = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--to") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "owl translate: --to requiert une valeur (en|fr).\n");
                return 64;
            }
            const char* v = argv[++i];
            if (strcasecmp(v, "en") == 0)      target = OWL_TRANSLATE_EN;
            else if (strcasecmp(v, "fr") == 0) target = OWL_TRANSLATE_FR;
            else {
                fprintf(stderr,
                    "owl translate: langue cible '%s' invalide (attendu : en|fr).\n", v);
                return 64;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "owl translate: %s requiert un chemin de sortie.\n", argv[i]);
                return 64;
            }
            outPath = argv[++i];
        } else if (strcmp(argv[i], "--project") == 0) {
            if (i + 1 < argc) projectDir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                "owl translate: option inconnue '%s'.\n"
                "  owl translate --to en|fr <fichier.owl> [-o <sortie>]\n", argv[i]);
            return 64;
        } else {
            path = argv[i];
        }
    }

    if (target == OWL_TRANSLATE_NONE) {
        fprintf(stderr,
            "owl translate: --to est requis.\n"
            "  owl translate --to en|fr <fichier.owl> [-o <sortie>]\n");
        return 64;
    }
    if (path == NULL) {
        fprintf(stderr,
            "owl translate: une cible est requise.\n"
            "  owl translate --to en|fr <fichier.owl> [-o <sortie>]\n");
        return 64;
    }
    if (!has_owl_extension(path)) {
        fprintf(stderr,
            "owl translate: cible '%s' n'est pas un fichier .owl.\n", path);
        return 64;
    }

    /* vm_init : peuple le registre vivant lu par le provider formatter
     * (natives FR/EN, props, types). Sans ca, translate ne traduirait que
     * les mots-cles. */
    vm_init();

    char* source = read_file(path);
    if (source == NULL) {
        fprintf(stderr, "owl translate: impossible de lire '%s'.\n", path);
        return 1;
    }

    /* Contexte cross-fichier (casse des symboles user voisins). Resolution
     * de frontiere identique a owl fmt. */
    const char* targetBasename = owl_format_path_basename(path);
    bool targetIsTest = owl_format_basename_is_test(targetBasename);
    char resolvedRoot[1024];
    const char* root;
    bool recursive;
    {
        char probe[1024];
        if (projectDir) snprintf(probe, sizeof(probe), "%s/__owl_probe.owl", projectDir);
        else            snprintf(probe, sizeof(probe), "%s", path);
        ProjectMode mode = resolve_project_root(probe, resolvedRoot, sizeof(resolvedRoot));
        recursive = (mode == PROJECT_MODE_PROJECT);
        if (recursive)        root = resolvedRoot;
        else if (projectDir)  root = projectDir;
        else                  root = resolvedRoot;
    }
    OwlFormatContext* ctx = owl_format_build_context(root, targetBasename,
                                                     targetIsTest, recursive);

    char* translated = owl_translate_ctx(source, target, ctx);
    owl_format_context_free(ctx);
    free(source);
    if (translated == NULL) {
        fprintf(stderr, "owl translate: echec de traduction de '%s'.\n", path);
        return 1;
    }

    if (outPath != NULL) {
        FILE* f = fopen(outPath, "wb");
        if (f == NULL) {
            fprintf(stderr, "owl translate: impossible d'ecrire '%s'.\n", outPath);
            free(translated);
            return 1;
        }
        fputs(translated, f);
        fclose(f);
    } else {
        /* stdout est en mode texte par defaut sous Windows : le CRT y
         * retraduit chaque '\n' emis en "\r\n". Le formatter preservant l'EOL
         * de la source, une source deja CRLF ressortirait en "\r\r\n" (support
         * #201, lignes vides intercalees a la relecture) et une source LF
         * serait convertie a l'insu de l'appelant. Le chemin `-o` ecrit deja
         * en binaire ("wb") : les deux canaux doivent livrer les memes octets.
         * Le mode precedent est restaure — le harnais --self-test-translate
         * tourne in-process et d'autres sorties suivent sur ce meme fd. */
        fflush(stdout);
#ifdef _WIN32
        int prevMode = _setmode(_fileno(stdout), _O_BINARY);
#endif
        fwrite(translated, 1, strlen(translated), stdout);
        fflush(stdout);
#ifdef _WIN32
        if (prevMode != -1) _setmode(_fileno(stdout), prevMode);
#endif
    }
    free(translated);
    return 0;
}

/* Parse test command arguments:
 * owl test                    → dir=".", verbosity=0 (compact)
 * owl test -v                 → verbosity=1 (normal: show fails)
 * owl test -vv                → verbosity=2 (verbose: show all)
 * owl test -q / --quiet       → verbosity=-1 (quiet: errors only)
 * owl test --fail-fast / -x   → stop at first failure
 * owl test --filter PATTERN   → run only tests matching PATTERN
 * owl test dir/               → dir="dir/", verbosity=0
 * owl test file.owl           → single file test
 *
 * Une seule cible : un second chemin est refuse (exit 64), jamais ignore.
 */
/* Non-static : drive avec un argv synthetique par --self-test-cli-ops
 * (src/internal_tests/cli_ops_test.c), comme handle_fmt_command. */
int handle_test_command(int argc, char* argv[]) {
    TestOptions opts = {
        .dir = ".",
        .singleFile = NULL,
        .verbosity = 0,  /* compact by default */
        .failFast = false,
        .filter = NULL,
        .jsonOutput = false,
        .strict = false,
        .listFiles = false,
    };

    const char* target = NULL;
    char* allocDir = NULL;
    char* allocFile = NULL;

    /* Parse remaining args after "test" */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-vv") == 0) {
            opts.verbosity = 2;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts.verbosity = 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts.verbosity = -1;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--fail-fast") == 0) {
            opts.failFast = true;
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 < argc) {
                opts.filter = argv[++i];
            } else {
                fprintf(stderr, "owl: --filter requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--json") == 0) {
            opts.jsonOutput = true;
        } else if (strcmp(argv[i], "--strict") == 0 || strcmp(argv[i], "-W") == 0) {
            opts.strict = true;
        } else if (strcmp(argv[i], "--list-files") == 0) {
            opts.listFiles = true;
        } else if (argv[i][0] == '-') {
            /*  — une faute de frappe sur la verbosite (`-vvv`) partait
             * en CIBLE : « cannot open directory '-vvv' », rc=1. `owl fmt` a
             * deja cette garde ; on la reprend telle quelle plutot que de
             * laisser un flag mal tape se faire passer pour un dossier. */
            fprintf(stderr,
                "owl test: option inconnue '%s'.\n"
                "  options : -v / -vv / -q, -x, --filter <motif>, --json,\n"
                "            --strict (-W), --list-files\n", argv[i]);
            return 64;
        } else if (target != NULL) {
            /*  — plusieurs cibles : refus explicite, jamais un
             * silence. Avant ce garde, `target` etait ecrase a chaque
             * positionnel (mesure : le DERNIER chemin gagnait) et le rapport
             * final annoncait « ALL CLEAR » sans un mot sur les chemins
             * ignores. Convention reprise de `owl --check a b`, qui exige un
             * argc exact et sort 64 avec l'usage : le multi-cible n'existe
             * pas dans les commandes qui prennent un chemin. */
            fprintf(stderr,
                "owl test: une seule cible a la fois (recu aussi '%s' apres "
                "'%s').\n"
                "  owl test                lance tout depuis le dossier courant\n"
                "  owl test <dossier>      lance ce dossier\n"
                "  owl test <fichier.owl>  lance ce fichier\n",
                argv[i], target);
            return 64;
        } else {
            target = argv[i];
        }
    }

    /* Determine dir and singleFile */
    if (target != NULL) {
        if (is_directory(target)) {
            opts.dir = target;
        } else if (has_owl_extension(target)) {
            allocDir = extract_directory(target);
            allocFile = extract_filename(target);
            opts.dir = allocDir;
            opts.singleFile = allocFile;
        } else {
            /* Le target n'est ni un dossier ni un fichier .owl explicite.
             * Tente la résolution par nom court : `test_xxx` → cherche
             * récursivement `test_xxx.owl` dans tests/. Confort CLI demandé
             * par utilisateur — `owl test test_ChaineDecoupe` doit
             * fonctionner comme `owl test tests/scenarios/natives/test_ChaineDecoupe.owl`. */
            char candidate[1024];
            snprintf(candidate, sizeof(candidate), "%s.owl", target);
            if (file_exists(candidate)) {
                allocDir = extract_directory(candidate);
                allocFile = extract_filename(candidate);
                opts.dir = allocDir;
                opts.singleFile = allocFile;
            } else {
                char* found = find_owl_file_recursive("tests", target);
                if (found == NULL) {
                    /* Fallback historique : traiter comme dossier (l'erreur
                     * « directory not found » remonte côté run_tests_ex). */
                    opts.dir = target;
                } else {
                    allocDir = extract_directory(found);
                    allocFile = extract_filename(found);
                    opts.dir = allocDir;
                    opts.singleFile = allocFile;
                    free(found);
                }
            }
        }
    }

    int result = run_tests_ex(&opts);
    free(allocDir);
    free(allocFile);
    return result;
}

/* Platform glue is implemented in src/platform.c so this translation unit
 * never has to include <windows.h> (which brings in winnt.h's TokenType
 * enum constant that clashes with OWL scanner's TokenType typedef). */
void owl_force_utf8_console(void);
void owl_install_interrupt_handler(void);
void owl_normalize_argv_utf8(int* pArgc, char*** pArgv);
/*  : filtre d'exception non gere (Windows) — dump stack symbolisee
 * dans %TEMP%\owl_crash.txt au moment d'un segfault. No-op ailleurs. */
void owl_install_crash_handler(void);

/* Lot D.2.3 : --debug-stdio active le mode debugger (lit JSON commands
 * sur stdin, repond sur stdout). Aucune surface reseau. Le parent (futur
 * owl-dap.exe) spawnera owl avec ce flag et stdin/stdout pipes. */
static bool g_debug_stdio = false;

/* Strip recognized session flags from argv in-place so subsequent
 * subcommand dispatch doesn't see them. Modifies argc. */
static void parse_session_flags(int* pArgc, char* argv[]) {
    int w = 1;
    for (int r = 1; r < *pArgc; r++) {
        if (strcmp(argv[r], "--quiet") == 0 || strcmp(argv[r], "--no-banner") == 0) {
            g_repl_quiet = true;
        } else if (strcmp(argv[r], "--repl") == 0) {
            g_repl_auto_print = true;
        } else if (strcmp(argv[r], "--json-errors") == 0) {
            g_json_errors = true;
        } else if (strcmp(argv[r], "--debug-stdio") == 0) {
            g_debug_stdio = true;
        } else if (strcmp(argv[r], "--lang") == 0) {
            /*  : langue de rendu des diagnostics (`--check`,
             * runtime). Defaut FR. `--lang en` bascule le rendu des
             * messages d'erreur via le selecteur owl_errors. Consomme la
             * valeur ; tout autre token que `en`/`fr` retombe sur FR. */
            if (r + 1 < *pArgc) {
                const char* val = argv[++r];
                owl_error_render_set_lang_en(strcmp(val, "en") == 0);
            }
        } else if (strncmp(argv[r], "--lang=", 7) == 0) {
            owl_error_render_set_lang_en(strcmp(argv[r] + 7, "en") == 0);
        } else {
            argv[w++] = argv[r];
        }
    }
    *pArgc = w;
}

/* `owl serve <fichier.owl> [--port N] [--bind adr]` — compile un fichier
 * declarant un ServiceREST et le sert reellement. Defauts (dev) :
 * 127.0.0.1:8080. Bind non-loopback (0.0.0.0) uniquement sur --bind explicite.
 * Pas de vm_init sur ce thread : owl_serve possede la VM sur le thread de son
 * executeur (contrainte VM mono-thread, ). */
/* Defini plus bas (partage avec handle_serve_command) : resout la frontiere
 * projet d'un fichier/dossier cible. */
static int serve_resout_frontiere(const char* cible, Project* outProject,
                                  char* rootOut, size_t rootSize,
                                  bool* outProjectMode);

/* `owl openapi <fichier.owl>` — compile la frontiere projet du fichier (comme
 * `owl serve` : les ressources voisines se resolvent) et imprime sur stdout le
 * document OpenAPI 3 du ServiceREST declare dans CE fichier (meme generateur que
 * la route GET /openapi.json de `owl serve`). Sortie = JSON pur, indente 2
 * espaces. */
static int handle_openapi_command(int argc, char** argv) {
    const char* fichier = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage : owl openapi <fichier.owl>\n"
                "  Imprime le document OpenAPI 3 derive du ServiceREST declare.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "owl openapi : argument inconnu '%s'\n", argv[i]);
            return 2;
        } else if (fichier == NULL) {
            fichier = argv[i];
        } else {
            fprintf(stderr, "owl openapi : un seul fichier attendu (recu aussi '%s')\n",
                    argv[i]);
            return 2;
        }
    }
    if (fichier == NULL) {
        fprintf(stderr, "owl openapi : fichier .owl obligatoire\n"
                        "Usage : owl openapi <fichier.owl>\n");
        return 2;
    }

    /* Validation de lisibilite AVANT la resolution de frontiere : conserve le
     * message d'erreur idoine du cas mono-fichier (le contenu n'est pas reutilise,
     * la compilation passe par la frontiere resolue). */
    char* probe = read_file(fichier);
    if (probe == NULL) {
        fprintf(stderr, "owl openapi : impossible de lire '%s'\n", fichier);
        return 1;
    }
    free(probe);

    /* Resout la frontiere DU FICHIER (compagnons + entree) comme `owl serve` :
     * les ressources voisines (ETAT_X, procedures) se resolvent a la compilation.
     * On documente le service declare dans CE fichier (entry-specific). */
    Project project;
    project_init(&project);
    char root[1024];
    bool projectMode = false;
    int rf = serve_resout_frontiere(fichier, &project, root, sizeof(root), &projectMode);
    if (rf != 0) {
        project_free(&project);
        return rf;
    }
    if (projectMode && owl_chdir(root) != 0) {
        fprintf(stderr, "owl openapi : impossible de se placer dans '%s'\n", root);
        project_free(&project);
        return 74;
    }

    /* Tableaux paralleles (non-test uniquement) empruntes le temps du compile. */
    int total_files = project.fileCount;
    const char** noms    = (const char**)malloc((size_t)(total_files > 0 ? total_files : 1) * sizeof(char*));
    const char** chemins = (const char**)malloc((size_t)(total_files > 0 ? total_files : 1) * sizeof(char*));
    const char** sources = (const char**)malloc((size_t)(total_files > 0 ? total_files : 1) * sizeof(char*));
    if (!noms || !chemins || !sources) {
        free(noms); free(chemins); free(sources);
        project_free(&project);
        fprintf(stderr, "owl openapi : allocation impossible\n");
        return 1;
    }
    int nf = 0, entryFiltered = -1;
    for (int i = 0; i < total_files; i++) {
        if (project.files[i].isTest) continue;
        if (i == project.entryIndex) entryFiltered = nf;
        noms[nf]    = project.files[i].name;
        chemins[nf] = project.files[i].path;
        sources[nf] = project.files[i].source;
        nf++;
    }
    if (entryFiltered < 0) {
        free(noms); free(chemins); free(sources);
        project_free(&project);
        fprintf(stderr, "owl openapi : fichier d'entree invalide (test ?)\n");
        return 65;
    }

    OwlServeProjet projet;
    projet.noms        = noms;
    projet.chemins     = chemins;
    projet.sources     = sources;
    projet.nb          = nf;
    projet.entree      = entryFiltered;
    /* Compilation en mode script (comme serve) : un fichier declarant seulement
     * un ServiceREST n'est pas reconnu comme bloc DSL par le prescan de
     * participation en mode projet ; le service doit etre trouve ou qu'il soit. */
    projet.mode_projet = 0;

    int entry_start = 0;
    char err[256] = {0};
    int total = owl_serve_compile_projet(&projet, &entry_start, err, sizeof(err));

    int rc = 0;
    if (total < 0) {
        /* Compilation echouee : la VM est deja liberee par owl_serve_compile_projet. */
        fprintf(stderr, "%s\n", err[0] ? err : "owl openapi : la compilation a echoue");
        rc = 1;
    } else {
        /* VM vivante : le generateur resout httpOK -> 200 via le registre de
         * constantes (libere par vm_free). On genere AVANT de liberer. */
        if (total > entry_start) {
            /* Le 1er service DE L'ENTREE (ses services occupent le suffixe du
             * registre, l'entree etant compilee en dernier). */
            char* doc = owl_openapi_generate(owl_service_registry_at(entry_start));
            if (doc == NULL) {
                fprintf(stderr, "owl openapi : generation du document impossible\n");
                rc = 1;
            } else {
                fputs(doc, stdout);
                free(doc);
            }
        } else {
            fprintf(stderr, "owl openapi : aucun ServiceREST declare dans '%s'\n", fichier);
            rc = 1;
        }
        vm_free();
    }
    compiler_reset_poo_state();  /* libere le registre de services peuple par le compile */

    free(noms); free(chemins); free(sources);
    project_free(&project);
    return rc;
}

/*  — resout la frontiere projet d'`owl serve` (meme moteur que
 * `owl <fichier>` / `owl .`) et remplit un Project decouvert + son entree.
 * `cible` : fichier, dossier, ou NULL (frontiere implicite = CWD). Renvoie 0
 * en cas de succes, un code d'erreur CLI (>0) sinon (message deja emis).
 * Sur succes, *outProject est decouvert, entryIndex pose, isProjectMode pose ;
 * *outProjectMode indique s'il faut se placer dans le dossier du projet. */
static int serve_resout_frontiere(const char* cible, Project* outProject,
                                  char* rootOut, size_t rootSize,
                                  bool* outProjectMode) {
    char dirbuf[1024];
    bool recursive;
    bool projectMode;
    char* entryBase = NULL;   /* basename entree, NULL => projet.owl */

    if (cible == NULL || is_directory(cible)) {
        /* `owl serve` / `owl serve <dossier>` / `owl serve .` : la frontiere
         * DOIT etre un projet (le service vit dans un des fichiers). */
        const char* base = (cible == NULL) ? "." : cible;
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/_", base);
        ProjectMode mode = resolve_project_root(probe, rootOut, rootSize);
        if (mode != PROJECT_MODE_PROJECT) {
            fprintf(stderr,
                "owl serve : aucun projet.owl dans '%s' — nomme un fichier "
                "(owl serve <fichier>.owl) ou cree un projet.owl\n", base);
            return 2;
        }
        snprintf(dirbuf, sizeof(dirbuf), "%s", rootOut);
        recursive = true; projectMode = true; entryBase = NULL;
    } else {
        /* `owl serve <fichier>` : frontiere du fichier (PROJECT ou SCRIPT). */
        ProjectMode mode = resolve_project_root(cible, rootOut, rootSize);
        if (mode == PROJECT_MODE_PROJECT) {
            snprintf(dirbuf, sizeof(dirbuf), "%s", rootOut);
            recursive = true; projectMode = true;
        } else {
            char* d = extract_directory(cible);
            snprintf(dirbuf, sizeof(dirbuf), "%s", d);
            free(d);
            recursive = false; projectMode = false;
        }
        entryBase = extract_filename(cible);
    }

    if (!project_discover_scoped(outProject, dirbuf, true, recursive)) {
        fprintf(stderr, "owl serve : decouverte du projet impossible ('%s')\n", dirbuf);
        free(entryBase);
        return 65;
    }
    outProject->isProjectMode = projectMode;

    if (entryBase != NULL) {
        outProject->entryIndex = project_find_file(outProject, entryBase);
        if (outProject->entryIndex < 0) {
            fprintf(stderr, "owl serve : '%s' introuvable dans le projet\n", entryBase);
            free(entryBase);
            return 65;
        }
        free(entryBase);
    } else {
        outProject->entryIndex = -1;
        for (int i = 0; i < outProject->fileCount; i++) {
            if (is_projet_marker_name(outProject->files[i].name)) {
                outProject->entryIndex = i;
                break;
            }
        }
        if (outProject->entryIndex < 0) {
            fprintf(stderr, "owl serve : aucun projet.owl dans '%s'\n", dirbuf);
            return 65;
        }
    }

    *outProjectMode = projectMode;
    return 0;
}

static int handle_serve_command(int argc, char** argv) {
    const char* cible = NULL;   /* fichier OU dossier ; NULL = frontiere CWD */
    const char* bind_adr = "127.0.0.1";
    int         port     = 8080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_adr = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage : owl serve [<fichier.owl>|<dossier>] [--port N] [--bind adr]\n"
                "  Compile la frontiere projet declarant un ServiceREST et la sert.\n"
                "  Sans argument : projet du dossier courant. Un fichier voisin\n"
                "  (ETAT_X, procedures) est resolu comme avec `owl .`.\n"
                "  Defaut : 127.0.0.1:8080. --bind 0.0.0.0 pour exposer au reseau.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "owl serve : argument inconnu '%s'\n", argv[i]);
            return 2;
        } else if (cible == NULL) {
            cible = argv[i];
        } else {
            fprintf(stderr, "owl serve : une seule cible attendue (recu aussi '%s')\n",
                    argv[i]);
            return 2;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "owl serve : port invalide (%d)\n", port);
        return 2;
    }

    /* Resout la frontiere + decouvre le projet (compagnons + entree). */
    Project project;
    project_init(&project);
    char root[1024];
    bool projectMode = false;
    int rf = serve_resout_frontiere(cible, &project, root, sizeof(root), &projectMode);
    if (rf != 0) {
        project_free(&project);
        return rf;
    }

    /* Se placer dans le dossier du projet (chemins relatifs du service :
     * iDestination, fCharge... resolvent depuis la frontiere, pas le CWD du
     * terminal). Symetrie avec run_project_scoped. Sources deja lues. */
    if (projectMode && owl_chdir(root) != 0) {
        fprintf(stderr, "owl serve : impossible de se placer dans '%s'\n", root);
        project_free(&project);
        return 74;
    }

    /* Tableaux paralleles (fichiers non-test uniquement — project_run les
     * ignore, mais notre voie serve les prendrait a plat). Empruntes le temps
     * du prepare : la validation les compile, l'executeur en prend une copie. */
    int total = project.fileCount;
    const char** noms    = (const char**)malloc((size_t)(total > 0 ? total : 1) * sizeof(char*));
    const char** chemins = (const char**)malloc((size_t)(total > 0 ? total : 1) * sizeof(char*));
    const char** sources = (const char**)malloc((size_t)(total > 0 ? total : 1) * sizeof(char*));
    if (!noms || !chemins || !sources) {
        free(noms); free(chemins); free(sources);
        project_free(&project);
        fprintf(stderr, "owl serve : allocation impossible\n");
        return 1;
    }
    int n = 0, entryFiltered = -1;
    for (int i = 0; i < total; i++) {
        if (project.files[i].isTest) continue;
        if (i == project.entryIndex) entryFiltered = n;
        noms[n]    = project.files[i].name;
        chemins[n] = project.files[i].path;
        sources[n] = project.files[i].source;
        n++;
    }
    if (entryFiltered < 0) {
        free(noms); free(chemins); free(sources);
        project_free(&project);
        fprintf(stderr, "owl serve : fichier d'entree invalide (test ?)\n");
        return 65;
    }

    OwlServeProjet projet;
    projet.noms        = noms;
    projet.chemins     = chemins;
    projet.sources     = sources;
    projet.nb          = n;
    projet.entree      = entryFiltered;
    /*  — compilation en VRAI mode projet quand la frontiere en est un
     * (projet.owl present). Le prescan reconnait desormais ServiceREST comme
     * bloc DSL conteneur (participation ), donc un fichier
     * ServiceREST-only compagnon n'est plus exclu : le service reste trouvable
     * pour `owl serve .` / `owl serve <dossier>`. Fidele = la cascade de
     * metadonnees projet.owl (MonApp..Champ) ET le boot i18n
     * (owl_boot_project_i18n, gate isProjectMode) tournent, posant la langue
     * declaree. Le teardown de fin de vie reste saute (l'executeur possede son
     * propre cycle, http_exec.c ). Une frontiere SCRIPT
     * (`owl serve <fichier>` hors projet.owl) reste en mode script. */
    projet.mode_projet = projectMode ? 1 : 0;

    OwlServeOptions opts;
    opts.bind_adr = bind_adr;
    opts.port     = port;

    char err[256] = {0};
    OwlServeMontage* m = owl_serve_prepare_projet(&projet, &opts, err, sizeof(err));
    free(noms); free(chemins); free(sources);
    project_free(&project);
    if (m == NULL) {
        fprintf(stderr, "%s\n", err[0] ? err : "owl serve : montage impossible");
        return 1;
    }

    /* SIGINT/SIGTERM -> arret propre (la boucle sort, sockets fermes).
     * Declare a la main : net_tcp_server.h tire la couche reseau (windows.h,
     * clash TokenType), que main.c n'inclut volontairement pas. */
    extern void owl_tcp_server_installe_arret(void);
    owl_tcp_server_installe_arret();

    if (!owl_serve_demarre(m, 0, stdout)) {   /* 0 = jusqu'a Ctrl-C */
        fprintf(stderr, "owl serve : demarrage impossible\n");
        owl_serve_libere(m);
        return 1;
    }
    fprintf(stdout, "  (Ctrl+C pour arreter)\n");
    fflush(stdout);

    owl_serve_attend(m);
    owl_serve_libere(m);
    return 0;
}

int main(int argc, char* argv[]) {
    owl_install_crash_handler();
    owl_force_utf8_console();
    owl_install_interrupt_handler();
    /* Sur Windows, argv arrive en CP-1252 — réencode en UTF-8 pour que
     * `owl explain httpRequête` matche le registre (qui est UTF-8 strict). */
    owl_normalize_argv_utf8(&argc, &argv);
    /*  : active stress-GC si env OWL_STRESS_GC=1. Utilisé par le
     * hook pré-commit (.claude/hooks/check_stressgc_sentinels.sh) et la
     * chasse manuelle . */
    owl_gc_init_stress_from_env();
    int exitCode = 0;

    parse_session_flags(&argc, argv);
    if (g_repl_auto_print) compile_set_auto_print(true);
    if (g_json_errors)     vm_set_json_errors(true);
    if (g_json_errors)     compile_set_json_errors(true);

    /* Un consommateur sous-process est attendu (kernel Jupyter, LSP,
     * debugger sur --debug-stdio) : chaque ligne doit partir a l'ecriture,
     * sinon la reponse dort dans le tampon jusqu'a la sortie du process
     * pendant que le parent l'attend.
     *
     * _IONBF et pas _IOLBF : sur ce toolchain (mesure — stdout redirige
     * vers un fichier ou une pipe, deux printf puis abort()), le defaut
     * rend 0 octet, _IOLBF 0 octet, _IONBF les deux lignes. Le
     * line-buffering est alias sur le full-buffering des que le handle
     * n'est pas un terminal : il ne protege rien. stderr est deja non
     * tamponne par defaut (meme mesure), d'ou l'absence de setvbuf dessus.
     *
     * Le non-tamponne coute une ecriture par caractere (mesure : ~3 s pour
     * 20 000 lignes contre 7 ms en tamponne) — acceptable ici, ces modes
     * sont des protocoles ligne-a-ligne a faible volume ou la latence
     * compte plus que le debit. Ne pas etendre a une sortie de masse. */
    if (g_json_errors || g_repl_auto_print || g_repl_quiet || g_debug_stdio) {
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    /* Sous OWL_GC_POISON on chasse un use-after-free qui tue le process :
     * la sortie du runner de tests doit survivre a l'arret brutal pour
     * borner l'endroit du crash. Meme raison que ci-dessus pour _IONBF —
     * en _IOLBF, un SIGSEGV reel emportait la totalite de stdout. Le cout
     * par caractere est negligeable devant celui du poisoning lui-meme. */
    if (g_gc_poison) {
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    /* Hors des modes protocole ci-dessus : si stdout n'est pas un terminal
     * (redirige vers fichier/pipe), il est plein-tampon et un worker qui ecrit
     * une ligne puis pompe en boucle parait muet jusqu'a sa sortie. _IOLBF est
     * alias sur le plein-tampon ici, et _IONBF (cout par-ecriture) a ete ecarte
     * pour le chemin normal : on demande donc aux natives ligne-orientees de
     * vider stdout apres chaque \n (cout borne a une fois par ligne). */
    {
#ifdef _WIN32
        bool stdout_is_tty = _isatty(_fileno(stdout)) != 0;
#else
        bool stdout_is_tty = isatty(STDOUT_FILENO) != 0;
#endif
        if (!stdout_is_tty) owl_io_set_line_flush(true);
    }

    /* Test command: manages its own VM lifecycle (init/free per test file) */
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        return handle_test_command(argc, argv);
    }

    /*  — génère le navigateur de doc HTML (flag interne build).
     * dump_doc_html() fait son propre vm_init()/vm_free(). */
    if (argc >= 2 && strcmp(argv[1], "--dump-doc-html") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: owl --dump-doc-html <template>\n");
            return 1;
        }
        dump_doc_html(argv[2]);
        return 0;
    }

    /*  — dump TSV de l'ensemble des fiches (baseline "Nouveautés").
     * Consommé par `make snapshot` pour figer tools/doc_fiches_baseline.tsv. */
    if (argc >= 2 && strcmp(argv[1], "--dump-doc-fiches") == 0) {
        dump_doc_fiches();
        return 0;
    }

    /* Sous-commandes documentaires CLI :
     *   owl help [<cmd>]   — synopsis général ou détail d'une commande
     *   owl primer         — alias de `owl learn apercu`
     *   owl learn [<fiche>]— liste les fiches ou en affiche une
     *
     * Toutes init VM+registry car le tokenizer markdown des fiches fait
     * du registry lookup (natives/types/consts) pour colorer les blocs
     * de code OWL embarqués. Sans init → tous les lookups ratent. */
    /* Note : vm_init() appelle stdlib_init() en interne (vm.c). Pas besoin
     * de rappeler stdlib_init() explicitement ici — bug avant qui causait
     * une double-registration du registry (chaque native, type, const
     * apparaissait en double dans `owl list`). */
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        vm_init();
        int rc = cli_doc_help(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "primer") == 0) {
        vm_init();
        int rc = cli_doc_primer(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "learn") == 0) {
        vm_init();
        int rc = cli_doc_learn(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "gotchas") == 0) {
        vm_init();
        int rc = cli_doc_gotchas(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "cheatsheet") == 0) {
        vm_init();
        int rc = cli_doc_cheatsheet(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        vm_init();
        int rc = cli_doc_list(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "explain") == 0) {
        vm_init();
        int rc = cli_doc_explain(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "examples") == 0) {
        vm_init();
        int rc = cli_doc_examples(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "search") == 0) {
        vm_init();
        int rc = cli_doc_search(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "where") == 0) {
        /* Reverse lookup : « où ce symbole est-il utilisé ? »
         * Convention IDE/LSP universelle (« Find references »). */
        vm_init();
        int rc = cli_doc_where(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "icons") == 0) {
        /*  — exploration du catalogue Iconify embarque.
         * vm_init requis : owl_icones_namespace_lookup materialise un
         * ObjIcone (GC roots), et le rasterizer Qt6Svg dans owl.exe
         * passe par le hook enregistre par stdlib_ui.c au boot. */
        vm_init();
        int rc = cli_doc_icons(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "template") == 0) {
        /* `owl template [<nom>]` — squelettes prêts à l'emploi. */
        vm_init();
        int rc = cli_doc_template(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "__complete") == 0) {
        /* Backend pour scripts shell de complétion (owl-completion.bash,
         * owl-completion.ps1). */
        vm_init();
        int rc = cli_doc_complete(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "mcp") == 0) {
        /* `owl mcp` — serveur Model Context Protocol sur stdio.
         * Pas de sous-mode : il n'y a qu'un seul rôle pour ce binaire en
         * mode MCP, lancer le serveur. Init VM+registry parce que les
         * tools/resources exposés (étape 2+) feront des lookups registry. */
        extern int cli_mcp_serve(int argc, char** argv);
        vm_init();
        int rc = cli_mcp_serve(argc - 1, argv + 1);
        vm_free();
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "serve-llm") == 0) {
        /* `owl serve-llm --model <x.gguf>` — serveur LLM local compatible
         * OpenAI (SSE). PAS de vm_init sur le thread principal : le modele se
         * charge en handles bruts (sans VM/GC) et c'est le thread executeur du
         * serveur qui possede la VM (le producteur mode C y parse le JSON). */
        extern int owl_serve_llm_main(int argc, char** argv);
        return owl_serve_llm_main(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "openapi") == 0) {
        /* `owl openapi <fichier.owl>` — export du document OpenAPI 3 derive.
         * vm_init requis : la compilation peuple le registre de services (AST)
         * et le generateur resout les codes HTTP via le registre de constantes. */
        return handle_openapi_command(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
        /* `owl serve <fichier.owl>` — sert un ServiceREST. PAS de vm_init sur
         * ce thread : owl_serve possede la VM sur le thread de son executeur
         * (VM mono-thread, ), comme serve-llm. */
        return handle_serve_command(argc - 1, argv + 1);
    }

    /* Build command : la commande a ete deplacee dans owl-build (binaire
     * dedie au packaging). owl.exe reste runtime-only. */
    /* Format command : handle_fmt_command fait son propre vm_init + pose le
     * provider de canon registry (le formatter canonise contre le registre
     * vivant). Pas le vm_init global ci-dessous. */
    if (argc >= 2 && strcmp(argv[1], "fmt") == 0) {
        return handle_fmt_command(argc, argv);
    }

    /*  : `owl translate --to en|fr <fichier>` fait son propre
     * vm_init (registre vivant pour le provider de canon). */
    if (argc >= 2 && strcmp(argv[1], "translate") == 0) {
        return handle_translate_command(argc, argv);
    }

    /* Phase H : `owl new ...` template generator.
     * vm_init requis : le contenu genere passe par le formateur
     * avant emission, et le formateur canonise contre le REGISTRE VIVANT.
     * Sans vm_init il ne canoniserait que les mots-cles et perdrait accents
     * et casse canon des natives — meme raison que handle_fmt_command. */
    if (argc >= 2 && strcmp(argv[1], "new") == 0) {
        vm_init();
        int rc = handle_new_command(argc, argv);
        vm_free();
        return rc;
    }

    vm_init();

    /* Lot D.2.3 : si --debug-stdio a ete passe, lit les commandes debugger
     * sur stdin et repond sur stdout. Aucune surface reseau, pattern parent-
     * child standard (gdb mi, lldb-vscode, debugpy). Spawn par owl-dap.exe
     * en sub-process avec stdin/stdout pipes. */
    if (g_debug_stdio) {
        if (!debug_stdio_start()) {
            fprintf(stderr, "owl: --debug-stdio failed to start\n");
            vm_free();
            return 1;
        }
        /* Gate anti-course : on attend que le debugger ait flushe ses
         * breakpoints et envoye `run` avant d'executer quoi que ce soit.
         * Sinon un script rapide depasse une ligne a breakpoint avant que
         * le set_breakpoint correspondant n'arrive. */
        debug_stdio_wait_for_run();
    }

    if (argc == 1) {
        /* Sans argument : si le dossier courant contient un projet.owl, on
         * lance le projet (meme dispatch que `owl .`, ). Sinon —
         * ou si --repl force le mode interactif — on ouvre le REPL (repli
         * interactif, pas un point d'entree par defaut). */
        if (g_repl_auto_print ||
            (!file_exists("projet.owl") && !file_exists("project.owl"))) {
            repl();
        } else {
            exitCode = run_directory(".");
        }
    } else if (argc == 2) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("OWL %s\n", OWL_VERSION);
        } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            exitCode = call_help();
        } else if (strcmp(argv[1], "--credits") == 0) {
            /* Attribution des bibliotheques tierces vendorees.
             * Satisfait CC-BY-3.0 game-icons + LGPL Qt6 + autres licences
             * permissives qui demandent une mention. Source de verite =
             * vendor/VERSIONS.md (hardcode minimal dans print_credits). */
            print_credits();
        } else if (strcmp(argv[1], "--vscode-data") == 0) {
            print_vscode_data();
        } else if (strcmp(argv[1], "--dump-lsp-catalog") == 0) {
            dump_lsp_catalog();
        } else if (strcmp(argv[1], "--check-server") == 0) {
            /*  Lot 2a — boucle de check resident sur stdin/stdout
             * (protocole detaille au-dessus de handle_check_server_command).
             * Sort a EOF : le serveur ne survit jamais a son client. */
            return handle_check_server_command();
#ifndef OWL_RELEASE_DISTRIB
        /* Self-tests (harnais CI / mecaniques internes) — exclus du binaire
         * distribuable. --vscode-data / --dump-lsp-catalog restent
         * (dump du catalogue public, requis par le build owl-lsp) mais sont
         * masques de `owl help`. snapshot l'est aussi (plus bas). */
        } else if (strcmp(argv[1], "--self-test-registry") == 0) {
            vm_init();
            int viol = registry_self_test_run(stdout);
            vm_free();
            exitCode = (viol > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-presentation") == 0) {
            /*  — garde anti-derive de la couche 2 (intention de
             * presentation du designer). Pour chaque widget couvert (opt-in :
             * Saisie pour l'instant), toute prop designer-exposee du catalogue
             * a une entree couche 2, et toute entree pointe une prop reelle.
             * Necessite le registry initialise (g_dsl_props peuple par
             * stdlib_ui_catalog_init via vm_init). */
            vm_init();
            int viol = owl_presentation_self_test_run(stdout);
            vm_free();
            exitCode = (viol > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-encoding") == 0) {
            /*  — garde d'hygiene encodage des sources (tests/src/lsp).
             * Scan byte-level pur (pas de vm_init, pas de locale) : UTF-8
             * strict + BOM + signature mojibake C3 83 (cf. ). */
            extern int encoding_self_test_run(FILE* out);
            exitCode = (encoding_self_test_run(stdout) > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-errors") == 0) {
            /* Self-test registry erreurs (+0073+0080+0082) : invariants +
             * rendu templates + helpers compile-aware + helpers runtime-aware +
             * helpers native-aware. Retourne 0 si tous les tests passent. */
            extern int owl_errors_run_tests(void);
            extern int owl_errors_compile_run_tests(void);
            extern int owl_errors_runtime_run_tests(void);
            extern int owl_errors_native_run_tests(void);
            extern int owl_errors_case_count(void);
            extern int owl_errors_compile_case_count(void);
            extern int owl_errors_runtime_case_count(void);
            extern int owl_errors_native_case_count(void);
            vm_init();
            int fails = owl_errors_run_tests();
            fails += owl_errors_compile_run_tests();
            fails += owl_errors_runtime_run_tests();
            fails += owl_errors_native_run_tests();
            int nReg  = owl_errors_case_count();
            int nComp = owl_errors_compile_case_count();
            int nRt   = owl_errors_runtime_case_count();
            int nNat  = owl_errors_native_case_count();
            int nTot  = nReg + nComp + nRt + nNat;
            fprintf(stderr,
                    "errors: %d cas exerces sur 4 suites "
                    "(registre %d, compile %d, runtime %d, natif %d)\n",
                    nTot, nReg, nComp, nRt, nNat);
            /* Garde ensemble vide : voir chaque suite. Filet global au cas ou
             * une suite entiere cesserait d'etre appelee. */
            if (nTot == 0) {
                fprintf(stderr, "errors: CORPUS VIDE — 0 cas exerce. "
                                "Un controle qui n'examine rien ne prouve rien.\n");
                fails++;
            }
            vm_free();
            exitCode = (fails > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-similarity") == 0) {
            /* Self-test famille SimilaritéTexte (TexteDistance). C pur :
             * exerce le décodeur codepoint + Levenshtein sans vm_init.
             * Prouve le comptage codepoint-level (vecteurs CJK non foldés). */
            extern int similarity_self_test_run(void);
            exitCode = (similarity_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-fmt-stability") == 0) {
            /*  — fmt-stability in-process : formate chaque fixture
             * .owl de tests/compiler/fmt-stability/ via owl_format et compare au
             * snapshot .expected.owl. Remplace la boucle bash homonyme de
             * run_compiler_tests.sh (1 boot au lieu de 26 spawns owl + mktemp).
             * Le formatter canonise contre le registry -> vm_init + stdlib_init. */
            extern int fmt_stability_self_test_run(void);
            vm_init();
            exitCode = fmt_stability_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-fmt-contract") == 0) {
            /*  — contrat du formateur, sans snapshot : idempotence et
             * « si l'entree compile, sa sortie formatee compile ». La seconde
             * est celle qui manquait — un formateur qui casse la compilation
             * doit etre arrete par la batterie. Compile -> vm_init. */
            extern int fmt_contract_self_test_run(void);
            vm_init();
            exitCode = fmt_contract_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-fmt-canon") == 0) {
            /*  — confronte la forme canon affichee par la fiche
             * keyword (owl explain / hover) a celle que fmt_keywords[] ecrit
             * au save. Deux sources de canon pour le meme mot ; personne ne
             * les comparait. C pur : ni vm_init ni registre, les deux tables
             * sont statiques. */
            extern int fmt_canon_self_test_run(void);
            exitCode = fmt_canon_self_test_run() > 0 ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-temporal-parity") == 0) {
            /* Lot 2B-1 (chantier  axe 4) — single-parser invariant
             * temporal. Itere un oracle canon (Date/Heure/DateHeure/Duree :
             * valides + format invalides + bornes invalides) sur les
             * parsers C exposes par stdlib_date_parsers.h. Les coerces
             * runtime et le compile-time literal filter passent EXACTEMENT
             * par ces parsers (single-parser invariant) — une divergence
             * oracle vs parser signale donc une regression sur les DEUX
             * chemins simultanement. C pur, pas de vm_init necessaire. */
            extern int temporal_parity_self_test_run(FILE* out);
            exitCode = temporal_parity_self_test_run(stdout) > 0 ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-compile-error") == 0) {
            /*  — compile-error fixtures in-process : remplace
             * run_compile_error_tests.sh (137 fixtures x 4 spawns shell). Chaque
             * fixture est isolee dans un workdir temp et validee via
             * handle_check_command_no_vm_lifecycle, sortie JSON comparee au
             * snapshot .expected.json. vm_init + stdlib_init (le check resout
             * des symboles registry). */
            extern int compile_error_self_test_run(void);
            /* Pas de vm_init ici : le self-test gere une VM fraiche PAR fixture
             * (vm_init + compiler_reset_poo_state + vm_free), pour reproduire
             * l'isolation process-par-fixture du script et eviter la fuite
             * d'etat compile-time entre fixtures. */
            exitCode = compile_error_self_test_run() > 0 ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-doc-phantom-lint") == 0) {
            /*  — calibration du lint anti-natives-fantomes (volet B).
             * Confronte le detecteur partage owl_doc_phantom_detect (le meme
             * que la Regle 63 du registry self-test) aux fixtures
             * tests/doc-lint-fixtures/ + oracle EXPECTED.json. Le detecteur
             * resout des symboles registre -> vm_init prealable. */
            extern int doc_phantom_lint_self_test_run(void);
            vm_init();
            exitCode = doc_phantom_lint_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-signature") == 0) {
            /*  — signature fixtures in-process : remplace la boucle
             * signature de run_compiler_tests.sh. Meme chemin (--check scanne
             * le dossier signature/) + capture stdout+stderr. VM fraiche par
             * fixture, geree dans le self-test. */
            extern int signature_self_test_run(void);
            exitCode = signature_self_test_run() > 0 ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-multi-instance") == 0) {
            /*  — tests structures multi-instance fenetre (ObjWindowInstance
             * + ObjUiControlInstance + ObjUiControlRef + stack VM + GC marking).
             * Pas besoin de stdlib_init : C pur, mock decls dans le test. vm_init
             * suffit pour initialiser le GC + vm.next_instance_id. */
            extern int multi_instance_test_main(void);
            vm_init();
            exitCode = multi_instance_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-mcp") == 0) {
            /* Self-test du serveur MCP (cf. src/internal_tests/cli_mcp_test.c).
             * Drive les handlers via cli_mcp_handle_request — pas de pipe. */
            extern int cli_mcp_self_test_run(void);
            vm_init();
            exitCode = cli_mcp_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-cli") == 0) {
            /* Tests de la suite documentaire CLI (help/primer/learn/list/
             * explain/examples/search/__complete). Requiert vm + registry
             * init car certains tests appellent les commandes qui font des
             * lookups registry. */
            extern int cli_doc_self_test_run(void);
            vm_init();
            exitCode = cli_doc_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-runner") == 0) {
            /* Meta self-test du test runner lui-meme : verifie que
             * un scenario qui fail (runtime ou assertion) provoque un exit
             * code non-zero + STATUS coherent (pas "ALL CLEAR"). */
            extern int test_runner_self_test_run(void);
            vm_init();
            exitCode = test_runner_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-cli-ops") == 0) {
            /* Self-test des commandes CLI OPERATIONNELLES :
             * verrouille le dispatch argv de fmt / check / list keywords
             * (fmt sans cible -> exit 64 + no-op ; list keywords -> section
             * Reserves). Complement de --self-test-cli (commandes doc). */
            extern int cli_ops_self_test_run(void);
            vm_init();
            exitCode = cli_ops_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-translate") == 0) {
            /*  — round-trip FR<->EN de la surface langage : identite
             * au canon (translate(translate(F,EN),FR) == fmt(F)), symetrie,
             * preservation identifiants/chaines/commentaires, EN produit
             * compile. Driven in-process (dup2 stdout comme cli_ops). */
            extern int translate_self_test_run(void);
            vm_init();
            exitCode = translate_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-go-local") == 0) {
            /*  — detection fichier-fenetre pur pour le mode GO
             * local (`owl FEN_X.owl` -> Ouvre implicite). C pur, pas de
             * vm_init : le scanner est stateless. */
            extern int go_local_self_test_run(void);
            exitCode = (go_local_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-repl-completion") == 0) {
            /*  — complétion Tab du REPL branchée sur le registre
             * vivant (natives/types/consts/enums) au lieu de listes figées.
             * Requiert vm_init (peuple le registre + vm.globals) : géré dans
             * le harnais. */
            extern int repl_completion_self_test_run(void);
            exitCode = (repl_completion_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-prescan-depth") == 0) {
            /*  Lot C — depth tracker prescan_file() face a un
             * bloc CONSTANTE imbrique dans une Fenetre. Reactive la
             * couverture perdue de l'ex window_constante_with_post_code
             * (devenu impossible sous canon strict). C pur. */
            extern int prescan_depth_self_test_run(void);
            exitCode = (prescan_depth_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-block-openers") == 0) {
            /*  — garde de coherence du discriminant « ce mot-cle ouvre-
             * t-il un bloc ferme par FIN ? ». Confronte la regle partagee
             * (prescan_depth_delta) et ses consommateurs (pre-passe d'element,
             * detecteur GO) sur un corpus de constructions connues : les 4
             * formes sans FIN et les mots-cles sains. Le cycle vm_init/vm_free
             * est pilote par cas (compile in-process). */
            extern int block_opener_guard_test_main(void);
            exitCode = (block_opener_guard_test_main() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-collection-prescan-guard") == 0) {
            /*  — garde de coherence entre l'indexation prealable des
             * membres de CollectionDeProcedures et le parse reel. Verrouille
             * la detection (membre defini mais non indexe = invisible des
             * autres fichiers) ET le silence sur le code legitime (membre
             * confine par LOCAL, source hors perimetre du scan). C pur. */
            extern int collection_prescan_guard_self_test_run(void);
            exitCode = (collection_prescan_guard_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-catalog-bilingual") == 0) {
            /*  — garde bilingue des catalogues generes : capture les
             * exports (--vscode-data, --dump-lsp-catalog) et verifie que tout
             * symbole exporte dont le registre porte une forme EN distincte
             * emet aussi cette forme EN. Trois pannes identiques (,
             * , ) avaient franchi la CI faute de ce controle.
             * Les exports pilotent eux-memes le cycle vm_init/vm_free. */
            extern int catalog_bilingual_self_test_run(FILE* out);
            exitCode = (catalog_bilingual_self_test_run(stdout) > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-ui-bindings") == 0) {
            /*  — contrat de la table de bindings GUI sous Ouvre
             * imbrique : mark/restore de g_binding_count, la fenetre parente
             * ne doit pas etre ecrasee par l'ouverture d'une fille. C pur,
             * pas de Qt ni vm_init. Sur un build sans GUI (owl-cli), pas de
             * table de bindings -> skip silencieux (exit 0). */
#ifdef OWL_HAS_GUI
            extern int ui_bindings_self_test_run(void);
            exitCode = (ui_bindings_self_test_run() > 0) ? 1 : 0;
#else
            fprintf(stdout, "ui-bindings: skip (build sans GUI)\n");
            exitCode = 0;
#endif
        } else if (strcmp(argv[1], "--self-test-ui-position") == 0) {
            /*  — math pure du centrage + clamp de position de fenetre
             * (owl_ui_center_clamp, owl_core). Aucune dependance Qt : pas de
             * garde OWL_HAS_GUI, disponible sur toute cible. */
            extern int ui_position_self_test_run(void);
            exitCode = (ui_position_self_test_run() > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-debug-locals") == 0) {
            /*  — tests de resolution des locales par nom dans
             * owl_debug_eval. Verifie que la table de noms de locales dans
             * ObjFunction permet de resoudre un identifiant local/param dans
             * la frame courante (contrat Voie B). ROUGE avant impl ,
             * VERT apres. Requiert vm_init (compile + execute du code OWL). */
            extern int debug_locals_test_main(void);
            vm_init();
            exitCode = debug_locals_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-format-projet-leak") == 0) {
            /*  — verrou anti-fuite de l'etat format projet compile-time
             * (FormatDate/Heure/Duree/Nombre/Monetaire) entre projets d'un meme
             * process : exhaustivite du reset + non-residu cross-projet. Tourne
             * sous `make all`, la ou la suite runtime `owl test` (verrou
             * d'integration a_/b_) ne tourne pas. Requiert vm_init (compile =
             * interning + pile VM). */
            extern int format_projet_leak_self_test_run(void);
            vm_init();
            exitCode = format_projet_leak_self_test_run() > 0 ? 1 : 0;
            vm_free();
        } else if (strcmp(argv[1], "--self-test-compile-split") == 0) {
            /*  — compile_in_project_split produit DEUX
             * ObjFunction (declarations / corps imperatif) avec pool de
             * constantes copie entier + terminateur reemis. Prouve le
             * decoupage sans changer de comportement observable (personne
             * n'appelle encore le split en dehors de ce test — le driver
             * deux phases arrive au Lot 2). Requiert vm_init (compile =
             * interning + pile VM). */
            extern int compile_split_test_main(void);
            vm_init();
            exitCode = compile_split_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-const-index") == 0) {
            /*  — l'index de constante est encode sur 16 bits sur les
             * 24 opcodes concernes. Verifie que la table canonique des formes
             * couvre tout l'enum OpCode, que la marche du bytecode emis
             * retombe exactement sur count (largeur emise == table), et qu'un
             * fichier depassant l'ancien mur de 256 constantes s'execute en
             * rendant les bonnes valeurs. */
            extern int const_index_test_main(void);
            vm_init();
            exitCode = const_index_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-vm-dispatch") == 0) {
            /* Self-test coherence dispatch VM : verifie table
             * dispatch ↔ enum OpCode (via _Static_assert sur OP_COUNT) +
             * execution dynamique programme synthetique couvrant toutes les
             * familles d'opcodes. Garantit qu'aucun opcode ne tombe sur
             * L_BAD_OP (miroir du default: runtime_error). */
            extern int vm_dispatch_test_main(void);
            vm_init();
            exitCode = vm_dispatch_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-multilingue-print") == 0) {
            /*  — l'affichage (Trace) d'une chaineMultilingue resout
             * la langue courante au lieu de dumper le dict brut. Verrouille
             * le chemin object_print case OBJ_MULTILINGUE non capturable par
             * le harness de scenarios .owl (stdout non capture). ROUGE avant
             * impl object.c, VERT apres. Requiert vm_init (compile + exec). */
            extern int multilingue_print_test_main(void);
            vm_init();
            exitCode = multilingue_print_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-case-accent") == 0) {
            /*  — garde fmt-immune de l'invariant 1 (resolution des
             * identifiants casse-insensible ET accent-insensible au runtime).
             * Les scenarios .owl en casse melangee sont canonicalises par
             * owl fmt au premier save et perdent leur valeur ; des snippets
             * portes par des chaines C litterales y sont immunises. Couvre
             * native / procedure user / locale / globale / mot-cle / double
             * type-native. Requiert vm_init (compile + exec). */
            extern int case_accent_guard_test_main(void);
            vm_init();
            exitCode = case_accent_guard_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-xls-styles") == 0) {
            /*  — sérialisation OOXML des styles de cellule (styles.xml
             * + attributs s= + cols/rows). Construit un classeur stylé via la
             * VM, l'écrit, puis dézippe/parse le XML produit pour asserter la
             * structure réelle (dédup, ordre couleur BGR->ARGB, unités). Non
             * capturable par le harness .owl (le contenu du .xlsx est opaque). */
            extern int xls_styles_test_main(void);
            vm_init();
            exitCode = xls_styles_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-xls-preservation") == 0) {
            /*  — préservation ZIP-level des parts non modélisées d'un
             * classeur ouvert (macros/graphiques/médias). Fabrique un .xlsx avec
             * une part custom, l'ouvre/modifie/réenregistre via la VM, puis
             * dézippe pour vérifier que la part custom survit octet pour octet.
             * Non capturable par le harnais .owl (contenu binaire opaque). */
            extern int xls_preservation_test_main(void);
            vm_init();
            exitCode = xls_preservation_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-xls-table-export") == 0) {
            /*  — round-trip de l'export « table -> .xlsx » du menu
             * contextuel de table. La logique data->.xlsx (owl_ui_table_export_xlsx,
             * src/stdlib_ui.c) est pure-données, sans widget Qt : on exporte un
             * tableau fabriqué, on le rouvre via xlsOuvre, puis on dézippe pour
             * asserter le contenu exact. Requiert vm_init (GC + xlsOuvre). */
            extern int xls_table_export_test_main(void);
            vm_init();
            exitCode = xls_table_export_test_main();
            vm_free();
        } else if (strcmp(argv[1], "--self-test-net") == 0) {
            /* Tests internes des briques reseau reutilisables (buffer BE,
             * couche TCP/TLS, SCRAM auth, ...). N'a pas besoin de vm_init
             * — c'est du C pur sans dependance OWL. */
            extern int net_be_buffer_self_test_run(void);
            extern int net_le_buffer_self_test_run(void);
            extern int net_tcp_self_test_run(void);
            extern int net_tcp_tls_self_test_run(void);
            extern int auth_scram_self_test_run(void);
            /*  — encodeurs MIME (QP + RFC 2047). C pur, pas de VM. */
            extern int mime_codec_self_test_run(void);
            /*  — ce controle meurt par intermittence en cours de
             * route, et sa sortie tamponnee partait avec lui : on ne
             * recuperait meme pas les compteurs des familles deja passees,
             * ce qui rend le defaut indiagnosticable (la derniere famille
             * imprimee borne l'endroit de l'arret).
             * _IONBF et pas _IOLBF : mesure sur ce toolchain (msvcrt),
             * stdout redirige vers un fichier, deux printf puis abort() —
             * defaut 0 octet, _IOLBF 0 octet, _IONBF les 2 lignes. Le
             * line-buffering est alias sur le full-buffering cote MSVCRT,
             * il ne protege rien ici. stderr est deja non tamponne par
             * defaut (meme mesure), d'ou l'absence de setvbuf dessus. */
            setvbuf(stdout, NULL, _IONBF, 0);
            int failures = 0;
            /* Garde de non-vacuite : ce controle agrege une liste
             * de familles ; chacune affiche son propre compte, mais rien ne
             * disait combien avaient tourne. Une liste amputee (ou videe par
             * un #ifdef) rendait "0 echec" en n'ayant rien execute. */
            int net_familles = 0;
            failures += mime_codec_self_test_run(); net_familles++;
            failures += net_be_buffer_self_test_run(); net_familles++;
            failures += net_le_buffer_self_test_run(); net_familles++;
            failures += net_tcp_self_test_run(); net_familles++;
            failures += net_tcp_tls_self_test_run(); net_familles++;
            failures += auth_scram_self_test_run(); net_familles++;
            /*  — parsing de reponse HTTP contre un pair mock qui
             * controle le decoupage de ses write(). Exception a la regle
             * ci-dessus : tout le parsing de stdlib_http.c est static, donc
             * le seul chemin d'entree reel est la native — d'ou vm_init. */
            {
                extern int http_llhttp_integration_self_test_run(void);
                vm_init();
                failures += http_llhttp_integration_self_test_run(); net_familles++;
                vm_free();
            }
            /*  — chemin CONNECT du client contre un proxy simule
             * (decoupage des write() dicte) et un amont TLS reel. vm_init
             * pour la meme raison qu'au-dessus : le seul point d'entree du
             * parsing est la native. */
            {
                extern int http_proxy_connect_self_test_run(void);
                vm_init();
                failures += http_proxy_connect_self_test_run(); net_familles++;
                vm_free();
            }
            /*  — precedence entre le proxy explicite et les
             * variables de proxy de l'environnement. En clair, donc sans
             * CONNECT : le sujet est resolve_proxy, qui est static — d'ou
             * vm_init, meme raison qu'au-dessus. */
            {
                extern int http_proxy_precedence_self_test_run(void);
                vm_init();
                failures += http_proxy_precedence_self_test_run(); net_familles++;
                vm_free();
            }
            /*  — jar de cookies et suivi des Location contre un
             * pair local multi-tours, sans mocker la native. vm_init pour
             * la meme raison : le jar et resolve_redirect sont static. */
            {
                extern int http_jar_redirect_self_test_run(void);
                vm_init();
                failures += http_jar_redirect_self_test_run(); net_familles++;
                vm_free();
            }
            /*  — corpus d'attaque du serveur HTTP (phase A0.1).
             * Pas de vm_init : le transport ne connait pas la VM, c'est
             * l'invariant du contrat (§6 « ni entrer dans la VM, ni allouer
             * un objet OWL, ni declencher un GC »). */
            {
                extern int http_server_corpus_self_test_run(void);
                failures += http_server_corpus_self_test_run(); net_familles++;
            }
            /*  — coeur reacteur (chantier W1). Mecanique
             * evenementielle pure (poll-many + roue de timers), sockets
             * loopback reelles + timers fictifs. Pas de vm_init : le
             * reacteur ne connait pas la VM (isolation GC, 100 % C-heap).
             * Pas encore branche sur le serving : additif et isole. */
            {
                extern int net_reacteur_self_test_run(void);
                failures += net_reacteur_self_test_run(); net_familles++;
            }
            /*  — reveil inter-thread self-pipe (chantier W4).
             * Paire de sockets loopback reelle + thread tiers reel :
             * un octet ecrit depuis un autre thread reveille la boucle,
             * draine cote boucle (coalescing). Aucun Obj* ne franchit la
             * frontiere de thread (isolation GC §8 Q1) — pas de vm_init. */
            {
                extern int net_reveil_self_test_run(void);
                failures += net_reveil_self_test_run(); net_familles++;
            }
            /*  — executeur et file (phase A0.2). Contrairement au
             * corpus ci-dessus, ce module POSSEDE la VM : chaque famille y
             * fait son propre vm_init/vm_free (le thread executeur fait le
             * sien), d'ou l'absence de vm_init ici. */
            {
                extern int http_exec_self_test_run(void);
                failures += http_exec_self_test_run(); net_familles++;
            }
            /*  — digue anti-famine VM (concurrence coopérative GIL).
             * F1 : un handler qui bloque sur la contre-pression flux cède la VM
             * (park/restore + relâche du GIL au point de yield) — un handler
             * sain concurrent se résout pendant qu'il est parké. F2 : contrat
             * de déterminisme  §3 (interleaving SEULEMENT aux yields).
             * Câblée au lot 4 : le mécanisme qu'elle vérifie existe désormais.
             * Possède sa propre VM (executeur), pas de vm_init ici. */
            {
                extern int vm_concurrence_famine_self_test_run(void);
                failures += vm_concurrence_famine_self_test_run(); net_familles++;
            }
            /*  — socle MQ, pont de livraison E1. Agnostique du
             * protocole : un bloc d'octets injecte par un pair, lu pendant
             * un tick de pompe, invoque une procedure OWL abonnee. Famille 0
             * fait son propre vm_init/vm_free (elle compile un handler et
             * pilote le reacteur), d'ou l'absence de vm_init ici. */
            {
                extern int mq_livraison_self_test_run(void);
                failures += mq_livraison_self_test_run(); net_familles++;
            }
            /*  — client MQTT 3.1.1 (Lot A). Handshake CONNECT/
             * CONNACK + DISCONNECT contre un broker mock. Compile un harnais
             * OWL et pilote les natives : possede la VM, d'ou son propre
             * vm_init/vm_free (pas de vm_init ici). */
            {
                extern int mqtt_connexion_self_test_run(void);
                failures += mqtt_connexion_self_test_run(); net_familles++;
            }
            /*  — politique et service statique (phase A0.3).
             * Familles 4 et 5 du corpus. Deux familles y font leur propre
             * vm_init/vm_free (mesure anti-fuite GC et vecteur 5.7), d'ou
             * l'absence de vm_init ici. */
            {
                extern int http_politique_self_test_run(void);
                failures += http_politique_self_test_run(); net_familles++;
            }
            /*  — pool de workers reseau (phase A1.1b). La
             * famille C4 fait son propre vm_init/vm_free : le thread
             * principal y possede la VM pendant que N threads reseau
             * tournent, ce qui est precisement ce qu'elle mesure. */
            {
                extern int http_concurrence_self_test_run(void);
                failures += http_concurrence_self_test_run(); net_familles++;
            }
            /*  — gardes stress concurrence/ressource (digue
             * test-first AVANT le reacteur W2, cf. design serveur_http_
             * concurrence_2026-08.md §8 Q2/§8.bis). Gate anti-hang et
             * famine fd sont ROUGES attendus contre le pool actuel
             * (repro du hang serve-llm) ; spin POLLOUT teste la
             * primitive reacteur W1 deja livree ; UAF close-mid-stream
             * et fuites fd/refcount stressent le socle streaming
             *  deja en service. Cf. commentaire de tete du
             * fichier pour l'etat attendu de chacune. */
            {
                extern int http_stress_gardes_self_test_run(void);
                failures += http_stress_gardes_self_test_run(); net_familles++;
            }
            /*  — socle streaming mode C. La file bornee est
             * drainee par le transport pendant que le producteur pousse :
             * les familles threadees font leur propre synchronisation, pas
             * de vm_init ici (les familles e2e possedent leur executeur). */
            {
                extern int http_stream_self_test_run(void);
                failures += http_stream_self_test_run(); net_familles++;
            }
            /*  P2 — native FluxÉcrit + slot ambiant flux courant +
             * park/restore. F0/F1 possedent leur executeur (VM du thread) ;
             * F2/F3 montent/demontent la VM globale eux-memes (vm_init/vm_free). */
            {
                extern int http_flux_ecrit_self_test_run(void);
                failures += http_flux_ecrit_self_test_run(); net_familles++;
            }
            /*  — serve-llm (API OpenAI-compat). Le producteur genere
             * via un seam INDIRECT (ServeLlmCtx.generate) : un faux generateur
             * injecte exerce tout le routage / framing SSE / single-shot sans
             * modele. Pas de vm_init : le producteur possede sa VM via le
             * thread executeur, comme le socle streaming au-dessus. */
            {
                extern int serve_llm_self_test_run(void);
                failures += serve_llm_self_test_run(); net_familles++;
            }
            /*  — convertisseur schema JSON -> GBNF (tool-calling, Voie
             * B). Model-free : le seul besoin VM est json_parse_cstr (parsing
             * du schema) -> le run fait son propre vm_init/vm_free en interne,
             * comme http_exec/politique plus bas. */
            {
                extern int llm_tool_grammar_self_test_run(void);
                failures += llm_tool_grammar_self_test_run(); net_familles++;
            }
            /*  — TLS serveur (phase A1.2). Pas de vm_init : le
             * transport chiffre ne connait pas plus la VM que le
             * transport en clair. */
            {
                extern int http_tls_self_test_run(void);
                failures += http_tls_self_test_run(); net_familles++;
            }
            /*  — TLS serveur pilote par le reacteur (chantier W3).
             * Meme handshake + I/O chiffres, menes SANS poll interne :
             * l'API event-driven rend WANT_LECTURE/ECRITURE a la boucle W1
             * qui re-pompe a l'evenement suivant. C'est le montage que la
             * machine a etats HTTP (W2) consommera. Pas de vm_init. */
            {
                extern int net_tls_reacteur_self_test_run(void);
                failures += net_tls_reacteur_self_test_run(); net_familles++;
            }
            /*  — machine a etats service HTTP, chantier W2 (le
             * pivot). Gates comportementaux boite-noire test-first : fast-
             * path plan de controle, back-pressure ecriture, keep-alive/
             * pipelining a travers l'executeur, trou de couverture cible
             * du reset d'etat en pipeline. Pas de vm_init (transport +
             * executeur C purs). Certains ROUGES tant que la boucle de
             * service reste bloquante (impl a venir). */
            {
                extern int http_reacteur_etats_self_test_run(void);
                failures += http_reacteur_etats_self_test_run(); net_familles++;
            }
            /*  — flux SSE dans la boucle reacteur, chantier W5.
             * Digue test-first : discriminant boite-noire (N >= workers
             * flux actifs ne doivent retenir aucun worker) + garde
             * anti-starvation. Model-free (producteur de test, pas de VM,
             * pas de LLM). ROUGE attendu sur le discriminant tant que le
             * data-plane streaming reste dispatche a des workers bloquants
             * (cf. commentaire de tete du fichier). */
            {
                extern int http_reacteur_stream_self_test_run(void);
                failures += http_reacteur_stream_self_test_run(); net_familles++;
            }
            /*  — annulation de generation a la deconnexion client.
             * Model-free (producteur C paced, PAS de LLM) : mesure la
             * promptitude de la detection d'abandon face a l'executeur
             * mono-thread reel (pool + reacteur post-W5). */
            {
                extern int http_llm_annulation_self_test_run(void);
                failures += http_llm_annulation_self_test_run(); net_familles++;
            }
            /*  — client FTP en clair : coeur protocole VM-free
             * (owl_ftp_open/list/retr/stor) contre un serveur mock stateful
             * multi-tours (banniere/auth/FEAT/PASV/MLSD/RETR/STOR). Prouve
             * le canal de donnees passif, le listage MLSD avec rappel par
             * entree, les transferts octet-exact et les callbacks de
             * progression. Pas de vm_init : le coeur ne connait pas la VM
             * (les natives marshalent seules vers vm_call_reentrant). */
            {
                extern int ftp_e2e_self_test_run(void);
                failures += ftp_e2e_self_test_run(); net_familles++;
            }
#ifdef OWL_HAS_SQL
            /* Drivers SQL : presents seulement dans owl.exe / owl-build,
             * absents d'owl-cli (lib owl_sql non liee). */
            extern int sql_postgres_wire_self_test_run(void);
            extern int sql_postgres_types_self_test_run(void);
            extern int sql_postgres_conn_self_test_run(void);
            extern int sql_postgres_e2e_self_test_run(void);
            extern int sql_postgres_query_self_test_run(void);
            extern int sql_mysql_wire_self_test_run(void);
            extern int auth_mysql_self_test_run(void);
            extern int sql_mysql_conn_self_test_run(void);
            extern int sql_mysql_e2e_self_test_run(void);
            extern int sql_mysql_query_self_test_run(void);
            extern int sql_mysql_types_self_test_run(void);
            extern int sql_duckdb_self_test_run(void);
            extern int sql_mssql_packet_self_test_run(void);
            extern int sql_mssql_prelogin_self_test_run(void);
            extern int sql_mssql_tls_self_test_run(void);
            extern int sql_mssql_login_self_test_run(void);
            extern int sql_mssql_query_self_test_run(void);
            extern int sql_mssql_types_self_test_run(void);
            extern int sql_cnx_string_self_test_run(void);
            failures += sql_postgres_wire_self_test_run(); net_familles++;
            failures += sql_postgres_types_self_test_run(); net_familles++;
            failures += sql_postgres_conn_self_test_run(); net_familles++;
            failures += sql_postgres_e2e_self_test_run(); net_familles++;
            failures += sql_postgres_query_self_test_run(); net_familles++;
            failures += sql_mysql_wire_self_test_run(); net_familles++;
            failures += auth_mysql_self_test_run(); net_familles++;
            failures += sql_mysql_conn_self_test_run(); net_familles++;
            failures += sql_mysql_e2e_self_test_run(); net_familles++;
            failures += sql_mysql_query_self_test_run(); net_familles++;
            failures += sql_mysql_types_self_test_run(); net_familles++;
            failures += sql_duckdb_self_test_run(); net_familles++;
            failures += sql_mssql_packet_self_test_run(); net_familles++;
            failures += sql_mssql_prelogin_self_test_run(); net_familles++;
            failures += sql_mssql_tls_self_test_run(); net_familles++;
            failures += sql_mssql_login_self_test_run(); net_familles++;
            failures += sql_mssql_query_self_test_run(); net_familles++;
            failures += sql_mssql_types_self_test_run(); net_familles++;
            /* Assembleurs de chaines de connexion : purs, sans serveur — la
             * seule chose verifiable pour HFSQL, qui n'a pas d'instance en
             * integration continue. */
            failures += sql_cnx_string_self_test_run(); net_familles++;
#endif
            fprintf(stderr, "net: %d familles executees, %d en echec\n",
                    net_familles, failures);
            if (net_familles == 0) {
                fprintf(stderr, "net: CORPUS VIDE — 0 famille executee. "
                                "Un controle qui n'examine rien ne prouve rien.\n");
                failures++;
            }
            exitCode = (failures > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-http-dsl-service") == 0) {
            /*  — fondation runtime du DSL de service : compilation de
             * surRequete en callable, registre de routes runtime, MoiMême =
             * reponse implicite, et un bout-en-bout minimal (+ controle
             * negatif). La famille e2e possede son executeur/VM ; la famille
             * deterministe fait son propre vm_init/vm_free. */
            extern int http_dsl_service_self_test_run(void);
            setvbuf(stdout, NULL, _IONBF, 0);
            int failures = http_dsl_service_self_test_run();
            exitCode = (failures > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-http-service-flux") == 0) {
            /*  P3 — 3e mode d'execution du DSL de service : une route
             * Flux=fluxSSE/fluxOctets streame E2E (multi-chunk sur le fil),
             * statut fige au 1er chunk, anti-croisement de deux flux parques,
             * anti-famine niveau service. Digue test-first : HORS batterie par
             * defaut jusqu'au vert (cf. en-tete du fichier), a lancer SEUL :
             *   ./build/owl.exe --self-test-http-service-flux */
            extern int http_service_flux_self_test_run(void);
            setvbuf(stdout, NULL, _IONBF, 0);
            int failures = http_service_flux_self_test_run();
            exitCode = (failures > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-http-serve") == 0) {
            /*  — `owl serve` : montage compile -> executeur -> listener
             * -> boucle, sur un VRAI port binde + un VRAI client TCP (ping 200,
             * inconnu 404, defaut localhost, sans-service, port pris). */
            extern int http_serve_self_test_run(void);
            setvbuf(stdout, NULL, _IONBF, 0);
            int failures = http_serve_self_test_run();
            exitCode = (failures > 0) ? 1 : 0;
        } else if (strcmp(argv[1], "--self-test-net-crash-1723") == 0) {
            /*  — repro ISOLE du crash race streaming (priorite 1 du
             * ticket). Delibrement HORS de la liste net_familles de
             * --self-test-net : un CRASH tue tout le process hote, ce qui
             * ferait planter la batterie partagee pour tous les agents en
             * parallele sur ce depot. A lancer SEUL :
             *   ./build/owl.exe --self-test-net-crash-1723
             * L'implementer le cablera dans net_familles par defaut une
             * fois la race corrigee (cf. commentaire de tete du fichier). */
            extern int http_llm_annulation_crash_self_test_run(void);
            exitCode = http_llm_annulation_crash_self_test_run();
#endif /* OWL_RELEASE_DISTRIB */
        } else if (is_directory(argv[1])) {
            /* owl <dossier> -> dispatch aligne sur la frontiere. */
            exitCode = run_directory(argv[1]);
        } else if (has_owl_extension(argv[1])) {
            /* owl <fichier.owl> -> contexte = frontiere du fichier,
             * autodiscovery des voisins + GO-mode auto-open. */
            exitCode = run_owl_file(argv[1]);
        } else {
            exitCode = call_help();
            if (exitCode == 0) exitCode = 64;
        }
    } else if (argc >= 3 && has_owl_extension(argv[1]) &&
               strcmp(argv[2], "--check") == 0) {
        /*  — `owl <fichier> --check` (flag APRES le fichier, ordre CLI
         * naturel « fichier puis option ») est strictement symetrique de
         * `owl --check <fichier>` : un check pur qui emet les diagnostics de
         * compilation. Sans ce garde, `--check` en position post-fichier
         * tombait dans la branche run-avec-args ci-dessous et etait avale comme
         * argument de script : le fichier partait en EXECUTION, et une entree
         * non lancable (structure/service...) sortait sur le message de
         * lancabilite en MASQUANT les diagnostics compile — meme sur un fichier
         * qui ne compile pas. Le check ne porte pas de verdict de lancabilite :
         * celui-ci reste l'apanage du chemin run (`owl <fichier>` sans --check).
         * Les trois formes reconnues miroir de `owl --check <fichier>[...]`. */
        if (argc == 3) {
            return handle_check_command(argv[1]);
        } else if (argc == 4 && strcmp(argv[3], "--overlay-stdin") == 0) {
            return handle_check_command_overlay(argv[1]);
        } else if (argc == 5 && strcmp(argv[3], "--project") == 0) {
            /* (dir, activeFile) : le fichier cible est argv[1], le point de
             * remontee argv[4]. Miroir exact de `owl --check <fichier>
             * --project <dir>` (handle_check_project_command(argv[4], argv[2])). */
            return handle_check_project_command(argv[4], argv[1], false);
        }
        /* Suite post-`--check` non reconnue : refus explicite plutot qu'un run
         * silencieux avec `--check` avale en argument (piege de l'ancien
         * comportement). */
        fprintf(stderr,
            "owl: apres un fichier, '--check' n'accepte que "
            "'--check', '--check --overlay-stdin' ou '--check --project <dir>'.\n");
        return 64;
    } else if (argc >= 2 && has_owl_extension(argv[1])) {
        /* owl file.owl arg1 arg2 ... -> run script with extra args, contexte
         * aligne sur la frontiere du fichier.  : les memes
         * arguments alimentent le canal `--arg` du lancement GO LOCAL. */
        stdlib_console_set_script_args(argc - 2, argv + 2);
        owl_startup_set_cli_args(argc - 2, argv + 2);
        exitCode = run_owl_file(argv[1]);
    } else if (argc == 3 && strcmp(argv[1], "--check") == 0) {
        if (is_directory(argv[2])) {
            return handle_check_project_command(argv[2], NULL, false);
        }
        return handle_check_command(argv[2]);
    } else if (argc == 4 && strcmp(argv[1], "--check") == 0 &&
               strcmp(argv[3], "--overlay-stdin") == 0) {
        if (is_directory(argv[2])) {
            /* `owl --check <dir> --overlay-stdin` : check projet complet ou les
             * buffers non-sauves passes en JSON sur stdin remplacent le disque
             * pour les fichiers overlayes ( Lot 1). Sans overlay
             * lisible, comportement identique a `--check <dir>`. */
            return handle_check_project_command(argv[2], NULL, true);
        }
        /* `owl --check <fichier> --overlay-stdin` : mode SCRIPT (fichier seul,
         * sans projet.owl ancetre force). Meme sortie mono que `--check
         * <fichier>` mais le contenu de la cible vient de l'overlay stdin
         * (, angle mort du Lot 1 qui ne gerait que la cible dossier). */
        return handle_check_command_overlay(argv[2]);
    } else if (argc == 5 && strcmp(argv[1], "--check") == 0 &&
               strcmp(argv[3], "--project") == 0) {
        /* `owl --check <fichier> --project <dir>` : check du fichier cible
         * AVEC contexte projet (prescan <dir>), rapport filtre au format
         * mono-fichier. Branche du diagnostic LSP au save. */
        return handle_check_project_command(argv[4], argv[2], false);
    } else if (argc == 6 && strcmp(argv[1], "--check") == 0 &&
               strcmp(argv[3], "--project") == 0 &&
               strcmp(argv[5], "--overlay-stdin") == 0) {
        /* `owl --check <fichier> --project <dir> --overlay-stdin` : mode fichier
         * actif avec contexte projet ET contenu des buffers ouverts injecte sur
         * stdin. Repli oneshot du LSP quand le resident est HS : la
         * frappe en cours doit garder des diagnostics justes cross-fichier. */
        return handle_check_project_command(argv[4], argv[2], true);
    } else if (argc == 3 && strcmp(argv[1], "preview") == 0) {
        return handle_preview_command(argv[2]);
    } else if (argc == 3 && strcmp(argv[1], "--gen-icon-assets") == 0) {
        /*  — Dump SVG + JSON catalogue Iconify pour le plugin
         * VSCode (designer panel browse icons). Phase 0a chantier #18.
         * Pattern coherent avec prop_registry_gen.js et lsp_catalog_gen.c :
         * single-source au build, plugin VSCode statique au runtime. */
        extern int cli_gen_icon_assets(const char* out_dir);
        return cli_gen_icon_assets(argv[2]);
    }
    else {
        exitCode = call_help();
        if (exitCode == 0) exitCode = 64;
    }

#ifdef OWL_HAS_GUI
    /*  — toast dans un script court : si l'utilisateur a appele
     * ToastAffiche sans Fenetre, le programme termine avant que Qt ait
     * pompe le event loop, et le toast n'est jamais paintee. On laisse
     * le backend pomper jusqu'a ce que tous les toasts actifs aient
     * fini leur fade-out (cap dur 10s cote ui_qt.cpp). No-op si Qt
     * jamais initialise (script sans aucun appel UI). */
    extern void owl_ui_toast_wait_all(int max_ms);
    owl_ui_toast_wait_all(10000);
#endif
    vm_free();
    return exitCode;
}
