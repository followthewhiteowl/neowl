// SPDX-License-Identifier: AGPL-3.0-only
/* build_main.c — Entry point de owl-build, l'outil de packaging OWL.
 *
 * Responsabilite : produire un binaire natif autonome a partir d'un
 * projet .owl. Reutilise build_project() de project.c, mais ajoute :
 *  - layout de sortie <projet>/exe/<target>/<nom>[.exe]
 *  - creation des dossiers de sortie si absents
 *  - detection du install root (lib/ + include/owl/)
 *
 * owl-build embarque sa propre toolchain (zig cc) + les bibliotheques
 * par cible (Qt6, libs OWL) : aucun compilateur externe requis sur la
 * machine qui package. Les cibles produisibles sont celles embarquees
 * dans ce binaire owl-build (--target, defaut = plateforme hote).
 */

#include "common.h"
#include "project.h"
#include "payload_extract.h"
#include "owl_errors.h"  /* owl_error_render_{set_lang_en,lang_is_en} : chrome bilingue */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define PATH_SEP "\\"
#  define EXE_SUFFIX ".exe"
#  define HOST_TARGET "windows-x86_64"
#else
#  include <sys/types.h>
#  include <unistd.h>
#  define PATH_SEP "/"
#  define EXE_SUFFIX ""
#  if defined(__APPLE__)
#    define HOST_TARGET "macos-x86_64"
#  else
#    define HOST_TARGET "linux-x86_64"
#  endif
#endif

/* Bascule la console Windows en UTF-8 (CP 65001) le temps du process, sinon
 * les accents de l'aide sortent en mojibake sur une console CP-1252/850.
 * Defini dans platform.c (version _WIN32 reelle + no-op Unix) ; owl.exe
 * l'appelle en tete de son main. owl-build a son propre main : forward-decl
 * locale, meme convention que main.c (pas de header partage pour ce helper). */
void owl_force_utf8_console(void);

static int dir_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* Vrai si `path` pointe sur un fichier regulier existant. */
static int is_regular_file(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* Vrai si `name` se termine par le suffixe ".owl" (casse-insensible). */
static int ends_with_owl(const char* name) {
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char* s = name + (n - 4);
    return s[0] == '.'
        && (s[1] == 'o' || s[1] == 'O')
        && (s[2] == 'w' || s[2] == 'W')
        && (s[3] == 'l' || s[3] == 'L');
}

static int has_install_layout(const char* root) {
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/include/owl/common.h", root);
    FILE* f = fopen(probe, "r");
    if (f != NULL) { fclose(f); return 1; }
    return 0;
}

/* Cherche le layout install ou dev a partir du chemin de l'executable. */
static char* find_install_root(const char* exePath) {
    const char* lastSep = NULL;
    for (const char* p = exePath; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    if (lastSep != NULL) {
        size_t len = (size_t)(lastSep - exePath);
        char* dir = (char*)malloc(len + 1);
        if (dir == NULL) { fprintf(stderr, "owl-build: out of memory\n"); exit(74); }
        memcpy(dir, exePath, len);
        dir[len] = '\0';
        if (has_install_layout(dir)) return dir;
        free(dir);
    }
    /* Fallback cwd */
    char* dir = (char*)malloc(2);
    if (dir == NULL) exit(74);
    dir[0] = '.'; dir[1] = '\0';
    if (has_install_layout(dir)) return dir;
    free(dir);
    return NULL;
}

/* Extrait le basename du chemin (sans separateur final, sans extension). */
static const char* basename_of(const char* path) {
    const char* p = path;
    const char* lastSep = NULL;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    return (lastSep != NULL && lastSep[1] != '\0') ? lastSep + 1 : path;
}

static void print_usage(void) {
    if (owl_error_render_lang_is_en()) {
        printf(
            "owl-build — OWL packager (produces a standalone binary from a project)\n"
            "\n"
            "Usage:\n"
            "  owl-build [PROJECT_DIR] [-o NAME] [--target TRIPLE]\n"
            "\n"
            "Arguments:\n"
            "  PROJECT_DIR     OWL project directory (required; use '.' for the\n"
            "                  current directory)\n"
            "  -o NAME         Name of the produced binary (default: project folder name)\n"
            "  --target TRIPLE Target of the produced binary (default: " HOST_TARGET ", the\n"
            "                  host platform). Available targets are those embedded in\n"
            "                  this owl-build binary. Output goes into the\n"
            "                  exe/<TRIPLE>/ subfolder.\n"
            "  --prepare       Warm up the extraction cache for the target, then exit\n"
            "                  (no project required). The next build for that target\n"
            "                  starts immediately. Handy for a background install step.\n"
            "  -h, --help      Show this help\n"
            "  -v, --version   Show the version\n"
            "  --lang fr|en    Output language (default: fr); --en / --fr shortcuts\n"
            "\n"
            "Output: <PROJECT_DIR>/exe/<TARGET>/<NAME>" EXE_SUFFIX "\n"
            "\n"
            "Examples:\n"
            "  owl-build .                     # build . into ./exe/" HOST_TARGET "/<dir>" EXE_SUFFIX "\n"
            "  owl-build my_project            # build my_project into my_project/exe/...\n"
            "  owl-build my_project -o app     # custom name (app" EXE_SUFFIX ")\n"
            "  owl-build --prepare             # warm up extraction for the host target\n"
            "  owl-build --prepare --target linux-x86_64  # warm up another target\n"
            "\n"
            "Requirements: no external compiler. owl-build embeds its own toolchain;\n"
            "it produces a standalone binary with no host dependency.\n"
        );
        return;
    }
    printf(
        "owl-build — Packager OWL (produit un binaire autonome a partir d'un projet)\n"
        "\n"
        "Usage:\n"
        "  owl-build [PROJECT_DIR] [-o NAME] [--target TRIPLE]\n"
        "\n"
        "Arguments:\n"
        "  PROJECT_DIR     Dossier du projet OWL (requis ; `.` pour le dossier\n"
        "                  courant)\n"
        "  -o NAME         Nom du binaire produit (défaut : nom du dossier projet)\n"
        "  --target TRIPLE Cible du binaire produit (défaut : " HOST_TARGET ", la\n"
        "                  plateforme hote). Les cibles disponibles sont celles\n"
        "                  embarquees dans ce binaire owl-build. La sortie va dans\n"
        "                  le sous-dossier exe/<TRIPLE>/.\n"
        "  --prepare       Prechauffe le cache d'extraction pour la cible, puis\n"
        "                  sort (aucun projet requis). Le build suivant pour cette\n"
        "                  cible demarre immediatement. Utile pour une etape\n"
        "                  d'installation en arriere-plan.\n"
        "  -h, --help      Affiche cette aide\n"
        "  -v, --version   Affiche la version\n"
        "  --lang fr|en    Langue de sortie (défaut : fr) ; raccourcis --en / --fr\n"
        "\n"
        "Sortie : <PROJECT_DIR>/exe/<TARGET>/<NAME>" EXE_SUFFIX "\n"
        "\n"
        "Exemples:\n"
        "  owl-build .                     # build . dans ./exe/" HOST_TARGET "/<dir>" EXE_SUFFIX "\n"
        "  owl-build mon_projet            # build mon_projet dans mon_projet/exe/...\n"
        "  owl-build mon_projet -o app     # nom personnalise (app" EXE_SUFFIX ")\n"
        "  owl-build --prepare             # prechauffe l'extraction pour la cible hote\n"
        "  owl-build --prepare --target linux-x86_64  # prechauffe une autre cible\n"
        "\n"
        "Pré-requis : aucun compilateur externe. owl-build embarque sa propre\n"
        "toolchain ; il produit un binaire autonome sans dependance hote.\n"
    );
}

/* Reconnait un flag de langue (`--lang en`/`--lang=en`/`--en`/`--fr`) et pose
 * le global de rendu owl_errors (meme mecanisme qu'owl.exe). Retourne le nombre
 * d'arguments consommes (0 = pas un flag de langue, 1 = flag simple, 2 = flag +
 * valeur). `rest` = argv[i+1] si present (pour la forme `--lang en`), sinon NULL.
 * owl-build a son propre main : il ne peut pas reutiliser parse_session_flags
 * (static dans main.c), d'ou ce parsing local qui pose le meme global partage. */
static int consume_lang_flag(const char* arg, const char* rest) {
    if (strcasecmp(arg, "--en") == 0) { owl_error_render_set_lang_en(1); return 1; }
    if (strcasecmp(arg, "--fr") == 0) { owl_error_render_set_lang_en(0); return 1; }
    if (strncasecmp(arg, "--lang=", 7) == 0) {
        owl_error_render_set_lang_en(strcasecmp(arg + 7, "en") == 0);
        return 1;
    }
    if (strcasecmp(arg, "--lang") == 0 && rest != NULL) {
        owl_error_render_set_lang_en(strcasecmp(rest, "en") == 0);
        return 2;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    /* UTF-8 sur la console AVANT toute sortie : l'aide et les messages
     * bilingues contiennent des accents (« Pré-requis », « défaut »). */
    owl_force_utf8_console();

    /* Sentinelle NULL : posee UNIQUEMENT par un argument positionnel. Sans
     * dossier projet explicite (invocation nue, ou flags seuls), on affiche
     * l'usage et on sort, sans rien extraire ni builder. Pour packager le
     * dossier courant, l'utilisateur le designe par `.`. */
    const char* dir = NULL;
    const char* outputName = NULL;
    const char* target = HOST_TARGET;  /* triple : windows-x86_64, linux-x86_64 */
    int prepareOnly = 0;  /* --prepare : prechauffe l'extraction du payload puis sort */
    /* Pose des qu'un flag qui n'a de sens QUE pour un build reel est consomme
     * (-o / --target). Sert a distinguer, en fin de parsing, l'invocation nue
     * (intention = voir les options -> usage exit 0) d'un build auquel il
     * manque juste le dossier positionnel (intention = builder, dossier
     * oublie -> erreur actionnable exit != 0, ). */
    int sawBuildIntent = 0;

    /* Pre-scan des flags de langue avant toute emission : `--help` et les
     * messages fail-fast doivent suivre --lang/--en/--fr quel que soit leur
     * ordre sur la ligne de commande (symetrie avec le pre-filtre d'owl.exe). */
    for (int i = 1; i < argc; i++) {
        consume_lang_flag(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
    }

    for (int i = 1; i < argc; i++) {
        int langConsumed = consume_lang_flag(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
        if (langConsumed > 0) {
            i += langConsumed - 1;  /* saute la valeur de `--lang en` le cas echeant */
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("owl-build %s\n", OWL_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputName = argv[++i];
            sawBuildIntent = 1;
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
            sawBuildIntent = 1;
        } else if (strcmp(argv[i], "--prepare") == 0) {
            prepareOnly = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl-build: unknown option '%s' — see --help\n"
                : "owl-build: option inconnue '%s' — voir --help\n", argv[i]);
            return 64;
        } else {
            dir = argv[i];
        }
    }

    /* --prepare : prechauffe le cache d'extraction du payload pour `target`,
     * puis sort. Ne resout aucun projet, ne cree aucun dossier exe/. Pensee
     * pour un installeur qui paie l'extraction a l'avance (en arriere-plan)
     * afin que le 1er vrai build demarre immediatement. Traite AVANT le check
     * `dir == NULL` : prepare n'exige pas de projet. */
    if (prepareOnly) {
        char selfPath[1024];
        uint64_t pOffset, pSize;
        int hasPayload = (owl_get_self_path(selfPath, sizeof(selfPath)) == 0
                          && owl_payload_check(selfPath, &pOffset, &pSize) == 0);
        if (!hasPayload) {
            /* Binaire dev / dist non single-file : rien a extraire. Pas une
             * erreur — l'installeur peut appeler --prepare inconditionnellement. */
            printf(owl_error_render_lang_is_en()
                ? "owl-build: nothing to prepare (this binary embeds no payload).\n"
                : "owl-build: rien à préparer (ce binaire n'embarque pas de charge).\n");
            return 0;
        }
        char prepRoot[1024];
        /* owl_payload_setup affiche la progression si extraction effective, et
         * ne dit rien si le cache est deja chaud. On emet un message unique
         * « prêt » valable dans les deux cas pour rester simple et robuste. */
        if (owl_payload_setup(target, prepRoot, sizeof(prepRoot)) != 0) {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl-build: failed to prepare the payload (target=%s).\n"
                : "owl-build: échec de la préparation de la charge (cible=%s).\n", target);
            return 70;
        }
        printf(owl_error_render_lang_is_en()
            ? "owl-build: ready (target=%s). Builds for this target will start immediately.\n"
            : "owl-build: prêt (cible=%s). Les builds pour cette cible démarreront immédiatement.\n",
            target);
        return 0;
    }

    /* Aucun dossier positionnel fourni. Deux intentions a distinguer :
     *  - Invocation nue ou flags seuls (langue, --help/-v deja traites plus haut) :
     *    l'utilisateur veut voir les options -> usage + exit 0. INCHANGE.
     *  - Flags de build consommes (-o / --target) mais dossier oublie : dumper
     *    l'usage en silence avec exit 0 laisse croire que la commande a ete
     *    ignoree. On emet une erreur actionnable sur stderr + exit != 0, en
     *    renvoyant vers `owl-build .` (dossier courant) + les flags deja fournis. */
    if (dir == NULL) {
        if (!sawBuildIntent) {
            print_usage();
            return 0;
        }
        /* Reconstruit un exemple reprenant les flags effectivement donnes :
         * `owl-build . [-o NAME] [--target TRIPLE]`. --target n'est repris que
         * s'il differe du defaut hote, pour ne pas noyer le conseil. */
        char hint[1024];
        int n = snprintf(hint, sizeof(hint), "owl-build .");
        if (outputName != NULL && n > 0 && (size_t)n < sizeof(hint)) {
            n += snprintf(hint + n, sizeof(hint) - (size_t)n, " -o %s", outputName);
        }
        if (strcmp(target, HOST_TARGET) != 0 && n > 0 && (size_t)n < sizeof(hint)) {
            snprintf(hint + n, sizeof(hint) - (size_t)n, " --target %s", target);
        }
        if (owl_error_render_lang_is_en()) {
            fprintf(stderr,
                "owl-build: no project directory given.\n"
                "  -> %s   (current directory)\n"
                "  see --help for options.\n", hint);
        } else {
            fprintf(stderr,
                "owl-build: aucun dossier de projet indiqué.\n"
                "  -> %s   (dossier courant)\n"
                "  voir --help pour les options.\n", hint);
        }
        return 2;
    }

    if (!dir_exists(dir)) {
        if (is_regular_file(dir) || ends_with_owl(dir)) {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl-build: '%s' is a file.\n"
                  "  owl-build expects a project directory (containing a projet.owl),\n"
                  "  not a file. Run: owl-build <directory>\n"
                : "owl-build: '%s' est un fichier.\n"
                  "  owl-build attend un dossier de projet (contenant un projet.owl),\n"
                  "  pas un fichier. Lancer : owl-build <dossier>\n", dir);
        } else {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl-build: project directory not found: '%s'\n"
                : "owl-build: dossier projet introuvable : '%s'\n", dir);
        }
        return 65;
    }

    /* Nom par defaut : basename du dossier projet (resolu vers absolu pour
     * le cas ".") . */
    char dirAbs[1024];
    if (strcmp(dir, ".") == 0) {
        if (
#ifdef _WIN32
            _getcwd(dirAbs, sizeof(dirAbs))
#else
            getcwd(dirAbs, sizeof(dirAbs))
#endif
            == NULL) {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl-build: cannot resolve current directory\n"
                : "owl-build: dossier courant irresoluble\n");
            return 65;
        }
    } else {
        snprintf(dirAbs, sizeof(dirAbs), "%s", dir);
    }
    if (outputName == NULL) {
        outputName = basename_of(dirAbs);
        if (*outputName == '\0') outputName = "app";
    }

    /* Construit le chemin <dir>/exe/<target>/ SANS le creer : le dossier de
     * sortie n'est materialise que lorsque le build va reellement produire
     * l'exe (build_project, juste avant le link). Sinon un build qui echoue en
     * amont (portail de compile, etc.) laisserait un `exe/<target>/` vide,
     * trompeur pour l'utilisateur. Ici on ne calcule que la chaine,
     * passee a build_project via outPath. */
    char outDir[1024];
    snprintf(outDir, sizeof(outDir), "%s%sexe%s%s", dir, PATH_SEP, PATH_SEP, target);

    /* Suffix .exe selon le target (pas le host) : windows-* -> .exe,
     * linux-* / macos-* -> rien. */
    const char* targetExeSuffix = "";
    if (strstr(target, "windows") != NULL) targetExeSuffix = ".exe";

    /* Chemin complet de sortie : <dir>/exe/<target>/<name>[.exe] */
    char outPath[1024];
    snprintf(outPath, sizeof(outPath), "%s%s%s%s", outDir, PATH_SEP, outputName, targetExeSuffix);

    /* Detection install root (lib/ + include/owl/).
     * Priorite :
     *   1. Single-file mode : si l'exe contient un payload (footer magic),
     *      on extrait dans %TEMP%/owl-build-<hash>/ (cache) et utilise
     *      ce chemin comme owlRoot.
     *   2. Dev / dist non-mega : cherche en relatif a argv[0] ou cwd. */
    char* installRoot = NULL;
    char extracted[1024];
    if (owl_payload_setup(target, extracted, sizeof(extracted)) == 0) {
        installRoot = strdup(extracted);
    } else {
        installRoot = find_install_root(argv[0]);
    }
    if (installRoot == NULL) {
        fprintf(stderr, owl_error_render_lang_is_en()
            ? "owl-build: install layout not found (looking for include/owl/common.h\n"
              "  next to owl-build" EXE_SUFFIX " or in the current directory).\n"
            : "owl-build: layout install introuvable (cherche include/owl/common.h\n"
              "  a cote de owl-build" EXE_SUFFIX " ou dans le dossier courant).\n");
        return 65;
    }

    printf(owl_error_render_lang_is_en()
        ? "owl-build: packaging '%s' (target=%s)...\n"
        : "owl-build: empaquetage de '%s' (cible=%s)...\n", dir, target);
    fflush(stdout);  /* ordonne la sortie face aux messages stderr de build_project */
    int rc = build_project(dir, outPath, installRoot, target);
    free(installRoot);
    if (rc == 0) {
        printf("owl-build: %s -> %s\n", dir, outPath);
    }
    return rc;
}
