// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test des sous-commandes documentaires CLI.
 *
 * Exécuté via `owl --self-test-cli` (cf main.c). Couvre :
 *   - Codes de sortie attendus (0 / 1 / 2 / 64)
 *   - Multi-bucket explain (type + fonction homonyme)
 *   - Filtre --cat (buckets sémantiques)
 *   - Validation flag inconnu
 *   - Types primitifs hardcodés + types composés registry
 *   - Signatures dans explain
 *   - Tab completion __complete
 *   - JSON output parsable
 *   - Recherche transversale
 *   - Tokenizer scenario body (présence du bullet vert foncé)
 *
 * Approche : in-process. Redirige stdout vers un fichier temporaire,
 * appelle directement `cli_doc_xxx(argc, argv)`, vérifie rc + substrings,
 * restaure stdout. Pas de spawn de sous-processus → 0 dépendance disque
 * pour le binaire testé, et tout passe dans le même owl.exe que celui
 * qui contient le runtime + le registry.
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'échecs. */

#include "cli_doc.h"
#include "cli_colors.h"
#include "owl_registry.h"          /* totaux de reference des buckets */
#include "owl_keyword_catalog.h"   /* owl_keyword_count */
#include "embedded_docs.h"  /* EMBEDDED_PUBLIC_DOCS */
#include "owl_errors.h"     /* owl_error_render_set_lang_en */
#include "owl_stdlib.h"     /* owl_color_catalog_* */
#include "ui_catalog.h"     /* owl_dsl_prop_at + owl_dsl_props_count */
#include "fixture_unique.h" /* noms de fixture propres au processus */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ================================================================
 * Infra
 * ================================================================ */

static int  g_pass = 0;
static int  g_fail = 0;
/* Assez large pour la plus grosse sortie d'un bucket sans filtre : `owl list
 * natives --json` depasse 160 Ko. Une capture tronquee ne se voit pas — elle
 * rend un JSON coupe au milieu d'un tableau, ce qu'une assertion de contenu
 * lit comme une absence. */
static char g_buf[512 * 1024];

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Vérifie que `needle` apparaît dans la dernière capture stdout. */
#define CHECK_HAS(needle, label) \
    CHECK(strstr(g_buf, (needle)) != NULL, label)

/* Vérifie que `needle` n'apparaît PAS dans la dernière capture stdout. */
#define CHECK_NO(needle, label) \
    CHECK(strstr(g_buf, (needle)) == NULL, label)

#define CHECK_RC(actual, expected, label) \
    CHECK((actual) == (expected), label)

/* Compte le nombre d'occurrences disjointes de `needle` dans `g_buf`. */
static int count_occurrences(const char* needle) {
    int n = 0;
    size_t nlen = strlen(needle);
    const char* p = g_buf;
    while ((p = strstr(p, needle)) != NULL) { n++; p += nlen; }
    return n;
}

/* Charge le fichier de capture dans g_buf, et refuse de rendre une capture
 * amputee sans le dire.
 *
 * Une lecture plafonnee a la taille du tampon est muette par construction :
 * les assertions de contenu lisent l'absence de la fin comme une absence tout
 * court, et un test qui balaye un catalogue n'en balaye qu'un prefixe. Mesure
 * de  : sous un tampon de 128 Ko, un test cense couvrir les 607
 * categories rendues n'en voyait que 498 — 82 % du catalogue — et restait vert.
 *
 * La garde vit donc ici, dans le harnais, et non a la charge de chaque auteur
 * de test : c'est la seule place ou elle couvre les captures qui n'y pensent
 * pas. Elle compte un echec explicite ; le contenu tronque est conserve pour
 * que le diagnostic reste lisible, mais la campagne est rouge. */
static void capture_slurp(const char* path, const char* who) {
    FILE* r = fopen(path, "rb");
    if (!r) return;
    size_t n = fread(g_buf, 1, sizeof(g_buf) - 1, r);
    g_buf[n] = '\0';
    /* Un octet de plus disponible = le fichier depassait le tampon. */
    char extra;
    int truncated = (fread(&extra, 1, 1, r) == 1);
    if (truncated) {
        fseek(r, 0, SEEK_END);
        long total = ftell(r);
        g_fail++;
        fprintf(stderr,
                "  FAIL [%s : capture tronquee — %ld octets pour un tampon de "
                "%lu] (agrandir g_buf, cf. fix/1585)\n",
                who, total, (unsigned long)sizeof(g_buf));
    }
    fclose(r);
}

/* Capture stdout dans g_buf le temps d'appeler fn(argc, argv).
 *
 * Approche POSIX : dup le fd 1 pour le sauver, ouvre un fichier temp,
 * dup2 sur fd 1 pour rediriger les writes, appelle, fflush, dup2
 * inverse pour restaurer, puis on lit le fichier APRÈS restauration.
 *
 * On évite freopen() sur stdout qui invalide le FILE* d'origine — sur
 * MSVCRT cela casse les écritures suivantes en cascade et sur MinGW
 * c'est tout autant fragile. Le pattern dup/dup2/open avec un fichier
 * en mode binaire est portable et robuste. */
static int capture_call(int (*fn)(int, char**), int argc, char** argv) {
    /* Nom propre au processus : a nom constant, deux `owl
     * --self-test-cli` lances du meme dossier se volent ce fichier — l'un le
     * tronque (O_TRUNC) pendant que l'autre le relit, g_buf arrive vide, et des
     * dizaines d'assertions de contenu accusent le CLI sans raison. */
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_cli_test_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);

    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return -1;

    int tmp_fd = owl_open(TMP,
                          O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                          0644);
    if (tmp_fd < 0) {
        owl_close(saved_fd);
        return -1;
    }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd);
        owl_close(saved_fd);
        return -1;
    }
    owl_close(tmp_fd);

    /* Désactive la couleur pour des assertions stables. */
    cli_colors_force(CLI_COLOR_PLAIN);

    int rc = fn(argc, argv);
    fflush(stdout);

    /* Restaure stdout AVANT de lire le fichier (sinon la lecture
     * vrille la position du fd qu'on est en train de re-binder). */
    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    /* Lit le contenu via une FILE* fraîche en lecture. */
    capture_slurp(TMP, "capture_call");
    owl_unlink(TMP);
    return rc;
}

/* Variante de capture_call qui capture stdout ET stderr dans g_buf.
 *
 * Les chemins d'erreur du CLI doc (symbole inconnu, indices associés) écrivent
 * sur stderr : sans cette variante ils sont invisibles pour les assertions.
 * capture_call reste le défaut — la fusion des deux flux changerait le contenu
 * observé par tous les tests existants. */
static int capture_call_err(int (*fn)(int, char**), int argc, char** argv) {
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_cli_test_err_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);
    fflush(stderr);

    int saved_out = owl_dup(owl_fileno(stdout));
    int saved_err = owl_dup(owl_fileno(stderr));
    if (saved_out < 0 || saved_err < 0) return -1;

    int tmp_fd = owl_open(TMP,
                          O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                          0644);
    if (tmp_fd < 0) {
        owl_close(saved_out);
        owl_close(saved_err);
        return -1;
    }
    owl_dup2(tmp_fd, owl_fileno(stdout));
    owl_dup2(tmp_fd, owl_fileno(stderr));
    owl_close(tmp_fd);

    cli_colors_force(CLI_COLOR_PLAIN);

    int rc = fn(argc, argv);
    fflush(stdout);
    fflush(stderr);

    owl_dup2(saved_out, owl_fileno(stdout));
    owl_dup2(saved_err, owl_fileno(stderr));
    owl_close(saved_out);
    owl_close(saved_err);

    capture_slurp(TMP, "capture_call_err");
    owl_unlink(TMP);
    return rc;
}

/* Helper court : prépare un tableau d'arguments const → char** non-const
 * (parse_doc_opts n'écrit jamais dedans mais sa signature C99 l'exige). */
#define ARGS(...) (char*[]){ __VA_ARGS__ }

/* ================================================================
 * Tests
 * ================================================================ */

static void test_help(void) {
    int rc = capture_call(cli_doc_help, 1, ARGS("help"));
    CHECK_RC(rc, 0, "help rc=0");
    CHECK_HAS("OWL", "help mentionne OWL");
    CHECK_HAS("explain", "help liste 'explain'");
    CHECK_HAS("primer",  "help liste 'primer'");
    CHECK_HAS("learn",   "help liste 'learn'");
}

static void test_help_completion(void) {
    int rc = capture_call(cli_doc_help, 2, ARGS("help", "completion"));
    CHECK_RC(rc, 0, "help completion rc=0");
    CHECK_HAS("__complete",  "help completion mentionne __complete");
    CHECK_HAS("PowerShell",  "help completion mentionne PowerShell");
    CHECK_HAS("Bash",        "help completion mentionne Bash");
}

/*  — garde-fou de complétude : chaque commande publique réellement
 * branchée dans main.c doit apparaître dans `owl --help` (print_help_top).
 * Une commande ajoutée au dispatch sans entrée d'aide dérive en silence
 * (c'est ce qui est arrivé à serve-llm / mcp / new). Retirer une commande de
 * l'aide fait rougir ce test. */
static void test_help_completeness(void) {
    /* Needles au format `owl <cmd>` tel qu'imprimé : un token nu comme "mcp"
     * matcherait "--self-test-mcp" du bloc outillage interne (dev build) et ne
     * prouverait rien. La forme "owl <cmd>" ancre sur la ligne de listing. */
    static const char* const EXPECTED[] = {
        "owl serve-llm", "owl serve <", "owl mcp", "owl new",
        "owl test", "owl fmt", "owl explain", "owl list",
        "owl primer", "owl template", "owl translate", "owl search", "owl where",
    };
    int rc = capture_call(cli_doc_help, 1, ARGS("help"));
    CHECK_RC(rc, 0, "help (top) rc=0");
    for (size_t i = 0; i < sizeof EXPECTED / sizeof EXPECTED[0]; i++) {
        char label[96];
        snprintf(label, sizeof label,
                 "help (top) liste la commande publique '%s'", EXPECTED[i]);
        CHECK_HAS(EXPECTED[i], label);
    }
}

/*  — `owl serve` etait une commande cachee : branchee au dispatch mais
 * sans page d'aide (« aide indisponible »). L'aide detaillee doit couvrir usage,
 * ports/bind par defaut, et l'exposition reseau via 0.0.0.0. */
static void test_help_serve(void) {
    int rc = capture_call(cli_doc_help, 2, ARGS("help", "serve"));
    CHECK_RC(rc, 0, "help serve rc=0");
    CHECK_NO("indisponible", "help serve : plus de fallback 'aide indisponible'");
    CHECK_HAS("owl serve",  "help serve : ligne usage");
    CHECK_HAS("--port",     "help serve : option --port documentee");
    CHECK_HAS("8080",       "help serve : port par defaut 8080");
    CHECK_HAS("--bind",     "help serve : option --bind documentee");
    CHECK_HAS("127.0.0.1",  "help serve : bind par defaut 127.0.0.1");
    CHECK_HAS("0.0.0.0",    "help serve : exposition reseau via 0.0.0.0");
}

static void test_primer(void) {
    int rc = capture_call(cli_doc_primer, 1, ARGS("primer"));
    CHECK_RC(rc, 0, "primer rc=0");
    CHECK_HAS("Aperçu",     "primer header présent");
    CHECK_HAS("PROCEDURE",  "primer contient des exemples PROCEDURE");
}

static void test_learn_list(void) {
    int rc = capture_call(cli_doc_learn, 1, ARGS("learn"));
    CHECK_RC(rc, 0, "learn (list) rc=0");
    CHECK_HAS("apercu",      "learn liste l'alias apercu");
    CHECK_HAS("types",       "learn liste l'alias types");
    CHECK_HAS("conventions", "learn liste l'alias conventions");
    CHECK_HAS("sql",         "learn liste l'alias sql");
}

static void test_learn_types(void) {
    int rc = capture_call(cli_doc_learn, 2, ARGS("learn", "types"));
    CHECK_RC(rc, 0, "learn types rc=0");
    /* Titre canon dans 01_types_et_coercions.md (vérifié à la main). */
    CHECK_HAS("Types et conversions", "learn types charge la bonne fiche");
}

static void test_learn_unknown(void) {
    int rc = capture_call(cli_doc_learn, 2, ARGS("learn", "ZzZz_inconnu"));
    CHECK(rc != 0, "learn fiche inconnue rc != 0");
}

/*  — toutes les fiches doivent avoir un alias court mémorisable,
 * pas leur nom de fichier brut affiché à la place. Quand le fallback
 * `short_name = d->name` du listing se déclenche, le stem apparaît DEUX
 * fois sur la même ligne (colonne alias + colonne nom). On vérifie ici
 * que chaque stem connu pour avoir manqué d'alias n'apparaît qu'une fois
 * dans la sortie. */
static void test_learn_list_alias_pour_toutes_les_fiches(void) {
    int rc = capture_call(cli_doc_learn, 1, ARGS("learn"));
    CHECK_RC(rc, 0, "learn (list) rc=0");
    CHECK(count_occurrences("03_controle_de_flux") == 1,
          "03_controle_de_flux apparait une seule fois (a un alias court)");
    CHECK(count_occurrences("06_poo") == 1,
          "06_poo apparait une seule fois (a un alias court)");
    CHECK(count_occurrences("07_poo_avance") == 1,
          "07_poo_avance apparait une seule fois (a un alias court)");
    CHECK(count_occurrences("08_exceptions") == 1,
          "08_exceptions apparait une seule fois (a un alias court)");
    CHECK(count_occurrences("20_groupes_de_champs") == 1,
          "20_groupes_de_champs apparait une seule fois (a un alias court)");
    CHECK(count_occurrences("21_internationalisation") == 1,
          "21_internationalisation apparait une seule fois (a un alias court)");
    /* Les nouveaux alias FR doivent etre listes (colonne gauche). */
    CHECK_HAS("controle",   "learn liste l'alias controle");
    CHECK_HAS("poo",        "learn liste l'alias poo");
    CHECK_HAS("heritage",   "learn liste l'alias heritage");
    CHECK_HAS("exceptions", "learn liste l'alias exceptions");
    CHECK_HAS("groupes",    "learn liste l'alias groupes");
    CHECK_HAS("i18n",       "learn liste l'alias i18n");
}

/*  — chaque nouvel alias ouvre la bonne fiche. */
static void test_learn_nouveaux_alias_resolvent(void) {
    int rc;

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "controle"));
    CHECK_RC(rc, 0, "learn controle rc=0");
    CHECK_HAS("Contrôle de flux", "learn controle ouvre 03_controle_de_flux");

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "poo"));
    CHECK_RC(rc, 0, "learn poo rc=0");
    CHECK_HAS("Programmation orientée objet",
              "learn poo ouvre 06_poo");

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "heritage"));
    CHECK_RC(rc, 0, "learn heritage rc=0");
    CHECK_HAS("concepts avancés",
              "learn heritage ouvre 07_poo_avance");

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "exceptions"));
    CHECK_RC(rc, 0, "learn exceptions rc=0");
    /* La fiche 08_exceptions commence par "# Exceptions". */
    CHECK_HAS("Une exception interrompt",
              "learn exceptions ouvre 08_exceptions");

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "groupes"));
    CHECK_RC(rc, 0, "learn groupes rc=0");
    CHECK_HAS("Groupes de champs",
              "learn groupes ouvre 20_groupes_de_champs");

    rc = capture_call(cli_doc_learn, 2, ARGS("learn", "i18n"));
    CHECK_RC(rc, 0, "learn i18n rc=0");
    CHECK_HAS("Internationalisation",
              "learn i18n ouvre 21_internationalisation");
}

/*  — garde-fou de couverture : CHAQUE fiche embarquée doit être
 * atteignable par un alias 'learn' court. Dans la liste `owl learn`, une fiche
 * SANS alias affiche son stem dans les DEUX colonnes (compte = 2) ; une fiche
 * AVEC alias ne l'affiche qu'une fois (compte = 1). Un futur chapitre ajouté
 * sans alias casse donc ce test (cf. le trou  sur 26->31, corrigé en
 * ) — il faut alors ajouter son entrée dans PUBLIC_DOC_ALIASES. */
static void test_learn_alias_coverage_exhaustive(void) {
    int rc = capture_call(cli_doc_learn, 1, ARGS("learn"));
    CHECK_RC(rc, 0, "learn (list) rc=0");
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        char label[192];
        snprintf(label, sizeof(label),
                 "fiche '%s' atteignable par un alias learn "
                 "(sinon l'ajouter dans PUBLIC_DOC_ALIASES)", d->name);
        CHECK(count_occurrences(d->name) == 1, label);
    }
}

static void test_list_top(void) {
    int rc = capture_call(cli_doc_list, 1, ARGS("list"));
    CHECK_RC(rc, 0, "list (top) rc=0");
    CHECK_HAS("categories", "list (top) propose 'categories'");
    CHECK_HAS("types",      "list (top) propose 'types'");
}

static void test_list_types(void) {
    int rc = capture_call(cli_doc_list, 2, ARGS("list", "types"));
    CHECK_RC(rc, 0, "list types rc=0");
    CHECK_HAS("Types primitifs", "list types affiche section primitifs");
    CHECK_HAS("chaîne",          "list types contient le primitif chaîne");
    CHECK_HAS("entier",          "list types contient le primitif entier");
    CHECK_HAS("Cadre",           "list types contient le composé Cadre");
    CHECK_HAS("Coin",            "list types contient Coin (post-refacto)");

    /* Cadre doit apparaître dans la liste des composés avec sa catégorie
     * sur la même ligne — détecte un éventuel doublon registry (qui se
     * verrait comme 2 lignes consécutives identiques). Le pattern précis
     * "  Cadre " + padding + "(Affichage)" caractérise l'entrée registry,
     * pas le mot Cadre cité dans d'autres descriptions. */
    CHECK(count_occurrences("(Affichage)") >= 2,
          "Coin et Cadre tous deux listés en catégorie Affichage");
}

static void test_list_types_json(void) {
    int rc = capture_call(cli_doc_list, 3, ARGS("list", "types", "--json"));
    CHECK_RC(rc, 0, "list types --json rc=0");
    CHECK_HAS("\"primitives\"",  "JSON contient bucket primitives");
    CHECK_HAS("\"types\"",       "JSON contient bucket types");
    CHECK_HAS("\"chaîne\"",      "JSON inclut chaîne avec accent");
    /* Schéma minimal : doit commencer par '{' */
    CHECK(g_buf[0] == '{', "JSON commence par {");
}

static void test_list_natives_cat_http(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "natives", "--cat", "HTTP"));
    CHECK_RC(rc, 0, "list natives --cat HTTP rc=0");
    CHECK_HAS("HTTPRequête", "filtre HTTP contient HTTPRequête");
    CHECK_HAS("HTTPEnvoie",  "filtre HTTP contient HTTPEnvoie");
}

static void test_list_natives_cat_bogus(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "natives", "--cat", "NeExistePas"));
    /* rc=0 (la commande n'est pas en erreur — juste 0 résultat), mais
     * on doit lister les catégories disponibles pour aider l'user. */
    CHECK_RC(rc, 0, "list natives --cat bogus rc=0");
    CHECK_HAS("Catégories disponibles", "catégorie inconnue suggère les valides");
    CHECK_HAS("HTTP",                   "liste de suggestion contient HTTP");
}

/*  — la categorie est la CLE du filtre, et cette cle est bilingue :
 * les deux formes resolvent vers la meme categorie du registre, l'affichage
 * rend celle de la langue courante. La resolution suit les invariants 1 et 2
 * (casse- et accent-insensible) comme partout ailleurs dans le langage. */
static void test_list_natives_cat_bilingue(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "natives", "--cat", "Arrays"));
    CHECK_RC(rc, 0, "list natives --cat Arrays rc=0");
    CHECK_HAS("catégorie Tableaux", "forme EN resout, en-tete FR");
    CHECK_HAS("TableauInfo",        "forme EN rend les natives de Tableaux");

    rc = capture_call(cli_doc_list, 6,
                      ARGS("list", "natives", "--cat", "Tableaux",
                           "--lang", "en"));
    CHECK_RC(rc, 0, "list natives --cat Tableaux --lang en rc=0");
    CHECK_HAS("category Arrays", "forme FR rend l'en-tete EN");
    CHECK_NO("Tableaux",         "aucune forme FR de categorie en --lang en");

    /* Casse + accents : « systeme » doit rendre « Système ». */
    rc = capture_call(cli_doc_list, 4,
                      ARGS("list", "natives", "--cat", "systeme"));
    CHECK_RC(rc, 0, "list natives --cat systeme rc=0");
    CHECK_HAS("catégorie Système", "resolution casse+accent insensible");
    CHECK_HAS("DuPressePapier",    "natives de Système rendues");

    /* Categorie scindee par langue : FR « File », EN « Queue ». */
    rc = capture_call(cli_doc_list, 4,
                      ARGS("list", "natives", "--cat", "Queue"));
    CHECK_RC(rc, 0, "list natives --cat Queue rc=0");
    CHECK_HAS("catégorie File", "forme EN Queue resout vers File");
    CHECK_HAS("Enfile",         "natives de File rendues");

    rc = capture_call(cli_doc_list, 6,
                      ARGS("list", "natives", "--cat", "File",
                           "--lang", "en"));
    CHECK_RC(rc, 0, "list natives --cat File --lang en rc=0");
    CHECK_HAS("category Queue", "forme FR File rend Queue en EN");
}

/*  — `--json` ne peut pas laisser confondre « categorie vide » et
 * « nom non reconnu » : un consommateur n'a aucun moyen de distinguer les
 * deux derriere un `count: 0`. */
static void test_list_natives_cat_json_unknown(void) {
    int rc = capture_call(cli_doc_list, 5,
                          ARGS("list", "natives", "--cat", "NeExistePas",
                               "--json"));
    CHECK_RC(rc, 0, "list natives --cat bogus --json rc=0");
    CHECK_HAS("\"unknown_category\": \"NeExistePas\"",
              "JSON nomme le nom non reconnu");
    CHECK_HAS("\"count\": 0", "JSON garde count=0");

    rc = capture_call(cli_doc_list, 5,
                      ARGS("list", "natives", "--cat", "HTTP", "--json"));
    CHECK_RC(rc, 0, "list natives --cat HTTP --json rc=0");
    CHECK_NO("\"unknown_category\"",
             "categorie reconnue : pas de champ unknown_category");
}

/*  — registry des limites compile-time (owl_limits.def). */
static void test_list_limits(void) {
    int rc = capture_call(cli_doc_list, 2, ARGS("list", "limits"));
    CHECK_RC(rc, 0, "list limits rc=0");
    CHECK_HAS("Limites compile-time", "list limits affiche le titre");
    CHECK_HAS("MAX_USER_TYPES",      "list limits contient MAX_USER_TYPES");
    CHECK_HAS("MAX_USER_PROC_PARAMS","list limits contient MAX_USER_PROC_PARAMS");
}

static void test_list_limits_json(void) {
    int rc = capture_call(cli_doc_list, 3, ARGS("list", "limits", "--json"));
    CHECK_RC(rc, 0, "list limits --json rc=0");
    CHECK_HAS("\"limits\"",            "JSON top key 'limits'");
    CHECK_HAS("\"MAX_USER_TYPES\"",    "JSON inclut MAX_USER_TYPES");
    CHECK_HAS("\"value\":",            "JSON inclut champ value");
    CHECK_HAS("\"category\":",         "JSON inclut champ category");
    CHECK(g_buf[0] == '{', "JSON commence par {");
}

static void test_explain_limit(void) {
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "MAX_USER_PROC_PARAMS"));
    CHECK_RC(rc, 0, "explain MAX_USER_PROC_PARAMS rc=0");
    CHECK_HAS("MAX_USER_PROC_PARAMS", "fiche affiche le nom");
    CHECK_HAS("Valeur : 64",          "fiche affiche la valeur");
    CHECK_HAS("Compiler/Procedures",  "fiche affiche la categorie");
}

static void test_explain_missing(void) {
    int rc = capture_call(cli_doc_explain, 1, ARGS("explain"));
    CHECK_RC(rc, 1, "explain sans symbole rc=1");
}

/*  — list constants --json embarque desc (mono-langue) + name_en. */
static void test_list_constants_json_desc(void) {
    int rc = capture_call(cli_doc_list, 3, ARGS("list", "constants", "--json"));
    CHECK_RC(rc, 0, "list constants --json rc=0");
    CHECK_HAS("\"desc\":",    "list constants JSON inclut desc");
    CHECK_HAS("\"name_en\":", "list constants JSON inclut name_en");
}

/*  — la desc suit --lang (mono-langue), pas de mix FR/EN. */
static void test_list_constants_lang_en(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "constants", "--lang", "en"));
    CHECK_RC(rc, 0, "list constants --lang en rc=0");
    CHECK_HAS("trim trailing spaces", "list constants --lang en : desc en anglais");
    CHECK_NO("supprime les espaces",  "list constants --lang en : pas de FR");
}

/*  — explain native --json : desc mono-langue, plus de desc_fr/desc_en
 * (le nom bilingue name_en reste). */
static void test_explain_native_mono_lang(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Gauche", "--json"));
    CHECK_RC(rc, 0, "explain Gauche --json rc=0");
    CHECK_HAS("\"desc\":",    "explain native JSON a desc");
    CHECK_HAS("\"name_en\":", "explain native JSON garde name_en");
    CHECK_NO("\"desc_fr\"",   "explain native JSON : plus de desc_fr");
    CHECK_NO("\"desc_en\"",   "explain native JSON : plus de desc_en");
}

/*  — le pre-filtre main.c consomme `--lang en` dans le global
 * diagnostics et le retire d'argv ; la couche doc doit lire ce global comme
 * defaut. In-process on ne peut pas rejouer le pre-filtre, on simule donc son
 * effet : global EN + aucun flag lang dans argv. Avant le fix, la desc sortait
 * en FR ("une seule fois") ; apres, en EN ("evaluated once"). */
static void test_explain_keyword_honors_global_lang(void) {
    int prev = owl_error_render_lang_is_en();
    owl_error_render_set_lang_en(1);
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "_TO_"));
    owl_error_render_set_lang_en(prev);  /* restore avant les autres tests */
    CHECK_RC(rc, 0, "explain _TO_ (global EN) rc=0");
    CHECK_HAS("evaluated once", "explain _TO_ : desc EN via global lang");
    CHECK_NO("une seule fois",  "explain _TO_ : pas de FR quand global EN");
}

/*  — quick win : les noms de champs d'un type composé suivent --lang
 * (OwlField.nameEN). Pas de CHECK_NO sur la forme FR : la prose FR des
 * scénarios (non traduite) contient légitimement "Méthode". */
static void test_explain_type_field_names_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "httprequete", "--lang", "en"));
    CHECK_RC(rc, 0, "explain httprequete --lang en rc=0");
    CHECK_HAS("Method",  "explain type --lang en : champ EN (Méthode->Method)");
    CHECK_HAS("Content", "explain type --lang en : champ EN (Contenu->Content)");
}

/*  — la ligne "voisins" de `owl explain` doit nommer les voisins
 * (types ET natives partageant la catégorie) par leur forme EN canon en
 * --lang en, avec repli sur canonical si aucun nameEN. Régression : les
 * noms restaient FR quelle que soit la langue. Coin partage sa catégorie
 * avec Police (->Font), Cadre (->Frame), Trait (->Stroke)… */
static void test_explain_neighbors_names_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "Coin", "--lang", "en"));
    CHECK_RC(rc, 0, "explain Coin --lang en rc=0");
    CHECK_HAS("Font",   "explain voisins --lang en : Police->Font");
    CHECK_HAS("Stroke", "explain voisins --lang en : Trait->Stroke");
    CHECK_NO("Police",  "explain voisins --lang en : plus de FR Police");
    CHECK_NO("Trait",   "explain voisins --lang en : plus de FR Trait");
}

/*  — pendant FR (défaut) : la ligne voisins reste en noms FR canon. */
static void test_explain_neighbors_names_fr(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Coin"));
    CHECK_RC(rc, 0, "explain Coin rc=0");
    CHECK_HAS("Police", "explain voisins FR : Police en clair");
    CHECK_HAS("Trait",  "explain voisins FR : Trait en clair");
}

/*  — le libellé de catégorie dans l'en-tête du détail `explain` suit
 * --lang (owl_category_display), au même titre que les chemins list/--cat.
 * Régression : `explain fExeDir --lang en` affichait "Fichiers" (FR) alors que
 * la desc était déjà en EN. Couvre native, type composé et constante. */
static void test_explain_detail_category_native_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "fExeDir", "--lang", "en"));
    CHECK_RC(rc, 0, "explain fExeDir --lang en rc=0");
    CHECK_HAS("Files",     "explain native --lang en : catégorie Fichiers->Files");
    CHECK_NO("Fichiers",   "explain native --lang en : plus de FR Fichiers");
}

/*  — en FR (défaut) la catégorie reste FR. */
static void test_explain_detail_category_native_fr(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "fExeDir"));
    CHECK_RC(rc, 0, "explain fExeDir rc=0");
    CHECK_HAS("Fichiers", "explain native FR : catégorie Fichiers en clair");
}

/*  — type composé : Vecteur (catégorie Vecteurs->Vectors). Pas de
 * CHECK_NO sur "Vecteurs" : la ligne "used by … family Vecteurs" nomme la
 * famille (self->category, surface distincte hors-scope) et reste FR. */
static void test_explain_detail_category_type_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "Vecteur", "--lang", "en"));
    CHECK_RC(rc, 0, "explain Vecteur --lang en rc=0");
    CHECK_HAS("Vectors",  "explain type --lang en : catégorie Vecteurs->Vectors");
}

/*  — constante : Rouge (catégorie Couleur->Color). Pas de CHECK_NO sur
 * "Couleur" : le nom du type propriétaire (`Couleur (type)`) est un titre de
 * sujet distinct, hors-scope de ce fix. */
static void test_explain_detail_category_const_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "Rouge", "--lang", "en"));
    CHECK_RC(rc, 0, "explain Rouge --lang en rc=0");
    CHECK_HAS("Color",   "explain const --lang en : catégorie Couleur->Color");
}

/*  — énumération : errInfo (catégorie Erreur->Error). Même famille de
 * défaut que native/type/const : le header détail affichait la catégorie brute
 * FR en --lang en. Pas de CHECK_NO large sur "Erreur" : la desc/les valeurs
 * peuvent porter le mot ; on cible le header via CHECK_HAS Error + CHECK_NO sur
 * la ligne header exacte. */
static void test_explain_detail_category_enum_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "errInfo", "--lang", "en"));
    CHECK_RC(rc, 0, "explain errInfo --lang en rc=0");
    CHECK_HAS("\n  Error\n", "explain enum --lang en : catégorie Erreur->Error (header)");
    CHECK_NO("\n  Erreur\n", "explain enum --lang en : plus de FR Erreur (header)");
}

static void test_explain_detail_category_enum_fr(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "errInfo"));
    CHECK_RC(rc, 0, "explain errInfo rc=0");
    CHECK_HAS("\n  Erreur\n", "explain enum FR : catégorie Erreur en clair (header)");
}

/*  — opérateur : `+` (catégorie Arithmétique->Arithmetic). Header détail
 * imprimait o->category brut. */
static void test_explain_detail_category_operator_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "+", "--lang", "en"));
    CHECK_RC(rc, 0, "explain + --lang en rc=0");
    CHECK_HAS("Arithmetic",   "explain opérateur --lang en : catégorie Arithmétique->Arithmetic");
    CHECK_NO("Arithmétique",  "explain opérateur --lang en : plus de FR Arithmétique");
}

/* Vérifie un équilibrage grossier du JSON capturé dans g_buf :
 * accolades/crochets balancés (hors chaînes), et aucune virgule
 * immédiatement suivie d'un '}' ou ']' (trailing comma = JSON invalide).
 * Smoke check suffisant pour attraper une régression d'ordre des printf. */
static int json_buf_is_balanced(void) {
    int depth_brace = 0, depth_brack = 0;
    int in_str = 0, esc = 0;
    char prev_significant = 0;
    for (const char* p = g_buf; *p; p++) {
        char c = *p;
        if (in_str) {
            if (esc) { esc = 0; }
            else if (c == '\\') { esc = 1; }
            else if (c == '"') { in_str = 0; }
            continue;
        }
        if (c == '"') { in_str = 1; prev_significant = c; continue; }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
        if ((c == '}' || c == ']') && prev_significant == ',') return 0;
        if (c == '{') depth_brace++;
        else if (c == '}') { depth_brace--; if (depth_brace < 0) return 0; }
        else if (c == '[') depth_brack++;
        else if (c == ']') { depth_brack--; if (depth_brack < 0) return 0; }
        prev_significant = c;
    }
    return depth_brace == 0 && depth_brack == 0 && !in_str;
}

/*  — explain native --json : bloc signatures (mono-signature).
 * Gauche(s : chaîne, n : entier) → chaîne : un seul élément, deux params
 * avec name/type/desc, return_type + return_desc. */
static void test_explain_native_json_signatures_mono(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Gauche", "--json"));
    CHECK_RC(rc, 0, "explain Gauche --json rc=0");
    CHECK_HAS("\"signatures\":",  "Gauche JSON a signatures");
    CHECK_HAS("\"params\":",      "Gauche JSON sig a params");
    CHECK_HAS("\"return_type\":", "Gauche JSON sig a return_type");
    CHECK_HAS("\"return_desc\":", "Gauche JSON sig a return_desc");
    /* valeur exacte : param 'n' typé entier, et un return_type chaîne. */
    CHECK_HAS("\"name\": \"n\"",       "Gauche JSON param n présent");
    CHECK_HAS("\"return_type\": \"chaîne\"", "Gauche JSON return_type=chaîne");
    /* une seule signature → un seul return_type. */
    CHECK(count_occurrences("\"return_type\":") == 1,
          "Gauche JSON : exactement 1 signature");
    CHECK(json_buf_is_balanced(), "Gauche JSON équilibré");
}

/*  — multi-signatures : TableauAjoute a 2 signatures. */
static void test_explain_native_json_signatures_multi(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "TableauAjoute", "--json"));
    CHECK_RC(rc, 0, "explain TableauAjoute --json rc=0");
    CHECK_HAS("\"signatures\":", "TableauAjoute JSON a signatures");
    CHECK(count_occurrences("\"return_type\":") == 2,
          "TableauAjoute JSON : 2 signatures");
    CHECK(json_buf_is_balanced(), "TableauAjoute JSON équilibré");
}

/*  — variadique : Trace (arity < 0) porte le flag variadic. */
static void test_explain_native_json_signatures_variadic(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Trace", "--json"));
    CHECK_RC(rc, 0, "explain Trace --json rc=0");
    CHECK_HAS("\"signatures\":",      "Trace JSON a signatures");
    CHECK_HAS("\"variadic\": true",   "Trace JSON sig variadic=true");
    CHECK(json_buf_is_balanced(), "Trace JSON équilibré");
}

/*  — native avec methodBindings (MockQuand → MockNative.Quand). */
static void test_explain_native_json_methods(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "MockQuand", "--json"));
    CHECK_RC(rc, 0, "explain MockQuand --json rc=0");
    CHECK_HAS("\"methods\":",   "MockQuand JSON a methods");
    CHECK_HAS("\"receiver\": \"MockNative\"", "MockQuand JSON method receiver");
    CHECK_HAS("\"name\": \"Quand\"",  "MockQuand JSON method name FR");
    CHECK_HAS("\"name\": \"When\"",   "MockQuand JSON method name EN");
    CHECK(json_buf_is_balanced(), "MockQuand JSON équilibré");
}

/*  — type avec méthodes fluent : explain MockNative --json
 * doit lister les natives fluides (MockQuand, MockAvec, …). */
static void test_explain_type_json_methods(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "MockNative", "--json"));
    CHECK_RC(rc, 0, "explain MockNative --json rc=0");
    CHECK_HAS("\"kind\": \"type\"", "MockNative est un type");
    CHECK_HAS("\"methods\":",       "MockNative JSON a methods");
    CHECK_HAS("\"native\": \"MockQuand\"",
              "MockNative JSON method pointe vers MockQuand");
    CHECK(json_buf_is_balanced(), "MockNative JSON équilibré");
}

/*  — méthode de type PURE (owl_register_method, sans native globale
 * correspondante). `explain Chat` doit résoudre la forme méthode et nommer le
 * receveur LLM, comme pour une native globale. */
static void test_explain_method_pure_mono(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Chat"));
    CHECK_RC(rc, 0, "explain Chat rc=0 (méthode pure résolue)");
    CHECK_HAS("Chat", "explain Chat cite la méthode");
    CHECK_HAS("LLM",  "explain Chat nomme le receveur LLM");
}

/*  — bilingue (invariant 3) : Génère (FR) et Generate (EN) résolvent
 * tous deux la même méthode LLM. */
static void test_explain_method_pure_bilingual(void) {
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "G\xc3\xa9n\xc3\xa8re"));
    CHECK_RC(rc, 0, "explain Génère rc=0");
    CHECK_HAS("LLM", "explain Génère nomme le receveur LLM");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Generate"));
    CHECK_RC(rc, 0, "explain Generate rc=0 (forme EN)");
    CHECK_HAS("LLM", "explain Generate nomme le receveur LLM");
}

/*  — homonyme multi-types : Charge existe sur LLM ET IAModèle. Le nom
 * nu liste les DEUX receveurs (désambiguïsation), pas juste le premier. */
static void test_explain_method_homonym(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Charge"));
    CHECK_RC(rc, 0, "explain Charge rc=0");
    CHECK_HAS("LLM",   "explain Charge liste le receveur LLM");
    CHECK_HAS("IAMod", "explain Charge liste le receveur IAModele");
}

/*  — forme qualifiée Type.Méthode : cible un seul receveur, sans
 * ambiguïté. `LLM.Charge` ne doit PAS afficher le receveur IAModèle. */
static void test_explain_method_qualified(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "LLM.Charge"));
    CHECK_RC(rc, 0, "explain LLM.Charge rc=0");
    CHECK_HAS("LLM",  "explain LLM.Charge cible le receveur LLM");
    CHECK_NO("IAMod", "explain LLM.Charge exclut IAModele (désambiguïsé)");
}

/*  — JSON : la forme méthode expose le receveur. */
static void test_explain_method_pure_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Chat", "--json"));
    CHECK_RC(rc, 0, "explain Chat --json rc=0");
    CHECK_HAS("\"forms\":", "explain Chat --json a forms");
    CHECK_HAS("\"receiver\": \"LLM\"", "explain Chat --json receiver=LLM");
    CHECK(json_buf_is_balanced(), "explain Chat --json équilibré");
}

/*  — non-régression : un type composé (MessageLLM) résout toujours,
 * l'ajout des méthodes ne détourne pas la résolution des types. */
static void test_explain_method_no_regression_type(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "MessageLLM"));
    CHECK_RC(rc, 0, "explain MessageLLM rc=0 (type inchangé)");
    CHECK_HAS("MessageLLM", "explain MessageLLM cite le type");
}

/*  — enum --json porte name_ascii. httpMéthodes est un enum
 * mono-bucket (pas d'homonyme type) → JSON document unique. */
static void test_explain_enum_json_name_ascii(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "httpMéthodes", "--json"));
    CHECK_RC(rc, 0, "explain httpMéthodes --json rc=0");
    CHECK_HAS("\"kind\": \"enum\"", "httpMéthodes est un enum");
    CHECK_HAS("\"name_ascii\": \"httpMethodes\"",
              "httpMéthodes JSON a name_ascii=httpMethodes");
    CHECK(json_buf_is_balanced(), "httpMéthodes JSON équilibré");
}

/*  — reverse linkage "Utilisé par" sur les fiches enum/const.
 *
 * Un enum réellement contraignant un champ de type (httpMéthodes ∈
 * httpRequête.Méthode) doit lister ses utilisateurs. httpMéthodes est
 * utilisé par le type httpRequête (champ Méthode, allowedBase). */
static void test_explain_enum_used_by_text(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "httpMéthodes"));
    CHECK_RC(rc, 0, "explain httpMéthodes rc=0");
    CHECK_HAS("Utilisé par", "httpMéthodes a une section Utilisé par");
    CHECK_HAS("httpRequête", "httpMéthodes utilisé par le type httpRequête");
}

static void test_explain_enum_used_by_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "httpMéthodes", "--json"));
    CHECK_RC(rc, 0, "explain httpMéthodes --json rc=0");
    CHECK_HAS("\"used_by\"", "JSON httpMéthodes contient used_by");
    CHECK_HAS("httpRequête", "JSON used_by liste httpRequête");
    CHECK(json_buf_is_balanced(), "httpMéthodes used_by JSON équilibré");
}

/* Un enum non utilisé par aucun param native ni champ de type ne doit
 * PAS afficher de section fantôme. TypeBouton est consommé par une
 * propriété DSL (catalogue widgets), pas par owl_registry params/fields
 * — hors périmètre du reverse lookup. */
static void test_explain_enum_unused_no_section(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "TypeBouton"));
    CHECK_RC(rc, 0, "explain TypeBouton rc=0");
    CHECK_NO("Utilisé par", "TypeBouton (non utilisé) n'affiche pas de section fantôme");
}

static void test_explain_enum_unused_no_section_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "TypeBouton", "--json"));
    CHECK_RC(rc, 0, "explain TypeBouton --json rc=0");
    CHECK_NO("\"used_by\"", "TypeBouton JSON sans champ used_by fantôme");
}

/* Une constante hérite du reverse lookup de son enum d'appartenance.
 * httpGet ∈ httpMéthodes → utilisé par httpRequête. */
static void test_explain_const_used_by_text(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "httpGet"));
    CHECK_RC(rc, 0, "explain httpGet rc=0");
    CHECK_HAS("Utilisé par", "httpGet a une section Utilisé par (via son enum)");
    CHECK_HAS("httpRequête", "httpGet utilisé par le type httpRequête");
}

static void test_explain_const_used_by_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "httpGet", "--json"));
    CHECK_RC(rc, 0, "explain httpGet --json rc=0");
    CHECK_HAS("\"used_by\"", "JSON httpGet contient used_by");
    CHECK_HAS("httpRequête", "JSON used_by httpGet liste httpRequête");
    CHECK(json_buf_is_balanced(), "httpGet used_by JSON équilibré");
}

/*  — reverse linkage "Utilisé par" sur les fiches de TYPE composé.
 *
 * Un type composé étroit (peu de consommateurs) liste les natives/types qui
 * le consomment. sqlRequête est consommé par SQLEnvoie (param `req`). Sous
 * le cap de 6 → liste nominative, pas de renvoi famille. */
static void test_explain_type_used_by_narrow_text(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "sqlRequête"));
    CHECK_RC(rc, 0, "explain sqlRequête rc=0");
    CHECK_HAS("utilisé par", "sqlRequête a une ligne 'utilisé par'");
    CHECK_HAS("SQLEnvoie",   "sqlRequête utilisé par la native SQLEnvoie");
    CHECK_HAS("(native)",    "SQLEnvoie annoncé comme native");
}

static void test_explain_type_used_by_narrow_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "sqlRequête", "--json"));
    CHECK_RC(rc, 0, "explain sqlRequête --json rc=0");
    CHECK_HAS("\"used_by\"", "JSON sqlRequête contient used_by");
    CHECK_HAS("\"name\": \"SQLEnvoie\"", "used_by liste SQLEnvoie");
    CHECK_HAS("\"kind\": \"native\"", "SQLEnvoie kind native");
    /* Étroit → pas de renvoi famille. */
    CHECK_HAS("\"used_by_family\": null", "sqlRequête étroit : used_by_family null");
    CHECK(json_buf_is_balanced(), "sqlRequête used_by JSON équilibré");
}

/* Un type composé large (> 6 consommateurs) ne liste PAS ses consommateurs :
 * il renvoie vers la famille = sa catégorie. Cadre (catégorie Affichage) est
 * consommé par > 6 types de style → déclenche le renvoi famille. */
static void test_explain_type_used_by_large_text(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Cadre"));
    CHECK_RC(rc, 0, "explain Cadre rc=0");
    CHECK_HAS("utilisé par",      "Cadre a une ligne 'utilisé par'");
    CHECK_HAS("fonctions",        "Cadre (large) annonce N fonctions");
    CHECK_HAS("famille Affichage","Cadre renvoie vers la famille Affichage");
    CHECK_HAS("owl list Affichage","Cadre renvoie owl list Affichage");
}

static void test_explain_type_used_by_large_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Cadre", "--json"));
    CHECK_RC(rc, 0, "explain Cadre --json rc=0");
    CHECK_HAS("\"used_by\": []", "Cadre large : used_by vide");
    CHECK_HAS("\"used_by_family\": \"Affichage\"", "Cadre used_by_family Affichage");
    /* used_by_total > 6 : la valeur exacte dérive avec le registre, on
     * vérifie seulement qu'elle n'est ni <= cap (6) ni nulle. */
    CHECK_NO("\"used_by_total\": 6", "Cadre used_by_total > 6 (pas 6)");
    CHECK_NO("\"used_by_total\": 0", "Cadre used_by_total > 6 (pas 0)");
    CHECK(json_buf_is_balanced(), "Cadre used_by JSON équilibré");
}

/* Les primitives sont EXCLUES du mécanisme : chaîne n'a pas de used_by. */
static void test_explain_primitive_no_used_by_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "chaîne", "--json"));
    CHECK_RC(rc, 0, "explain chaîne --json rc=0");
    CHECK_NO("\"used_by\"", "chaîne (primitive) : pas de champ used_by");
}

/*  — une constante résout aussi par sa forme EN canonique dans
 * explain (invariant bilinguisme FR⇄EN, règle 6 §3). La forme FR résolvait
 * déjà ; la forme EN (name_en) était le trou de lookup. */
static void test_explain_const_en_form(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "languageFrench"));
    CHECK_RC(rc, 0, "explain languageFrench (forme EN) rc=0");
    CHECK_HAS("langueFrançais", "explain languageFrench résout la fiche de la constante");
    CHECK_NO("inconnu", "explain languageFrench ne répond pas 'inconnu'");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "languageEnglish"));
    CHECK_RC(rc, 0, "explain languageEnglish (forme EN) rc=0");
    CHECK_HAS("langueAnglais", "explain languageEnglish résout la fiche de la constante");

    /* Non-régression : la forme FR continue de résoudre. */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "langueFrançais"));
    CHECK_RC(rc, 0, "explain langueFrançais (forme FR) rc=0");
    CHECK_HAS("langueFrançais", "explain langueFrançais résout toujours");
}

/*  — search remonte une constante par sa forme EN, comme il le
 * fait déjà pour les natives (name_en dans le matcher). */
static void test_search_const_en_form(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "languageFrench"));
    CHECK_RC(rc, 0, "search languageFrench rc=0");
    CHECK_HAS("[const]",        "search languageFrench tague la constante (liste plate 1302)");
    CHECK_HAS("langueFrançais", "search languageFrench liste la constante");
}

/*  (support #216) — tri de pertinence : un nom qui COMMENCE par le
 * terme (palier 3) passe avant un nom qui ne le CONTIENT qu'au milieu (palier
 * 2). Cas FR "chaine" : ChaîneVersEntier (préfixe) avant BufferVersChaîne
 * (sous-chaîne). Verifie aussi le fix #2 : "chaine" (sans accent) matche bien
 * les entrees "chaîne". */
static void test_search_relevance_ranking(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "chaine"));
    CHECK_RC(rc, 0, "search chaine rc=0");
    /*  : liste UNIQUE triee par pertinence pure. Le type primitif
     * `chaîne` (match EXACT, palier 4) doit sortir AVANT les natives ChaîneVers*
     * (prefixe, palier 3), qui sortent avant BufferVersChaîne (sous-chaine, 2). */
    const char* primTag   = strstr(g_buf, "[type primitif]");            /* ligne du primitif chaîne */
    const char* prefixHit = strstr(g_buf, "Cha\xc3\xae" "neVersEntier");  /* ChaîneVersEntier */
    const char* substrHit = strstr(g_buf, "BufferVersCha\xc3\xae" "ne");  /* BufferVersChaîne */
    CHECK(primTag != NULL,   "1302 primitif chaine present (accent-insensible)");
    CHECK(prefixHit != NULL, "1302 ChaineVersEntier present");
    CHECK(substrHit != NULL, "1302 BufferVersChaine present");
    CHECK(primTag && prefixHit && primTag < prefixHit,
          "1302 match exact (primitif) classe avant prefixe");
    CHECK(prefixHit && substrHit && prefixHit < substrHit,
          "1302 prefixe classe avant sous-chaine");
}

static void test_explain_unknown(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "ZzZz_inconnu"));
    CHECK_RC(rc, 2, "explain symbole inconnu rc=2");
}

static void test_explain_bad_flag(void) {
    int rc = capture_call(cli_doc_explain, 3,
                          ARGS("explain", "Trace", "--xxx"));
    CHECK_RC(rc, 64, "flag inconnu rc=64");
}

/*  — famille errors exposée via owl explain.
 *
 * Le registry des codes d'erreur OWL (owl_errors.def, 710 codes au moment
 * où ce test est écrit) n'était pas câblé au dispatcher CLI/MCP : volontaire
 * pendant la fondation registry erreurs (-0093). MVP minimaliste :
 * `owl explain E_XXX` rend name + category + template FR (par défaut) ou EN
 * (sur `--lang en`). Pas de `owl list errors` ni de search étendu — les
 * erreurs sont rencontrées, pas explorées. */
static void test_explain_error_fr(void) {
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "E_LEX_UNTERMINATED_STRING"));
    CHECK_RC(rc, 0, "explain E_LEX_UNTERMINATED_STRING rc=0");
    CHECK_HAS("E_LEX_UNTERMINATED_STRING", "fiche affiche le name");
    CHECK_HAS("lex",                       "fiche affiche la categorie");
    CHECK_HAS("Chaîne non terminée",       "fiche affiche template FR par defaut");
}

static void test_explain_error_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "E_LEX_UNTERMINATED_STRING",
                               "--lang", "en"));
    CHECK_RC(rc, 0, "explain E_LEX_UNTERMINATED_STRING --lang en rc=0");
    CHECK_HAS("E_LEX_UNTERMINATED_STRING", "fiche EN affiche le name");
    CHECK_HAS("Unterminated string",       "fiche EN rend template EN");
    CHECK_NO("Chaîne non",                 "fiche EN n'a pas le FR");
}

static void test_explain_error_case_insensitive(void) {
    /* owl_registry_error_find est case-insensitive — `explain` doit
     * en bénéficier sans cast manuel à uppercase. */
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "e_lex_unterminated_string"));
    CHECK_RC(rc, 0, "explain e_lex_... (lowercase) rc=0");
    CHECK_HAS("E_LEX_UNTERMINATED_STRING", "lookup case-insensitive marche");
}

static void test_explain_error_unknown(void) {
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "E_FOOBAR_QUI_NEXISTE_PAS"));
    CHECK_RC(rc, 2, "explain E_FOOBAR... inconnu rc=2");
}

static void test_explain_error_cat_filter(void) {
    /* `--cat errors` doit accepter et restreindre au bucket errors. */
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "E_LEX_UNTERMINATED_STRING",
                               "--cat", "errors"));
    CHECK_RC(rc, 0, "--cat errors rc=0");
    CHECK_HAS("E_LEX_UNTERMINATED_STRING", "filtre errors rend la fiche");
}

static void test_explain_error_json(void) {
    int rc = capture_call(cli_doc_explain, 3,
                          ARGS("explain", "E_LEX_UNTERMINATED_STRING",
                               "--json"));
    CHECK_RC(rc, 0, "explain E_LEX_... --json rc=0");
    CHECK_HAS("\"kind\": \"error\"",         "JSON : kind=error");
    CHECK_HAS("\"name\": \"E_LEX_UNTERMINATED_STRING\"",
              "JSON : name canonique");
    CHECK_HAS("\"category\": \"lex\"",       "JSON : category");
    CHECK_HAS("\"template\":",               "JSON : template present");
}

static void test_explain_cat_bogus(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "Trace", "--cat", "bogus"));
    CHECK_RC(rc, 64, "--cat bogus rc=64");
}

static void test_explain_native(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Trace"));
    CHECK_RC(rc, 0, "explain Trace rc=0");
    CHECK_HAS("Trace",      "explain Trace contient le nom");
    CHECK_HAS("Signatures", "explain Trace contient une section Signatures");
    CHECK_NO("Trace / Trace",
             "explain Trace évite le doublon canonical/nameEN");
}

/*  (support #217) — un migrant peut attendre des UUID 256 bits.
 * La fiche `explain UUID` doit répondre sans ambiguïté : taille FIXE 128 bits,
 * PAS de variante 256 bits (décision , break-net). Garde de doc. */
static void test_explain_uuid_128bit_only(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "UUID"));
    CHECK_RC(rc, 0, "explain UUID rc=0");
    CHECK_HAS("128 bits", "1298 UUID : taille 128 bits énoncée");
    CHECK_HAS("256 bits", "1298 UUID : question 256 bits adressée (aucune variante)");
}

static void test_explain_primitive(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "chaîne"));
    CHECK_RC(rc, 0, "explain chaîne rc=0 (UTF-8 lookup)");
    CHECK_HAS("Type primitif", "explain chaîne marqué primitif");
    CHECK_HAS("UTF-8",         "explain chaîne mentionne UTF-8");
}

static void test_explain_primitive_ascii(void) {
    /* Accent-insensible : `chaine` doit matcher `chaîne`. */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "chaine"));
    CHECK_RC(rc, 0, "explain chaine (ASCII) rc=0");
    CHECK_HAS("Type primitif", "lookup accent-insensible OK");
}

static void test_explain_multi_bucket(void) {
    /* `httpRequête` désigne à la fois un type composé et la fonction
     * native — invariant règle 6.5 (cf. CLAUDE.md). On doit voir les
     * DEUX sections, séparées par une étiquette. */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "httpRequête"));
    CHECK_RC(rc, 0, "explain httpRequête multi-bucket rc=0");
    CHECK_HAS("[Type composé]",   "multi-bucket : section type présente");
    CHECK_HAS("[Fonction native]","multi-bucket : section fonction présente");
}

static void test_explain_cat_filter(void) {
    /* Avec --cat natives, le bucket type est exclu — donc pas de section
     * '[Type composé]' (qui n'est imprimée que quand >1 bucket matche
     * de toute façon, mais surtout le contenu type ne doit pas être là). */
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "httpRequête", "--cat", "natives"));
    CHECK_RC(rc, 0, "explain --cat natives rc=0");
    CHECK_HAS("HTTPRequête", "filtre natives garde la fonction");
    CHECK_NO("[Type composé]",
             "filtre natives n'inclut pas le bucket type");
}

static void test_explain_cat_all(void) {
    /* --cat all doit être équivalent à pas de --cat (régression sur le
     * bug BUCKET_ALL=0 qui faisait planter en erreur 64). */
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "httpRequête", "--cat", "all"));
    CHECK_RC(rc, 0, "--cat all rc=0 (pas 64)");
    CHECK_HAS("[Type composé]",   "--cat all inclut le bucket type");
    CHECK_HAS("[Fonction native]","--cat all inclut le bucket fonction");
}

static void test_explain_type_props(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Cadre"));
    CHECK_RC(rc, 0, "explain Cadre rc=0");
    CHECK_HAS("Propriétés", "explain Cadre liste ses propriétés");
    CHECK_HAS("Couleur",    "Cadre.Couleur affiché");
    CHECK_HAS("Trait",      "Cadre.Trait affiché");
}

static void test_explain_signatures(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "ChaineDecoupe"));
    CHECK_RC(rc, 0, "explain ChaineDecoupe rc=0");
    CHECK_HAS("Signatures", "signatures présentes");
    CHECK_HAS("→",          "flèche de retour rendue");
    CHECK_HAS("tableau",    "type de retour 'tableau' présent");
}

static void test_explain_scenario_bullet(void) {
    /* Quand un type a des scénarios canoniques, le bullet '►' marque
     * chaque procédure publique. Vérifie qu'au moins un est rendu. */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "HTTPRequête"));
    CHECK_RC(rc, 0, "explain HTTPRequête rc=0");
    CHECK_HAS("►",                       "bullet de scénario présent");
    CHECK_HAS("scenario_creation",       "nom de procédure rendu");
}

static void test_examples(void) {
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples", "HTTPRequête"));
    CHECK_RC(rc, 0, "examples rc=0");
    CHECK_HAS("scenario_",       "examples liste des scénarios");
    /* Pas de doublon de header 'Exemples' (régression sur le fix
     * découvert en auto-test). */
    CHECK(count_occurrences("Exemples\n") <= 1,
          "examples n'a pas de doublon de header");
}

static void test_examples_missing(void) {
    int rc = capture_call(cli_doc_examples, 1, ARGS("examples"));
    CHECK_RC(rc, 1, "examples sans symbole rc=1");
}

/*  — la fiche Table doit renvoyer explicitement vers Colonne :
 * l'utilisateur qui consulte Table ne trouvait rien sur la gestion des
 * colonnes (support #308). Le renvoi cite la commande d'exploration. */
static void test_explain_table_renvoi_colonne(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Table"));
    CHECK_RC(rc, 0, "explain Table rc=0");
    CHECK_HAS("owl explain Colonne",
              "fiche Table renvoie vers owl explain Colonne");
}

static void test_explain_table_renvoi_colonne_en(void) {
    int rc = capture_call(cli_doc_explain, 4,
                          ARGS("explain", "Table", "--lang", "en"));
    CHECK_RC(rc, 0, "explain Table --lang en rc=0");
    CHECK_HAS("owl explain Column",
              "fiche Table (EN) renvoie vers owl explain Column");
}

/*  — un scénario public illustre la déclaration Table + colonnes,
 * catalogué à la fois sous Table et sous Colonne. */
static void test_examples_table_et_colonne(void) {
    int rc = capture_call(cli_doc_examples, 2, ARGS("examples", "Table"));
    CHECK_RC(rc, 0, "examples Table rc=0");
    CHECK_HAS("scenario_", "examples Table liste au moins un scénario");

    int rc2 = capture_call(cli_doc_examples, 2, ARGS("examples", "Colonne"));
    CHECK_RC(rc2, 0, "examples Colonne rc=0");
    CHECK_HAS("scenario_", "examples Colonne liste au moins un scénario");
}

/*  — le corps des exemples CLI/MCP ne doit plus contenir le
 * wrapper `PROCEDURE PUBLIC <nom>() ... FIN`. Le titre (► nom) et la
 * description restent émis séparément. Le code interne est dé-indenté
 * d'un niveau (tab ou 1-4 espaces). */
static void test_examples_strip_wrapper_texte(void) {
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples", "DateVersEntier"));
    CHECK_RC(rc, 0, "examples DateVersEntier rc=0");
    /* Le bullet de titre reste. */
    CHECK_HAS("► scenario_conversion_date_historique",
              "titre du scénario présent");
    /* Le wrapper a disparu du corps. */
    CHECK_NO("PROCEDURE PUBLIC scenario_conversion_date_historique",
             "ligne PROCEDURE PUBLIC retirée du corps");
    /* Le FIN terminal du wrapper a disparu : avant strip le corps
     * finissait par "    FIN\n". Le dé-indent ramène le contenu interne
     * à 4 espaces d'indent (le préfixe d'affichage), donc une ligne
     * "    FIN\n" isolée ne doit plus exister. */
    CHECK_NO("\n    FIN\n", "FIN terminal du wrapper retiré");
    /* Le code interne survit, dé-indenté : la 1ère ligne de contenu
     * était indentée d'un tab, qui est retiré. Reste le préfixe "    "
     * d'affichage + le commentaire. */
    CHECK_HAS("    // DateVersEntier accepte Date",
              "1er commentaire interne présent, dé-indenté");
    CHECK_HAS("d1 est une date", "déclaration interne présente");
    CHECK_HAS("DateVersEntier(d1)", "appel interne présent");
}

/*  — un FIN interne (bloc SI/POUR) ne doit PAS être retiré,
 * seul le FIN terminal du wrapper l'est. Le scénario
 * `scenario_element_absent` de Cherche contient un `POUR ... FIN`
 * interne SUIVI d'autres instructions, puis le FIN terminal du
 * wrapper. Après strip : le FIN du POUR survit, le FIN du wrapper non. */
static void test_examples_strip_preserve_fin_interne(void) {
    int rc = capture_call(cli_doc_examples, 2, ARGS("examples", "Cherche"));
    CHECK_RC(rc, 0, "examples Cherche rc=0");
    CHECK_NO("PROCEDURE PUBLIC scenario_element_absent",
             "wrapper PROCEDURE du scénario absent retiré");
    /* Le POUR interne survit avec son FIN. Le contenu du bloc est
     * dé-indenté d'un niveau : "\tPOUR" → "POUR" (préfixe affichage). */
    CHECK_HAS("POUR i = 1", "boucle POUR interne présente");
    /* Le FIN du POUR est indenté plus profond que le FIN du wrapper :
     * dans le body brut, le FIN du POUR est précédé de 1 tab (donc rendu
     * "    " préfixe seul après dé-indent — mais c'était \t\tFIN au
     * départ donc reste \tFIN après retrait d'un tab → "    \tFIN" ?).
     * Plus robuste : on vérifie qu'un FIN survit (celui du POUR) ET
     * que l'instruction qui SUIT le bloc (position est un entier...)
     * est présente — preuve que seul le FIN terminal a été retiré. */
    CHECK_HAS("position est un entier = Cherche(nombres, 99)",
              "instruction APRÈS le bloc POUR présente (FIN interne préservé)");
    CHECK_HAS("Élément absent", "assertion finale du scénario présente");
}

/*  — le champ `body` du JSON (consommé par le MCP owl_examples)
 * ne contient plus le wrapper, et le code est dé-indenté. */
static void test_examples_strip_wrapper_json(void) {
    int rc = capture_call(cli_doc_examples, 3,
                          ARGS("examples", "DateVersEntier", "--json"));
    CHECK_RC(rc, 0, "examples DateVersEntier --json rc=0");
    CHECK_HAS("\"body\":", "JSON a un champ body");
    /* Plus de wrapper dans body. */
    CHECK_NO("PROCEDURE PUBLIC scenario_conversion_date_historique",
             "JSON body : PROCEDURE PUBLIC retirée");
    /* Le FIN terminal était précédé d'un tab : "\\tFIN" en JSON échappé.
     * Après strip il ne doit plus y avoir de ligne FIN du wrapper.
     * Le dé-indent retire le tab de tête de chaque ligne interne, donc
     * le 1er commentaire commence directement par "//" après "\n". */
    CHECK_HAS("// DateVersEntier accepte Date",
              "JSON body : commentaire interne présent");
    /* Dé-indent : la ligne de contenu n'est plus préfixée d'un \t.
     * Le body commence directement par le commentaire (pas de \t ni
     * de PROCEDURE). On vérifie l'ouverture du body sur le commentaire. */
    CHECK_HAS("\"body\": \"// DateVersEntier",
              "JSON body débute sur le code interne dé-indenté");
    CHECK(json_buf_is_balanced(), "examples --json équilibré");
}

/*  — la reconnaissance de l'enveloppe de scénario doit accepter le
 * CANON des mots-clés, pas seulement la graphie littérale d'origine.
 * `PROCEDURE PUBLIQUE scenario_x()` : `PUBLIQUE` n'était pas reconnu comme
 * modificateur, donc le nom extrait était « PUBLIQUE » et le titre rendu
 * `► PUBLIQUE` au lieu de `► scenario_x`. Verrou sur le canon FR féminin
 * (919 scénarios du corpus l'utilisent). */
static void test_examples_titre_visibilite_canon(void) {
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples", "EnModeTestUnitaire"));
    CHECK_RC(rc, 0, "examples EnModeTestUnitaire rc=0");
    CHECK_HAS("► scenario_detection_contexte_test_runner",
              "titre du scénario PUBLIQUE rendu (pas le mot-clé)");
    CHECK_HAS("► scenario_pattern_adaptation_comportement",
              "2e titre PUBLIQUE rendu");
    CHECK_HAS("► scenario_stabilite_valeur_runtime",
              "3e titre PUBLIQUE rendu");
    /* Aucun titre ne doit être un mot-clé de visibilité, quelle que soit
     * la graphie. */
    CHECK_NO("► PUBLIQUE", "PUBLIQUE n'est jamais rendu comme titre");
    CHECK_NO("► PUBLIC",   "PUBLIC n'est jamais rendu comme titre");
    /* Les scénarios PRIVÉE restent hors vitrine (le modificateur accentué
     * doit rester compris comme visibilité restreinte). */
    CHECK_NO("scenario_edge_alias_fr_en",
             "scénario PRIVÉE toujours exclu de la vitrine");
}

/*  — le nom déclaré était extrait en ASCII pur : un identifiant
 * accentué était tronqué au 1er accent. `PROCEDURE aprèsChaqueTest()`
 * donnait le nom « apr », qui n'est plus reconnu comme fixture — la
 * fixture fuyait donc dans la vitrine sous le titre `► apr`. */
static void test_examples_fixture_nom_accentue(void) {
    int rc = capture_call(cli_doc_examples, 2, ARGS("examples", "Confirmer"));
    CHECK_RC(rc, 0, "examples Confirmer rc=0");
    CHECK_NO("► apr", "fixture aprèsChaqueTest non tronquée en titre");
    CHECK_NO("► aprèsChaqueTest",
             "fixture accentuée reconnue comme fixture, hors vitrine");
    CHECK_HAS("► scenario_appel_simple",
              "les vrais scénarios restent rendus");
}

/*  — un `@sujet` composite `Symbole (précision)` doit rester
 * découvrable par `owl examples <Symbole>`. Les scénarios 2D du Tenseur
 * portent `Tenseur.ProduitMatriciel (Matrice × Vecteur)` /
 * `(Matrice × Matrice)` ; sans match sur le préfixe, ils restaient
 * invisibles alors que les scénarios 1D (`@sujet` exact) sortaient. */
static void test_examples_sujet_composite_prefixe(void) {
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples", "Tenseur.ProduitMatriciel"));
    CHECK_RC(rc, 0, "examples Tenseur.ProduitMatriciel rc=0");
    /* Non-régression : le scénario 1D (@sujet exact) reste rendu. */
    CHECK_HAS("► scenario_produit_ligne_colonne_forward",
              "scénario 1D (sujet exact) toujours visible");
    /* Le fix : les scénarios 2D (@sujet composite) apparaissent. */
    CHECK_HAS("► scenario_forward_matrice_non_carree_2x3",
              "scénario 2D Matrice × Vecteur découvert via préfixe");
    CHECK_HAS("► scenario_forward_matmul_2x3_3x2",
              "scénario 2D Matrice × Matrice découvert via préfixe");
}

/*  — balayage exhaustif des graphies reconnues par la logique de
 * retrait d'enveloppe. Le corpus embarqué ne contient qu'une partie des
 * graphies légales (aucun scénario n'est écrit `PROCÉDURE PROTÉGÉE`, aucun
 * ne finit par `END`) : on sonde donc directement les deux fonctions qui
 * décident, pour qu'aucune forme canon ne puisse redevenir muette.
 *
 * Référence des graphies légales : la table `keywords` de scanner.c (les
 * mots-clés y sont accent-strippés, donc toute variante accentuée /
 * de casse est légale) et fmt_visibility_inline_canon() de formatter.c
 * (accord féminin imposé sur une déclaration de méthode). */
extern int  cli_doc_scenario_parse_decl(const char* line, long len,
                                        char* name_out, size_t name_size,
                                        int* is_private_out);
extern void cli_doc_scenario_strip_wrapper(const char* body_start, long body_len,
                                           const char** out_start, long* out_len);

static void check_decl(const char* line, const char* want_name,
                       int want_restricted, const char* label) {
    char name[128];
    int  restricted = -1;
    int  ok = cli_doc_scenario_parse_decl(line, (long)strlen(line),
                                          name, sizeof name, &restricted);
    int good = ok && strcmp(name, want_name) == 0 && restricted == want_restricted;
    if (!good) {
        fprintf(stderr, "    [%s] attendu name='%s' restreint=%d | obtenu "
                        "name='%s' restreint=%d\n",
                line, want_name, want_restricted,
                ok ? name : "<refusé>", restricted);
    }
    CHECK(good, label);
}

static void test_scenario_decl_balayage_graphies(void) {
    /* --- Mot-clé d'introduction, toutes graphies --- */
    check_decl("PROCEDURE scenario_x()", "scenario_x", 0,
               "intro PROCEDURE");
    check_decl("PROC\xc3\x89""DURE scenario_x()", "scenario_x", 0,
               "intro PROCÉDURE (accentué)");
    check_decl("procedure scenario_x()", "scenario_x", 0,
               "intro procedure (minuscules)");
    check_decl("Proc\xc3\xa9""dure scenario_x()", "scenario_x", 0,
               "intro Procédure (casse mixte accentuée)");
    check_decl("FONCTION scenario_x() : entier", "scenario_x", 0,
               "intro FONCTION");
    check_decl("FUNCTION scenario_x() : integer", "scenario_x", 0,
               "intro FUNCTION (canon EN)");
    check_decl("\tPROCEDURE scenario_x()", "scenario_x", 0,
               "intro indentée (tabulation)");

    /* --- Visibilité : les DEUX graphies rendent le même nom --- */
    check_decl("PROCEDURE PUBLIC scenario_x()",   "scenario_x", 0,
               "PUBLIC (forme littérale historique)");
    check_decl("PROCEDURE PUBLIQUE scenario_x()", "scenario_x", 0,
               "PUBLIQUE (canon inline FR)");
    check_decl("PROCEDURE publique scenario_x()", "scenario_x", 0,
               "publique (minuscules)");
    check_decl("PROCEDURE PRIVE scenario_x()",             "scenario_x", 1,
               "PRIVE");
    check_decl("PROCEDURE PRIV\xc3\x89 scenario_x()",       "scenario_x", 1,
               "PRIVÉ (canon standalone FR)");
    check_decl("PROCEDURE PRIVEE scenario_x()",            "scenario_x", 1,
               "PRIVEE");
    check_decl("PROCEDURE PRIV\xc3\x89""E scenario_x()",    "scenario_x", 1,
               "PRIVÉE (canon inline FR)");
    check_decl("PROCEDURE priv\xc3\xa9""e scenario_x()",    "scenario_x", 1,
               "privée (minuscules accentuées)");
    check_decl("PROCEDURE PRIVATE scenario_x()",           "scenario_x", 1,
               "PRIVATE (canon EN)");
    check_decl("PROCEDURE PROTEGE scenario_x()",           "scenario_x", 1,
               "PROTEGE");
    check_decl("PROCEDURE PROT\xc3\x89G\xc3\x89 scenario_x()", "scenario_x", 1,
               "PROTÉGÉ (canon standalone FR)");
    check_decl("PROCEDURE PROT\xc3\x89G\xc3\x89""E scenario_x()", "scenario_x", 1,
               "PROTÉGÉE (canon inline FR)");
    check_decl("PROCEDURE PROTECTED scenario_x()",         "scenario_x", 1,
               "PROTECTED (canon EN)");

    /* --- Modificateurs non-visibilité de la grammaire canon --- */
    check_decl("PROCEDURE GLOBALE scenario_x()",   "scenario_x", 0, "GLOBALE");
    check_decl("PROCEDURE ABSTRAITE scenario_x()", "scenario_x", 0, "ABSTRAITE");
    check_decl("PROCEDURE ABSTRACT scenario_x()",  "scenario_x", 0,
               "ABSTRACT (canon EN)");
    check_decl("PROCEDURE PUBLIQUE GLOBALE scenario_x()", "scenario_x", 0,
               "cumul PUBLIQUE + GLOBALE");
    check_decl("FUNCTION PRIV\xc3\x89""E en_square(n is an integer) : integer",
               "en_square", 1, "FUNCTION + PRIVÉE (mix EN/FR du corpus)");

    /* --- Nom déclaré accentué : plus de troncature au 1er accent --- */
    check_decl("PROCEDURE apr\xc3\xa8""sChaqueTest()",
               "apr\xc3\xa8""sChaqueTest", 0, "nom accentué préservé");
    check_decl("PROCEDURE PUBLIQUE sc\xc3\xa9""nario_\xc3\xa9""tendu()",
               "sc\xc3\xa9""nario_\xc3\xa9""tendu", 0,
               "nom accentué après modificateur");

    /* --- Refus : ce qui n'est pas une déclaration --- */
    char name[128];
    int  restricted = 0;
    CHECK(cli_doc_scenario_parse_decl("PROCEDUREX scenario_x()", 22,
                                      name, sizeof name, &restricted) == 0,
          "PROCEDUREX n'est pas une déclaration (frontière de mot)");
    CHECK(cli_doc_scenario_parse_decl("// PROCEDURE scenario_x()", 25,
                                      name, sizeof name, &restricted) == 0,
          "ligne de commentaire refusée");
    CHECK(cli_doc_scenario_parse_decl("PROCEDURE", 9,
                                      name, sizeof name, &restricted) == 0,
          "mot-clé sans nom refusé");
    CHECK(cli_doc_scenario_parse_decl("PROCEDURE PUBLIQUE", 18,
                                      name, sizeof name, &restricted) == 0,
          "mot-clé + modificateur sans nom refusé");
    CHECK(cli_doc_scenario_parse_decl("appelProcedure(1)", 17,
                                      name, sizeof name, &restricted) == 0,
          "appel de fonction refusé");
}

/*  — même balayage sur le retrait du wrapper : la 1ère ligne
 * (toutes graphies d'introduction) et le terminateur (`FIN` comme son
 * canon EN `END`) doivent disparaître, un FIN interne jamais. */
static void check_strip(const char* body, const char* want, const char* label) {
    const char* out = NULL;
    long        n   = 0;
    cli_doc_scenario_strip_wrapper(body, (long)strlen(body), &out, &n);
    int good = (n == (long)strlen(want)) && memcmp(out, want, (size_t)n) == 0;
    if (!good) {
        fprintf(stderr, "    [strip] attendu \"%s\" | obtenu \"%.*s\"\n",
                want, (int)n, out ? out : "");
    }
    CHECK(good, label);
}

/* NB : quand la ligne terminale est retirée, la borne de fin est le DÉBUT de
 * cette ligne — le `\n` de la ligne de contenu précédente est donc conservé
 * (comportement d'origine, le rendu ligne-à-ligne l'absorbe). */
static void test_scenario_strip_balayage_graphies(void) {
    check_strip("PROCEDURE PUBLIQUE s()\n\tTrace(1)\nFIN\n", "\tTrace(1)\n",
                "strip PROCEDURE ... FIN");
    check_strip("PROC\xc3\x89""DURE PUBLIQUE s()\n\tTrace(1)\nFIN\n",
                "\tTrace(1)\n", "strip PROCÉDURE (accentué)");
    check_strip("FONCTION s() : entier\n\tRENVOYER 1\nFIN\n", "\tRENVOYER 1\n",
                "strip FONCTION");
    check_strip("FUNCTION s() : integer\n\tRETURN 1\nFIN\n", "\tRETURN 1\n",
                "strip FUNCTION (canon EN)");
    check_strip("PROCEDURE s()\n\tTrace(1)\nEND\n", "\tTrace(1)\n",
                "strip terminateur END (canon EN)");
    check_strip("PROCEDURE s()\n\tSI a ALORS\n\t\tTrace(1)\n\tFIN\nFIN\n",
                "\tSI a ALORS\n\t\tTrace(1)\n\tFIN\n",
                "FIN interne préservé");
    check_strip("PROCEDURE s()\n\tIF a THEN\n\t\tTrace(1)\n\tEND\nEND\n",
                "\tIF a THEN\n\t\tTrace(1)\n\tEND\n",
                "END interne préservé");
    /* Pas d'enveloppe : le corps est rendu tel quel (hors blancs de bord). */
    check_strip("Trace(1)\n", "Trace(1)", "corps sans wrapper intact");
}

/*  — bornage du corps d'un scénario : la profondeur de blocs suivie
 * ligne à ligne doit passer par la règle partagée, seule à distinguer
 * `SI c ALORS instr` (forme mono-ligne, aucun FIN) de `SI c ALORS` + saut de
 * ligne (forme bloc). Comptée comme ouvrante, la forme mono-ligne empêche la
 * profondeur de retomber à zéro : le corps rendu avale les déclarations
 * suivantes, qui disparaissent de la vitrine et réapparaissent au milieu de
 * l'exemple précédent.
 *
 * Sonde directe : le corpus embarqué ne contient pas toutes les graphies
 * légales (aucun scénario n'écrit `CONSTANTE` en bloc dans un corps, ni la
 * forme EN `IF ... THEN`), et un verrou qui ne passerait que par le rendu
 * laisserait ces formes muettes. */
extern int cli_doc_scenario_line_depth_delta(const char* line, long len);

static void check_delta(const char* line, int want, const char* label) {
    int got = cli_doc_scenario_line_depth_delta(line, (long)strlen(line));
    if (got != want) {
        fprintf(stderr, "    [%s] attendu delta=%d | obtenu %d\n",
                line, want, got);
    }
    CHECK(got == want, label);
}

static void test_scenario_line_depth_delta(void) {
    /* --- Formes SANS FIN : delta 0 obligatoire --- */
    check_delta("\tSI n >= 10 ALORS x = \"a\" SINON x = \"b\"", 0,
                "SI mono-ligne avec SINON n'ouvre pas de bloc");
    check_delta("\tSI n >= 10 ALORS x = \"a\"", 0,
                "SI mono-ligne sans SINON n'ouvre pas de bloc");
    check_delta("\tIF n >= 10 THEN x = 1", 0,
                "IF/THEN mono-ligne (canon EN) n'ouvre pas de bloc");
    check_delta("\tCONSTANTE MAX = 500", 0,
                "CONSTANTE inline n'ouvre pas de bloc");

    /* --- Formes bloc : delta +1 --- */
    check_delta("\tSI n >= 10 ALORS", 1, "SI bloc ouvre");
    check_delta("\tIF n >= 10 THEN", 1, "IF bloc ouvre (canon EN)");
    check_delta("\tPOUR i = 1 A 3", 1, "POUR ouvre");
    check_delta("\tTANTQUE n > 0", 1, "TANTQUE ouvre");
    check_delta("\tBOUCLE", 1, "BOUCLE ouvre");
    check_delta("\tSELON n", 1, "SELON ouvre");
    check_delta("\tQUAND EXCEPTION DANS", 1, "QUAND ouvre");
    check_delta("\tCONSTANTE", 1, "CONSTANTE bloc ouvre");
    check_delta("\tAVEC oClient", 1, "AVEC en tête de ligne ouvre");
    check_delta("PROCEDURE scenario_x()", 1, "PROCEDURE ouvre");

    /* --- Fermeture --- */
    check_delta("\tFIN", -1, "FIN ferme");
    check_delta("END", -1, "END ferme (canon EN)");

    /* --- Neutres : rien ne doit bouger --- */
    check_delta("\tSINON SI n > 5 ALORS", 0,
                "SINON SI est une clause elif, pas un bloc de plus");
    check_delta("\tSINON", 0, "SINON seul ne rouvre rien");
    check_delta("\t// SI n >= 10 ALORS", 0, "ligne de commentaire neutre");
    check_delta("", 0, "ligne vide neutre");
    check_delta("\tdbgVérifieVrai(b, \"SI ALORS\")", 0,
                "SI dans un littéral chaîne ne compte pas");
    check_delta("\tp est une Procedure = &Callback", 0,
                "annotation de type post-copule ne compte pas");
}

/*  — verrou de bout en bout sur le corpus : le sujet
 * « SI ... DANS / PAS DANS / SINON court » déclare 7 procédures publiques dont
 * une contient DEUX `SI ... ALORS ... SINON` mono-ligne. Avec un comptage local
 * qui les prend pour des ouvertures, la vitrine n'en rendait que 5 : les deux
 * suivantes étaient avalées par le corps de la 4e. */
static void test_examples_si_monoligne_ne_avale_pas(void) {
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples",
                               "SI ... DANS / PAS DANS / SINON court"));
    CHECK_RC(rc, 0, "examples sujet SI ... DANS rc=0");
    /* Valeur exacte : 7 titres rendus, ni 5 ni 8. */
    CHECK(count_occurrences("►") == 7,
          "les 7 scénarios publics du sujet sont rendus");
    /* Les deux qui étaient avalés. */
    CHECK_HAS("► scenario_dans_combine_avec_et",
              "scénario suivant le SI mono-ligne rendu comme titre");
    CHECK_HAS("► scenario_dans_un_seul_element",
              "2e scénario suivant le SI mono-ligne rendu comme titre");
    /* Aucune ligne de déclaration ne doit subsister DANS un corps : c'est la
     * signature de l'avalement. */
    CHECK_NO("PROCEDURE scenario_dans_combine_avec_et",
             "aucune déclaration avalée dans le corps d'un autre scénario");
    CHECK_NO("PROCEDURE scenario_dans_un_seul_element",
             "aucune 2e déclaration avalée dans un corps");
    /* Le scénario PRIVÉE reste hors vitrine (bornage correct ≠ vitrine
     * élargie). */
    CHECK_NO("scenario_pas_prefixe_equivalent",
             "scénario PRIVÉE toujours exclu de la vitrine");
    /* Le corps du scénario mono-ligne garde ses deux formes courtes. */
    CHECK(count_occurrences("SI nNote >= 10 ALORS sMention = \"admis\"") == 2,
          "les deux SI mono-ligne du corps sont rendus");
}

/*  — le séparateur '/' d'un @sujet (formes bilingues FR/EN, ou
 * variantes multiples d'un même scénario) doit être compris par
 * owl examples / explain au même titre que la virgule. Un scénario
 * `@sujet : IoU / SuppressionNonMax (NonMaxSuppression)` restait invisible
 * pour `examples IoU` alors qu'il l'illustre — 41 scénarios du corpus
 * utilisaient ce séparateur, tous orphelins côté CLI doc. Le self-test de
 * registre (règle 47) acceptait déjà le slash : divergence entre
 * ce que le corpus déclare et ce que la CLI sait lire. */
static void test_examples_sujet_slash_separateur(void) {
    /* 1re forme du slash, dans natives/. */
    int rc = capture_call(cli_doc_examples, 2, ARGS("examples", "IoU"));
    CHECK_RC(rc, 0, "examples IoU rc=0");
    CHECK_NO("Aucun scénario",
             "examples IoU : plus de 'aucun scénario' (slash lu)");
    CHECK_HAS("► scenario_iou_boites_identiques",
              "examples IoU liste son scénario (1re forme avant le slash)");

    /* 2e forme du slash, avec tag parenthésé `(NonMaxSuppression)`. */
    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "SuppressionNonMax"));
    CHECK_RC(rc, 0, "examples SuppressionNonMax rc=0");
    CHECK_HAS("► scenario_nms_supprime_chevauchement",
              "examples SuppressionNonMax (2e forme + paren) résout");

    /* Description em-dash après le slash : `PrécisionMoyenne /
     * MeanAveragePrecision — métrique …`. */
    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "Pr\xc3\xa9""cisionMoyenne"));
    CHECK_RC(rc, 0, "examples PrécisionMoyenne rc=0");
    CHECK_HAS("► scenario_map_detection_parfaite",
              "examples PrécisionMoyenne liste son scénario");

    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "AssocieCiblesD\xc3\xa9""tection"));
    CHECK_RC(rc, 0, "examples AssocieCiblesDétection rc=0");
    CHECK_HAS("► scenario_boite_cellule_responsable_exacte",
              "examples AssocieCiblesDétection liste son scénario");
}

/*  — un @sujet en forme pointée `Type.Méthode` (convention des
 * scénarios de types/, ex. `Tenseur.TrancheCanaux / Tenseur.ChannelSlice`)
 * doit résoudre à la fois par la forme qualifiée ET par le nom de méthode nu.
 * Une description em-dash après le symbole (`Tenseur.Convolution2D —
 * convolution 2D (…)`) ne doit pas masquer le symbole. */
static void test_examples_sujet_methode_pointee(void) {
    /* Nom de méthode nu -> match sur le suffixe après le dernier point. */
    int rc = capture_call(cli_doc_examples, 2,
                          ARGS("examples", "TrancheCanaux"));
    CHECK_RC(rc, 0, "examples TrancheCanaux rc=0");
    CHECK_NO("Aucun scénario",
             "examples TrancheCanaux (méthode nue) : plus de 'aucun'");
    CHECK_HAS("► scenario_tranchecanaux_rang2_extrait_plage_canaux",
              "examples TrancheCanaux liste son scénario");

    /* Forme qualifiée -> match sur le token pointé complet. */
    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "Tenseur.TrancheCanaux"));
    CHECK_RC(rc, 0, "examples Tenseur.TrancheCanaux rc=0");
    CHECK_HAS("► scenario_tranchecanaux_rang2_extrait_plage_canaux",
              "examples Tenseur.TrancheCanaux (qualifié) résout");

    /* Description em-dash directement collée au symbole (pas de slash). */
    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "Convolution2D"));
    CHECK_RC(rc, 0, "examples Convolution2D rc=0");
    CHECK_HAS("► scenario_convolution2d_forward_mono_canal",
              "examples Convolution2D (nom nu, sujet em-dash) résout");

    rc = capture_call(cli_doc_examples, 2,
                      ARGS("examples", "Tenseur.Convolution2D"));
    CHECK_RC(rc, 0, "examples Tenseur.Convolution2D rc=0");
    CHECK_HAS("► scenario_convolution2d_forward_mono_canal",
              "examples Tenseur.Convolution2D (qualifié em-dash) résout");
}

/*  — `owl explain <méthode>` sur une native forme-méthode (sans
 * native globale homonyme) doit aussi joindre ses scénarios. La branche
 * `explain_method` ne les imprimait pas du tout. */
static void test_explain_method_scenarios(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "TrancheCanaux"));
    CHECK_RC(rc, 0, "explain TrancheCanaux rc=0");
    CHECK_NO("Aucun scénario",
             "explain TrancheCanaux : joint ses scénarios");
    CHECK_HAS("► scenario_tranchecanaux_rang2_extrait_plage_canaux",
              "explain TrancheCanaux liste son scénario");
}

static void test_search(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "HTTPRequête"));
    CHECK_RC(rc, 0, "search HTTPRequête rc=0");
    /*  : liste plate — plus d'entête de section, un tag [native] par
     * ligne. On verifie le resultat lui-meme + son tag de categorie. */
    CHECK_HAS("HTTPRequête", "search trouve HTTPRequête");
    CHECK_HAS("[native]", "search tague la categorie native");
}

static void test_search_no_hit(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "azerqwertyuiop"));
    CHECK_RC(rc, 0, "search no-hit rc=0");
    CHECK_HAS("Aucun résultat", "search no-hit message présent");
}

/* Multi-mots : la requête est tokenisée sur les espaces et chaque token doit
 * matcher (AND). `ChaîneVersJSON` contient les deux tokens dans son nom. */
static void test_search_multimot(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "JSON chaine"));
    CHECK_RC(rc, 0, "search multimot rc=0");
    CHECK_HAS("ChaîneVersJSON", "search 'JSON chaine' trouve ChaîneVersJSON");
}

/* Tolérance casse + accents préservée sur chaque token. */
static void test_search_multimot_casse_accents(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "json chaîne"));
    CHECK_RC(rc, 0, "search multimot casse/accents rc=0");
    CHECK_HAS("ChaîneVersJSON", "search 'json chaîne' trouve ChaîneVersJSON");
}

/* Ordre des tokens indifférent (AND commutatif). */
static void test_search_multimot_ordre(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "chaine JSON"));
    CHECK_RC(rc, 0, "search multimot ordre inversé rc=0");
    CHECK_HAS("ChaîneVersJSON", "search 'chaine JSON' trouve ChaîneVersJSON");
}

/* Espaces multiples / tokens vides ne crashent pas et restent matchants. */
static void test_search_multimot_espaces(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "  JSON   chaine  "));
    CHECK_RC(rc, 0, "search multimot espaces multiples rc=0");
    CHECK_HAS("ChaîneVersJSON", "search espaces multiples trouve ChaîneVersJSON");
}

/* Sémantique OR avec score : une requête multi-mots dont certains tokens
 * ne matchent rien (typo / mot vide de sens) ne doit PAS écraser les
 * matchs des autres tokens. Ici 'xyzzyzero' matche zéro symbole mais
 * 'JSON' et 'ajoute' matchent — JSONSérialise et/ou Ajoute doivent
 * apparaître. Avant fix : AND strict → 0 résultat. */
static void test_search_multimot_or_partial(void) {
    int rc = capture_call(cli_doc_search, 2,
                          ARGS("search", "JSON xyzzyzero ajoute"));
    CHECK_RC(rc, 0, "search OR partiel rc=0");
    CHECK(strstr(g_buf, "JSONSérialise") != NULL ||
          strstr(g_buf, "Ajoute") != NULL,
          "search OR partiel ramène au moins une entrée pertinente");
}

/* No-hit multi-mots : aucun token ne matche → message « Aucun résultat ».
 * Garantit que le OR ne dégénère pas en faux positifs muets. */
static void test_search_multimot_no_hit(void) {
    int rc = capture_call(cli_doc_search, 2,
                          ARGS("search", "xyzzyzero foobarbaz"));
    CHECK_RC(rc, 0, "search multimot no-hit rc=0");
    CHECK_HAS("Aucun résultat", "search multimot no-hit message présent");
}

/*  — le catalogue des contrôles DSL (widgets + sous-blocs) doit
 * être indexé par `owl search`, à parité avec `owl explain`. Support #84 :
 * F1 (lookup direct) trouvait la fiche Fenêtre/Window, mais la recherche
 * libre ne la ramenait jamais, faute d'itérer owl_control_kinds_table(). */
static void test_search_widgets(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "fenetre"));
    CHECK_RC(rc, 0, "search fenetre rc=0");
    CHECK_HAS("[champ]", "search fenetre : widget tagué [champ] (liste plate 1302)");
    CHECK_HAS("Fenêtre", "search fenetre : widget Fenêtre remonte");

    rc = capture_call(cli_doc_search, 2, ARGS("search", "Bouton"));
    CHECK_RC(rc, 0, "search Bouton rc=0");
    CHECK_HAS("Bouton", "search Bouton : widget Bouton remonte");
}

/* Terme EN : la recherche par nom anglais doit ramener la fiche canon FR. */
static void test_search_widgets_en(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "Window"));
    CHECK_RC(rc, 0, "search Window rc=0");
    CHECK_HAS("Fenêtre", "search Window : ramène le widget Fenêtre");
}

/* Branche JSON : clé `widgets` non vide et total cohérent. */
static void test_search_widgets_json(void) {
    int rc = capture_call(cli_doc_search, 3,
                          ARGS("search", "fenetre", "--json"));
    CHECK_RC(rc, 0, "search fenetre --json rc=0");
    CHECK_HAS("\"widgets\":", "JSON : clé widgets présente");
    CHECK_HAS("Fenêtre", "JSON : Fenêtre listée dans widgets");
    CHECK(json_buf_is_balanced(), "search widgets --json équilibré");
}

/*  (support #324) — les propriétés (champs) des types composés sont
 * désormais indexées par `owl search` : nom + description FR ET EN. Repro : le
 * terme `naming-prefix` n'existe que dans la descEN de la propriété
 * ActiveAvertissementConventionDeNommage (type Projet) ; il doit sortir même en
 * sortie FR (indépendance langue sur la desc), qualifié par son type parent. */
static void test_search_type_property_descen(void) {
    int rc = capture_call(cli_doc_search, 2, ARGS("search", "naming-prefix"));
    CHECK_RC(rc, 0, "search naming-prefix rc=0");
    CHECK_NO("Aucun résultat", "search naming-prefix : au moins un hit");
    CHECK_HAS("[propriété]", "search naming-prefix : hit tagué [propriété]");
    CHECK_HAS("Projet..ActiveAvertissementConventionDeNommage",
              "search naming-prefix : prop qualifiée par son type parent");
}

/* Match sur le NOM de la propriété (pas seulement sa description). */
static void test_search_type_property_byname(void) {
    int rc = capture_call(cli_doc_search, 2,
                          ARGS("search", "ActiveAvertissementConventionDeNommage"));
    CHECK_RC(rc, 0, "search prop par nom rc=0");
    CHECK_HAS("Projet..ActiveAvertissementConventionDeNommage",
              "search : prop trouvée par son nom canonique + type parent");
}

/* Branche JSON : la clé `properties` liste la prop avec son type parent. */
static void test_search_type_property_json(void) {
    int rc = capture_call(cli_doc_search, 3,
                          ARGS("search", "naming-prefix", "--json"));
    CHECK_RC(rc, 0, "search naming-prefix --json rc=0");
    CHECK_HAS("\"properties\":", "JSON : clé properties présente");
    CHECK_HAS("ActiveAvertissementConventionDeNommage",
              "JSON : prop listée dans properties");
    CHECK_HAS("\"Projet\"", "JSON : type parent renseigné");
    CHECK(json_buf_is_balanced(), "search properties --json équilibré");
}

/*  — `owl explain <kind non-widget>` routait TOUT non-widget vers
 * le catalogue d'events Etat (Filtre/Lecture/Fermeture). Les kinds hors
 * famille Etat (blocs de menu, Colonne) héritaient donc d'events bidon. On
 * vérifie le routage par famille : OptionMenu → son event réel, les blocs de
 * menu / Colonne → aucun event Etat, et la famille Etat reste intacte. */
static void test_explain_events_kind_routing(void) {
    /* OptionMenu : son event réel surSélection, pas les events Etat. */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "OptionMenu"));
    CHECK_RC(rc, 0, "explain OptionMenu rc=0");
    CHECK_HAS("surSélection", "explain OptionMenu : event réel surSélection");
    CHECK_NO("Filtre", "explain OptionMenu : pas d'event Etat Filtre");
    CHECK_NO("Fermeture", "explain OptionMenu : pas d'event Etat Fermeture");
    /* Contexte parent : OptionMenu s'imbrique dans un SousMenu, PAS un Etat. */
    CHECK_HAS("fait partie d'un SousMenu",
              "explain OptionMenu : contexte parent SousMenu");
    CHECK_NO("d'un Etat", "explain OptionMenu : contexte parent pas Etat");
    /* État initial (Etat) unifié — miroir du mécanisme des champs, additif. */
    CHECK_HAS("Propriétés (7)", "explain OptionMenu : 7 props (Etat ajouté)");
    CHECK_HAS("..Etat", "explain OptionMenu : prop Etat (état initial unifié)");

    /* Colonne : sous-bloc de Table, aucun event — surtout pas ceux d'Etat. */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Colonne"));
    CHECK_RC(rc, 0, "explain Colonne rc=0");
    CHECK_NO("Filtre", "explain Colonne : pas d'event Etat parasite");
    CHECK_NO("Events", "explain Colonne : pas de section Events");

    /* MenuPrincipal / SousMenu / MenuSéparateur : blocs structurels, aucun
     * event. */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "MenuPrincipal"));
    CHECK_RC(rc, 0, "explain MenuPrincipal rc=0");
    CHECK_NO("Filtre", "explain MenuPrincipal : pas d'event Etat parasite");
    CHECK_NO("Events", "explain MenuPrincipal : pas de section Events");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "SousMenu"));
    CHECK_RC(rc, 0, "explain SousMenu rc=0");
    CHECK_NO("Filtre", "explain SousMenu : pas d'event Etat parasite");
    CHECK_HAS("Propriétés (4)", "explain SousMenu : 4 props (Etat ajouté)");
    CHECK_HAS("..Etat", "explain SousMenu : prop Etat (état initial unifié)");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "MenuSéparateur"));
    CHECK_RC(rc, 0, "explain MenuSéparateur rc=0");
    CHECK_NO("Filtre", "explain MenuSéparateur : pas d'event Etat parasite");

    /* Non-régression famille Etat : le rapport et ses sous-blocs gardent
     * les events lifecycle Etat. */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Corps"));
    CHECK_RC(rc, 0, "explain Corps rc=0");
    CHECK_HAS("Filtre", "explain Corps : events Etat intacts (Filtre)");
    CHECK_HAS("Fermeture", "explain Corps : events Etat intacts (Fermeture)");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "EnTeteDocument"));
    CHECK_RC(rc, 0, "explain EnTeteDocument rc=0");
    CHECK_HAS("Filtre", "explain EnTeteDocument : events Etat intacts");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Etat"));
    CHECK_RC(rc, 0, "explain Etat rc=0");
    CHECK_HAS("Filtre", "explain Etat : events Etat intacts");
}

static void test_complete_top(void) {
    int rc = capture_call(cli_doc_complete, 1, ARGS("__complete"));
    CHECK_RC(rc, 0, "__complete top-level rc=0");
    CHECK_HAS("help\n",    "complete top : help");
    CHECK_HAS("primer\n",  "complete top : primer");
    CHECK_HAS("learn\n",   "complete top : learn");
    CHECK_HAS("explain\n", "complete top : explain");
}

static void test_complete_explain_prefix(void) {
    int rc = capture_call(cli_doc_complete, 3,
                          ARGS("__complete", "explain", "Cad"));
    CHECK_RC(rc, 0, "__complete explain Cad rc=0");
    CHECK_HAS("Cadre\n",    "complete explain : Cadre");
    /* Au moins une autre suggestion en Cad* — CadrageH/V, cadCentre… */
    CHECK(count_occurrences("\n") >= 2,
          "complete explain Cad : plusieurs candidats");
}

static void test_complete_explain_accent(void) {
    /* Une saisie sans accent doit proposer les formes avec ET sans. */
    int rc = capture_call(cli_doc_complete, 3,
                          ARGS("__complete", "explain", "Chaine"));
    CHECK_RC(rc, 0, "__complete explain Chaine rc=0");
    CHECK_HAS("ChaineVersEntier\n",  "complete propose forme ASCII");
    CHECK_HAS("ChaîneVersEntier\n", "complete propose forme accentuée");
}

/* ================================================================
 * V1.5 — Maillage "Voir aussi" (cap dur N=3 fiches, N=5 voisins)
 * ================================================================ */

/* Compte les occurrences d'une substring entre deux marqueurs. Si
 * after est absent on prend jusqu'a la fin. Sert a verifier le cap
 * N=3 fiches sans hardcoder les noms (qui peuvent changer). */
static int count_between(const char* needle,
                         const char* after_marker,
                         const char* before_marker) {
    const char* start = strstr(g_buf, after_marker);
    if (!start) return 0;
    start += strlen(after_marker);
    const char* end = before_marker ? strstr(start, before_marker) : NULL;
    if (!end) end = g_buf + strlen(g_buf);
    int n = 0;
    size_t nlen = strlen(needle);
    const char* p = start;
    while (p < end && (p = strstr(p, needle)) != NULL && p < end) {
        n++; p += nlen;
    }
    return n;
}

static void test_explain_related_native_trace(void) {
    /* Trace est ultra-commune : >> 9 fiches matchent. Le cap se manifeste
     * sous l'une des deux formes :
     *  - enum-cap (RELATED_MAX < N <= 3*RELATED_MAX) :
     *    "fiches  Doc1, Doc2, Doc3 (+M autres)"
     *  - flood   (N > 3*RELATED_MAX) :
     *    "fiches  présent dans N fiches (owl search Trace)"
     * On verifie LA SEMANTIQUE (cap declenche), pas la forme precise,
     * pour rester resilient a la croissance du repo. La forme JSON
     * (public_documentation_more > 0) reste le test source-of-truth
     * du cap, cf test_explain_related_native_trace_json. */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Trace"));
    CHECK_RC(rc, 0, "explain Trace rc=0");
    CHECK_HAS("Voir aussi",     "Voir aussi present");
    CHECK_HAS("fiches",         "Voir aussi : fiches presente");
    CHECK_HAS("voisins",        "Voir aussi : voisins presente (native Console)");
    /* Cap declenche : enum-cap "(+N autres)" OU flood "présent dans N fiches". */
    int has_enum_cap = strstr(g_buf, "(+") != NULL &&
                       strstr(g_buf, " autres)") != NULL;
    int has_flood    = strstr(g_buf, "présent dans") != NULL &&
                       strstr(g_buf, "fiches (owl search") != NULL;
    CHECK(has_enum_cap || has_flood,
          "Cap declenche : enum-cap (+N autres) OU flood (N fiches)");
    /* La section Voir aussi ne doit pas exploser : <= 3 noms separes
     * par ", " soit max 2 virgules dans la ligne fiches.
     * (Flood : 0 virgule. Enum-cap : 2 virgules max.) */
    int commas = count_between(",", "fiches", "\n");
    CHECK(commas <= 2, "fiches : max 3 noms (donc <= 2 virgules)");
}

static void test_explain_related_native_trace_json(void) {
    int rc = capture_call(cli_doc_explain, 3,
                          ARGS("explain", "Trace", "--json"));
    CHECK_RC(rc, 0, "explain Trace --json rc=0");
    CHECK_HAS("\"related\"",            "JSON : champ related present");
    CHECK_HAS("\"public_documentation\"",        "JSON : public_documentation present");
    CHECK_HAS("\"public_documentation_more\":",  "JSON : public_documentation_more present");
    CHECK_HAS("\"neighbors\"",          "JSON : neighbors present");
    CHECK_HAS("\"neighbors_more\":",    "JSON : neighbors_more present");
    /* Cap dur : public_documentation doit contenir AU PLUS 3 entrees {"name":...} */
    int names = count_between("{\"name\":", "\"public_documentation\"", "\"public_documentation_more\"");
    CHECK(names <= 3, "JSON : public_documentation cap a 3");
    /* public_documentation_more > 0 pour Trace (cap declenche). Pas de valeur
     * exacte hardcodee — depend du contenu courant des fiches. */
    CHECK_NO("\"public_documentation_more\": 0", "JSON : cap declenche, public_documentation_more > 0");
}

static void test_explain_related_dsl_prop_grise(void) {
    /* Grise est une DSL prop : la section Voir aussi doit inclure
     * `where` avec un count > 0 (plusieurs widgets l'acceptent). */
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Grise"));
    CHECK_RC(rc, 0, "explain Grise rc=0");
    CHECK_HAS("Voir aussi",     "Voir aussi present (DSL prop)");
    CHECK_HAS("where",          "Voir aussi : ligne where (DSL prop)");
    CHECK_HAS("champs accept", "where annonce les champs acceptants");
}

/*  — voisins d'un type composé SANS native homonyme. Avant le fix,
 * compute_related ne peuplait neighbors que pour REL_KIND_NATIVE ; les types
 * conteneurs (IAModèle, LLM, Options*) tombaient en REL_KIND_TYPE et rendaient
 * "neighbors": []. On vérifie que la navigation structurée relie désormais ces
 * types par catégorie, sans régresser le chemin native (Tenseur). */
static void test_explain_related_type_neighbors_json(void) {
    /* IAModèle : type sans native homonyme, catégorie "Intelligence
     * artificielle". Ses voisins doivent inclure les autres types de la
     * catégorie (LLM au minimum). */
    capture_call(cli_doc_explain, 3, ARGS("explain", "IAModèle", "--json"));
    CHECK_HAS("\"neighbors\"",  "IAModèle json : neighbors present");
    CHECK_HAS("\"LLM\"",        "IAModèle json : voisin LLM peuplé (catégorie IA)");

    /* LLM : réciproque, IAModèle est un de ses voisins. */
    capture_call(cli_doc_explain, 3, ARGS("explain", "LLM", "--json"));
    CHECK_HAS("\"IAModèle\"",   "LLM json : voisin IAModèle peuplé (catégorie IA)");

    /* Tenseur : type AVEC native homonyme → chemin REL_KIND_NATIVE inchangé.
     * Non-régression : voisins natifs identiques (voisins de catégorie
     * "Tenseurs" côté natives, pas de bascule sur les voisins de type). */
    capture_call(cli_doc_explain, 3, ARGS("explain", "Tenseur", "--json"));
    CHECK_HAS("\"TenseurProduitMatriciel\"",
              "Tenseur json : voisins natifs inchangés (REL_KIND_NATIVE)");
    CHECK_HAS("\"neighbors_more\": 17",
              "Tenseur json : neighbors_more (catégorie Tenseurs, +2 BCE-logits/L1 feat/1842)");
}

/*  Veine Y — defauts canon des props DSL exposes par explain.
 * Verrouille les valeurs exactes peuplees (texte "Défaut :" + JSON
 * default_value/default_kind). Sourcees du comportement runtime. */
static void test_explain_dsl_prop_defaults(void) {
    /* Visible : bool, defaut Vrai (root cause du ticket). */
    capture_call(cli_doc_explain, 2, ARGS("explain", "Visible"));
    CHECK_HAS("Défaut : Vrai", "explain Visible : defaut Vrai (texte)");
    capture_call(cli_doc_explain, 3, ARGS("explain", "Visible", "--json"));
    CHECK_HAS("\"default_value\": true", "Visible json default_value true");
    CHECK_HAS("\"default_kind\": \"bool\"", "Visible json default_kind bool");

    /* Grisé : bool, defaut Faux. */
    capture_call(cli_doc_explain, 3, ARGS("explain", "Grise", "--json"));
    CHECK_HAS("\"default_value\": false", "Grise json default_value false");

    /* Opacité : int, defaut 100. */
    capture_call(cli_doc_explain, 3, ARGS("explain", "Opacite", "--json"));
    CHECK_HAS("\"default_value\": 100", "Opacite json default_value 100");

    /* Disposition : enum, defaut dispVerticale (1re valeur canon). */
    capture_call(cli_doc_explain, 3, ARGS("explain", "Disposition", "--json"));
    CHECK_HAS("\"default_value\": \"dispVerticale\"", "Disposition json default dispVerticale");
    CHECK_HAS("\"default_kind\": \"enum\"", "Disposition json default_kind enum");

    /* Espacement : int, defaut 6 (sourcé runtime, PAS 0). */
    capture_call(cli_doc_explain, 3, ARGS("explain", "Espacement", "--json"));
    CHECK_HAS("\"default_value\": 6", "Espacement json default_value 6");

    /* AlignH : enum mais defaut runtime 0 = non specifie -> sentinelle null. */
    capture_call(cli_doc_explain, 3, ARGS("explain", "AlignH", "--json"));
    CHECK_HAS("\"default_kind\": \"null\"", "AlignH json default_kind null");

    /* X : geometrie position, defaut auto (sourcee runtime). */
    capture_call(cli_doc_explain, 3, ARGS("explain", "X", "--json"));
    CHECK_HAS("\"default_kind\": \"auto\"", "X json default_kind auto");
}

static void test_explain_related_rare_symbol(void) {
    /* MarkdownVersTexte : peu/pas dans les fondations en code, mais
     * forcement des voisins dans la cat String. */
    int rc = capture_call(cli_doc_explain, 2,
                          ARGS("explain", "MarkdownVersTexte"));
    CHECK_RC(rc, 0, "explain MarkdownVersTexte rc=0");
    /* Doit avoir au moins une section Voir aussi grace aux voisins. */
    CHECK_HAS("Voir aussi", "Voir aussi present meme pour symbole rare");
    CHECK_HAS("voisins",    "voisins (cat String)");
}

/*  — table de cross-references declaratives fiche->guide.
 * Toute entree shippee doit resoudre des deux bouts (miroir in-process de la
 * rule 62 du self-test registry). */
static void test_crossref_table_entries_valid(void) {
    int n = cli_doc_crossref_count();
    CHECK(n >= 1, "cross-ref : table non vide (seed ST1)");
    for (int i = 0; i < n; i++) {
        const char* sym = NULL;
        const char* stem = NULL;
        CHECK(cli_doc_crossref_at(i, &sym, &stem), "cross-ref : accesseur OK");
        int sok = 0, dok = 0;
        int ok = cli_doc_crossref_entry_valid(sym, stem, &sok, &dok);
        CHECK(ok, "cross-ref : entree valide des deux bouts");
    }
}

/* Preuve du negatif SANS shipper d'entree invalide : le predicat partage par la
 * rule 62 rejette bien un symbole et/ou un stem inexistant (l'entree factice
 * {"SymboleInexistantXyz","99_guide_inexistant"} ferait echouer le self-test). */
static void test_crossref_entry_valid_detects_dangling(void) {
    int sok, dok;
    /* Les deux bouts casses. */
    CHECK(cli_doc_crossref_entry_valid("SymboleInexistantXyz",
                                       "99_guide_inexistant", &sok, &dok) == 0,
          "cross-ref negatif : symbole+stem inexistants rejetes");
    CHECK(sok == 0, "cross-ref negatif : symbole inexistant signale");
    CHECK(dok == 0, "cross-ref negatif : stem inexistant signale");
    /* Symbole valide, stem casse. */
    CHECK(cli_doc_crossref_entry_valid("RegexRemplace",
                                       "99_guide_inexistant", &sok, &dok) == 0,
          "cross-ref negatif : stem casse suffit a rejeter");
    CHECK(sok == 1 && dok == 0, "cross-ref negatif : bout symbole OK, stem KO");
    /* Symbole casse, stem valide. */
    CHECK(cli_doc_crossref_entry_valid("SymboleInexistantXyz",
                                       "05_operateurs", &sok, &dok) == 0,
          "cross-ref negatif : symbole casse suffit a rejeter");
    CHECK(sok == 0 && dok == 1, "cross-ref negatif : bout stem OK, symbole KO");
    /* L'entree seed resout. */
    CHECK(cli_doc_crossref_entry_valid("RegexRemplace",
                                       "05_operateurs", &sok, &dok) == 1,
          "cross-ref : seed RegexRemplace->05_operateurs valide");
}

/* Le lien declaratif epingle apparait dans explain (texte + JSON), meme quand
 * l'heuristique ne le trouve pas (RegexRemplace n'est dans aucun bloc owl). */
static void test_crossref_pin_visible_in_explain(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "RegexRemplace"));
    CHECK_RC(rc, 0, "explain RegexRemplace rc=0");
    CHECK_HAS("Voir aussi", "cross-ref : section Voir aussi presente");
    CHECK_HAS("05_operateurs", "cross-ref : guide epingle en texte");

    rc = capture_call(cli_doc_explain, 3, ARGS("explain", "RegexRemplace", "--json"));
    CHECK_RC(rc, 0, "explain RegexRemplace --json rc=0");
    CHECK_HAS("\"public_documentation\"", "cross-ref json : public_documentation present");
    CHECK_HAS("\"05_operateurs\"", "cross-ref json : guide epingle en JSON");
}

/* ================================================================
 * Templates — owl template [<nom>]
 * ================================================================ */

static void test_template_list(void) {
    int rc = capture_call(cli_doc_template, 1, ARGS("template"));
    CHECK_RC(rc, 0, "template list rc=0");
    CHECK_HAS("Templates disponibles", "header de liste");
    CHECK_HAS("hello",   "list : hello");
    CHECK_HAS("classe",  "list : classe");
    CHECK_HAS("test",    "list : test");
    CHECK_HAS("fenetre", "list : fenetre");
    CHECK_HAS("basique", "list : niveau basique annonce");
    CHECK_HAS("-o", "list : footer mentionne l'export -o");
}

static void test_template_show_hello(void) {
    int rc = capture_call(cli_doc_template, 2, ARGS("template", "hello"));
    CHECK_RC(rc, 0, "template hello rc=0");
    CHECK_HAS("Trace(",      "show hello : Trace present");
    CHECK_NO("@nom:",        "show : header @nom strippe");
    CHECK_NO("@description:","show : header @description strippe");
    CHECK_NO("@niveau:",     "show : header @niveau strippe");
}

static void test_template_show_classe(void) {
    int rc = capture_call(cli_doc_template, 2, ARGS("template", "classe"));
    CHECK_RC(rc, 0, "template classe rc=0");
    CHECK_HAS("est une Classe", "show classe : declaration classe");
    CHECK_HAS("PROCEDURE",      "show classe : constructeur");
}

static void test_template_unknown(void) {
    int rc = capture_call(cli_doc_template, 2,
                          ARGS("template", "zzz_inexistant"));
    CHECK_RC(rc, 2, "template inconnu rc=2");
}

static void test_template_list_json(void) {
    int rc = capture_call(cli_doc_template, 2,
                          ARGS("template", "--json"));
    CHECK_RC(rc, 0, "template list --json rc=0");
    CHECK_HAS("\"templates\":", "JSON : champ templates");
    CHECK_HAS("\"name\":",      "JSON : champ name par entree");
    CHECK_HAS("\"level\":",     "JSON : champ level par entree");
    CHECK_HAS("\"count\":",     "JSON : count");
}

static void test_template_show_json(void) {
    int rc = capture_call(cli_doc_template, 3,
                          ARGS("template", "hello", "--json"));
    CHECK_RC(rc, 0, "template hello --json rc=0");
    CHECK_HAS("\"name\":",        "JSON show : name");
    CHECK_HAS("\"description\":", "JSON show : description");
    CHECK_HAS("\"code\":",        "JSON show : code");
}

/*  — export fichier : forme longue canon `--output` (forme GNU,
 * alignee sur `owl build`), forme courte `-o` conservee, `--out` abandonne.
 *
 * L'export ecrit sur DISQUE (pas stdout) et le message de succes part sur
 * stderr — capture_call (qui ne lit que stdout) ne convient pas. On appelle
 * cli_doc_template directement, on verifie rc + existence + contenu du
 * fichier, puis on nettoie. */
static void test_template_export_output(void) {
    /* Point initial : les temporaires `.owl` de ce fichier naissent
     * dans le repertoire courant — la racine du depot quand `make` lance ce
     * self-test — donc en plein dans l'arbre walke par `owl test`. Les walkers
     * sautent les entrees pointees : une campagne concurrente ne les charge
     * plus comme compagnons pour abandonner quand on les efface en route. */
    /* Nom propre au processus : le `owl_unlink` de depart d'une
     * execution voisine effacait le fichier que celle-ci vient d'ecrire, et
     * l'assertion "fichier ecrit" tombait sur un export parfaitement sain. */
    char OUT[128];
    fixture_unique(OUT, sizeof OUT, ".owl_cli_export_output_", ".tmp.owl");
    owl_unlink(OUT); /* point de depart propre (refus d'ecraser sinon) */

    char* argv[] = { "template", "hello", "--output", (char*)OUT };
    int rc = cli_doc_template(4, argv);
    CHECK_RC(rc, 0, "template hello --output rc=0");

    FILE* f = fopen(OUT, "rb");
    CHECK(f != NULL, "template --output : fichier ecrit");
    if (f) {
        char body[4096];
        size_t n = fread(body, 1, sizeof(body) - 1, f);
        body[n] = '\0';
        fclose(f);
        CHECK(strstr(body, "Trace(") != NULL,
              "template --output : le corps du template est ecrit");
    }
    owl_unlink(OUT);
}

/*  — la forme courte `-o` continue de marcher (non regression). */
static void test_template_export_o_court(void) {
    char OUT[128];
    fixture_unique(OUT, sizeof OUT, ".owl_cli_export_o_", ".tmp.owl");
    owl_unlink(OUT);

    char* argv[] = { "template", "hello", "-o", (char*)OUT };
    int rc = cli_doc_template(4, argv);
    CHECK_RC(rc, 0, "template hello -o rc=0");

    FILE* f = fopen(OUT, "rb");
    CHECK(f != NULL, "template -o : fichier ecrit");
    if (f) fclose(f);
    owl_unlink(OUT);
}

/*  — `--out` (ancienne forme longue) est desormais un flag inconnu
 * (break-net alpha, pas d'alias redondant) → rc 64, aucun fichier ecrit. */
static void test_template_out_rejete(void) {
    /* L'assertion porte sur une ABSENCE : le nom par processus la renforce
     * (une execution voisine ne peut plus creer ce chemin a notre place). */
    char OUT[128];
    fixture_unique(OUT, sizeof OUT, ".owl_cli_export_out_", ".tmp.owl");
    owl_unlink(OUT);

    char* argv[] = { "template", "hello", "--out", (char*)OUT };
    int rc = cli_doc_template(4, argv);
    CHECK_RC(rc, 64, "template hello --out rc=64 (flag inconnu)");

    FILE* f = fopen(OUT, "rb");
    CHECK(f == NULL, "template --out : aucun fichier ecrit");
    if (f) { fclose(f); owl_unlink(OUT); }
}

/* ================================================================
 *  — owl gotchas & cheatsheet
 * ================================================================ */

static void test_gotchas(void) {
    int rc = capture_call(cli_doc_gotchas, 1, ARGS("gotchas"));
    CHECK_RC(rc, 0, "gotchas rc=0");
    CHECK_HAS("monétaire",   "gotchas mentionne monétaire");
    CHECK_HAS("..",          "gotchas mentionne ..");
    CHECK_HAS("owl test",    "gotchas mentionne 'owl test'");
    CHECK_HAS("Préfixer",    "gotchas mentionne préfixer");
}

static void test_cheatsheet(void) {
    int rc = capture_call(cli_doc_cheatsheet, 1, ARGS("cheatsheet"));
    CHECK_RC(rc, 0, "cheatsheet rc=0");
    CHECK_HAS("est une chaîne", "cheatsheet mentionne déclaration variable");
    CHECK_HAS("SI",             "cheatsheet mentionne SI");
    CHECK_HAS("POUR",           "cheatsheet mentionne POUR");
    CHECK_HAS("PROCEDURE",      "cheatsheet mentionne PROCEDURE");
    CHECK_HAS("Classe",         "cheatsheet mentionne Classe");
    CHECK_HAS("QUAND EXCEPTION","cheatsheet mentionne QUAND EXCEPTION");
}

/* Compile-check des exemples du cheatsheet. Pour éviter le bug #1
 * (primer qui enseignait une syntaxe non-compilable), on vérifie que
 * les fragments de code présentés dans le cheatsheet compilent bien.
 *
 * Approche : extraire les blocs ```owl...``` du doc embarqué réel
 * (23_aide_memoire.md), compiler chacun via handle_check_command_isolated(),
 * vérifier rc == 0. Si un bloc casse, le test rougit — digue dynamique
 * garantie.
 *
 * Extrait entre ```owl\n et \n``` (délimiteurs Markdown canoniques). */

extern int handle_check_command_isolated(const char* filepath);

/* Portable memmem fallback : cherche needle dans haystack. Retourne
 * un pointeur sur la première occurrence ou NULL. Naive O(n*m) mais
 * suffisant pour parsing doc (petits blocs, peu de blocs). */
static const void* portable_memmem(const void* haystack, size_t haystack_len,
                                    const void* needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (haystack_len < needle_len) return NULL;
    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(h + i, n, needle_len) == 0) {
            return h + i;
        }
    }
    return NULL;
}

/* Extrait tous les blocs délimités par ```owl\n...``` depuis les
 * data du doc. Retourne le nombre de blocs trouvés. Pour chaque bloc
 * trouvé, alloue un buffer et le stocke dans blocks[]. Le caller doit
 * free() chaque blocks[i] après usage. */
static int extract_owl_blocks(const unsigned char* data, long len,
                              char** blocks, int max_blocks) {
    int count = 0;
    const char* p = (const char*)data;
    const char* end = p + len;

    /* Motif de début : ```owl\n */
    const char* needle_start = "```owl\n";
    const size_t n_start = strlen(needle_start);
    /* Motif de fin : \n``` (ou début de ligne ``` si block commence
     * directement sans contenu après le \n, mais ici on sait que les
     * blocs ont du contenu). */
    const char* needle_end = "\n```";
    const size_t n_end = strlen(needle_end);

    while (p < end && count < max_blocks) {
        /* Cherche le début ```owl\n */
        const char* block_start_marker = (const char*)
            portable_memmem(p, (size_t)(end - p), needle_start, n_start);
        if (!block_start_marker) break;
        const char* code_start = block_start_marker + n_start;

        /* Cherche le \n``` qui suit */
        const char* block_end_marker = (const char*)
            portable_memmem(code_start, (size_t)(end - code_start), needle_end, n_end);
        if (!block_end_marker) break;  /* bloc non terminé — erreur doc */

        /* Le code est entre code_start et block_end_marker */
        long code_len = (long)(block_end_marker - code_start);
        char* code = malloc((size_t)(code_len + 1));
        if (!code) break;  /* OOM */
        memcpy(code, code_start, (size_t)code_len);
        code[code_len] = '\0';
        blocks[count++] = code;

        /* Avance après le marqueur de fin pour chercher le bloc suivant */
        p = block_end_marker + n_end;
    }
    return count;
}

static void test_cheatsheet_examples_compile(void) {
    /* Cherche la fiche cheatsheet embarquée (name="23_aide_memoire"). */
    const OwlEmbeddedDoc* doc = NULL;
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        if (strcmp(d->name, "23_aide_memoire") == 0) {
            doc = d;
            break;
        }
    }
    if (!doc) {
        g_fail++;
        fprintf(stderr, "  FAIL [cheatsheet doc not found in embedded_docs]\n");
        return;
    }

    /* Extrait tous les blocs ```owl...``` */
    char* blocks[32];
    int nblocks = extract_owl_blocks(doc->data, doc->len, blocks, 32);

    /* Vérifie qu'on a bien trouvé au moins 1 bloc (la fiche actuelle en a 9,
     * mais on ne hardcode pas le nombre exact — il évoluera). */
    CHECK(nblocks > 0, "cheatsheet contient au moins 1 bloc ```owl");

    /* Compile chaque bloc via handle_check_command_isolated(). */
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, ".owl_cheatsheet_example_test_", ".owl");
    for (int i = 0; i < nblocks; i++) {
        FILE* w = fopen(TMP, "wb");
        if (!w) {
            g_fail++;
            fprintf(stderr, "  FAIL [cheatsheet block %d: cannot write tmp file]\n", i);
            continue;
        }
        fwrite(blocks[i], 1, strlen(blocks[i]), w);
        fclose(w);

        int rc = handle_check_command_isolated(TMP);
        char label[64];
        snprintf(label, sizeof label, "cheatsheet block %d compiles", i);
        /* handle_check_command_isolated retourne 0 si compile OK, sinon != 0. */
        CHECK(rc == 0, label);
    }
    owl_unlink(TMP);

    /* Libère les buffers alloués. */
    for (int i = 0; i < nblocks; i++) {
        free(blocks[i]);
    }
}

/* ================================================================
 * owl icons — exploration du catalogue Iconify embarque.
 *
 * Tests in-process : meme pattern que les autres verbes doc. Tous
 * les rendus visuels passent par --ascii pour rester reproductibles
 * (les codes ANSI 24-bit du chemin par defaut sont fragiles selon
 * l'env terminal et le mode tty).
 *
 * Note : `save` est garanti present dans plusieurs sets du catalogue
 * (material-symbols, carbon, game-icons au moins). `mdi` est le 2e
 * set canonique. `mdj` est volontairement bidon pour le did-you-mean.
 * ================================================================ */

static void test_icons_aide_top(void) {
    int rc = capture_call(cli_doc_icons, 1, ARGS("icons"));
    CHECK_RC(rc, 0, "icons (sans verbe) rc=0");
    CHECK_HAS("list",    "icons aide mentionne 'list'");
    CHECK_HAS("search",  "icons aide mentionne 'search'");
    CHECK_HAS("preview", "icons aide mentionne 'preview'");
    CHECK_HAS("--set",   "icons aide mentionne --set");
}

static void test_icons_list_sans_set_refuse(void) {
    int rc = capture_call(cli_doc_icons, 2, ARGS("icons", "list"));
    CHECK_RC(rc, 1, "icons list sans --set rc=1 (refus usage)");
    /*  : on liste les 6 categories OWL, plus les 12 sets
     * Iconify (regle 7 — pas de marques tierces user-facing). */
    CHECK_HAS("ui",         "list sans set : liste categorie ui");
    CHECK_HAS("marques",    "list sans set : liste categorie marques");
    CHECK_HAS("classiques", "list sans set : liste categorie classiques");
    CHECK_HAS("icone",      "list sans set : mention du compteur 'icones'");
}

static void test_icons_list_avec_set_ok(void) {
    int rc = capture_call(cli_doc_icons, 6,
                          ARGS("icons", "list", "--set", "mdi", "--limit", "5"));
    CHECK_RC(rc, 0, "icons list --set mdi --limit 5 rc=0");
    /* Au moins une ligne avec un caractere de nom — on n'asserte pas un
     * nom precis (le set evolue) mais on verifie qu'il y a plusieurs
     * lignes de sortie. */
    CHECK(count_occurrences("\n") >= 3,
          "list --set mdi --limit 5 : au moins quelques lignes");
}

static void test_icons_list_set_inexistant(void) {
    int rc = capture_call(cli_doc_icons, 4,
                          ARGS("icons", "list", "--set", "mdj"));
    CHECK_RC(rc, 1, "icons list --set mdj rc=1");
    CHECK_HAS("mdj",              "list set inconnu : reprend la saisie");
    CHECK_HAS("mdi",              "list set inconnu : liste mdi disponible");
    /* did-you-mean : Levenshtein('mdj','mdi') = 1, doit suggerer. */
    CHECK(strstr(g_buf, "mdi") != NULL,
          "list set inconnu : did-you-mean propose 'mdi'");
}

static void test_icons_search_basique(void) {
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "search", "save"));
    CHECK_RC(rc, 0, "icons search save rc=0");
    /* Le format de sortie doit etre 'set/nom' pour disambiguer. */
    CHECK_HAS("save", "search save : mentionne le nom 'save'");
    CHECK_HAS("/",    "search save : separateur set/nom present");
}

static void test_icons_search_multi_mot_and(void) {
    /* `file` ET `pdf` doivent tous les deux figurer dans le nom retourne.
     * Au moins un set du catalogue a un `file-type-pdf` ou similaire.
     * `--limit 0` pour ne pas limiter et garantir qu'on capture une
     * entree pdf (les premieres centaines de matches file sont sans pdf
     * dans l'ordre catalogue trie par set_id puis name). */
    int rc = capture_call(cli_doc_icons, 6,
                          ARGS("icons", "search", "file", "pdf",
                               "--limit", "0"));
    CHECK_RC(rc, 0, "icons search file pdf rc=0");
    CHECK_HAS("pdf", "search file pdf : retourne au moins une entree avec 'pdf'");
}

/*  — Tests bilingue FR/EN sur owl icons search. */
static void test_icons_search_fr_maison(void) {
    /* Pre- : `search maison` ne retournait rien (catalogue EN).
     * Post : doit matcher des `home` / `house` via le mapping FR. */
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "search", "maison"));
    CHECK_RC(rc, 0, "icons search maison rc=0");
    CHECK_HAS("home", "search maison : matche au moins une entree home");
    /* Note : la summary "expansion FR/EN" sort en stderr (capture_call
     * teste stdout uniquement), donc pas verifiable ici. La preuve
     * fonctionnelle est qu'on matche `home` malgre une needle FR. */
}

static void test_icons_search_fr_coeur(void) {
    /* `coeur` doit matcher heart via mapping FR. */
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "search", "coeur"));
    CHECK_RC(rc, 0, "icons search coeur rc=0");
    CHECK_HAS("heart", "search coeur : matche heart via mapping FR");
}

static void test_icons_search_fr_accent(void) {
    /* `cafe` (sans accent) doit matcher coffee / espresso via mapping FR. */
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "search", "cafe"));
    CHECK_RC(rc, 0, "icons search cafe rc=0");
    CHECK_HAS("coffee", "search cafe : matche coffee via mapping FR");
}

static void test_icons_search_fallback_en(void) {
    /* `home` n'est PAS dans le mapping FR (cle FR = "maison"). Le matching
     * doit retomber sur le path direct EN (= ancien comportement). */
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "search", "home"));
    CHECK_RC(rc, 0, "icons search home rc=0");
    CHECK_HAS("home", "search home : fallback EN direct OK");
}

static void test_icons_search_sans_mot(void) {
    int rc = capture_call(cli_doc_icons, 2, ARGS("icons", "search"));
    CHECK_RC(rc, 1, "icons search sans mot rc=1");
    CHECK_HAS("search", "search sans mot : message d'usage mentionne 'search'");
}

static void test_icons_preview(void) {
    /*  : --ascii retire. Le rendu Unicode+ANSI est l'unique
     * mode preview. On verifie le snippet OWL + couleur_modifiable +
     * presence de demi-blocs UTF-8. */
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "preview", "save"));
    CHECK_RC(rc, 0, "icons preview save rc=0");
    CHECK_HAS("icones.save",
              "preview save : snippet OWL 'icones.save' present");
    CHECK_HAS("couleur_modifiable",
              "preview save : ligne couleur_modifiable presente");
    /* Demi-bloc UTF-8 ▀ = 0xE2 0x96 0x80 OU sequence ANSI escape. */
    CHECK(strstr(g_buf, "\xe2\x96\x80") != NULL ||
          strstr(g_buf, "\x1b[") != NULL,
          "preview save : rendu Unicode demi-blocs ou ANSI present");
}

static void test_icons_preview_inexistant(void) {
    int rc = capture_call(cli_doc_icons, 3,
                          ARGS("icons", "preview", "zxqnotanicone"));
    CHECK_RC(rc, 2, "icons preview <inexistant> rc=2");
    CHECK_HAS("zxqnotanicone",
              "preview inexistant : reprend la saisie pour situer l'erreur");
}

static void test_icons_preview_size_hors_borne(void) {
    /* size < 16 : refuse usage. */
    int rc = capture_call(cli_doc_icons, 5,
                          ARGS("icons", "preview", "save", "--size", "12"));
    CHECK_RC(rc, 1, "icons preview --size 12 rc=1 (hors borne basse)");

    /* size > 128 : refuse usage. */
    rc = capture_call(cli_doc_icons, 5,
                      ARGS("icons", "preview", "save", "--size", "999"));
    CHECK_RC(rc, 1, "icons preview --size 999 rc=1 (hors borne haute)");
}

static void test_icons_complete_top_inclut_icons(void) {
    /* Le backend completion top-level doit lister 'icons'. */
    int rc = capture_call(cli_doc_complete, 1, ARGS("__complete"));
    CHECK_RC(rc, 0, "__complete top rc=0");
    CHECK_HAS("icons\n", "complete top : 'icons' present");
}

static void test_icons_complete_position_icons(void) {
    /* `owl icons <TAB>` doit proposer les 3 sous-verbes. */
    int rc = capture_call(cli_doc_complete, 2,
                          ARGS("__complete", "icons"));
    CHECK_RC(rc, 0, "__complete icons rc=0");
    CHECK_HAS("list\n",    "complete icons : list");
    CHECK_HAS("search\n",  "complete icons : search");
    CHECK_HAS("preview\n", "complete icons : preview");
}

/* ================================================================
 *  — enum touche, allowedBase plain text, --cat général
 * ================================================================ */

/* Volet 1 : enum touche doit exister dans le registry et être explorable.
 *
 * Pré-impl : `owl explain touche` → rc=2 (symbole inconnu).
 * Post-impl attendu : rc=0, section Valeurs avec au moins tkHaut/tkF1/teShift.
 *
 * ROUGE attendu : enum touche inexistant. */
static void test_explain_enum_touche(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "touche"));
    CHECK_RC(rc, 0, "explain touche rc=0");
    CHECK_HAS("touche",  "fiche affiche le nom de l'enum");
    CHECK_HAS("tkHaut",  "enum touche liste tkHaut");
    CHECK_HAS("tkF1",    "enum touche liste tkF1");
    CHECK_HAS("teShift", "enum touche liste teShift");
    CHECK_HAS("Clavier", "enum touche catégorie Clavier");
}

/*  — `owl list enums` n'est plus une catégorie navigable : les enums
 * sont un concept interne, accessibles via références (explain) mais pas comme
 * une liste de catégorie à part. Le dispatch doit rejeter la sous-commande
 * (rc != 0) et ne PAS lister d'enum.
 *
 * Le menu `owl list` (sans argument) ne doit plus mentionner "enums".
 *
 * ROUGE attendu pré-impl : `list enums` résout encore (rc=0, liste touche). */
static void test_list_enums_retire_categorie(void) {
    int rc = capture_call(cli_doc_list, 2, ARGS("list", "enums"));
    CHECK(rc != 0, "list enums n'est plus une catégorie reconnue (rc != 0)");
    CHECK_NO("touche", "list enums ne liste plus l'enum touche");

    /* Le menu d'aide (owl list sans argument) ne propose plus 'enums'. */
    int rc2 = capture_call(cli_doc_list, 1, ARGS("list"));
    CHECK_RC(rc2, 0, "list (menu) rc=0");
    CHECK_NO("owl list enums", "menu list ne propose plus 'enums'");

    /* Reachability préservée : l'enum reste résolvable via explain. */
    int rc3 = capture_call(cli_doc_explain, 2, ARGS("explain", "touche"));
    CHECK_RC(rc3, 0, "explain touche résout encore l'enum (reachability)");
    CHECK_HAS("tkHaut", "explain touche affiche toujours les valeurs de l'enum");
}

/* Volet 2 : explain EnvoieTouche doit afficher le lien enum touche sur le
 * paramètre `touche`. Format attendu : `∈ touche` dans la section Signatures
 * (pattern identique aux fields de type composé qui utilisent allowedBase).
 *
 * ROUGE attendu : allowedBase non déclaré sur le param + enum absent. */
static void test_explain_envoie_touche_allowedbase(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "EnvoieTouche"));
    CHECK_RC(rc, 0, "explain EnvoieTouche rc=0");
    /* Le paramètre `touche` doit afficher l'enum source.
     * `∈` = U+2208 encodé en UTF-8 : E2 88 88. */
    CHECK_HAS("\xe2\x88\x88 touche",
              "EnvoieTouche : param touche affiche ∈ touche (allowedBase)");
    /* La mention de Asc() doit figurer quelque part (desc ou note). */
    CHECK_HAS("Asc(",
              "EnvoieTouche : mention Asc() presente");
}

/* Volet 2 (bis) : même vérification pour ToucheEnfoncée.
 *
 * ROUGE attendu : idem. */
static void test_explain_touche_enfoncee_allowedbase(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "ToucheEnfoncee"));
    CHECK_RC(rc, 0, "explain ToucheEnfoncee rc=0");
    CHECK_HAS("\xe2\x88\x88 touche",
              "ToucheEnfoncee : param touche affiche ∈ touche (allowedBase)");
    CHECK_HAS("Asc(",
              "ToucheEnfoncee : mention Asc() presente");
}

/* Volet 3 : `list consts --cat Clavier` doit filtrer sur la catégorie.
 *
 * Pré-impl : sans filtrage, toutes les 695+ constantes sont listées —
 * tkHaut est perdu dans la masse, et httpGet/sscDroite apparaissent.
 * Post-impl : seules les constantes de catégorie Clavier sont listées.
 *
 * ROUGE attendu : filtre --cat non implémenté sur list_constants. */
static void test_list_consts_cat_clavier(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "consts", "--cat", "Clavier"));
    CHECK_RC(rc, 0, "list consts --cat Clavier rc=0");
    CHECK_HAS("tkHaut",   "filtre Clavier contient tkHaut");
    CHECK_HAS("tkF1",     "filtre Clavier contient tkF1");
    CHECK_HAS("teShift",  "filtre Clavier contient teShift");
    /* Les constantes hors-Clavier ne doivent PAS apparaître. */
    CHECK_NO("httpGet",   "filtre Clavier exclut httpGet (HTTP)");
    CHECK_NO("sscDroite", "filtre Clavier exclut sscDroite (Chaines)");
}

/* Volet 3 (bis) : `list consts --cat HTTP` — preuve de généricité du filtre.
 *
 * ROUGE attendu : idem. */
static void test_list_consts_cat_http(void) {
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "consts", "--cat", "HTTP"));
    CHECK_RC(rc, 0, "list consts --cat HTTP rc=0");
    CHECK_HAS("httpGet",  "filtre HTTP contient httpGet");
    /* tkHaut est en catégorie Clavier — ne doit pas apparaître ici. */
    CHECK_NO("tkHaut",    "filtre HTTP exclut tkHaut (Clavier)");
}

/*  — convention casse enums : 13 noms d'enums passent en
 * minuscule-first (FR canon + nameEN), cohérence avec les 56 autres enums
 * déjà minuscule-first et distinction visuelle de l'enum `couleur` vs le
 * type `Couleur`. Le runtime reste casse-insensible ; ce test verrouille la
 * forme canon AFFICHÉE (registry → explain --json / list enums).
 *
 * ROUGE attendu pré-impl : explain renvoie encore "name": "Forme" (Capital). */

/* Helper local : explain <sym> --json puis vérifie la forme canon
 * minuscule-first du name FR et du name_en de l'enum. La forme minuscule-first
 * ne peut provenir que du bucket enum (aucun type/propriété/native homonyme
 * n'est minuscule-first), donc la présence positive suffit comme verrou —
 * pas de CHECK_NO global (multi-bucket : un type/prop Capital homonyme reste
 * légitimement présent). */
static void check_enum_canon_json(const char* sym,
                                  const char* expectName,
                                  const char* expectNameEN,
                                  const char* label) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", (char*)sym, "--json"));
    CHECK_RC(rc, 0, label);
    CHECK_HAS("\"kind\": \"enum\"", label);
    char needleFR[160], needleEN[160];
    snprintf(needleFR, sizeof needleFR, "\"name\": \"%s\"", expectName);
    snprintf(needleEN, sizeof needleEN, "\"name_en\": \"%s\"", expectNameEN);
    CHECK_HAS(needleFR, label);
    CHECK_HAS(needleEN, label);
}

/* ================================================================
 *  — export du catalogue couleurs (nom FR/EN → #RRGGBB) pour
 * --vscode-data. On teste directement l'accesseur + la conversion
 * BGR→RGB (logique porteuse), pas le rendu JSON (vérifié via snapshot).
 * ================================================================ */

/* Retourne l'entrée du catalogue couleur dont le nom FR == frName, NULL
 * si absente. */
static const OwlColorInfo* find_color_fr(const char* frName) {
    int n = owl_color_catalog_count();
    for (int i = 0; i < n; i++) {
        const OwlColorInfo* c = owl_color_catalog_at(i);
        if (c && c->nameFR && strcmp(c->nameFR, frName) == 0) return c;
    }
    return NULL;
}

static void check_color_hex(const char* frName, const char* expectEN,
                            const char* expectHex, const char* label) {
    const OwlColorInfo* c = find_color_fr(frName);
    CHECK(c != NULL, label);
    if (!c) return;
    CHECK(c->sentinel == false, label);
    CHECK(c->nameEN != NULL && strcmp(c->nameEN, expectEN) == 0, label);
    char hex[16] = {0};
    bool ok = owl_color_bgr_to_html(c->value, hex, sizeof hex);
    CHECK(ok == true, label);
    CHECK(strcmp(hex, expectHex) == 0, label);
}

static void test_colors_catalog_export(void) {
    /* Couverture complète : au moins les 44 constantes couleur canon. */
    CHECK(owl_color_catalog_count() >= 44, "colors: catalogue peuplé");

    /* Primaires pleines : BGR stocké → RGB #RRGGBB (Rouge=255 → #FF0000). */
    check_color_hex("Rouge", "Red",   "#FF0000", "colors: Rouge #FF0000");
    check_color_hex("Vert",  "Green", "#00FF00", "colors: Vert #00FF00");
    check_color_hex("Bleu",  "Blue",  "#0000FF", "colors: Bleu #0000FF");

    /* Couleur nommée dérivée (RougeClair partage la valeur de Rouge). */
    check_color_hex("RougeClair", "LightRed", "#FF0000",
                    "colors: RougeClair #FF0000");
    /* Une couleur non-primaire pour verrouiller la décomposition des 3 octets. */
    check_color_hex("Blanc", "White", "#FFFFFF", "colors: Blanc #FFFFFF");
    check_color_hex("Noir",  "Black", "#000000", "colors: Noir #000000");

    /* Sentinelles : pas de RGB fixe → sentinel=true, conversion refusée. */
    const OwlColorInfo* cdef = find_color_fr("Couleur" "D\xc3\xa9" "faut");
    CHECK(cdef != NULL, "colors: CouleurDéfaut présente");
    if (cdef) {
        CHECK(cdef->sentinel == true, "colors: CouleurDéfaut sentinelle");
        char hex[16] = {0};
        CHECK(owl_color_bgr_to_html(cdef->value, hex, sizeof hex) == false,
              "colors: CouleurDéfaut sans hex");
    }
    const OwlColorInfo* csys = find_color_fr("iSyst\xc3\xa8me");
    CHECK(csys != NULL, "colors: iSystème présente");
    if (csys) {
        CHECK(csys->sentinel == true, "colors: iSystème sentinelle");
        CHECK(csys->nameEN != NULL && strcmp(csys->nameEN, "iSystem") == 0,
              "colors: iSystème nameEN");
    }
    /* Transparent : négatif → sentinelle également (pas de #RRGGBB). */
    const OwlColorInfo* ctr = find_color_fr("Transparent");
    CHECK(ctr != NULL, "colors: Transparent présente");
    if (ctr) CHECK(ctr->sentinel == true, "colors: Transparent sentinelle");
}

static void test_enums_casse_minuscule_first(void) {
    /* FR canon attendu (minuscule-first), nameEN attendu (minuscule-first). */
    check_enum_canon_json("Couleur",      "couleur",      "color",
                          "enum couleur minuscule-first");
    check_enum_canon_json("Forme",        "forme",        "shape",
                          "enum forme minuscule-first");
    check_enum_canon_json("TypeBouton",   "typeBouton",   "buttonType",
                          "enum typeBouton minuscule-first");
    check_enum_canon_json("TypeLibelle",  "typeLibell\xc3\xa9", "labelType",
                          "enum typeLibellé minuscule-first");
    check_enum_canon_json("TypeSaisie",   "typeSaisie",   "inputType",
                          "enum typeSaisie minuscule-first");
    check_enum_canon_json("CadrageH",     "cadrageH",     "horizontalAlignment",
                          "enum cadrageH minuscule-first");
    check_enum_canon_json("CadrageV",     "cadrageV",     "verticalAlignment",
                          "enum cadrageV minuscule-first");
    check_enum_canon_json("Cadrage",      "cadrage",      "imageFraming",
                          "enum cadrage minuscule-first");
    check_enum_canon_json("Etat",         "etat",         "fieldState",
                          "enum etat minuscule-first");
    /* L'enum etat expose exactement 4 valeurs distinctes ; Inactif reste une
     * constante compilable (alias d'AffichageSeulement, meme sentinel) mais
     * n'apparait PAS comme valeur exposee (explain / completion). */
    {
        int rc = capture_call(cli_doc_explain, 3,
                              ARGS("explain", "etat", "--json"));
        CHECK_RC(rc, 0, "explain etat --json rc");
        CHECK_HAS("\"Actif\"", "etat values: Actif expose");
        CHECK_HAS("\"AffichageSeulement\"", "etat values: AffichageSeulement expose");
        CHECK_HAS("\"Gris\xc3\xa9\"", "etat values: Grise expose");
        CHECK_HAS("\"Invisible\"", "etat values: Invisible expose");
        CHECK_NO("\"Inactif\"", "etat values: Inactif PAS expose (alias en prose)");
    }
    check_enum_canon_json("Disposition",  "disposition",  "layout",
                          "enum disposition minuscule-first");
    check_enum_canon_json("Alignement",   "alignement",   "alignment",
                          "enum alignement minuscule-first");
    check_enum_canon_json("ActionSouris", "actionSouris", "mouseAction",
                          "enum actionSouris minuscule-first");
    /* ThèmeSnapshot : seul le FR change (nameEN snapshotTheme déjà ok). */
    check_enum_canon_json("ThemeSnapshot", "th\xc3\xa8meSnapshot", "snapshotTheme",
                          "enum thèmeSnapshot minuscule-first");
    /*  : le verrou de forme canon passe désormais uniquement par
     * explain --json (la catégorie `list enums` a été retirée). */
}

/* ================================================================
 *  — coloration des membres d'un littéral { ... }
 *
 * Le tokenizer OWL des blocs de code (render_code_line_owl) est par-ligne.
 * À l'intérieur d'un littéral `{ ... }`, le nom de field à gauche d'un `=`
 * doit sortir en vert foncé membre (C_PROC) au lieu de tomber sur la
 * classification registry (qui sortait `Couleur`/`Type` en violet type).
 * Les valeurs (RHS) gardent leur coloration normale.
 *
 * On exerce le pipeline RÉEL via `owl learn litterale` couleurs FORCÉES :
 * la fiche 17_init_litterale.md contient le littéral Cadre de référence.
 * ================================================================ */

/* Snapshots des séquences ANSI prises PENDANT que les couleurs sont forcées.
 * Les macros C_PROC/C_TYPE retournent "" hors mode couleur ; il faut donc
 * capturer leur valeur active avant le repli plain de capture_call_colored,
 * sinon les needles se réduisent au texte nu et les assertions deviennent
 * trivialement vraies (piège du test vert par construction). */
static char g_col_proc[32];
static char g_col_type[32];

/* Variante de capture_call qui FORCE les couleurs (séquences ANSI émises),
 * pour pouvoir assertionner sur la coloration effective d'un token. Sinon
 * identique à capture_call (cf. son commentaire pour le pattern dup/dup2). */
static int capture_call_colored(int (*fn)(int, char**), int argc, char** argv) {
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_cli_test_col_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return -1;

    int tmp_fd = owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return -1; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd); owl_close(saved_fd); return -1;
    }
    owl_close(tmp_fd);

    cli_colors_force(CLI_COLOR_FORCE);
    /* Snapshot des codes actifs AVANT le repli plain plus bas. */
    snprintf(g_col_proc, sizeof(g_col_proc), "%s", C_PROC);
    snprintf(g_col_type, sizeof(g_col_type), "%s", C_TYPE);

    int rc = fn(argc, argv);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    /* Repli sur plain pour ne pas contaminer les captures suivantes. */
    cli_colors_force(CLI_COLOR_PLAIN);

    capture_slurp(TMP, "capture_call_colored");
    owl_unlink(TMP);
    return rc;
}

static void test_learn_litterale_membres_vert_fonce(void) {
    /* --color : cli_doc_learn → parse_doc_opts ré-applique le mode couleur
     * selon les flags ; sans --color il retomberait en PLAIN (pas de tty) et
     * le rendu serait sans séquences ANSI, rendant les needles introuvables. */
    int rc = capture_call_colored(cli_doc_learn, 3,
                                  ARGS("learn", "litterale", "--color"));
    CHECK_RC(rc, 0, "learn litterale rc=0");

    const char* proc = g_col_proc;   /* vert foncé membre (snapshot forcé) */
    const char* type = g_col_type;   /* violet type/const (snapshot forcé) */
    /* Garde-fou anti-test-trivial : les snapshots doivent être non vides,
     * sinon les needles ci-dessous dégénèrent en texte nu. */
    CHECK(proc[0] != '\0' && type[0] != '\0',
          "snapshots couleur non vides (couleurs bien forcées)");
    char needle[128];

    /* Membres du littéral → vert foncé (C_PROC immédiatement avant le nom). */
    #define MEMBER_GREEN(name, label) do {                                  \
        snprintf(needle, sizeof(needle), "%s%s", proc, (name));             \
        CHECK_HAS(needle, label);                                           \
    } while (0)

    MEMBER_GREEN("Couleur",        "membre Couleur en vert foncé");
    MEMBER_GREEN("CoinHautGauche", "membre CoinHautGauche en vert foncé");
    MEMBER_GREEN("Largeur",        "membre Largeur en vert foncé");
    MEMBER_GREEN("Hauteur",        "membre Hauteur en vert foncé");
    MEMBER_GREEN("TraitBas",       "membre TraitBas en vert foncé");
    MEMBER_GREEN("Epaisseur",      "membre Epaisseur en vert foncé");
    MEMBER_GREEN("Type",           "membre Type en vert foncé");
    #undef MEMBER_GREEN

    /* Valeurs (RHS) → PAS en vert foncé membre. BleuFoncé/RougeFoncé sont
     * des constantes couleur ; TraitTiret une valeur d'enum. Aucune ne doit
     * être précédée de C_PROC. */
    snprintf(needle, sizeof(needle), "%sBleuFonc", proc);
    CHECK_NO(needle, "valeur BleuFoncé pas en vert foncé membre");
    snprintf(needle, sizeof(needle), "%sRougeFonc", proc);
    CHECK_NO(needle, "valeur RougeFoncé pas en vert foncé membre");
    snprintf(needle, sizeof(needle), "%sTraitTiret", proc);
    CHECK_NO(needle, "valeur TraitTiret pas en vert foncé membre");

    /* Hors `{}` : `Cadre` (type cible, avant l'ouverture de l'accolade)
     * reste violet type — le tracking d'accolades ne doit pas le toucher. */
    snprintf(needle, sizeof(needle), "%sCadre", type);
    CHECK_HAS(needle, "type Cadre hors-{} reste violet");

    /* Hors `{}` : enum combinable côté valeur (Bords = BordHaut + BordBas)
     * — BordHaut est une valeur, pas un membre, donc pas C_PROC. */
    snprintf(needle, sizeof(needle), "%sBordHaut", proc);
    CHECK_NO(needle, "valeur BordHaut pas en vert foncé membre");
}

/* ================================================================
 *  — opérateurs de base explain-ables + desc dans list
 *
 * Lot 1 : les opérateurs arithmétiques (+, -, *, /, ^) et de comparaison
 * (<, >, <=, >=, <>) doivent être ajoutés au catalogue OWL_OPERATOR_DEFS.
 * Pré-impl : explain "+" → rc=2 (symbole inconnu).
 * Post-impl : rc=0, fiche riche (Syntaxe + Exemple).
 *
 * Lot 2 : list operators (CLI) et --json (MCP) affichent une description
 * courte par opérateur.
 * Pré-impl : la sortie text n'a aucune desc, le JSON n'a pas de champ desc.
 *
 * ROUGE ATTENDU : tous ces tests jusqu'à l'implémentation.
 * ================================================================ */

/* Lot 1a : explain opérateurs de base — présence des marqueurs structurels.
 *
 * On ne teste PAS le wording exact des descriptions (l'implémenteur les
 * aligne sur 05_operateurs.md — Règle 1). On teste la STRUCTURE :
 * - rc=0 (plus de "symbole inconnu")
 * - présence de "Syntaxe" (marqueur de section, émis par print_keyword_sense_text)
 * - présence de "Exemple" (idem)
 * - présence du symbole lui-même dans la sortie (header fiche)
 *
 * ROUGE ATTENDU pré-impl : rc=2 ("symbole inconnu") pour chaque op de base. */

static void test_explain_op_plus(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "+"));
    CHECK_RC(rc, 0, "explain '+' rc=0 (plus symbole inconnu)");
    CHECK_HAS("Syntaxe",  "explain '+' : section Syntaxe présente");
    CHECK_HAS("Exemple",  "explain '+' : section Exemple présente");
    CHECK_HAS("+",        "explain '+' : symbole dans la fiche");
}

static void test_explain_op_mul(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "*"));
    CHECK_RC(rc, 0, "explain '*' rc=0");
    CHECK_HAS("Syntaxe",  "explain '*' : section Syntaxe présente");
    CHECK_HAS("Exemple",  "explain '*' : section Exemple présente");
}

static void test_explain_op_ineq(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "<>"));
    CHECK_RC(rc, 0, "explain '<>' rc=0");
    CHECK_HAS("Syntaxe",  "explain '<>' : section Syntaxe présente");
    CHECK_HAS("Exemple",  "explain '<>' : section Exemple présente");
    CHECK_HAS("<>",       "explain '<>' : symbole dans la fiche");
}

static void test_explain_op_lte(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "<="));
    /* <= est déjà dans le catalogue spéciaux (sens Affectation).
     * Post-impl : doit aussi porter le sens Comparaison ≤.
     * On vérifie simplement que rc=0 et qu'une Syntaxe est présente. */
    CHECK_RC(rc, 0, "explain '<=' rc=0");
    CHECK_HAS("Syntaxe",  "explain '<=' : section Syntaxe présente");
}

/* Lot 1b : explain sur les autres ops arithmétiques et de comparaison.
 * Moins verbeux : rc=0 + marqueur Syntaxe suffisent pour chaque. */

static void test_explain_op_base_famille_arithmetique(void) {
    int rc;

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "-"));
    CHECK_RC(rc, 0, "explain '-' rc=0");
    CHECK_HAS("Syntaxe", "explain '-' : Syntaxe présente");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "/"));
    CHECK_RC(rc, 0, "explain '/' rc=0");
    CHECK_HAS("Syntaxe", "explain '/' : Syntaxe présente");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "^"));
    CHECK_RC(rc, 0, "explain '^' rc=0");
    CHECK_HAS("Syntaxe", "explain '^' : Syntaxe présente");
}

static void test_explain_op_base_famille_comparaison(void) {
    int rc;

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "<"));
    CHECK_RC(rc, 0, "explain '<' rc=0");
    CHECK_HAS("Syntaxe", "explain '<' : Syntaxe présente");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", ">"));
    CHECK_RC(rc, 0, "explain '>' rc=0");
    CHECK_HAS("Syntaxe", "explain '>' : Syntaxe présente");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", ">="));
    CHECK_RC(rc, 0, "explain '>=' rc=0");
    CHECK_HAS("Syntaxe", "explain '>=' : Syntaxe présente");
}

/* Lot 2a : list operators texte — desc une ligne par opérateur.
 *
 * Pré-impl : la sortie affiche le symbole seul (+ optionnel "(N sens)"),
 * sans description. Post-impl : une desc courte suit chaque symbole.
 *
 * On teste que `+` apparaît dans la sortie ET que la ligne correspondante
 * n'est pas réduite au seul symbole paddé (= qu'il y a du texte après).
 * Méthode : count_occurrences de `+` dans le listing (au moins 1) et
 * CHECK_HAS d'un marqueur de structure `desc` sur la zone totale.
 *
 * On ne hardcode PAS le texte des descriptions (celui-là c'est le boulot
 * de l'implémenteur, aligné sur 05_operateurs.md).
 *
 * ROUGE ATTENDU pré-impl : listing sans desc → les asserts de longueur de
 * ligne rougissent. */

/* Cherche la première occurrence de `needle` dans g_buf et retourne un
 * pointeur vers la fin de la ligne (ou NULL si absent). Permet de mesurer
 * si la ligne contenant le symbole a plus que N chars après le symbole. */
static const char* line_after_first(const char* needle) {
    const char* p = strstr(g_buf, needle);
    if (!p) return NULL;
    /* Avance jusqu'à la fin de la ligne. */
    const char* eol = strchr(p, '\n');
    return eol ? eol : g_buf + strlen(g_buf);
}

static void test_list_operators_desc_present_texte(void) {
    int rc = capture_call(cli_doc_list, 2, ARGS("list", "operators"));
    CHECK_RC(rc, 0, "list operators rc=0");

    /* +, *, <>, <= doivent apparaître dans le listing. */
    CHECK(count_occurrences("+") >= 1,  "list operators texte : '+' présent");
    CHECK(count_occurrences("*") >= 1,  "list operators texte : '*' présent");
    CHECK(count_occurrences("<>") >= 1, "list operators texte : '<>' présent");

    /* La ligne contenant '+' doit avoir plus de 12 chars après le symbole
     * (8 de padding + quelque chose = desc non vide).
     * Avant impl : la ligne est " +       \n" (padding seul) — longueur ~10.
     * Après impl : " +       Addition…\n" — longueur >> 20. */
    const char* eol_plus = line_after_first("+");
    CHECK(eol_plus != NULL, "list operators texte : ligne '+' trouvable");
    if (eol_plus) {
        const char* start_plus = strstr(g_buf, "+");
        CHECK((int)(eol_plus - start_plus) > 14,
              "list operators texte : ligne '+' a une desc (longueur > 14 chars)");
    }

    /* Idem pour <> */
    const char* eol_ineq = line_after_first("<>");
    CHECK(eol_ineq != NULL, "list operators texte : ligne '<>' trouvable");
    if (eol_ineq) {
        const char* start_ineq = strstr(g_buf, "<>");
        CHECK((int)(eol_ineq - start_ineq) > 14,
              "list operators texte : ligne '<>' a une desc (longueur > 14 chars)");
    }
}

/* Lot 2b : list operators --json — champ "desc" présent par opérateur.
 *
 * Pré-impl : JSON = { "symbol": "...", "category": "...", "senses_count": N }
 * Post-impl : JSON = { "symbol": "...", "category": "...", "senses_count": N, "desc": "..." }
 *
 * On vérifie : présence de la clé "desc": au moins une fois dans le JSON
 * (signale que le serialiseur l'émet), et validité JSON (accolades équilibrées).
 *
 * ROUGE ATTENDU pré-impl : aucun champ "desc" → CHECK_HAS rougit. */

static void test_list_operators_desc_json(void) {
    int rc = capture_call(cli_doc_list, 3, ARGS("list", "operators", "--json"));
    CHECK_RC(rc, 0, "list operators --json rc=0");
    CHECK_HAS("\"desc\":",    "list operators JSON : champ desc présent");
    /* Sanity : les autres champs existants restent. */
    CHECK_HAS("\"symbol\":",  "list operators JSON : champ symbol présent");
    CHECK_HAS("\"category\":", "list operators JSON : champ category présent");
    CHECK(g_buf[0] == '{',    "list operators JSON : commence par {");
    CHECK(json_buf_is_balanced(), "list operators JSON : JSON équilibré");
}

/* Lot 3 : explain operator --json — "desc" présent dans les senses.
 *
 * Le champ desc est déjà émis par explain_operator (le code C l'émet pour
 * chaque sense via type_to_lang). Ce test vérifie que les NOUVEAUX opérateurs
 * de base l'ont aussi (pas seulement les 17 spéciaux qui avaient déjà des senses).
 *
 * ROUGE ATTENDU pré-impl : explain '+' --json → rc=2 (symbole inconnu). */

static void test_explain_op_plus_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "+", "--json"));
    CHECK_RC(rc, 0, "explain '+' --json rc=0");
    CHECK_HAS("\"kind\": \"operator\"", "explain '+' JSON : kind=operator");
    CHECK_HAS("\"desc\":",              "explain '+' JSON : champ desc dans sense");
    CHECK_HAS("\"syntax\":",            "explain '+' JSON : champ syntax dans sense");
    CHECK_HAS("\"example\":",           "explain '+' JSON : champ example dans sense");
    CHECK(json_buf_is_balanced(),       "explain '+' JSON : JSON équilibré");
}

static void test_explain_op_ineq_json(void) {
    int rc = capture_call(cli_doc_explain, 3, ARGS("explain", "<>", "--json"));
    CHECK_RC(rc, 0, "explain '<>' --json rc=0");
    CHECK_HAS("\"kind\": \"operator\"", "explain '<>' JSON : kind=operator");
    CHECK_HAS("\"desc\":",              "explain '<>' JSON : desc présent");
    CHECK(json_buf_is_balanced(),       "explain '<>' JSON : équilibré");
}

/* Lot 4 : régression — les 17 opérateurs spéciaux existants restent
 * explain-ables et listés.
 *
 * Pré-impl : ces tests doivent passer VERT (régression guard).
 * Post-impl : doivent toujours passer vert après l'ajout des ops de base.
 *
 * On teste un échantillon représentatif : ??, [~~, ~=, <- (un par catégorie). */

static void test_regression_ops_speciaux_explain(void) {
    int rc;

    /* Null-safe ?? (déjà présent, Syntaxe documentée) */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "??"));
    CHECK_RC(rc, 0, "regression: explain null-safe rc=0 (op special preserve)");
    CHECK_HAS("Syntaxe", "regression: explain null-safe a Syntaxe");
    CHECK_HAS("Exemple", "regression: explain null-safe a Exemple");

    /* Comparaison chaîne [~~ */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "[~~"));
    CHECK_RC(rc, 0, "regression: explain '[~~' rc=0");
    CHECK_HAS("Syntaxe", "regression: explain '[~~' a Syntaxe");

    /* Égalité souple ~= */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "~="));
    CHECK_RC(rc, 0, "regression: explain '~=' rc=0");
    CHECK_HAS("Syntaxe", "regression: explain '~=' a Syntaxe");

    /* Affectation <- */
    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "<-"));
    CHECK_RC(rc, 0, "regression: explain '<-' rc=0");
    CHECK_HAS("Syntaxe", "regression: explain '<-' a Syntaxe");
}

static void test_regression_ops_speciaux_listes(void) {
    int rc = capture_call(cli_doc_list, 2, ARGS("list", "operators"));
    CHECK_RC(rc, 0, "regression: list operators rc=0");
    /* Les 17 spéciaux existants restent dans le listing. */
    CHECK_HAS("??",   "regression: list operators contient op null-safe");
    CHECK_HAS("[~~",  "regression: list operators contient '[~~'");
    CHECK_HAS("~=",   "regression: list operators contient '~='");
    CHECK_HAS("<-",   "regression: list operators contient '<-'");
}

/* Lot 4b : ET/OU (mots-clés) restent explain-ables, inchangés. */
static void test_regression_et_ou_keywords(void) {
    int rc;

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "ET"));
    CHECK_RC(rc, 0, "regression: explain 'ET' rc=0 (mot-clé)");
    CHECK_HAS("(mot-cl", "regression: ET affiché comme mot-clé");

    rc = capture_call(cli_doc_explain, 2, ARGS("explain", "OU"));
    CHECK_RC(rc, 0, "regression: explain 'OU' rc=0 (mot-clé)");
    CHECK_HAS("(mot-cl", "regression: OU affiché comme mot-clé");
}

/* ================================================================
 *  — signatures à préfixe de params partagé bornées par l'arité
 *
 * Plusieurs signatures d'arités différentes peuvent référencer le MÊME
 * tableau OwlParam (idiome sain « préfixe partagé », ex sig_floor :
 * arité 1 et arité 2 pointent p_round[n, décimales]). Le renderer humain
 * doit tronquer chaque forme à son arité : la forme 1-arg n'affiche QUE
 * `n`, pas `n, décimales`.
 *
 * ROUGE ATTENDU pré-fix : la forme 1-arg affiche `n, décimales` (le loop
 * va jusqu'au terminateur {0} sans borner). Donc la forme courte
 * `ArrondiInférieur(n : numérique)` (parenthèse juste après le type)
 * n'existe pas, et le JSON compte 2 occurrences de `"name": "décimales"`.
 * ================================================================ */

static void test_explain_shared_prefix_arity_texte(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "ArrondiInférieur"));
    CHECK_RC(rc, 0, "explain ArrondiInférieur rc=0");
    /* Forme 1-arg : `n` SEUL, parenthèse fermante juste après le type.
     * Ce substring est absent pré-fix (la forme 1-arg montrait décimales). */
    CHECK_HAS("ArrondiInférieur(n : numérique)",
              "forme 1-arg n'affiche que n (pas n, décimales)");
    /* Forme 2-arg : toujours présente (non-régression du préfixe partagé). */
    CHECK_HAS("ArrondiInférieur(n : numérique, décimales : entier)",
              "forme 2-arg affiche n + décimales");
}

static void test_explain_shared_prefix_arity_json(void) {
    /* ArrondiInférieur : sig arité 1 (n) + arité 2 (n, décimales), même
     * tableau p_round. Post-fix : décimales n'apparaît QUE dans la forme
     * 2-arg → exactement 1 occurrence. Pré-fix : 2. */
    int rc = capture_call(cli_doc_explain, 3,
                          ARGS("explain", "ArrondiInférieur", "--json"));
    CHECK_RC(rc, 0, "explain ArrondiInférieur --json rc=0");
    CHECK(count_occurrences("\"name\": \"décimales\"") == 1,
          "ArrondiInférieur JSON : décimales dans la seule forme 2-arg");
    /* n est dans les deux formes : 2 occurrences (stable avant/après). */
    CHECK(count_occurrences("\"name\": \"n\"") == 2,
          "ArrondiInférieur JSON : n dans les 2 formes");
    CHECK(json_buf_is_balanced(), "ArrondiInférieur JSON équilibré");

    /* Arrondi : 3 formes (arité 3 [n, décimales, mode] via p_round3,
     * arité 1 [n] et arité 2 [n, décimales] via p_round). Post-fix :
     * décimales dans arité 2 + arité 3 = 2 occurrences (pré-fix : 3).
     * mode reste à 1 (seul p_round3 le porte, indépendant du bug). */
    rc = capture_call(cli_doc_explain, 3, ARGS("explain", "Arrondi", "--json"));
    CHECK_RC(rc, 0, "explain Arrondi --json rc=0");
    CHECK(count_occurrences("\"name\": \"décimales\"") == 2,
          "Arrondi JSON : décimales dans formes 2-arg et 3-arg (pas 1-arg)");
    CHECK(count_occurrences("\"name\": \"mode\"") == 1,
          "Arrondi JSON : mode dans la seule forme 3-arg");
    CHECK(json_buf_is_balanced(), "Arrondi JSON équilibré");
}

/*  — non-régression variadique : un OwlSignature arity == -1
 * (ex Trace) ne doit PAS perdre ses params nommés sous la borne d'arité
 * (la borne ne s'applique qu'aux arités >= 0). Trace n'a pas de param
 * nommé mais Min/Max (variadiques avec params a/b) si : on vérifie que la
 * forme variadique liste bien ses params déclarés + le marqueur `...`. */
static void test_explain_variadic_params_preserves(void) {
    int rc = capture_call(cli_doc_explain, 2, ARGS("explain", "Min"));
    CHECK_RC(rc, 0, "explain Min rc=0");
    /* Min(a, b, ...) variadique : params a et b conservés malgré arity<0. */
    CHECK_HAS("a : numérique", "Min variadique conserve le param a");
    CHECK_HAS("...",           "Min variadique affiche le marqueur ...");
}

/* ================================================================
 *  — page "Nouveautés" : contrat du diff baseline vs courant
 *
 * Cœur logique testable, exposé (à câbler par l'implementer) via une
 * fonction C pure, sans I/O, sans dépendance registry — le test fournit
 * lui-même les deux ensembles.
 *
 * CONTRAT IMPOSÉ à l'implementer (déclaration attendue dans cli_doc.h,
 * définition dans cli_doc.c) :
 *
 *   typedef struct {
 *       const char* kind;  // "native" | "type" | "enum" | "const" |
 *                          // "widget" | "reference" (chapitre
 *                          // documentation_public émis via
 *                          // EMBEDDED_PUBLIC_DOCS)
 *       const char* name;  // nom canonique du symbole, ou id de
 *                          // chapitre (ex: "31_nouveau_chapitre")
 *   } OwlDocFicheId;
 *
 *   // Nouveautés = fiches présentes dans `current` et ABSENTES de
 *   // `baseline` (comparaison exacte kind+name — les deux jeux sont
 *   // déjà sous forme canonique, aucune normalisation à faire ici).
 *   // Les SUPPRESSIONS (présent en baseline, absent du courant) ne
 *   // sont PAS retournées : uniquement les AJOUTS (cf. hors-scope
 *   // ticket : pas de détection des fiches MODIFIÉES ni des retraits).
 *   // Résultat trié : d'abord par kind (strcmp ASCII), puis par name
 *   // (idem), pour un rendu déterministe/snapshotable. Écrit au plus
 *   // `out_cap` entrées dans `out` (pointeurs kind/name aliasant ceux
 *   // de `current`, pas de copie/alloc) ; retourne le nombre d'entrées
 *   // écrites (0 si aucune nouveauté, jamais négatif, jamais > out_cap).
 *   // Ne modifie ni baseline ni current. nb==0 et/ou nc==0 valides
 *   // (baseline/current peuvent alors être NULL).
 *   int owl_doc_novelties_diff(const OwlDocFicheId* baseline, int nb,
 *                               const OwlDocFicheId* current, int nc,
 *                               OwlDocFicheId* out, int out_cap);
 *
 * Tant que cette fonction n'existe pas, ce fichier ne link pas
 * (undefined reference) — rouge attendu, cf. rapport test_writer.
 * ================================================================ */

typedef struct {
    const char* kind;
    const char* name;
} OwlDocFicheId;

extern int owl_doc_novelties_diff(const OwlDocFicheId* baseline, int nb,
                                   const OwlDocFicheId* current, int nc,
                                   OwlDocFicheId* out, int out_cap);

/* Cas 1 — ajout d'un symbole (native) : absent de la baseline, présent
 * dans le courant -> doit apparaître dans les nouveautés. */
static void test_novelties_ajout_symbole(void) {
    OwlDocFicheId baseline[] = {
        { "native", "Trace" },
        { "native", "Gauche" },
    };
    OwlDocFicheId courant[] = {
        { "native", "Trace" },
        { "native", "Gauche" },
        { "native", "MaNativeX" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(baseline, 2, courant, 3, out, 8);
    CHECK(n == 1, "ajout symbole : exactement 1 nouveauté");
    if (n == 1) {
        CHECK(strcmp(out[0].kind, "native") == 0, "ajout symbole : kind native");
        CHECK(strcmp(out[0].name, "MaNativeX") == 0, "ajout symbole : name MaNativeX");
    }
}

/* Cas 2 — ajout d'un chapitre documentation_public : le dev exige
 * explicitement qu'une fiche doc (pas seulement un symbole) soit listée
 * dans « Nouveautés ». */
static void test_novelties_ajout_chapitre_doc(void) {
    OwlDocFicheId baseline[] = {
        { "reference", "30_chapitre_existant" },
    };
    OwlDocFicheId courant[] = {
        { "reference", "30_chapitre_existant" },
        { "reference", "31_nouveau_chapitre" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(baseline, 1, courant, 2, out, 8);
    CHECK(n == 1, "ajout chapitre : exactement 1 nouveauté");
    if (n == 1) {
        CHECK(strcmp(out[0].kind, "reference") == 0, "ajout chapitre : kind reference");
        CHECK(strcmp(out[0].name, "31_nouveau_chapitre") == 0,
              "ajout chapitre : name 31_nouveau_chapitre");
    }
}

/* Cas 3 — aucun changement : baseline == courant -> pas de nouveauté,
 * pas d'erreur (retour 0, pas de faux positif). */
static void test_novelties_aucun_changement(void) {
    OwlDocFicheId ensemble[] = {
        { "native", "Trace" },
        { "type",   "Cadre" },
        { "reference", "05_erreurs" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(ensemble, 3, ensemble, 3, out, 8);
    CHECK(n == 0, "aucun changement : 0 nouveauté");
}

/* Cas 4 — suppression : une fiche présente en baseline mais absente du
 * courant NE doit PAS apparaître dans « Nouveautés » (on liste les
 * AJOUTS uniquement, jamais les retraits — hors-scope ticket assumé). */
static void test_novelties_suppression_non_listee(void) {
    OwlDocFicheId baseline[] = {
        { "native", "Trace" },
        { "native", "NativeSupprimee" },
    };
    OwlDocFicheId courant[] = {
        { "native", "Trace" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(baseline, 2, courant, 1, out, 8);
    CHECK(n == 0, "suppression seule : 0 nouveauté (pas de retrait listé)");
}

/* Cas 5 — tri déterministe : plusieurs ajouts de kinds/noms différents ->
 * sortie triée par kind puis name (strcmp ASCII), pour un rendu stable
 * (snapshot reproductible). */
static void test_novelties_tri_deterministe(void) {
    OwlDocFicheId baseline[] = { { "native", "Trace" } };
    OwlDocFicheId courant[] = {
        { "native", "Trace" },
        { "reference", "31_zzz_chapitre" },
        { "native", "ZNative" },
        { "const", "cUneConstante" },
        { "native", "ANative" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(baseline, 1, courant, 5, out, 8);
    CHECK(n == 4, "tri : 4 nouveautés (toutes sauf Trace)");
    if (n == 4) {
        /* kind ASCII : "const" < "native" < "reference" ; à l'intérieur
         * de "native" : "ANative" < "ZNative". */
        CHECK(strcmp(out[0].kind, "const") == 0 &&
              strcmp(out[0].name, "cUneConstante") == 0,
              "tri : entrée 0 = const/cUneConstante");
        CHECK(strcmp(out[1].kind, "native") == 0 &&
              strcmp(out[1].name, "ANative") == 0,
              "tri : entrée 1 = native/ANative (A avant Z)");
        CHECK(strcmp(out[2].kind, "native") == 0 &&
              strcmp(out[2].name, "ZNative") == 0,
              "tri : entrée 2 = native/ZNative");
        CHECK(strcmp(out[3].kind, "reference") == 0 &&
              strcmp(out[3].name, "31_zzz_chapitre") == 0,
              "tri : entrée 3 = reference/31_zzz_chapitre");
    }
}

/* Cas 6 — chaque entrée porte de quoi bâtir un lien : kind ET name non
 * nuls / non vides, pour toutes les nouveautés retournées. */
static void test_novelties_champs_non_vides(void) {
    OwlDocFicheId baseline[] = { { "native", "Trace" } };
    OwlDocFicheId courant[] = {
        { "native", "Trace" },
        { "native", "MaNativeX" },
        { "reference", "31_nouveau_chapitre" },
    };
    OwlDocFicheId out[8];
    int n = owl_doc_novelties_diff(baseline, 1, courant, 3, out, 8);
    CHECK(n == 2, "champs non vides : 2 nouveautés");
    for (int i = 0; i < n; i++) {
        CHECK(out[i].kind != NULL && out[i].kind[0] != '\0',
              "champs non vides : kind rempli");
        CHECK(out[i].name != NULL && out[i].name[0] != '\0',
              "champs non vides : name rempli");
    }
}

/* Cas 7 (edge, proactif) — out_cap plus petit que le nombre réel de
 * nouveautés : borne dure, pas de débordement du buffer appelant. */
static void test_novelties_out_cap_borne(void) {
    OwlDocFicheId baseline[] = { { "native", "Trace" } };
    OwlDocFicheId courant[] = {
        { "native", "Trace" },
        { "native", "A" },
        { "native", "B" },
        { "native", "C" },
    };
    OwlDocFicheId out[2];
    int n = owl_doc_novelties_diff(baseline, 1, courant, 4, out, 2);
    CHECK(n == 2, "out_cap borne : n plafonné à out_cap (2)");
}

/* Cas 8 (edge, proactif) — sets vides des deux côtés : pas de crash,
 * 0 nouveauté. */
static void test_novelties_sets_vides(void) {
    OwlDocFicheId out[4];
    int n = owl_doc_novelties_diff(NULL, 0, NULL, 0, out, 4);
    CHECK(n == 0, "sets vides : 0 nouveauté, pas de crash");
}

/* ================================================================
 *  — indice de migration sur les formes héritées de fin de ligne
 * ================================================================
 *
 * RC et CR ne sont plus des symboles du langage mais restent
 * résolubles : le code compile, et le formateur les réécrit en CRLF à
 * l'enregistrement. Interroger le catalogue est le moment où l'utilisateur
 * apprend cette bascule — l'indice doit y être, dans les deux langues.
 *
 * Les deux propriétés verrouillées ensemble :
 *   1. l'indice est rendu, et cite le nom TAPÉ (RC pour rc, CR pour cR),
 *      jamais CRLF que l'utilisateur n'a pas écrit ;
 *   2. rendre un indice ne fait PAS de ces formes une entrée du catalogue :
 *      le code de sortie reste 2 (« ce symbole n'existe pas »), et un
 *      symbole réellement inconnu n'hérite d'aucun indice.
 * L'absence de RC/CR du registre lui-même est tenue par le snapshot
 * tests/compiler/catalog/eol_rc_cr_absents_registre. */

static void test_explain_rc_indice_migration_fr(void) {
    int rc = capture_call_err(cli_doc_explain, 3, ARGS("explain", "RC", "--fr"));
    CHECK_RC(rc, 2, "explain RC : rc=2 (pas une entrée du catalogue)");
    CHECK_HAS("symbole 'RC' inconnu",
              "explain RC : le symbole cité est celui tapé");
    CHECK_HAS("RC : forme héritée, équivaut à CRLF. Réécrite en CRLF à "
              "l'enregistrement.",
              "explain RC : indice FR, forme héritée + réécriture");
    CHECK_HAS("Pour une fin de ligne simple, utiliser LF.",
              "explain RC : indice FR oriente vers LF");
}

static void test_explain_rc_indice_migration_en(void) {
    int rc = capture_call_err(cli_doc_explain, 4,
                              ARGS("explain", "rc", "--lang", "en"));
    CHECK_RC(rc, 2, "explain rc --lang en : rc=2");
    CHECK_HAS("RC: legacy form, equals CRLF. Rewritten to CRLF on save.",
              "explain rc : indice EN, cite RC (forme tapée en majuscules)");
    CHECK_HAS("For a plain end of line, use LF.",
              "explain rc : indice EN oriente vers LF");
    CHECK_NO("forme héritée",
             "explain rc --lang en : pas de fuite FR dans l'indice");
}

static void test_explain_cr_indice_migration_fr(void) {
    int rc = capture_call_err(cli_doc_explain, 3, ARGS("explain", "CR", "--fr"));
    CHECK_RC(rc, 2, "explain CR : rc=2");
    CHECK_HAS("CR : forme héritée, équivaut à CRLF. Réécrite en CRLF à "
              "l'enregistrement.",
              "explain CR : l'indice cite CR, pas CRLF");
    CHECK_NO("symbole 'CRLF' inconnu",
             "explain CR : la chose cherchée reste CR");
    CHECK_HAS("Pour une fin de ligne simple, utiliser LF.",
              "explain CR : indice FR oriente vers LF");
}

static void test_explain_cr_indice_migration_en(void) {
    int rc = capture_call_err(cli_doc_explain, 4,
                              ARGS("explain", "cR", "--lang", "en"));
    CHECK_RC(rc, 2, "explain cR --lang en : rc=2");
    CHECK_HAS("CR: legacy form, equals CRLF. Rewritten to CRLF on save.",
              "explain cR : indice EN cite CR");
    CHECK_HAS("For a plain end of line, use LF.",
              "explain cR : indice EN oriente vers LF");
}

/* CRLF est un vrai symbole : fiche normale, rc=0, aucun indice de migration. */
static void test_explain_crlf_reste_symbole_sans_indice(void) {
    int rc = capture_call_err(cli_doc_explain, 3,
                              ARGS("explain", "CRLF", "--fr"));
    CHECK_RC(rc, 0, "explain CRLF : rc=0 (constante du langage)");
    CHECK_NO("forme héritée", "explain CRLF : aucun indice de migration");
}

/* Un symbole réellement inconnu garde le chemin not-found nu : même rc, mais
 * pas d'indice — l'indice est ciblé, pas un ornement du chemin d'erreur. */
static void test_explain_symbole_inconnu_sans_indice(void) {
    int rc = capture_call_err(cli_doc_explain, 3,
                              ARGS("explain", "ZZZZ", "--fr"));
    CHECK_RC(rc, 2, "explain ZZZZ : rc=2");
    CHECK_HAS("symbole 'ZZZZ' inconnu", "explain ZZZZ : message not-found nu");
    CHECK_NO("forme héritée", "explain ZZZZ : aucun indice de migration");
    CHECK_NO("utiliser LF", "explain ZZZZ : pas d'orientation vers LF");
}

/* Le bloc not-found lui-même doit être bilingue (invariant 3). Avant 
 * ses trois lignes étaient FR en dur : `--lang en` produisait un bloc FR, et
 * l'ajout de l'indice EN y aurait créé un îlot anglais au milieu du français.
 * On verrouille donc le bloc ENTIER en anglais, indice compris, et l'absence
 * de tout résidu FR. */
static void test_explain_inconnu_bloc_notfound_en(void) {
    int rc = capture_call_err(cli_doc_explain, 4,
                              ARGS("explain", "ZZZZ", "--lang", "en"));
    CHECK_RC(rc, 2, "explain ZZZZ --lang en : rc=2");
    CHECK_HAS("owl explain: unknown symbol 'ZZZZ'.",
              "explain ZZZZ --lang en : constat en anglais");
    CHECK_HAS("Not found in natives / types / enums / constants / errors.",
              "explain ZZZZ --lang en : énumération des buckets en anglais");
    CHECK_HAS("Try 'owl search ZZZZ' for a broader search.",
              "explain ZZZZ --lang en : conseil de repli en anglais");
    CHECK_NO("symbole", "explain ZZZZ --lang en : aucun résidu FR (symbole)");
    CHECK_NO("Pas trouvé", "explain ZZZZ --lang en : aucun résidu FR (Pas trouvé)");
    CHECK_NO("Essayez", "explain ZZZZ --lang en : aucun résidu FR (Essayez)");
}

/* Contre-épreuve : le bloc reste intégralement français en FR. */
static void test_explain_inconnu_bloc_notfound_fr(void) {
    int rc = capture_call_err(cli_doc_explain, 3, ARGS("explain", "ZZZZ", "--fr"));
    CHECK_RC(rc, 2, "explain ZZZZ --fr : rc=2");
    CHECK_HAS("owl explain : symbole 'ZZZZ' inconnu.",
              "explain ZZZZ --fr : constat en français");
    CHECK_HAS("Pas trouvé dans natives / types / enums / constantes / erreurs.",
              "explain ZZZZ --fr : énumération des buckets en français");
    CHECK_HAS("Essayez 'owl search ZZZZ' pour une recherche large.",
              "explain ZZZZ --fr : conseil de repli en français");
    CHECK_NO("unknown symbol", "explain ZZZZ --fr : aucun résidu EN");
}

/* Bloc complet en anglais sur une forme héritée : constat + indice + repli,
 * une seule langue de bout en bout. C'est le cas qui a motivé l'élargissement
 * du périmètre — l'indice EN seul dans un bloc FR était pire que les deux
 * extrêmes. */
static void test_explain_rc_bloc_complet_en(void) {
    int rc = capture_call_err(cli_doc_explain, 4,
                              ARGS("explain", "RC", "--lang", "en"));
    CHECK_RC(rc, 2, "explain RC --lang en : rc=2");
    CHECK_HAS("owl explain: unknown symbol 'RC'.",
              "explain RC --lang en : constat en anglais");
    CHECK_HAS("RC: legacy form, equals CRLF. Rewritten to CRLF on save.",
              "explain RC --lang en : indice en anglais");
    CHECK_HAS("Try 'owl search RC' for a broader search.",
              "explain RC --lang en : repli en anglais");
    CHECK_NO("Essayez", "explain RC --lang en : plus d'îlot FR autour de l'indice");
}

/* ================================================================
 * Verrou de coherence : completion `owl list` <-> dispatch reel
 * ================================================================ */

/* Decoupe une capture en lignes non vides. Retourne le nombre de lignes
 * ecrites dans `out`. */
static int split_lines(const char* src, char out[][64], int cap) {
    int n = 0;
    const char* p = src;
    while (*p && n < cap) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && p[len - 1] == '\r') len--;
        if (len && len < 64) {
            memcpy(out[n], p, len);
            out[n][len] = '\0';
            n++;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}

static int line_in(const char* needle, char list[][64], int n) {
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i], needle) == 0) return 1;
    }
    return 0;
}

/* Trois surfaces annoncent les sous-commandes de `owl list` : la completion
 * (`__complete list`), le menu d'aide (`owl list` sans argument) et le
 * dispatch de cli_doc_list. Elles ont diverge sans bruit — la completion a
 * propose une sous-commande que le dispatch refuse. Ce test les recoud :
 *
 *   1. tout ce que la completion propose est reellement dispatche ;
 *   2. tout ce que le menu d'aide annonce est propose en completion.
 *
 * Le refus se detecte sur le TEXTE du diagnostic, pas sur rc : le code de
 * retour de cli_doc_list sur sous-commande inconnue n'est pas fiable selon
 * le chemin d'appel, ce qui est precisement ce qui a laisse la divergence
 * dormir cote serveur MCP.
 *
 * Reste hors de portee : une sous-commande dispatchee mais absente a la fois
 * du menu et de la completion — le dispatch n'est pas enumerable depuis
 * l'exterieur. Fermer ce dernier trou demanderait une table unique pilotant
 * dispatch + menu + completion. */
static void test_list_completion_coherente_avec_dispatch(void) {
    static char cands[32][64];
    char label[192];

    int rc = capture_call(cli_doc_complete, 2, ARGS("__complete", "list"));
    CHECK_RC(rc, 0, "__complete list rc=0");
    int ncand = split_lines(g_buf, cands, 32);

    /* Garde anti-vacuite : sans candidats, les boucles ci-dessous passeraient
     * vert sans rien verifier. 11 = sous-commandes servies par le dispatch. */
    CHECK(ncand == 11, "__complete list propose 11 sous-commandes");
    CHECK(!line_in("enums", cands, ncand),
          "__complete list ne propose plus 'enums'");

    /* 1. completion -> dispatch */
    for (int i = 0; i < ncand; i++) {
        char needle[128];
        char* av[2];
        av[0] = "list";
        av[1] = cands[i];
        snprintf(needle, sizeof needle,
                 "sous-commande '%s' inconnue", cands[i]);
        capture_call_err(cli_doc_list, 2, av);
        snprintf(label, sizeof label,
                 "completion '%s' : dispatchee par owl list", cands[i]);
        CHECK(strstr(g_buf, needle) == NULL, label);
    }

    /* 2. menu d'aide -> completion */
    capture_call(cli_doc_list, 1, ARGS("list"));
    int nmenu = 0;
    const char* p = g_buf;
    while ((p = strstr(p, "owl list ")) != NULL) {
        char tok[64];
        int k = 0;
        p += 9;
        while (p[k] >= 'a' && p[k] <= 'z' && k < 63) { tok[k] = p[k]; k++; }
        tok[k] = '\0';
        if (k == 0) continue;  /* titre "owl list <quoi>" */
        nmenu++;
        snprintf(label, sizeof label,
                 "menu 'owl list %s' : proposee en completion", tok);
        CHECK(line_in(tok, cands, ncand), label);
    }
    CHECK(nmenu == 11, "menu owl list annonce 11 sous-commandes");
}

/* ================================================================
 * Verrou de coherence : diagnostic `--cat` <-> predicat exporte
 * ================================================================ */

/* `--cat <X>` ne signalait un nom de famille hors domaine que sur `natives`.
 * Ailleurs, une liste vide passait pour une reponse legitime : « cette famille
 * n'existe pas » et « cette famille est vide » se lisent pareil et se corrigent
 * a l'oppose. Le diagnostic doit donc exister sur chaque bucket qui filtre par
 * famille, nommer la valeur fautive et enumerer les valables — corriger l'appel
 * sans deviner et sans relancer une autre commande.
 *
 * Le predicat exporte est la source unique du verdict : c'est lui qui decide
 * d'imprimer le diagnostic, et c'est lui que les canaux machine interrogent.
 * Aucun consommateur ne relit le texte affiche. */

#define CAT_HORS_DOMAINE "ZzzPasUneFamille"

typedef struct {
    const char* bucket;
    const char* valide;       /* famille reellement portee par ce bucket */
    const char* echantillon;  /* attendue dans l'enumeration des valables */
    int         champ;        /* 1 si `--cat` designe un champ DSL */
} CatBucketCase;

/* Les familles ne sont pas les memes d'un bucket a l'autre : celles des
 * natives ne sont ni celles des constantes, ni celles des limites, et sur
 * events / props `--cat` designe un champ DSL, pas une categorie. */
static const CatBucketCase CAT_BUCKETS[] = {
    { "natives",   "HTTP",           "Tableaux",     0 },
    { "types",     "PDF",            "SQL",          0 },
    { "constants", "Clavier",        "Encodage",     0 },
    { "keywords",  "Contrôle",       "Exceptions",   0 },
    { "limits",    "Compiler/Types", "Compiler/POO", 0 },
    { "events",    "Bouton",         "Saisie",       1 },
    { "props",     "Saisie",         "Bouton",       1 },
};
#define CAT_BUCKETS_COUNT ((int)(sizeof(CAT_BUCKETS) / sizeof(CAT_BUCKETS[0])))

static void test_list_cat_hors_domaine_uniforme(void) {
    char label[192];
    char needle[128];
    for (int i = 0; i < CAT_BUCKETS_COUNT; i++) {
        const CatBucketCase* c = &CAT_BUCKETS[i];
        char* av[5];
        av[0] = "list";
        av[1] = (char*)c->bucket;
        av[2] = "--cat";
        av[3] = CAT_HORS_DOMAINE;
        av[4] = "--json";

        snprintf(label, sizeof label,
                 "%s : '%s' hors domaine", c->bucket, CAT_HORS_DOMAINE);
        CHECK(cli_doc_list_category_is_known(c->bucket, CAT_HORS_DOMAINE) == 0,
              label);
        snprintf(label, sizeof label,
                 "%s : '%s' reconnue", c->bucket, c->valide);
        CHECK(cli_doc_list_category_is_known(c->bucket, c->valide) == 1, label);

        /* Texte : valeur fautive rappelee + valables enumerees. */
        int rc = capture_call(cli_doc_list, 4, av);
        snprintf(label, sizeof label, "list %s --cat inconnue rc=0", c->bucket);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label,
                 "%s --cat inconnue : nom fautif rappele", c->bucket);
        CHECK_HAS(CAT_HORS_DOMAINE, label);
        snprintf(label, sizeof label,
                 "%s --cat inconnue : diagnostic explicite", c->bucket);
        CHECK_HAS(c->champ ? "champ inconnu :" : "catégorie inconnue :", label);
        snprintf(label, sizeof label,
                 "%s --cat inconnue : '%s' dans les valables",
                 c->bucket, c->echantillon);
        CHECK_HAS(c->echantillon, label);

        /* JSON : meme verdict, sans avoir a lire une phrase en francais. */
        capture_call(cli_doc_list, 5, av);
        snprintf(needle, sizeof needle,
                 "\"unknown_category\": \"%s\"", CAT_HORS_DOMAINE);
        snprintf(label, sizeof label,
                 "%s --cat inconnue --json : verdict machine", c->bucket);
        CHECK_HAS(needle, label);
        snprintf(label, sizeof label,
                 "%s --cat inconnue --json : valables enumerees", c->bucket);
        CHECK_HAS("\"valid_categories\"", label);
        snprintf(label, sizeof label,
                 "%s --cat inconnue --json : '%s' dans les valables",
                 c->bucket, c->echantillon);
        CHECK_HAS(c->echantillon, label);

        /* Famille valable : ni diagnostic, ni verdict d'erreur. */
        av[3] = (char*)c->valide;
        capture_call(cli_doc_list, 5, av);
        snprintf(label, sizeof label,
                 "%s --cat %s --json : aucun verdict d'erreur",
                 c->bucket, c->valide);
        CHECK_NO("\"unknown_category\"", label);
        capture_call(cli_doc_list, 4, av);
        snprintf(label, sizeof label,
                 "%s --cat %s : aucun diagnostic", c->bucket, c->valide);
        CHECK_NO("inconnue :", label);
    }
}

/* Une famille valable mais reellement vide n'est pas une erreur : la question
 * a ete comprise, la reponse est vide. `Colonne` est un champ DSL sans event
 * propre — le cas doit rester distinct du nom hors domaine. */
static void test_list_cat_valide_mais_vide(void) {
    CHECK(cli_doc_list_category_is_known("events", "Colonne") == 1,
          "events : Colonne est un champ valable");
    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "events", "--cat", "Colonne"));
    CHECK_RC(rc, 0, "list events --cat Colonne rc=0");
    CHECK_NO("champ inconnu", "Colonne sans event : pas signale comme inconnu");
    CHECK_HAS("(aucun event)", "Colonne sans event : reponse vide assumee");

    capture_call(cli_doc_list, 5,
                 ARGS("list", "events", "--cat", "Colonne", "--json"));
    CHECK_NO("\"unknown_category\"",
             "Colonne sans event --json : aucun verdict d'erreur");
    CHECK_HAS("\"count\": 0", "Colonne sans event --json : 0 resultat");
}

/* `--cat` sur un champ DSL respecte l'invariant 2 : `Fenêtre`, `FENÊTRE` et
 * `Fenetre` designent le meme champ. La forme accentuee est la forme canonique
 * du champ le plus courant du langage — la rapporter inconnue, en enumerant sa
 * variante ASCII juste dessous, affirme une contre-verite sur le langage.
 * Les cles du catalogue restent ASCII : c'est la comparaison a l'entree qui
 * doit etre insensible, pas le stockage. */
static void test_list_cat_champ_dsl_accent_insensible(void) {
    static const char* const FORMES[] = { "Fenêtre", "FENÊTRE", "Fenetre",
                                          "fenetre" };
    static const char* const BUCKETS[] = { "events", "props" };
    char label[192];
    char ref[128 * 1024];

    for (int b = 0; b < 2; b++) {
        for (int f = 0; f < 4; f++) {
            char* av[5];
            av[0] = "list";
            av[1] = (char*)BUCKETS[b];
            av[2] = "--cat";
            av[3] = (char*)FORMES[f];
            av[4] = "--json";

            snprintf(label, sizeof label, "%s : '%s' est un champ valable",
                     BUCKETS[b], FORMES[f]);
            CHECK(cli_doc_list_category_is_known(BUCKETS[b], FORMES[f]) == 1,
                  label);

            int rc = capture_call(cli_doc_list, 4, av);
            snprintf(label, sizeof label, "list %s --cat %s rc=0",
                     BUCKETS[b], FORMES[f]);
            CHECK_RC(rc, 0, label);
            snprintf(label, sizeof label,
                     "%s --cat %s : pas signale comme inconnu",
                     BUCKETS[b], FORMES[f]);
            CHECK_NO("champ inconnu", label);

            /* Meme contenu sous toutes les formes : le JSON n'echote pas le
             * filtre, la comparaison octet a octet est donc legitime. */
            capture_call(cli_doc_list, 5, av);
            if (f == 0) {
                memcpy(ref, g_buf, sizeof ref);
                snprintf(label, sizeof label,
                         "%s --cat %s --json : contenu non vide",
                         BUCKETS[b], FORMES[f]);
                CHECK(strstr(g_buf, "\"count\": 0") == NULL, label);
            } else {
                snprintf(label, sizeof label,
                         "%s --cat %s --json : meme contenu que la forme canon",
                         BUCKETS[b], FORMES[f]);
                CHECK(strcmp(ref, g_buf) == 0, label);
            }
            snprintf(label, sizeof label,
                     "%s --cat %s --json : aucun verdict d'erreur",
                     BUCKETS[b], FORMES[f]);
            CHECK_NO("\"unknown_category\"", label);
        }
    }
}

/* Le diagnostic sort dans la langue demandee : un anglophone ne doit pas
 * recevoir la phrase francaise (invariant bilingue). */
static void test_list_cat_hors_domaine_en(void) {
    int rc = capture_call(cli_doc_list, 6,
                          ARGS("list", "constants", "--cat", CAT_HORS_DOMAINE,
                               "--lang", "en"));
    CHECK_RC(rc, 0, "list constants --cat inconnue --lang en rc=0");
    CHECK_HAS("unknown category", "diagnostic EN sur constants");
    CHECK_HAS("Available categories", "valables enumerees en EN");
    CHECK_NO("catégorie inconnue", "aucune forme FR en --lang en");

    rc = capture_call(cli_doc_list, 6,
                      ARGS("list", "props", "--cat", CAT_HORS_DOMAINE,
                           "--lang", "en"));
    CHECK_RC(rc, 0, "list props --cat inconnue --lang en rc=0");
    CHECK_HAS("unknown control", "diagnostic EN sur props");
    CHECK_HAS("Available controls", "champs valables enumeres en EN");
}

/* Verrou principal du ticket. Le predicat exporte et le diagnostic imprime
 * doivent dire la meme chose sur CHAQUE sous-commande que `owl list`
 * dispatche — y compris celles qui ignorent `--cat`, ou il n'y a rien a
 * valider et rien a signaler.
 *
 * C'est ce qui rend impossible la derive que le ticket ferme : un canal
 * machine qui deduirait le verdict du texte affiche (au lieu du predicat)
 * ne pourrait pas suivre une reformulation, et un bucket qui gagnerait
 * `--cat` sans entrer dans le domaine du predicat rougirait ici. */
static void test_list_cat_predicat_est_la_source_du_diagnostic(void) {
    static char cands[32][64];
    char label[192];

    capture_call(cli_doc_complete, 2, ARGS("__complete", "list"));
    int ncand = split_lines(g_buf, cands, 32);
    CHECK(ncand == 11, "__complete list : 11 buckets (garde anti-vacuite)");

    int filtrants = 0;
    for (int i = 0; i < ncand; i++) {
        char* av[5];
        av[0] = "list";
        av[1] = cands[i];
        av[2] = "--cat";
        av[3] = CAT_HORS_DOMAINE;
        av[4] = "--json";
        capture_call(cli_doc_list, 5, av);
        int diagnostique = strstr(g_buf, "\"unknown_category\"") != NULL;
        int connue = cli_doc_list_category_is_known(cands[i], CAT_HORS_DOMAINE);
        snprintf(label, sizeof label,
                 "list %s : predicat et diagnostic d'accord", cands[i]);
        CHECK(diagnostique == !connue, label);
        if (!connue) filtrants++;
    }
    CHECK(filtrants == CAT_BUCKETS_COUNT,
          "7 sous-commandes filtrent par famille");
}

/* Une famille ajoutee au catalogue doit etre reconnue par le predicat sans
 * edit ici : on relit les familles que le bucket rend lui-meme. Sans ce
 * verrou, le predicat peut se figer sur un domaine perime et declarer
 * inconnue une famille que le filtre honore. */
static void test_list_cat_predicat_couvre_les_familles_rendues(void) {
    char label[192];
    for (int i = 0; i < CAT_BUCKETS_COUNT; i++) {
        const CatBucketCase* c = &CAT_BUCKETS[i];
        capture_call(cli_doc_list, 3,
                     ARGS("list", (char*)c->bucket, "--json"));
        const char* key = c->champ ? "\"widget\": " : "\"category\": ";
        size_t klen = strlen(key);
        int seen = 0;
        const char* p = g_buf;
        while ((p = strstr(p, key)) != NULL) {
            p += klen;
            if (*p != '"') continue;
            p++;
            char val[96];
            int k = 0;
            while (*p && *p != '"' && k < 95) val[k++] = *p++;
            val[k] = '\0';
            if (!k) continue;
            /* `État (sous-contrôle)` est un libelle de regroupement du
             * catalogue d'events, pas un champ filtrable : ces events sortent
             * sous `--cat État`. Aucun champ DSL ne porte de parenthese. */
            if (strstr(val, " (")) continue;
            seen++;
            snprintf(label, sizeof label,
                     "%s : famille rendue '%s' reconnue", c->bucket, val);
            CHECK(cli_doc_list_category_is_known(c->bucket, val) == 1, label);
        }
        snprintf(label, sizeof label,
                 "%s : au moins une famille rendue (garde anti-vacuite)",
                 c->bucket);
        CHECK(seen > 0, label);
    }
}

/* ================================================================
 * `--cat` sur une liste qui n'a pas de familles
 * ================================================================
 *
 * Quatre sous-commandes acceptaient `--cat`, ne s'en servaient pas, et n'en
 * disaient rien : la sortie etait identique a celle sans filtre. Un argument
 * accepte sans effet et sans un mot est indiscernable d'un argument applique —
 * cote humain comme cote machine. Elles refusent desormais l'appel.
 *
 * Le verrou ne teste pas ces quatre-la : il balaye les ONZE sous-commandes que
 * `owl list` dispatche et exige de chacune qu'elle tombe dans l'une des deux
 * categories — elle filtre et sait nommer la famille inconnue, ou elle ne
 * filtre pas et sait refuser. Aucune ne peut rester muette, y compris celles
 * qui n'existent pas encore. */

/* Le bucket n'est pas decoupe en familles : `--cat` ne peut rien vouloir dire
 * dessus, et rendre la liste complete avec un avertissement laisserait une
 * sortie exploitable comme si elle etait filtree. */
static void test_list_cat_non_filtrable_refuse(void) {
    static const struct {
        const char* bucket;
        const char* ancre;   /* contenu du dump, qui ne doit PAS sortir */
    } NON_FILTRANTS[] = {
        { "operators",  "[~~" },
        { "controls",   "Fenêtre" },
        { "globalvars", "ErreurDétectée" },
        { "categories", "Tableaux" },
    };
    char label[224], needle[160];

    for (int i = 0; i < 4; i++) {
        const char* b = NON_FILTRANTS[i].bucket;
        char* av[5];
        av[0] = "list";
        av[1] = (char*)b;
        av[2] = "--cat";
        av[3] = CAT_HORS_DOMAINE;
        av[4] = "--json";

        snprintf(label, sizeof label, "%s : ne filtre pas par famille", b);
        CHECK(cli_doc_list_bucket_filters_by_category(b) == 0, label);

        /* Texte : le refus part sur stderr, comme les autres erreurs d'usage. */
        int rc = capture_call_err(cli_doc_list, 4, av);
        snprintf(label, sizeof label, "list %s --cat : rc=1 (refus)", b);
        CHECK_RC(rc, 1, label);
        snprintf(label, sizeof label, "list %s --cat : bucket nomme", b);
        CHECK_HAS(b, label);
        snprintf(label, sizeof label, "list %s --cat : option nommee", b);
        CHECK_HAS("--cat", label);
        snprintf(label, sizeof label, "list %s --cat : valeur fautive rappelee", b);
        CHECK_HAS(CAT_HORS_DOMAINE, label);
        snprintf(label, sizeof label, "list %s --cat : raison donnee", b);
        CHECK_HAS("pas découpée en catégories", label);
        snprintf(label, sizeof label,
                 "list %s --cat : listes acceptant --cat enumerees", b);
        CHECK_HAS("natives", label);
        /* Un refus qui dumperait quand meme la liste ne corrigerait rien. */
        snprintf(label, sizeof label, "list %s --cat : aucun dump", b);
        CHECK_NO(NON_FILTRANTS[i].ancre, label);

        /* JSON : meme verdict, lisible sans lire une phrase. */
        rc = capture_call_err(cli_doc_list, 5, av);
        snprintf(label, sizeof label, "list %s --cat --json : rc=1", b);
        CHECK_RC(rc, 1, label);
        CHECK_HAS("\"error\": \"category_filter_not_supported\"", label);
        snprintf(needle, sizeof needle, "\"bucket\": \"%s\"", b);
        snprintf(label, sizeof label,
                 "list %s --cat --json : bucket dans le verdict", b);
        CHECK_HAS(needle, label);
        snprintf(needle, sizeof needle,
                 "\"requested_category\": \"%s\"", CAT_HORS_DOMAINE);
        snprintf(label, sizeof label,
                 "list %s --cat --json : valeur fautive rappelee", b);
        CHECK_HAS(needle, label);
        snprintf(label, sizeof label,
                 "list %s --cat --json : buckets acceptant --cat", b);
        CHECK_HAS("\"buckets_accepting_cat\": [", label);
        snprintf(label, sizeof label,
                 "list %s --cat --json : aucun dump", b);
        CHECK_NO(NON_FILTRANTS[i].ancre, label);

        /* Le verdict ne depend pas de la valeur : meme une famille valable
         * ailleurs (`HTTP` sur natives) n'a pas de sens ici. */
        av[3] = "HTTP";
        rc = capture_call_err(cli_doc_list, 5, av);
        snprintf(label, sizeof label,
                 "list %s --cat HTTP --json : refus aussi", b);
        CHECK_RC(rc, 1, label);
        CHECK_HAS("\"error\": \"category_filter_not_supported\"", label);
    }
}

/* Le refus sort dans la langue demandee (invariant bilingue). */
static void test_list_cat_non_filtrable_en(void) {
    int rc = capture_call_err(cli_doc_list, 6,
                              ARGS("list", "operators", "--cat",
                                   CAT_HORS_DOMAINE, "--lang", "en"));
    CHECK_RC(rc, 1, "list operators --cat --lang en : rc=1");
    CHECK_HAS("does not apply", "refus EN sur operators");
    CHECK_HAS("Lists accepting --cat", "listes acceptantes enumerees en EN");
    CHECK_NO("pas découpée", "aucune forme FR en --lang en");
}

/* Non-regression : sans `--cat`, ces quatre buckets rendent ce qu'ils
 * rendaient. Le refus ne doit pas deborder sur l'appel normal. */
static void test_list_sans_cat_buckets_non_filtrables_inchanges(void) {
    static const struct {
        const char* bucket;
        const char* ancre;
    } CAS[] = {
        { "operators",  "[~~" },
        { "controls",   "Fenêtre" },
        { "globalvars", "ErreurDétectée" },
        { "categories", "Tableaux" },
    };
    char label[224];
    for (int i = 0; i < 4; i++) {
        const char* b = CAS[i].bucket;
        int rc = capture_call_err(cli_doc_list, 2, ARGS("list", (char*)b));
        snprintf(label, sizeof label, "list %s sans --cat : rc=0", b);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label, "list %s sans --cat : contenu rendu", b);
        CHECK_HAS(CAS[i].ancre, label);
        snprintf(label, sizeof label, "list %s sans --cat : aucun refus", b);
        CHECK_NO("category_filter_not_supported", label);

        rc = capture_call_err(cli_doc_list, 3, ARGS("list", (char*)b, "--json"));
        snprintf(label, sizeof label, "list %s --json sans --cat : rc=0", b);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label,
                 "list %s --json sans --cat : contenu rendu", b);
        CHECK_HAS(CAS[i].ancre, label);
        snprintf(label, sizeof label,
                 "list %s --json sans --cat : aucun refus", b);
        CHECK_NO("category_filter_not_supported", label);
    }
}

/* Verrou principal du ticket : aucune sous-commande de `owl list` ne peut
 * avaler `--cat`.
 *
 * Il ne nomme aucun bucket. Il relit ceux que `owl list` dispatche, et pour
 * chacun exige deux choses : que la reponse a un `--cat` hors domaine DIFFERE
 * de la reponse sans filtre (le defaut ferme ici etait l'egalite exacte des
 * deux), et qu'elle porte exactement un des deux verdicts machine, celui que
 * le predicat exporte annonce. Un bucket futur qui accepterait `--cat` sans
 * rien en faire rougit ici sans edit. */
static void test_list_cat_aucun_bucket_ne_reste_muet(void) {
    static char cands[32][64];
    static char ref[sizeof g_buf];
    char label[224];

    capture_call(cli_doc_complete, 2, ARGS("__complete", "list"));
    int ncand = split_lines(g_buf, cands, 32);
    CHECK(ncand == 11, "__complete list : 11 buckets (garde anti-vacuite)");

    int filtrants = 0, refusants = 0;
    for (int i = 0; i < ncand; i++) {
        const char* b = cands[i];
        int filtre = cli_doc_list_bucket_filters_by_category(b);

        /* Sans filtre : la reference. La troncature est desormais refusee par
         * le harnais lui-meme ; reste la vacuite, qui rendrait la
         * comparaison suivante vraie sans rien comparer. */
        capture_call_err(cli_doc_list, 3, ARGS("list", (char*)b, "--json"));
        size_t nref = strlen(g_buf);
        snprintf(label, sizeof label, "%s : capture sans filtre non vide", b);
        CHECK(nref > 0, label);
        memcpy(ref, g_buf, nref + 1);
        snprintf(label, sizeof label,
                 "%s : sans filtre, aucun verdict d'erreur", b);
        CHECK(strstr(ref, "\"unknown_category\"") == NULL &&
              strstr(ref, "category_filter_not_supported") == NULL, label);

        char* av[5];
        av[0] = "list";
        av[1] = (char*)b;
        av[2] = "--cat";
        av[3] = CAT_HORS_DOMAINE;
        av[4] = "--json";
        int rc = capture_call_err(cli_doc_list, 5, av);
        size_t ncur = strlen(g_buf);
        snprintf(label, sizeof label, "%s : capture sous --cat non vide", b);
        CHECK(ncur > 0, label);

        /* Le defaut ferme par le ticket, dit litteralement. */
        snprintf(label, sizeof label,
                 "%s --cat hors domaine : la reponse change", b);
        CHECK(strcmp(ref, g_buf) != 0, label);

        int inconnue = strstr(g_buf, "\"unknown_category\"") != NULL;
        int refus    = strstr(g_buf, "category_filter_not_supported") != NULL;
        snprintf(label, sizeof label,
                 "%s --cat hors domaine : exactement un verdict", b);
        CHECK(inconnue + refus == 1, label);
        snprintf(label, sizeof label,
                 "%s : le verdict rendu est celui du predicat", b);
        CHECK(inconnue == (filtre != 0) && refus == (filtre == 0), label);
        snprintf(label, sizeof label, "%s : rc coherent avec le verdict", b);
        CHECK_RC(rc, filtre ? 0 : 1, label);

        /* Contrat de `cli_doc_list_category_is_known`, inchange :
         * un bucket sans familles n'a aucune famille a invalider — c'est le
         * refus en amont qui traite le cas, pas ce predicat. */
        snprintf(label, sizeof label,
                 "%s : les deux predicats se recoupent", b);
        CHECK(cli_doc_list_category_is_known(b, CAT_HORS_DOMAINE)
              == (filtre ? 0 : 1), label);

        if (filtre) filtrants++; else refusants++;
    }
    CHECK(filtrants == CAT_BUCKETS_COUNT, "7 sous-commandes filtrent");
    CHECK(refusants == 4, "4 sous-commandes refusent `--cat`");
}

/* ================================================================
 * Verrou : un compteur JSON compte ce qu'il accompagne
 * ================================================================ */

/* Sous `--cat`, certains buckets rendaient un tableau filtre sous un compteur
 * reste global : `list keywords --cat Logique --json` annoncait 63 pour 8
 * entrees. Un compteur n'a qu'une raison d'exister — etre cru sans recompter.
 * Servi a un modele par l'outil MCP, il fait conclure a une reponse tronquee.
 *
 * Le verrou ne nomme pas les buckets fautifs : il relit les sous-commandes que
 * `owl list` dispatche, retient celles qui filtrent (verdict du predicat
 * exporte, pas du texte affiche), leur demande elles-memes leurs familles, et
 * compare chaque compteur au tableau qu'il accompagne — sans filtre et sous
 * chaque famille. Un bucket qui gagnerait `--cat` sans ajuster son compteur
 * rougit ici sans edit. */

/* Curseur pose sur le guillemet ouvrant d'une chaine JSON -> juste apres son
 * guillemet fermant. Sans ca, une description qui contient `{` ou `]` se
 * compte comme une entree. */
static const char* json_skip_string(const char* p) {
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p ? p + 1 : p;
}

/* Nombre d'entrees `{ ... }` de premier niveau du tableau `"<name>": [` place
 * a la racine de l'objet. -1 si le tableau est absent, -2 s'il n'est pas
 * referme (sortie tronquee : le silence serait pris pour un tableau vide). */
static int json_array_entries(const char* buf, const char* name) {
    char needle[96];
    snprintf(needle, sizeof needle, "\n  \"%s\": [", name);
    const char* p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    int depth = 0, n = 0;
    while (*p) {
        if (*p == '"') { p = json_skip_string(p); continue; }
        if (*p == '{') { if (depth == 0) n++; depth++; }
        else if (*p == '[') depth++;
        else if (*p == '}') depth--;
        else if (*p == ']') { if (depth == 0) return n; depth--; }
        p++;
    }
    return -2;
}

/* Chaines d'un tableau `"<name>": [ "a", "b" ]` de premier niveau. */
static int json_string_array(const char* buf, const char* name,
                             char out[][96], int max) {
    char needle[96];
    snprintf(needle, sizeof needle, "\n  \"%s\": [", name);
    const char* p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    int n = 0;
    while (*p && *p != ']') {
        if (*p != '"') { p++; continue; }
        const char* end = json_skip_string(p);
        size_t len = (size_t)(end - p) - 2;
        if (n < max && len < 96) {
            memcpy(out[n], p + 1, len);
            out[n][len] = '\0';
            n++;
        }
        p = end;
    }
    return n;
}

/* Compare chaque compteur de premier niveau au tableau qu'il accompagne :
 * `count` accompagne le tableau du bucket, `<X>_count` le tableau `<X>`.
 * Rend le nombre de compteurs examines — 0 signale une forme de sortie que ce
 * verrou ne sait pas lire, ce que l'appelant traite comme un echec plutot que
 * comme un silence. */
static int check_json_counters(const char* bucket, const char* cas) {
    char label[256];
    int examined = 0;

    /* La sortie JSON est refermee. Ce n'est plus une garde de troncature (le
     * harnais la refuse en amont) mais un controle du CLI lui-meme : un objet
     * non referme rendrait les comptages suivants absurdes. */
    size_t len = strlen(g_buf);
    snprintf(label, sizeof label, "%s [%s] : objet JSON refermé",
             bucket, cas);
    CHECK(len > 2 && g_buf[len - 1] == '\n' && g_buf[len - 2] == '}', label);

    const char* p = g_buf;
    while ((p = strstr(p, "\n  \"")) != NULL) {
        p += 4;
        char key[64];
        int k = 0;
        while (*p && *p != '"' && k < 63) key[k++] = *p++;
        key[k] = '\0';
        if (*p != '"' || strncmp(p, "\": ", 3) != 0) continue;
        const char* v = p + 3;
        if (*v < '0' || *v > '9') continue;   /* pas un compteur */
        int annonce = atoi(v);

        char arr[96];
        size_t klen = strlen(key);
        if (strcmp(key, "count") == 0) {
            snprintf(arr, sizeof arr, "%s", bucket);
        } else if (klen > 6 && strcmp(key + klen - 6, "_count") == 0) {
            snprintf(arr, sizeof arr, "%.*s", (int)(klen - 6), key);
        } else {
            continue;
        }

        int reel = json_array_entries(g_buf, arr);
        snprintf(label, sizeof label,
                 "%s [%s] : compteur \"%s\" accompagne un tableau \"%s\"",
                 bucket, cas, key, arr);
        CHECK(reel >= 0, label);
        if (reel < 0) continue;
        snprintf(label, sizeof label,
                 "%s [%s] : \"%s\" annonce %d, le tableau \"%s\" en rend %d",
                 bucket, cas, key, annonce, arr, reel);
        CHECK(annonce == reel, label);
        examined++;
    }
    return examined;
}

static void test_list_json_compteur_egale_taille_du_tableau(void) {
    static char cands[32][64];
    static char cats[128][96];
    char label[256];

    capture_call(cli_doc_complete, 2, ARGS("__complete", "list"));
    int ncand = split_lines(g_buf, cands, 32);
    CHECK(ncand == 11, "__complete list : 11 buckets (garde anti-vacuite)");

    int filtrants = 0, compteurs = 0;
    for (int i = 0; i < ncand; i++) {
        /* Le predicat exporte decide qui filtre : un bucket qui ignore `--cat`
         * n'a pas de compteur a accorder. */
        if (cli_doc_list_category_is_known(cands[i], CAT_HORS_DOMAINE)) continue;
        filtrants++;

        char* av[5];
        av[0] = "list";
        av[1] = cands[i];
        av[2] = "--cat";
        av[3] = CAT_HORS_DOMAINE;
        av[4] = "--json";

        /* Sans filtre : le compteur reste le total du bucket. */
        capture_call(cli_doc_list, 3, ARGS("list", cands[i], "--json"));
        int n = check_json_counters(cands[i], "sans filtre");
        snprintf(label, sizeof label,
                 "%s : au moins un compteur lu sans filtre", cands[i]);
        CHECK(n > 0, label);
        compteurs += n;

        /* Familles demandees au bucket lui-meme : une famille ajoutee au
         * catalogue entre dans le balayage sans edit ici. */
        capture_call(cli_doc_list, 5, av);
        int ncat = json_string_array(g_buf, "valid_categories", cats, 128);
        snprintf(label, sizeof label,
                 "%s : familles valables enumerees (garde anti-vacuite)",
                 cands[i]);
        CHECK(ncat > 0, label);

        for (int c = 0; c < ncat; c++) {
            av[3] = cats[c];
            capture_call(cli_doc_list, 5, av);
            n = check_json_counters(cands[i], cats[c]);
            snprintf(label, sizeof label,
                     "%s --cat %s : au moins un compteur lu",
                     cands[i], cats[c]);
            CHECK(n > 0, label);
            compteurs += n;
        }

        /* Famille hors domaine : le tableau est vide, le compteur aussi. */
        av[3] = CAT_HORS_DOMAINE;
        capture_call(cli_doc_list, 5, av);
        n = check_json_counters(cands[i], CAT_HORS_DOMAINE);
        compteurs += n;
    }
    CHECK(filtrants == CAT_BUCKETS_COUNT,
          "7 sous-commandes filtrent par famille");
    CHECK(compteurs > 100, "compteurs examines en nombre (garde anti-vacuite)");
}

/* Non-regression de l'autre moitie du contrat : sans `--cat`, les compteurs
 * restent le total du bucket. L'egalite compteur <-> tableau ci-dessus ne le
 * dit pas — un filtre applique par erreur sans filtre demande reduirait les
 * deux ensemble et resterait vert. On les ancre donc aux totaux du catalogue,
 * qui ne dependent pas de la sortie testee. */
static void test_list_json_compteur_sans_filtre_reste_le_total(void) {
    capture_call(cli_doc_list, 3, ARGS("list", "types", "--json"));
    char needle[64];
    snprintf(needle, sizeof needle, "\"types_count\": %d",
             owl_registry_types_count());
    CHECK_HAS(needle, "list types --json : types_count = total du registre");
    snprintf(needle, sizeof needle, "\"primitives_count\": %d",
             cli_doc_primitive_count());
    CHECK_HAS(needle, "list types --json : primitives_count = total primitifs");

    capture_call(cli_doc_list, 3, ARGS("list", "keywords", "--json"));
    snprintf(needle, sizeof needle, "\"count\": %d", owl_keyword_count());
    CHECK_HAS(needle, "list keywords --json : count = total des keywords");

    /* Et le filtre reduit reellement : sans cette borne, un compteur fige a 0
     * satisferait l'egalite compteur <-> tableau sur toutes les familles. */
    capture_call(cli_doc_list, 5,
                 ARGS("list", "keywords", "--cat", "Logique", "--json"));
    int sous_filtre = json_array_entries(g_buf, "keywords");
    CHECK(sous_filtre > 0 && sous_filtre < owl_keyword_count(),
          "keywords --cat Logique : sous-ensemble strict et non vide");
    capture_call(cli_doc_list, 5,
                 ARGS("list", "types", "--cat", "SQL", "--json"));
    sous_filtre = json_array_entries(g_buf, "types");
    CHECK(sous_filtre > 0 && sous_filtre < owl_registry_types_count(),
          "types --cat SQL : sous-ensemble strict et non vide");
}

/* ================================================================
 * Verrou : la sortie de `list props` enseigne la forme canon du champ
 * ================================================================ */

/* Le catalogue de proprietes DSL est indexe par des cles ASCII (`Fenetre`,
 * `ZoneRepetee`, `Libelle`) et les sorties les recopiaient telles quelles.
 * Ce que `owl list props` affiche est pourtant ce que l'appelant — humain ou
 * modele branche sur l'outil MCP — va recopier dans son code : une graphie
 * qui n'est pas celle du langage y est enseignee, et la liste de secours
 * servie sur champ inconnu l'enseigne au pire moment.
 *
 * La table ci-dessous est le contrat, ecrit en toutes lettres plutot que
 * derive du code qu'elle verifie. Les cinq dernieres entrees sont des
 * variantes contextuelles propres au catalogue de props (cellule de Table,
 * sous-champs d'un Etat) : elles n'ont pas d'entree dans le catalogue des
 * sortes de champ, leur forme canon est la concatenation des formes canon
 * des mots qui les composent. Toute forme retenue ici se replie sur sa cle
 * par suppression des accents : elle reste donc recevable telle quelle en
 * `--cat`, ce que le test verifie explicitement. */
static const struct { const char* cle; const char* canon; } PROPS_CANON[] = {
    { "Fenetre",        "Fenêtre"        },
    { "Libelle",        "Libellé"        },
    { "Bouton",         "Bouton"         },
    { "Saisie",         "Saisie"         },
    { "Table",          "Table"          },
    { "Conteneur",      "Conteneur"      },
    { "Image",          "Image"          },
    { "Colonne",        "Colonne"        },
    { "Liste",          "Liste"          },
    { "Combo",          "Combo"          },
    { "Selecteur",      "Sélecteur"      },
    { "Interrupteur",   "Interrupteur"   },
    { "ZoneRepetee",    "ZoneRépétée"    },
    { "Jauge",          "Jauge"          },
    { "CodeBarres",     "CodeBarres"     },
    { "Onglet",         "Onglet"         },
    { "Volet",          "Volet"          },
    { "Etat",           "État"           },
    { "MenuPrincipal",  "MenuPrincipal"  },
    { "MenuContextuel", "MenuContextuel" },
    { "SousMenu",       "SousMenu"       },
    { "OptionMenu",     "OptionMenu"     },
    { "MenuSeparateur", "MenuSéparateur" },
    { "CelluleTable",   "CelluleTable"   },
    { "blocEtat",       "blocÉtat"       },
    { "LibelleEtat",    "LibelléÉtat"    },
    { "ImageEtat",      "ImageÉtat"      },
    { "CodeBarresEtat", "CodeBarresÉtat" },
    { "ServiceREST",    "ServiceREST"    },
    { "OperationHTTP",  "OpérationHTTP"  },
};
#define PROPS_CANON_COUNT \
    ((int)(sizeof(PROPS_CANON) / sizeof(PROPS_CANON[0])))

/* Cles distinctes du catalogue de props, dans l'ordre de rencontre. */
static int props_kinds_distincts(const char* out[], int max) {
    int n = 0;
    for (int i = 0, t = owl_dsl_props_count(); i < t; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->kind) continue;
        int vu = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(out[j], e->kind) == 0) { vu = 1; break; }
        if (!vu && n < max) out[n++] = e->kind;
    }
    return n;
}

/* Verrou de totalite : aucun kind ne peut retomber en silence sur sa cle
 * brute. Un champ ajoute au catalogue sans forme canon rougit ici, et une
 * ligne devenue orpheline aussi — les deux sens sont verifies. */
static void test_list_props_tout_kind_a_une_forme_canon(void) {
    const char* kinds[64];
    char label[192];
    int n = props_kinds_distincts(kinds, 64);

    CHECK(n > 0, "props : catalogue non vide (garde anti-vacuite)");
    CHECK(n == PROPS_CANON_COUNT,
          "props : autant de formes canon attendues que de kinds au catalogue");

    for (int i = 0; i < n; i++) {
        int trouve = 0;
        for (int j = 0; j < PROPS_CANON_COUNT; j++)
            if (strcmp(PROPS_CANON[j].cle, kinds[i]) == 0) { trouve = 1; break; }
        snprintf(label, sizeof label,
                 "props : le kind '%s' a une forme canon connue", kinds[i]);
        CHECK(trouve, label);
    }
    for (int j = 0; j < PROPS_CANON_COUNT; j++) {
        int trouve = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(PROPS_CANON[j].cle, kinds[i]) == 0) { trouve = 1; break; }
        snprintf(label, sizeof label,
                 "props : la forme canon '%s' designe un kind reel",
                 PROPS_CANON[j].canon);
        CHECK(trouve, label);
    }
}

/* L'en-tete de groupe porte la forme canon, comme le bucket voisin `events`
 * le fait deja pour la meme sorte de champ. */
static void test_list_props_entetes_en_forme_canon(void) {
    char needle[128];
    char label[192];

    int rc = capture_call(cli_doc_list, 2, ARGS("list", "props"));
    CHECK_RC(rc, 0, "list props rc=0");

    for (int j = 0; j < PROPS_CANON_COUNT; j++) {
        snprintf(needle, sizeof needle, "── %s ──", PROPS_CANON[j].canon);
        snprintf(label, sizeof label,
                 "list props : en-tete '%s' en forme canon",
                 PROPS_CANON[j].canon);
        CHECK_HAS(needle, label);

        /* Et la forme sans accent a bien disparu de l'en-tete. */
        if (strcmp(PROPS_CANON[j].cle, PROPS_CANON[j].canon) == 0) continue;
        snprintf(needle, sizeof needle, "── %s ──", PROPS_CANON[j].cle);
        snprintf(label, sizeof label,
                 "list props : plus d'en-tete '%s' sans accent",
                 PROPS_CANON[j].cle);
        CHECK_NO(needle, label);
    }

    /* Meme forme que le temoin : `list events` sert de reference vivante. */
    capture_call(cli_doc_list, 2, ARGS("list", "events"));
    CHECK_HAS("── Fenêtre ──", "list events : en-tete canon (temoin)");
}

/* La liste de secours enumere ce que l'appelant est invite a recopier : elle
 * doit donc etre en forme canon, et chaque forme proposee doit etre acceptee
 * telle quelle par `--cat` — proposer une graphie que l'outil refuse serait
 * le pire des deux mondes. */
static void test_list_props_liste_de_secours_en_forme_canon(void) {
    char label[192];

    int rc = capture_call(cli_doc_list, 4,
                          ARGS("list", "props", "--cat", CAT_HORS_DOMAINE));
    CHECK_RC(rc, 0, "list props --cat hors domaine rc=0");
    CHECK_HAS("Champs disponibles", "liste de secours presente");
    for (int j = 0; j < PROPS_CANON_COUNT; j++) {
        snprintf(label, sizeof label, "liste de secours : '%s' propose",
                 PROPS_CANON[j].canon);
        CHECK_HAS(PROPS_CANON[j].canon, label);
    }
    CHECK_NO("Fenetre", "liste de secours : plus aucune forme sans accent");
    CHECK_NO("ZoneRepetee", "liste de secours : ZoneRepetee disparue");
    CHECK_NO("Selecteur", "liste de secours : Selecteur disparu");

    /* Et chaque forme proposee est effectivement recevable. */
    for (int j = 0; j < PROPS_CANON_COUNT; j++) {
        char* av[4];
        av[0] = "list";
        av[1] = "props";
        av[2] = "--cat";
        av[3] = (char*)PROPS_CANON[j].canon;
        snprintf(label, sizeof label, "props : '%s' est un champ valable",
                 PROPS_CANON[j].canon);
        CHECK(cli_doc_list_category_is_known("props", PROPS_CANON[j].canon) == 1,
              label);
        rc = capture_call(cli_doc_list, 4, av);
        snprintf(label, sizeof label, "list props --cat %s rc=0",
                 PROPS_CANON[j].canon);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label,
                 "list props --cat %s : pas signale comme inconnu",
                 PROPS_CANON[j].canon);
        CHECK_NO("champ inconnu", label);
    }

    /* Meme verdict cote machine : le tableau JSON des familles valables est
     * ce que l'outil MCP renvoie au modele. */
    capture_call(cli_doc_list, 5,
                 ARGS("list", "props", "--cat", CAT_HORS_DOMAINE, "--json"));
    CHECK_HAS("\"valid_categories\"", "JSON : familles valables enumerees");
    CHECK_HAS("\"Fenêtre\"", "JSON : famille valable en forme canon");
    CHECK_HAS("\"ZoneRépétée\"", "JSON : ZoneRépétée en forme canon");
    CHECK_NO("\"Fenetre\"", "JSON : plus de forme sans accent");
}

/* Le JSON porte le meme nom canon, sans qu'aucune cle ne soit renommee. */
static void test_list_props_json_widget_en_forme_canon(void) {
    capture_call(cli_doc_list, 5,
                 ARGS("list", "props", "--cat", "Fenetre", "--json"));
    CHECK_HAS("\"widget\": \"Fenêtre\"", "JSON : widget en forme canon");
    CHECK_NO("\"widget\": \"Fenetre\"", "JSON : plus de widget sans accent");
    CHECK_HAS("\"name_ascii\": ", "JSON : cle name_ascii inchangee");
    CHECK_HAS("\"name_en\": ", "JSON : cle name_en inchangee");
    CHECK_HAS("\"count\": ", "JSON : cle count inchangee");

    capture_call(cli_doc_list, 5,
                 ARGS("list", "props", "--cat", "ZoneRepetee", "--json"));
    CHECK_HAS("\"widget\": \"ZoneRépétée\"",
              "JSON : ZoneRépétée en forme canon");
    CHECK_NO("\"widget\": \"ZoneRepetee\"",
             "JSON : plus de ZoneRepetee sans accent");
}

/* Non-regression de  : un accent rendu en sortie ne doit pas devenir
 * une exigence a l'entree. Les quatre graphies designent le meme champ et
 * rendent toutes le meme en-tete canon. */
static void test_list_props_quatre_formes_entree_restent_acceptees(void) {
    static const char* const FORMES[] = { "Fenêtre", "FENÊTRE", "Fenetre",
                                          "FENETRE" };
    char label[192];

    for (int f = 0; f < 4; f++) {
        char* av[4];
        av[0] = "list";
        av[1] = "props";
        av[2] = "--cat";
        av[3] = (char*)FORMES[f];

        snprintf(label, sizeof label, "props : '%s' est un champ valable",
                 FORMES[f]);
        CHECK(cli_doc_list_category_is_known("props", FORMES[f]) == 1, label);

        int rc = capture_call(cli_doc_list, 4, av);
        snprintf(label, sizeof label, "list props --cat %s rc=0", FORMES[f]);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label,
                 "list props --cat %s : pas signale comme inconnu", FORMES[f]);
        CHECK_NO("champ inconnu", label);
        snprintf(label, sizeof label,
                 "list props --cat %s : en-tete rendu en forme canon",
                 FORMES[f]);
        CHECK_HAS("── Fenêtre ──", label);
    }
}

/* ================================================================
 * Verrou : `owl where <prop>` rend les champs en forme canon
 * ================================================================ */

/* Second point d'emission des cles brutes du catalogue de props, apres
 * `list props` : la liste « Acceptee par » de `where` / `explain`, alimentee
 * par find_prop_users. Meme contrat que le bucket `props`, meme table
 * PROPS_CANON — l'existence d'une seconde table de correspondance serait
 * precisement le defaut que la serie demonte. */

/* Chaines du tableau `"users": [...]` de `where --json`, a n'importe quelle
 * profondeur d'indentation (il est imbrique sous `"property"`). */
static int where_users(char out[][96], int max) {
    const char* p = strstr(g_buf, "\"users\": [");
    if (!p) return -1;
    p += sizeof "\"users\": [" - 1;
    int n = 0;
    while (*p && *p != ']') {
        if (*p != '"') { p++; continue; }
        const char* end = json_skip_string(p);
        size_t len = (size_t)(end - p) - 2;
        if (n < max && len < 96) {
            memcpy(out[n], p + 1, len);
            out[n][len] = '\0';
            n++;
        }
        p = end;
    }
    return n;
}

/* Forme canon attendue pour une cle du catalogue, selon PROPS_CANON. NULL si
 * la cle n'y figure pas — le verrou de totalite voisin traite ce cas. */
static const char* props_canon_de(const char* cle) {
    for (int j = 0; j < PROPS_CANON_COUNT; j++)
        if (strcmp(PROPS_CANON[j].cle, cle) == 0) return PROPS_CANON[j].canon;
    return NULL;
}

static void test_where_prop_users_forme_canon_texte(void) {
    int rc = capture_call(cli_doc_where, 2, ARGS("where", "Cadrage"));
    CHECK_RC(rc, 0, "where Cadrage rc=0");
    CHECK_HAS("Acceptée par (3 champs)", "where Cadrage : 3 champs");
    CHECK_HAS("\n  Libellé\n",   "where : Libellé en forme canon");
    CHECK_HAS("\n  ImageÉtat\n", "where : ImageÉtat en forme canon");
    CHECK_NO("\n  Libelle\n",    "where : plus de Libelle sans accent");
    CHECK_NO("\n  ImageEtat\n",  "where : plus de ImageEtat sans accent");

    /* Une prop largement partagee : les kinds a accent y passent tous. */
    rc = capture_call(cli_doc_where, 2, ARGS("where", "Grisé"));
    CHECK_RC(rc, 0, "where Grisé rc=0");
    CHECK_HAS("\n  Fenêtre\n",     "where Grisé : Fenêtre en forme canon");
    CHECK_HAS("\n  ZoneRépétée\n", "where Grisé : ZoneRépétée en forme canon");
    CHECK_HAS("\n  Sélecteur\n",   "where Grisé : Sélecteur en forme canon");
    CHECK_NO("\n  Fenetre\n",      "where Grisé : plus de Fenetre");
    CHECK_NO("\n  ZoneRepetee\n",  "where Grisé : plus de ZoneRepetee");
    CHECK_NO("\n  Selecteur\n",    "where Grisé : plus de Selecteur");
}

/* Le canal machine porte la meme forme : c'est celle que l'outil MCP sert au
 * modele, qui la recopiera dans du code OWL. */
static void test_where_prop_users_forme_canon_json(void) {
    int rc = capture_call(cli_doc_where, 3, ARGS("where", "Cadrage", "--json"));
    CHECK_RC(rc, 0, "where Cadrage --json rc=0");
    CHECK_HAS("\"Libellé\"",   "where JSON : Libellé en forme canon");
    CHECK_HAS("\"ImageÉtat\"", "where JSON : ImageÉtat en forme canon");
    CHECK_NO("\"Libelle\"",    "where JSON : plus de Libelle sans accent");
    CHECK_NO("\"ImageEtat\"",  "where JSON : plus de ImageEtat sans accent");
    CHECK_HAS("\"users\": [",  "where JSON : cle users inchangee");
}

/* Verrou de totalite, sans nommer de champ : pour chaque cle du catalogue de
 * props, on interroge `where` sur une propriete que ce champ accepte, et on
 * exige que le nom rendu soit la forme canon de PROPS_CANON — jamais la cle
 * brute. Un champ ajoute au catalogue est couvert sans edit ici. */
static void test_where_tout_kind_rendu_en_forme_canon(void) {
    const char* kinds[64];
    char label[224];
    int n = props_kinds_distincts(kinds, 64);
    CHECK(n > 0, "where : catalogue de props non vide (garde anti-vacuite)");

    int couverts = 0;
    for (int i = 0; i < n; i++) {
        /* Premiere propriete declaree par ce kind : elle suffit, `where` la
         * resout et doit renommer le kind dans la liste des utilisateurs. */
        const char* prop = NULL;
        for (int k = 0, t = owl_dsl_props_count(); k < t; k++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(k);
            if (e && e->kind && e->def && strcmp(e->kind, kinds[i]) == 0) {
                prop = e->def->nameFR;
                break;
            }
        }
        snprintf(label, sizeof label,
                 "where : le champ '%s' declare au moins une propriete",
                 kinds[i]);
        CHECK(prop != NULL, label);
        if (!prop) continue;

        const char* attendu = props_canon_de(kinds[i]);
        snprintf(label, sizeof label,
                 "where : le champ '%s' a une forme canon connue", kinds[i]);
        CHECK(attendu != NULL, label);
        if (!attendu) continue;

        char* av[4];
        av[0] = "where";
        av[1] = (char*)prop;
        av[2] = "--json";
        capture_call(cli_doc_where, 3, av);

        char users[64][96];
        int nu = where_users(users, 64);
        snprintf(label, sizeof label,
                 "where %s --json : liste des utilisateurs lisible", prop);
        CHECK(nu > 0, label);
        if (nu <= 0) continue;

        int vu_canon = 0, vu_cle = 0;
        for (int u = 0; u < nu; u++) {
            if (strcmp(users[u], attendu) == 0) vu_canon = 1;
            if (strcmp(users[u], kinds[i]) == 0 &&
                strcmp(kinds[i], attendu) != 0) vu_cle = 1;
        }
        snprintf(label, sizeof label,
                 "where %s : '%s' rendu en forme canon '%s'",
                 prop, kinds[i], attendu);
        CHECK(vu_canon, label);
        snprintf(label, sizeof label,
                 "where %s : la cle brute '%s' n'est plus rendue",
                 prop, kinds[i]);
        CHECK(!vu_cle, label);
        couverts++;
    }
    CHECK(couverts == PROPS_CANON_COUNT,
          "where : les 28 champs du catalogue sont couverts");
}

/* La regle que `` s'est imposee, appliquee au second point
 * d'emission : tout nom rendu par `where` reste recevable la ou il peut etre
 * resoumis — proposer une graphie que l'outil refuserait serait pire que la
 * cle brute. */
static void test_where_formes_rendues_restent_recevables(void) {
    char label[224];
    capture_call(cli_doc_where, 3, ARGS("where", "Grisé", "--json"));

    char users[64][96];
    int nu = where_users(users, 64);
    CHECK(nu > 0, "where Grisé --json : utilisateurs lisibles");

    for (int u = 0; u < nu; u++) {
        snprintf(label, sizeof label,
                 "where : la forme rendue '%s' est un champ valable", users[u]);
        CHECK(cli_doc_list_category_is_known("props", users[u]) == 1, label);

        char* av[4];
        av[0] = "list";
        av[1] = "props";
        av[2] = "--cat";
        av[3] = users[u];
        int rc = capture_call(cli_doc_list, 4, av);
        snprintf(label, sizeof label,
                 "list props --cat %s (forme rendue par where) rc=0", users[u]);
        CHECK_RC(rc, 0, label);
        snprintf(label, sizeof label,
                 "list props --cat %s : pas signale comme inconnu", users[u]);
        CHECK_NO("champ inconnu", label);
    }
}

/* ================================================================
 * Entry point
 * ================================================================ */

/*  — le surligneur de code CLI reconnaît une instance UI par son
 * préfixe de nommage. Ces préfixes DOIVENT rester alignés sur le canon du
 * registre (`owl_control_kinds_table()` .canonicalPrefix + conteneurs FEN_/
 * ETAT_), sinon un contrôle nommé canoniquement (TABLE_X) n'est pas coloré
 * comme instance tandis qu'un nom périmé (TBL_X) l'est à tort. */
static void test_ui_instance_prefixes_canon(void) {
    #define UI_YES(name, label) \
        CHECK(cli_doc_looks_like_ui_instance((name), (long)strlen(name)) == 1, label)
    #define UI_NO(name, label) \
        CHECK(cli_doc_looks_like_ui_instance((name), (long)strlen(name)) == 0, label)

    /* Préfixes canon divergents avant le fix — cœur de la garde. */
    UI_YES("TABLE_Commandes", "TABLE_ (canon) reconnu comme instance UI");
    UI_YES("COMBO_Pays",      "COMBO_ (canon) reconnu comme instance UI");
    UI_YES("LISTE_Produits",  "LISTE_ (canon) reconnu comme instance UI");
    UI_YES("SEL_Civilite",    "SEL_ (canon) reconnu comme instance UI");
    UI_YES("CONT_Formulaire", "CONT_ (canon) reconnu comme instance UI");

    /* Préfixes canon déjà OK avant le fix — non-régression. */
    UI_YES("FEN_Login",       "FEN_ (conteneur canon) reconnu");
    UI_YES("ETAT_Facture",    "ETAT_ (conteneur canon) reconnu");
    UI_YES("LIB_Titre",       "LIB_ (canon) reconnu");
    UI_YES("BTN_Valider",     "BTN_ (canon) reconnu");
    UI_YES("SAI_MotDePasse",  "SAI_ (canon) reconnu");
    UI_YES("IMG_Logo",        "IMG_ (canon) reconnu");
    UI_YES("INT_Newsletter",  "INT_ (canon) reconnu");
    UI_YES("COL_Reference",   "COL_ (canon) reconnu");
    UI_YES("ZR_Lignes",       "ZR_ (canon) reconnu");
    UI_YES("JAU_Progression", "JAU_ (canon) reconnu");

    /* Préfixes périmés (divergents du canon) : ne doivent PAS être reconnus,
     * sinon le surligneur bénit un nom que le compilateur déprécie. */
    UI_NO("TBL_X", "TBL_ (périmé) plus reconnu");
    UI_NO("CMB_X", "CMB_ (périmé) plus reconnu");
    UI_NO("SLT_X", "SLT_ (périmé) plus reconnu");
    UI_NO("LIS_X", "LIS_ (périmé) plus reconnu");
    UI_NO("CTR_X", "CTR_ (périmé) plus reconnu");
    UI_NO("ZRP_X", "ZRP_ (périmé) plus reconnu");
    UI_NO("RAP_X", "RAP_ (périmé) plus reconnu");

    /* Non-UI : variables ordinaires jamais colorées comme instances. */
    UI_NO("nCompteur",  "variable entière non reconnue comme instance UI");
    UI_NO("MaVariable", "identifiant nu non reconnu comme instance UI");
    /* Préfixe nu sans suffixe (len == préfixe) : pas une instance. */
    UI_NO("TABLE_", "préfixe seul (sans nom) non reconnu");

    #undef UI_YES
    #undef UI_NO
}

int cli_doc_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_help();
    test_help_completion();
    test_help_completeness();
    test_help_serve();
    test_primer();
    test_learn_list();
    test_learn_list_alias_pour_toutes_les_fiches();
    test_learn_nouveaux_alias_resolvent();
    test_learn_alias_coverage_exhaustive();
    test_learn_types();
    test_learn_unknown();
    test_list_top();
    test_list_types();
    test_list_types_json();
    test_list_natives_cat_http();
    test_list_natives_cat_bogus();
    test_list_natives_cat_bilingue();
    test_list_natives_cat_json_unknown();
    test_list_limits();
    test_list_limits_json();
    test_explain_limit();
    test_list_constants_json_desc();
    test_list_constants_lang_en();
    test_explain_native_mono_lang();
    test_explain_keyword_honors_global_lang();
    test_explain_type_field_names_en();
    test_explain_neighbors_names_en();
    test_explain_neighbors_names_fr();
    test_explain_detail_category_native_en();
    test_explain_detail_category_native_fr();
    test_explain_detail_category_type_en();
    test_explain_detail_category_const_en();
    test_explain_detail_category_enum_en();
    test_explain_detail_category_enum_fr();
    test_explain_detail_category_operator_en();
    test_explain_native_json_signatures_mono();
    test_explain_native_json_signatures_multi();
    test_explain_native_json_signatures_variadic();
    test_explain_native_json_methods();
    test_explain_type_json_methods();
    test_explain_method_pure_mono();
    test_explain_method_pure_bilingual();
    test_explain_method_homonym();
    test_explain_method_qualified();
    test_explain_method_pure_json();
    test_explain_method_no_regression_type();
    test_explain_enum_json_name_ascii();
    test_explain_enum_used_by_text();
    test_explain_enum_used_by_json();
    test_explain_enum_unused_no_section();
    test_explain_enum_unused_no_section_json();
    test_explain_const_used_by_text();
    test_explain_const_used_by_json();
    test_explain_type_used_by_narrow_text();
    test_explain_type_used_by_narrow_json();
    test_explain_type_used_by_large_text();
    test_explain_type_used_by_large_json();
    test_explain_primitive_no_used_by_json();
    test_explain_const_en_form();
    test_search_const_en_form();
    test_search_relevance_ranking();
    test_explain_missing();
    test_explain_unknown();
    test_explain_bad_flag();
    test_explain_cat_bogus();
    test_explain_error_fr();
    test_explain_error_en();
    test_explain_error_case_insensitive();
    test_explain_error_unknown();
    test_explain_error_cat_filter();
    test_explain_error_json();
    test_explain_native();
    test_explain_uuid_128bit_only();
    test_explain_primitive();
    test_explain_primitive_ascii();
    test_explain_multi_bucket();
    test_explain_cat_filter();
    test_explain_cat_all();
    test_explain_type_props();
    test_explain_signatures();
    /*  — signatures à préfixe de params partagé bornées par l'arité */
    test_explain_shared_prefix_arity_texte();
    test_explain_shared_prefix_arity_json();
    test_explain_variadic_params_preserves();
    test_explain_scenario_bullet();
    test_examples();
    test_examples_missing();
    test_explain_table_renvoi_colonne();
    test_explain_table_renvoi_colonne_en();
    test_examples_table_et_colonne();
    test_examples_strip_wrapper_texte();
    test_examples_strip_preserve_fin_interne();
    test_examples_strip_wrapper_json();
    test_examples_titre_visibilite_canon();
    test_examples_fixture_nom_accentue();
    test_examples_sujet_composite_prefixe();
    test_scenario_decl_balayage_graphies();
    test_scenario_strip_balayage_graphies();
    test_scenario_line_depth_delta();
    test_examples_si_monoligne_ne_avale_pas();
    test_examples_sujet_slash_separateur();
    test_examples_sujet_methode_pointee();
    test_explain_method_scenarios();
    test_search();
    test_search_no_hit();
    test_search_multimot();
    test_search_multimot_casse_accents();
    test_search_multimot_ordre();
    test_search_multimot_espaces();
    test_search_multimot_or_partial();
    test_search_multimot_no_hit();
    test_search_widgets();
    test_search_widgets_en();
    test_search_widgets_json();
    test_search_type_property_descen();
    test_search_type_property_byname();
    test_search_type_property_json();
    test_explain_events_kind_routing();
    test_complete_top();
    test_complete_explain_prefix();
    test_complete_explain_accent();
    test_explain_related_native_trace();
    test_explain_related_native_trace_json();
    test_explain_related_dsl_prop_grise();
    test_explain_related_type_neighbors_json();
    test_explain_dsl_prop_defaults();
    test_explain_related_rare_symbol();
    test_crossref_table_entries_valid();
    test_crossref_entry_valid_detects_dangling();
    test_crossref_pin_visible_in_explain();
    test_template_list();
    test_template_show_hello();
    test_template_show_classe();
    test_template_unknown();
    test_template_list_json();
    test_template_show_json();
    test_template_export_output();
    test_template_export_o_court();
    test_template_out_rejete();
    test_gotchas();
    test_cheatsheet();
    test_cheatsheet_examples_compile();

    /*  — owl icons (list/search/preview) */
    test_icons_aide_top();
    test_icons_list_sans_set_refuse();
    test_icons_list_avec_set_ok();
    test_icons_list_set_inexistant();
    test_icons_search_basique();
    test_icons_search_multi_mot_and();
    test_icons_search_sans_mot();
    test_icons_search_fr_maison();
    test_icons_search_fr_coeur();
    test_icons_search_fr_accent();
    test_icons_search_fallback_en();
    test_icons_preview();
    test_icons_preview_inexistant();
    test_icons_preview_size_hors_borne();
    test_icons_complete_top_inclut_icons();
    test_icons_complete_position_icons();

    /*  — enum touche, allowedBase plain text, --cat général */
    test_explain_enum_touche();
    /*  — list enums retiré comme catégorie (explain conservé) */
    test_list_enums_retire_categorie();
    test_explain_envoie_touche_allowedbase();
    test_explain_touche_enfoncee_allowedbase();
    test_list_consts_cat_clavier();
    test_list_consts_cat_http();

    /*  — convention casse enums minuscule-first */
    test_enums_casse_minuscule_first();

    /*  — export catalogue couleurs (nom FR/EN → #RRGGBB) */
    test_colors_catalog_export();

    /*  — membres d'un littéral { ... } en vert foncé */
    test_learn_litterale_membres_vert_fonce();

    /*  — opérateurs de base explain-ables + desc dans list/--json */
    test_explain_op_plus();
    test_explain_op_mul();
    test_explain_op_ineq();
    test_explain_op_lte();
    test_explain_op_base_famille_arithmetique();
    test_explain_op_base_famille_comparaison();
    test_list_operators_desc_present_texte();
    test_list_operators_desc_json();
    test_explain_op_plus_json();
    test_explain_op_ineq_json();
    /* Régressions : ops spéciaux et ET/OU inchangés */
    test_regression_ops_speciaux_explain();
    test_regression_ops_speciaux_listes();
    test_regression_et_ou_keywords();

    /*  — diff "Nouveautés" (contrat testable, rouge attendu
     * tant que owl_doc_novelties_diff n'existe pas encore). */
    test_novelties_ajout_symbole();
    test_novelties_ajout_chapitre_doc();
    test_novelties_aucun_changement();
    test_novelties_suppression_non_listee();
    test_novelties_tri_deterministe();
    test_novelties_champs_non_vides();
    test_novelties_out_cap_borne();
    test_novelties_sets_vides();

    /*  — indice de migration sur les formes héritées de fin de ligne */
    test_explain_rc_indice_migration_fr();
    test_explain_rc_indice_migration_en();
    test_explain_cr_indice_migration_fr();
    test_explain_cr_indice_migration_en();
    test_explain_crlf_reste_symbole_sans_indice();
    test_explain_symbole_inconnu_sans_indice();
    test_explain_inconnu_bloc_notfound_en();
    test_explain_inconnu_bloc_notfound_fr();
    test_explain_rc_bloc_complet_en();

    /*  — completion `owl list` recousue au dispatch reel */
    test_list_completion_coherente_avec_dispatch();
    test_list_cat_hors_domaine_uniforme();
    test_list_cat_valide_mais_vide();
    test_list_cat_champ_dsl_accent_insensible();
    test_list_cat_hors_domaine_en();
    test_list_cat_predicat_est_la_source_du_diagnostic();
    test_list_cat_predicat_couvre_les_familles_rendues();

    /*  — `--cat` n'est plus avale par les buckets sans familles */
    test_list_cat_non_filtrable_refuse();
    test_list_cat_non_filtrable_en();
    test_list_sans_cat_buckets_non_filtrables_inchanges();
    test_list_cat_aucun_bucket_ne_reste_muet();

    /*  — un compteur JSON compte ce qu'il accompagne */
    test_list_json_compteur_egale_taille_du_tableau();
    test_list_json_compteur_sans_filtre_reste_le_total();

    /*  — `list props` enseigne la forme canon du nom de champ */
    test_list_props_tout_kind_a_une_forme_canon();
    test_list_props_entetes_en_forme_canon();
    test_list_props_liste_de_secours_en_forme_canon();
    test_list_props_json_widget_en_forme_canon();
    test_list_props_quatre_formes_entree_restent_acceptees();

    /*  lot 1 — `where` rend les champs en forme canon */
    test_where_prop_users_forme_canon_texte();
    test_where_prop_users_forme_canon_json();
    test_where_tout_kind_rendu_en_forme_canon();
    test_where_formes_rendues_restent_recevables();

    /*  — préfixes UI du surligneur alignés sur le canon du registre */
    test_ui_instance_prefixes_canon();

    fprintf(stderr, "cli_doc_self_test: %d cas executes (%d pass, %d fail)\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide : "0 fail" ne doit pas pouvoir signifier
     * "rien n'a ete execute". */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "cli_doc_self_test: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
