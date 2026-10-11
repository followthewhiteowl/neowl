// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_doc.c — sous-commandes documentaires CLI.
 *
 * Conventions :
 *   - Sortie par défaut : texte ANSI coloré sur tty, plain sinon.
 *   - --plain / --no-color force le plain.
 *   - --json sort un JSON structuré (sans couleur, schéma stable).
 *   - --lang en / --en bascule en EN (avec fallback FR si non traduit).
 *
 * Modules consommés :
 *   - cli_colors  : palette ANSI canon OWL
 *   - embedded_docs : fondations + scénarios embarqués au build
 */

#include "cli_doc.h"
#include "cli_colors.h"
#include "common.h"           /* OWL_VERSION */
#include "embedded_docs.h"
#include "owl_registry.h"
#include "owl_limits.h"
/*  — registry des codes d'erreur. On bypasse l'API compile-aware
 * (qui tire scanner.h, déjà inclus plus bas — pas de collision ici, mais
 * cli_doc.c n'a besoin que de l'API d'introspection). */
#include "owl_errors.h"
#include "ui_decl.h"           /* OwlControlKind + owl_control_kinds_table */
#include "ui_catalog.h"        /* owl_dsl_prop_at + owl_dsl_props_count */
#include "owl_event_catalog.h" /* owl_event_catalog_count/at */
#include "owl_keyword_catalog.h" /* catalogue keyword (bucket KEYWORD) */
#include "owl_operator_catalog.h" /* catalogue opérateur (bucket OPERATOR) */
#include "owl_globalvar_catalog.h" /* catalogue variables globales prédéfinies */
#include "compiler.h" /* compiler_iter_builtin_props_full pour BUCKET_BUILTIN_PROP */
#include "owl_methods.h" /* method_catalog pour BUCKET_METHOD */
#include "scanner.h"  /* scanner_strip_accents — comparaison de mots-clés accent-insensible */
#include "prescan.h"  /* prescan_depth_delta — règle unique « ce token ouvre-t-il un bloc ? » */
#include "owl_categories.h" /* table bilingue des catégories (owl_categories.def) */
#include "formatter.h" /* owl_format — canonisation du corps des templates émis */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 * Parsing des flags communs
 * ------------------------------------------------------------------ */

typedef enum {
    DOC_LANG_FR = 0,
    DOC_LANG_EN = 1,
} DocLang;

typedef struct {
    int         plain;          /* 1 si --plain / --no-color */
    int         force_color;    /* 1 si --color (force même hors tty / pipe) */
    int         json;           /* 1 si --json */
    DocLang     lang;           /* FR par défaut */
    const char* cat;            /* --cat <X> : sens dépend de la sous-commande */
    const char* out;            /* -o / --output <path> : sortie fichier (template) */
    int         bad_flag;       /* 1 si un --xxx inconnu a été rencontré */
    const char* bad_flag_name;  /* nom du flag inconnu (pour message d'erreur) */
    /* Args positionnels après consommation des flags. */
    int       pos_argc;
    char**    pos_argv;
} DocOpts;

/* Filtre argc/argv : pop les flags reconnus, garde le reste dans
 * opts->pos_argv/pos_argc. argv reste valide (on ne free rien, le caller
 * gère). */
static void parse_doc_opts(int argc, char** argv, DocOpts* opts,
                           char** scratch) {
    opts->plain         = 0;
    opts->force_color   = 0;
    opts->json          = 0;
    /*  : defaut = langue globale. `--lang en`/`--lang=en` sont
     * consommes par le pre-filtre main.c et stockes dans
     * owl_errors, hors d'atteinte d'argv ici. Lire ce global comme defaut
     * fait honorer `--lang en` aux commandes doc. Les flags doc-locaux
     * --en/--fr/--lang ci-dessous restent prioritaires. */
    opts->lang          = owl_error_render_lang_is_en() ? DOC_LANG_EN
                                                        : DOC_LANG_FR;
    opts->cat           = NULL;
    opts->out           = NULL;
    opts->bad_flag      = 0;
    opts->bad_flag_name = NULL;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--plain") == 0 || strcmp(a, "--no-color") == 0) {
            opts->plain = 1;
        } else if (strcmp(a, "--color") == 0 || strcmp(a, "--force-color") == 0) {
            opts->force_color = 1;
        } else if (strcmp(a, "--json") == 0) {
            opts->json = 1;
        } else if (strcmp(a, "--en") == 0) {
            opts->lang = DOC_LANG_EN;
        } else if (strcmp(a, "--fr") == 0) {
            opts->lang = DOC_LANG_FR;
        } else if (strcmp(a, "--lang") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (strcmp(v, "en") == 0 || strcmp(v, "EN") == 0)
                opts->lang = DOC_LANG_EN;
            else
                opts->lang = DOC_LANG_FR;
        } else if ((strcmp(a, "--cat") == 0 || strcmp(a, "--category") == 0) &&
                   i + 1 < argc) {
            /* La catégorie est une CLÉ, et cette clé est bilingue : la forme
             * FR et la forme EN désignent la même catégorie. On normalise ici,
             * au point d'entrée unique, vers la forme portée par le registre —
             * les filtres en aval restent de simples comparaisons.
             *
             * Un nom absent de la table est laissé tel quel : `--cat` désigne
             * aussi des kinds de widget (`list props --cat Fenetre`), des
             * buckets d'`explain` (`--cat natives`) et des catégories de
             * limites, qui ne relèvent pas de cette table. */
            opts->cat = argv[++i];
            const OwlCategoryPair* pair = owl_category_resolve(opts->cat);
            if (pair) opts->cat = pair->fr;
        } else if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) &&
                   i + 1 < argc) {
            opts->out = argv[++i];
        } else if (a[0] == '-' && a[1] == '-') {
            /* Flag long inconnu — on le mémorise pour signaler l'erreur
             * une fois (la sous-commande choisit comment réagir). On ne
             * touche pas pos_argv pour ne pas masquer ce qui suivait. */
            if (!opts->bad_flag) {
                opts->bad_flag      = 1;
                opts->bad_flag_name = a;
            }
        } else {
            scratch[n++] = argv[i];
        }
    }
    opts->pos_argc = n;
    opts->pos_argv = scratch;

    /* Activation effective des couleurs.
     * Priorité : --plain > --json > --color > AUTO. */
    if (opts->plain || opts->json) {
        cli_colors_force(CLI_COLOR_PLAIN);
    } else if (opts->force_color || getenv("FORCE_COLOR")) {
        cli_colors_force(CLI_COLOR_FORCE);
    } else {
        cli_colors_force(CLI_COLOR_AUTO);
    }
}

/* Vrai si l'argument est un flag long non reconnu. Imprime un message
 * d'erreur sur stderr listant les flags valides pour cette sous-commande
 * et le code de sortie 64 (usage error, sysexits convention). */
static int reject_bad_flag(const DocOpts* opts, const char* subcmd_hint) {
    if (!opts->bad_flag) return 0;
    fprintf(stderr, "owl %s : flag inconnu '%s'.\n",
            subcmd_hint, opts->bad_flag_name);
    fprintf(stderr, "  Flags valides : --plain | --color | --json | "
                    "--lang fr|en | --cat <X> | -o|--output <fichier>\n");
    return 1;
}

/* Catégories de bucket pour `explain --cat <X>`. Aligné sur les buckets
 * de `cli_doc_explain` : primitives, types composés, fonctions, enums,
 * constantes. Plusieurs alias FR/EN par bucket pour ergonomie. */
typedef enum {
    BUCKET_PRIMITIVE  = 1 << 0,
    BUCKET_TYPE       = 1 << 1,
    BUCKET_NATIVE     = 1 << 2,
    BUCKET_ENUM       = 1 << 3,
    BUCKET_CONST      = 1 << 4,
    BUCKET_WIDGET     = 1 << 5,  /* DSL Fenêtre : Bouton, Saisie, Libellé… */
    BUCKET_DSL_PROP   = 1 << 6,  /* Propriété DSL : Grisé, Visible, Titre — fiche sans reverse lookup (cf. `owl where` pour ça). */
    BUCKET_KEYWORD    = 1 << 7,  /* Mot-clé du langage : SI, SELON, PAS, etc. */
    BUCKET_OPERATOR   = 1 << 8,  /* Opérateur symbolique : +, [~~, ??*, etc. */
    BUCKET_EVENT      = 1 << 9,  /* Event DSL : surClic, surMolette, AvantImpression… */
    BUCKET_GLOBALVAR  = 1 << 10, /* Variable globale prédéfinie : ErreurDétectée… */
    BUCKET_BUILTIN_PROP = 1 << 11, /* Propriété sur primitif : .Année, .EnHeures, .Vide… */
    BUCKET_METHOD     = 1 << 12, /* Forme méthode d'une native : .VersChaîne, .Debut, .Taille… */
    BUCKET_LIMIT      = 1 << 13, /* Limite compile-time : MAX_USER_TYPES, MAX_DECLARED_METHODS… */
    BUCKET_ERROR      = 1 << 14, /* Code d'erreur runtime/compile : E_LEX_*, E_RUNTIME_*, …. */
    BUCKET_ALL        = BUCKET_PRIMITIVE | BUCKET_TYPE | BUCKET_NATIVE |
                        BUCKET_ENUM | BUCKET_CONST | BUCKET_WIDGET |
                        BUCKET_DSL_PROP | BUCKET_KEYWORD | BUCKET_OPERATOR |
                        BUCKET_EVENT | BUCKET_GLOBALVAR | BUCKET_BUILTIN_PROP |
                        BUCKET_METHOD | BUCKET_LIMIT | BUCKET_ERROR,
} BucketMask;

/* Parse `--cat <X>` pour la sémantique explain. Retourne 0 si la chaîne
 * n'est pas un bucket reconnu ; retourne un mask non-nul sinon. */
static BucketMask parse_explain_cat(const char* cat) {
    if (!cat) return BUCKET_ALL;
    /* Pluriel + singulier + FR/EN + sans accent acceptés. */
    if (!strcmp(cat, "all") || !strcmp(cat, "tout") || !strcmp(cat, "tous"))
        return BUCKET_ALL;
    if (!strcmp(cat, "type") || !strcmp(cat, "types"))
        return BUCKET_PRIMITIVE | BUCKET_TYPE;
    if (!strcmp(cat, "primitive") || !strcmp(cat, "primitives") ||
        !strcmp(cat, "primitif")  || !strcmp(cat, "primitifs"))
        return BUCKET_PRIMITIVE;
    if (!strcmp(cat, "composé")  || !strcmp(cat, "compose")  ||
        !strcmp(cat, "composés") || !strcmp(cat, "composes") ||
        !strcmp(cat, "composite") || !strcmp(cat, "composites"))
        return BUCKET_TYPE;
    if (!strcmp(cat, "native")    || !strcmp(cat, "natives") ||
        !strcmp(cat, "fonction")  || !strcmp(cat, "fonctions") ||
        !strcmp(cat, "function")  || !strcmp(cat, "functions"))
        return BUCKET_NATIVE;
    if (!strcmp(cat, "enum")  || !strcmp(cat, "enums") ||
        !strcmp(cat, "énum") || !strcmp(cat, "énums") ||
        !strcmp(cat, "énumération") || !strcmp(cat, "énumérations") ||
        !strcmp(cat, "enumeration")   || !strcmp(cat, "enumerations"))
        return BUCKET_ENUM;
    if (!strcmp(cat, "const") || !strcmp(cat, "consts") ||
        !strcmp(cat, "constant") || !strcmp(cat, "constants") ||
        !strcmp(cat, "constante") || !strcmp(cat, "constantes"))
        return BUCKET_CONST;
    if (!strcmp(cat, "control") || !strcmp(cat, "controls") ||
        !strcmp(cat, "champ")   || !strcmp(cat, "champs"))
        return BUCKET_WIDGET;
    if (!strcmp(cat, "prop") || !strcmp(cat, "props") ||
        !strcmp(cat, "propriété")  || !strcmp(cat, "propriétés") ||
        !strcmp(cat, "propriete")  || !strcmp(cat, "proprietes") ||
        !strcmp(cat, "property")   || !strcmp(cat, "properties"))
        return BUCKET_DSL_PROP;
    if (!strcmp(cat, "keyword") || !strcmp(cat, "keywords") ||
        !strcmp(cat, "mot-clé") || !strcmp(cat, "mot-cle") ||
        !strcmp(cat, "mots-clés") || !strcmp(cat, "mots-cles"))
        return BUCKET_KEYWORD;
    if (!strcmp(cat, "operator") || !strcmp(cat, "operators") ||
        !strcmp(cat, "opérateur") || !strcmp(cat, "operateur") ||
        !strcmp(cat, "opérateurs") || !strcmp(cat, "operateurs"))
        return BUCKET_OPERATOR;
    if (!strcmp(cat, "event") || !strcmp(cat, "events") ||
        !strcmp(cat, "évent") || !strcmp(cat, "évents") ||
        !strcmp(cat, "événement") || !strcmp(cat, "événements") ||
        !strcmp(cat, "evenement") || !strcmp(cat, "evenements"))
        return BUCKET_EVENT;
    if (!strcmp(cat, "globalvar") || !strcmp(cat, "globalvars") ||
        !strcmp(cat, "variable")  || !strcmp(cat, "variables"))
        return BUCKET_GLOBALVAR;
    if (!strcmp(cat, "builtin-prop") || !strcmp(cat, "builtin-props") ||
        !strcmp(cat, "builtinprop")  || !strcmp(cat, "builtinprops"))
        return BUCKET_BUILTIN_PROP;
    if (!strcmp(cat, "method") || !strcmp(cat, "methods") ||
        !strcmp(cat, "méthode") || !strcmp(cat, "méthodes") ||
        !strcmp(cat, "methode") || !strcmp(cat, "methodes"))
        return BUCKET_METHOD;
    if (!strcmp(cat, "limit") || !strcmp(cat, "limits") ||
        !strcmp(cat, "limite") || !strcmp(cat, "limites"))
        return BUCKET_LIMIT;
    if (!strcmp(cat, "error") || !strcmp(cat, "errors") ||
        !strcmp(cat, "erreur") || !strcmp(cat, "erreurs"))
        return BUCKET_ERROR;
    return 0; /* inconnu */
}

/* ------------------------------------------------------------------
 * Aliases de fondations
 *
 * Permet à l'utilisateur d'écrire `owl learn types` au lieu du nom de
 * fichier complet `01_types_et_coercions`.
 * ------------------------------------------------------------------ */

typedef struct {
    const char* alias;         /* nom court accepté en ligne de commande */
    const char* file_stem;     /* nom de fichier (basename sans .md) */
} PublicDocAlias;

static const PublicDocAlias PUBLIC_DOC_ALIASES[] = {
    { "apercu",       "00_apercu_du_langage" },
    { "primer",       "00_apercu_du_langage" },
    { "overview",     "00_apercu_du_langage" },
    { "types",        "01_types_et_coercions" },
    { "coercions",    "01_types_et_coercions" },
    { "null",         "02_null_et_nullable" },
    { "nullable",     "02_null_et_nullable" },
    { "controle",     "03_controle_de_flux" },
    { "flow",         "03_controle_de_flux" },
    { "operateurs",   "05_operateurs" },
    { "operators",    "05_operateurs" },
    { "poo",          "06_poo" },
    { "oop",          "06_poo" },
    { "classes",      "06_poo" },
    { "heritage",     "07_poo_avance" },
    { "inheritance",  "07_poo_avance" },
    { "exceptions",   "08_exceptions" },
    { "errors",       "08_exceptions" },
    { "conventions",  "10_conventions_de_code" },
    { "code",         "10_conventions_de_code" },
    { "test",         "11_charte_de_test" },
    { "tests",        "11_charte_de_test" },
    { "charte",       "11_charte_de_test" },
    { "prefixes",     "12_prefixes_de_nommage" },
    { "naming",       "12_prefixes_de_nommage" },
    { "sql",          "13_sql_modele" },
    { "modele",       "13_sql_modele" },
    { "api",          "13_sql_modele" },
    { "connecteurs",  "14_sql_connecteurs" },
    { "backends",     "14_sql_connecteurs" },
    { "drivers",      "14_sql_connecteurs" },
    { "lifecycle",    "15_lifecycle_champs" },
    { "events",       "15_lifecycle_champs" },
    { "etats",        "16_lifecycle_etats" },
    { "rapports",     "16_lifecycle_etats" },
    { "reports",      "16_lifecycle_etats" },
    { "litterale",    "17_init_litterale" },
    { "literal",      "17_init_litterale" },
    { "inline",       "17_init_litterale" },
    { "init",         "17_init_litterale" },
    { "layouts",      "18_layouts" },
    { "layout",       "18_layouts" },
    { "disposition",  "18_layouts" },
    { "texte",        "19_texte_riche" },
    { "richtext",     "19_texte_riche" },
    { "markdown",     "19_texte_riche" },
    { "groupes",      "20_groupes_de_champs" },
    { "groups",       "20_groupes_de_champs" },
    { "i18n",         "21_internationalisation" },
    { "langues",      "21_internationalisation" },
    { "gotchas",      "22_pieges_courants" },
    { "pieges",       "22_pieges_courants" },
    { "cheatsheet",   "23_aide_memoire" },
    { "aidememoire",  "23_aide_memoire" },
    { "projet",       "24_projet" },
    { "project",      "24_projet" },
    { "ia",           "25_agents_ia" },
    { "ai",           "25_agents_ia" },
    { "mcp",          "25_agents_ia" },
    { "agents",       "25_agents_ia" },
    { "encodage",     "26_encodage_et_io" },
    { "encoding",     "26_encodage_et_io" },
    { "io",           "26_encodage_et_io" },
    { "buffers",      "27_buffers" },
    { "buffer",       "27_buffers" },
    { "tampons",      "27_buffers" },
    { "debogueur",    "28_debogueur" },
    { "debugger",     "28_debogueur" },
    { "debug",        "28_debogueur" },
    { "couleurs",     "29_couleurs" },
    { "colors",       "29_couleurs" },
    { "couleur",      "29_couleurs" },
    { "tableaux",     "30_tableaux_associatifs" },
    { "associatifs",  "30_tableaux_associatifs" },
    { "maps",         "30_tableaux_associatifs" },
    { "indirection",  "31_indirection" },
    { "indirect",     "31_indirection" },
    { "themes",       "32_themes" },
    { "theme",        "32_themes" },
    { "vecteurs",     "33_vecteurs" },
    { "vecteur",      "33_vecteurs" },
    { "vectors",      "33_vecteurs" },
    { "vector",       "33_vecteurs" },
    { "iamodele",     "34_iamodele" },
    { "aimodel",      "34_iamodele" },
    { "reseau",       "34_iamodele" },
    { "neurones",     "34_iamodele" },
    { "llm",          "35_llm" },
    { "gguf",         "35_llm" },
    { "aws",          "36_aws" },
    { "cloud",        "36_aws" },
    { "tenseurs",     "37_tenseurs_et_donnees" },
    { "tenseur",      "37_tenseurs_et_donnees" },
    { "tensors",      "37_tenseurs_et_donnees" },
    { "tensor",       "37_tenseurs_et_donnees" },
    { "donnees",      "37_tenseurs_et_donnees" },
    { "entrainement", "38_entrainement_et_detection" },
    { "training",     "38_entrainement_et_detection" },
    { "detection",    "38_entrainement_et_detection" },
    { "detect",       "38_entrainement_et_detection" },
    { "ml",              "39_machine_learning" },
    { "apprentissage",   "39_machine_learning" },
    { "machinelearning", "39_machine_learning" },
    { "service",      "40_services_rest" },
    { "services",     "40_services_rest" },
    { "servicerest",  "40_services_rest" },
    { "rest",         "40_services_rest" },
    { "messagerie",   "41_messagerie" },
    { "messaging",    "41_messagerie" },
    { "mq",           "41_messagerie" },
    { "courriel",     "42_courriel" },
    { "email",        "42_courriel" },
    { "smtp",         "42_courriel" },
    { "packaging",    "43_packaging" },
    { "package",      "43_packaging" },
    { "empaqueter",   "43_packaging" },
    { "distribuer",   "43_packaging" },
    { NULL, NULL }
};

/* Cherche une fondation par alias OU nom de fichier exact. Retourne
 * l'entrée embarquée ou NULL. */
static const OwlEmbeddedDoc* find_public_doc(const char* query) {
    if (!query || !*query) return NULL;

    /* 1. Match exact sur le name (stem) */
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        if (strcmp(d->name, query) == 0) return d;
    }
    /* 2. Match par alias */
    for (const PublicDocAlias* a = PUBLIC_DOC_ALIASES; a->alias; a++) {
        if (strcmp(a->alias, query) == 0) {
            for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
                if (strcmp(d->name, a->file_stem) == 0) return d;
            }
        }
    }
    /* 3. Match préfixe : "01" matche "01_types_et_coercions" */
    size_t qn = strlen(query);
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        if (strncmp(d->name, query, qn) == 0 &&
            (d->name[qn] == '_' || d->name[qn] == '\0')) {
            return d;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Rendu markdown minimaliste vers terminal
 *
 * V0 :
 *   - Lignes commençant par "# ", "## ", "### " → C_SECTION + bold
 *   - Lignes commençant par "```" → marqueur de bloc code (couleur off)
 *   - Le reste : passe-through
 *
 * Pas de support inline `*italic*` / `**bold**` ni des tables — la lecture
 * markdown brute reste correcte. À étendre quand le besoin émerge.
 * ------------------------------------------------------------------ */

/* --- Tokenizer OWL minimaliste pour la coloration des blocs de code -
 *
 * Pas un parser complet : juste assez pour donner du sens visuel aux
 * exemples affichés dans `owl learn`. Couvre mots-clés, types builtin,
 * constantes nommées du registry, natives, chaînes (`...` / "..."),
 * commentaires // et nombres. Le reste sort en clair.
 * ----------------------------------------------------------------- */

/* UTF-8 byte appartenant à une séquence multi-octet (start ou continuation). */
static int is_utf8_lead(unsigned char c) { return c >= 0x80; }

/* Compte les graphèmes (codepoints) d'une chaîne UTF-8 — utile pour le
 * padding des colonnes texte. Bytes de continuation `10xxxxxx` ne
 * comptent pas. */
static int utf8_visual_width(const char* s) {
    int w = 0;
    if (!s) return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if ((c & 0xC0) != 0x80) w++;
    }
    return w;
}

/* Imprime une chaîne suivie d'autant d'espaces qu'il faut pour atteindre
 * une largeur visuelle `target`. Si la chaîne est plus longue, n'ajoute
 * rien (pas de truncation — fait foi sur le contenu). Utile pour aligner
 * des noms accentués `chaîne` à côté de `entier` sans drift visuel. */
static void print_padded_utf8(const char* s, int target) {
    int w = utf8_visual_width(s);
    fputs(s, stdout);
    while (w++ < target) putchar(' ');
}

/* Compare deux mots case ET accent insensible — invariant runtime OWL
 * (cf. règle 6.1/6.2). Délègue à `scanner_strip_accents` pour la
 * normalisation UTF-8 → ASCII, puis compare en lowercase. */
static int word_eq_ci(const char* a, long alen, const char* b) {
    char abuf[128], bbuf[128];
    long n = scanner_strip_accents(a, (int)alen, abuf, (int)sizeof abuf);
    long m = scanner_strip_accents(b, (int)strlen(b), bbuf, (int)sizeof bbuf);
    if (n != m) return 0;
    for (long i = 0; i < n; i++) {
        char ca = abuf[i], cb = bbuf[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Liste des mots-clés OWL — case+accent insensible au runtime.
 *
 * Aligné sur les buckets `keywords_control` / `keywords_declaration` /
 * `keywords_oop` / `keywords_visibility` / `keywords_logical` du dump
 * `--vscode-data`. Mots de catégorie type (STRUCTURE/CLASSE/...) NE SONT
 * PAS ici — ils tombent en TYPE_LIST (storage.type.declaration violet),
 * conformément au générateur owl-integration/generate_vscode.py. */
static const char* KW_LIST[] = {
    /* control flow */
    "SI","SINON","ALORS","FIN","END","IF","ELSE","THEN",
    "POUR","TOUT","DE","A","PAS","STEP","FOR","IN","OF",
    "TANTQUE","WHILE","BOUCLE","LOOP","SORTIR","BREAK",
    "SELON","CAS","AUTRE","SWITCH","CASE","DEFAULT","OTHER",
    "QUAND","EXCEPTION","DANS","FAIRE","WHEN","DO",
    "AVEC","WITH",
    "RENVOYER","RETURN",
    /* declaration introducers (qui prennent un nom derrière) */
    "PROCEDURE","FONCTION","FUNCTION",
    /* declaration helpers — particules grammaticales */
    "EST","UN","UNE","DES","DU","LE","LA","LES","ES","IS","A_AN","AN",
    /* visibility modifiers (storage.modifier dans le générateur — même teinte
     * orange gras dans le thème, donc traités comme KW ici) */
    "PUBLIC","PUBLIQUE","PRIVE","PRIVEE","PRIVÉE","PRIVATE",
    "PROTEGE","PROTEGEE","PROTÉGÉ","PROTECTED","RESTREINT","GLOBALE","VIRTUELLE",
    "ABSTRAITE","ABSTRACT",
    "INTERNE","INTERNAL",
    /* logical operators */
    "ET","OU","PAS_OP","AND","OR","NOT","MODULO",
    /* OOP */
    "HERITE","HÉRITE","IMPLEMENTE","IMPLÉMENTE","OBJET","SELF","SOI",
    /* DSL Fenêtre / Etat — blocs de propriétés. */
    "PROPRIETES","PROPRIÉTÉS","PROPERTIES",
    "COLONNES","COLUMNS",
    NULL
};

/* Types builtin + mots de catégorie utilisés en déclaration de type composé.
 * Tous case-insensibles ici (TYPE_LIST). */
static const char* TYPE_LIST[] = {
    /* types primitifs */
    "chaine","chaîne","chaines","chaînes","entier","entiers","reel","réel",
    "monetaire","monétaire","numerique","numérique","booleen","booléen",
    "date","heure","dateheure","duree","durée","buffer","JSON",
    /* containers */
    "tableau","tableaux","liste","listes","pile","piles","file","files",
    "array","list","stack","queue",
    /* mots de catégorie de type composé — storage.type.declaration */
    "Structure","Structures","Classe","Class","Classes",
    "Enumeration","Énumération","Enum",
    "Combinaison","Combination","Interface","Procedure","Procédure",
    /* DSL UI / Etat — categories de widget */
    "Fenetre","Fenêtre","Window","Etat","État","Report",
    NULL
};

/* Containers qui transforment un `de`/`of` adjacent en violet plutôt
 * qu'orange : `tableau DE chaînes` lit comme une catégorie de type. */
static const char* CONTAINER_LIST[] = {
    "tableau","tableaux","liste","listes","pile","piles","file","files",
    "array","list","stack","queue",
    "associatif","associative","fixe","fixed","dynamique","dynamic","par",
    NULL
};

static const char* BUILTIN_CONST[] = {
    "Vrai","Faux","Null","True","False","Nil","RC","TAB","LF","CR","CRLF",
    NULL
};

/* Mots-clés de visibilité — toléré entre PROCEDURE et le nom déclaré.
 * `PROCEDURE PRIVÉE foo()` : `PRIVÉE` est un modifier, `foo` est le nom. */
static const char* VISIBILITY_LIST[] = {
    "PUBLIC","PUBLIQUE","PRIVE","PRIVEE","PRIVÉE","PRIVATE",
    "PROTEGE","PROTEGEE","PROTÉGÉ","PROTECTED","RESTREINT",
    "GLOBALE","VIRTUELLE","ABSTRAITE","ABSTRACT",
    "INTERNE","INTERNAL",
    NULL
};

static int word_in_list(const char* word, long len, const char* const* list) {
    for (int i = 0; list[i]; i++) {
        if (word_eq_ci(word, len, list[i])) return 1;
    }
    return 0;
}

/* Vrai si `word` est un nom canon (FR/ASCII/EN) d'un widget kind ou
 * d'un sous-bloc DSL — itère `owl_control_kinds_table()`. Garantit
 * qu'ajouter un kind côté ui_decl.c CONTROL_KINDS le colore
 * automatiquement en violet (storage.type) côté CLI. */
static int is_owl_widget_kind(const char* word, long len) {
    const OwlControlKindInfo* t = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = t; k && k->nameFR; k++) {
        if (k->nameFR    && word_eq_ci(word, len, k->nameFR))    return 1;
        if (k->nameAscii && word_eq_ci(word, len, k->nameAscii)) return 1;
        if (k->nameEN    && word_eq_ci(word, len, k->nameEN))    return 1;
    }
    return 0;
}

/* Vrai si `word` est un event canon enregistré dans le catalogue runtime
 * (surClic, surMolette, FinInitialisation, Fermeture, …) — peu importe le
 * widget kind. On itère tous les kinds, sans cache : ~12 kinds × ~20
 * events = quelques centaines de compare-ci, négligeable pour le
 * tokenizer (jamais en hot path).
 *
 * Avantage : zéro liste hardcoded. Ajouter un event au catalogue côté
 * stdlib → automatiquement coloré bleu dans la doc CLI / MCP. */
static int is_owl_event(const char* word, long len) {
    /* Liste exhaustive des kinds qui peuvent avoir des events. WINDOW
     * couvre les lifecycle Fenêtre (FinInitialisation, etc.). */
    static const int KINDS[] = {
        OWL_CONTROL_LABEL, OWL_CONTROL_BUTTON, OWL_CONTROL_INPUT,
        OWL_CONTROL_TABLE, OWL_CONTROL_CONTAINER, OWL_CONTROL_IMAGE,
        OWL_CONTROL_LISTE, OWL_CONTROL_COMBO, OWL_CONTROL_SELECTEUR,
        OWL_CONTROL_INTERRUPTEUR, OWL_CONTROL_ZONE_REPETEE,
        OWL_CONTROL_WINDOW,
    };
    for (size_t k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); k++) {
        int n = owl_event_catalog_count((OwlControlKind)KINDS[k]);
        for (int i = 0; i < n; i++) {
            const OwlEventDef* ev =
                owl_event_catalog_at((OwlControlKind)KINDS[k], i);
            if (!ev) continue;
            if (ev->canonical && word_eq_ci(word, len, ev->canonical))
                return 1;
            if (ev->nameEN && word_eq_ci(word, len, ev->nameEN))
                return 1;
        }
    }
    /* Catalogue Etat séparé (Filtre / Lecture / Fermeture). */
    int n = owl_etat_event_catalog_count();
    for (int i = 0; i < n; i++) {
        const OwlEventDef* ev = owl_etat_event_catalog_at(i);
        if (!ev) continue;
        if (ev->canonical && word_eq_ci(word, len, ev->canonical))
            return 1;
        if (ev->nameEN && word_eq_ci(word, len, ev->nameEN))
            return 1;
    }
    return 0;
}

/* Vrai si un identifiant commence par un préfixe de nommage canon de champ
 * ou de conteneur UI (suivi d'au moins un caractère) : le surligneur le
 * colorise en vert foncé comme un nom déclaré. Aligné sur la grammaire VSCode
 * `dsl-control-decl` (entity.name.dsl.owl) et la fiche
 * `docs/public_documentation/12_prefixes_de_nommage.md`.
 *
 * Les préfixes de CHAMP sont dérivés du registre `owl_control_kinds_table()`
 * (.canonicalPrefix) — source unique, même origine que le warning compile-time
 * W_SEM_FIELD_NONCANONICAL_PREFIX. Toute sorte ajoutée au registre avec un
 * canonicalPrefix est reconnue ici sans retoucher ce code, ce qui interdit le
 * drift (une liste en dur re-divergeait : TBL_/CMB_/SLT_/LIS_/CTR_ périmés).
 *
 * Les CONTENEURS (Fenêtre, État) n'ont pas de .canonicalPrefix dans le registre
 * (`FEN_X = "titre"` serait ambigu au top-level, cf. ui_decl.c) mais portent
 * bien un préfixe de nommage canon côté convention : on les supplémente ici. */
int cli_doc_looks_like_ui_instance(const char* word, long len) {
    static const char* CONTAINER_PFX[] = { "FEN_", "ETAT_", NULL };
    for (int i = 0; CONTAINER_PFX[i]; i++) {
        long pn = (long)strlen(CONTAINER_PFX[i]);
        if (len > pn && strncmp(word, CONTAINER_PFX[i], (size_t)pn) == 0)
            return 1;
    }
    for (const OwlControlKindInfo* k = owl_control_kinds_table();
         k->nameFR; k++) {
        if (!k->canonicalPrefix) continue;
        long pn = (long)strlen(k->canonicalPrefix);
        if (len > pn && strncmp(word, k->canonicalPrefix, (size_t)pn) == 0)
            return 1;
    }
    return 0;
}

/* Types primitifs intégrés à la VM — pas dans le registry runtime
 * (qui ne contient que les types composés). Hardcoder ici la liste
 * canonique permet à `owl list types` et `owl explain <primitive>` de
 * documenter les briques de base. */
typedef struct {
    const char* name;         /* forme canon avec accent */
    const char* nameAscii;    /* sans accent — accepté en saisie */
    const char* nameEN;       /* anglais (pour --lang en) */
    const char* descFR;
    const char* descEN;
} PrimitiveType;

static const PrimitiveType PRIMITIVE_TYPES[] = {
    {"chaîne",    "chaine",    "string",
     "Texte UTF-8. La taille est mesurée en octets. Indexation 1-based.",
     "UTF-8 text. Size measured in bytes. 1-based indexing."},
    {"entier",    "entier",    "integer",
     "Entier 64 bits signé. Promotion automatique vers numérique en cas de débordement.",
     "64-bit signed integer. Auto-promotes to numérique on overflow."},
    {"réel",      "reel",      "real",
     "Flottant double précision IEEE-754 (64 bits).",
     "IEEE-754 double-precision float (64-bit)."},
    {"monétaire", "monetaire", "currency",
     "Décimal exact, 6 chiffres après la virgule. Pas d'erreur d'arrondi binaire.",
     "Exact decimal, 6 digits after decimal point. No binary rounding error."},
    {"numérique", "numerique", "numeric",
     "Décimal exact à précision illimitée (croît avec la mémoire). Type pivot du calcul exact et financier.",
     "Exact decimal with unlimited precision (grows with memory). Pivot type for exact and financial computation."},
    {"booléen",   "booleen",   "boolean",
     "Vrai ou Faux. Les chaînes et nombres ne sont PAS booléens implicitement.",
     "True or False. Strings and numbers are NOT implicitly boolean."},
    {"date",      "date",      "date",
     "Date civile sans fuseau (année, mois, jour).",
     "Civil date without timezone (year, month, day)."},
    {"heure",     "heure",     "time",
     "Heure du jour, précision milliseconde.",
     "Time of day, millisecond precision."},
    {"dateheure", "dateheure", "datetime",
     "Combinaison date + heure.",
     "Date + time combination."},
    {"durée",     "duree",     "duration",
     "Intervalle de temps. Arithmétique avec dates et heures.",
     "Time interval. Arithmetic with dates and times."},
    {"buffer",    "buffer",    "buffer",
     "Séquence brute d'octets. Pour binaire (fichiers, réseau, crypto).",
     "Raw byte sequence. For binary content (files, network, crypto)."},
    {"tableau",   "tableau",   "array",
     "Collection ordonnée, indexée 1-based, dynamique sauf option fixe. "
     "Forme générique : `tableau de <type>`.",
     "Ordered collection, 1-based indexed, dynamic unless fixed option. "
     "Generic form: `array of <type>`."},
    {"JSON",      "JSON",      "JSON",
     "Document structuré (objet/tableau/valeur) avec accès par opérateur `..`.",
     "Structured document (object/array/value) with `..` accessor."},
    {NULL, NULL, NULL, NULL, NULL}
};

int cli_doc_primitive_count(void) {
    int n = 0;
    for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) n++;
    return n;
}

int cli_doc_primitive_at(int i, const char** out_name,
                                const char** out_nameAscii,
                                const char** out_nameEN) {
    int n = 0;
    for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++, n++) {
        if (n == i) {
            if (out_name)      *out_name      = p->name;
            if (out_nameAscii) *out_nameAscii = p->nameAscii;
            if (out_nameEN)    *out_nameEN    = p->nameEN;
            return 1;
        }
    }
    return 0;
}

int cli_doc_primitive_doc_at(int i, const char** out_name,
                                    const char** out_nameAscii,
                                    const char** out_nameEN,
                                    const char** out_descFR,
                                    const char** out_descEN) {
    int n = 0;
    for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++, n++) {
        if (n == i) {
            if (out_name)      *out_name      = p->name;
            if (out_nameAscii) *out_nameAscii = p->nameAscii;
            if (out_nameEN)    *out_nameEN    = p->nameEN;
            if (out_descFR)    *out_descFR    = p->descFR;
            if (out_descEN)    *out_descEN    = p->descEN;
            return 1;
        }
    }
    return 0;
}

/* Cherche un type primitif par nom (case + accent insensible). */
static const PrimitiveType* find_primitive_type(const char* name) {
    if (!name) return NULL;
    long nlen = (long)strlen(name);
    for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) {
        if (word_eq_ci(name, nlen, p->name) ||
            word_eq_ci(name, nlen, p->nameAscii) ||
            (p->nameEN && word_eq_ci(name, nlen, p->nameEN))) {
            return p;
        }
    }
    return NULL;
}

/* Classification d'un mot identifiant.
 * Le tokenizer a besoin du verdict pour appliquer C_PROC après PROCEDURE,
 * pour distinguer méthode (.X() bleu) vs champ (.x vert) après un point,
 * et pour distinguer mot-clé/type/native/var inconnue. */
typedef enum {
    TOK_KEYWORD,     /* SI/POUR/PROCEDURE/EST/DE/UN/... — orange gras */
    TOK_TYPE,        /* chaîne/entier/tableau/... ou type registry — violet */
    TOK_CONST,       /* Vrai/Faux/Null + constantes du registry — jaune-vert */
    TOK_NATIVE,      /* fonction native du registry — bleu */
    TOK_ERR_GLOBAL,  /* ErreurDétectée — indigo (TODO si jamais) */
    TOK_IDENT,       /* identifiant inconnu — vert clair (variable) */
} TokKind;

/* Classifie un identifiant.
 *   `is_call_site`   : 1 si suivi de '(' → autorise le lookup natives.
 *                      Aligné sur le pattern VSCode `(?i)\b(...)\b(?=\s*\()`.
 *                      Sans `(` derrière, on ne colore PAS `age` en bleu
 *                      à cause d'une native `Age()` du registry.
 *   `is_var_decl`    : 1 si suivi de `est`/`is` → on exclut le lookup
 *                      constants registry (sinon une variable user nommée
 *                      `actif` est colorisée comme constante d'enum).
 *                      Aligné sur `(?!\s+(est|is)\b)` du générateur Python. */
static TokKind classify_word(const char* word, long len,
                             int is_call_site, int is_var_decl) {
    /* Mots-clés (case+accent insensibles). */
    if (word_in_list(word, len, KW_LIST))    return TOK_KEYWORD;
    if (word_in_list(word, len, TYPE_LIST))  return TOK_TYPE;
    if (word_in_list(word, len, BUILTIN_CONST)) return TOK_CONST;

    char tmp[128];
    long n = (len < (long)sizeof(tmp) - 1) ? len : (long)sizeof(tmp) - 1;
    memcpy(tmp, word, (size_t)n); tmp[n] = '\0';

    /* Natives — uniquement si appel (suivies de `(`). */
    if (is_call_site && owl_registry_find(tmp)) return TOK_NATIVE;

    /* Constantes — uniquement hors contexte de déclaration de variable. */
    if (!is_var_decl && owl_registry_const_find(tmp)) return TOK_CONST;

    if (owl_registry_type_find(tmp))  return TOK_TYPE;
    if (owl_registry_enum_find(tmp))  return TOK_TYPE;
    return TOK_IDENT;
}

static const char* color_for_kind(TokKind k) {
    switch (k) {
        case TOK_KEYWORD:    return C_KEYWORD;
        case TOK_TYPE:       return C_TYPE;
        case TOK_CONST:      return C_CONST;
        case TOK_NATIVE:     return C_NATIVE;
        case TOK_ERR_GLOBAL: return C_ERRGLOBAL;
        case TOK_IDENT:      return C_IDENT;
    }
    return "";
}

/* Vrai si `word` est un mot-clé qui introduit une déclaration nommée :
 * `PROCEDURE` / `FONCTION` / `FUNCTION`. Le mot suivant (modulo visibilité)
 * est le nom déclaré → vert foncé canon `entity.name.function`.
 *
 * `STRUCTURE` / `CLASSE` / `ÉNUMÉRATION` / `COMBINAISON` ne sont PAS dans
 * cette liste : un type composé se déclare par élision —
 * `Personne est une Structure` — donc c'est l'élision (mot avant `est`)
 * qui porte le nom, pas le mot de catégorie. */
static int kw_introduces_name(const char* word, long len) {
    static const char* INTRO[] = {
        "PROCEDURE","FONCTION","FUNCTION",
        NULL
    };
    for (int i = 0; INTRO[i]; i++) {
        if (word_eq_ci(word, len, INTRO[i])) return 1;
    }
    return 0;
}

/* Sniff du prochain caractère non-blanc à partir de la position `from`.
 * Retourne 0 si rien trouvé sur la fin de ligne. */
static char peek_nonblank(const char* line, long from, long line_len) {
    while (from < line_len && (line[from] == ' ' || line[from] == '\t')) from++;
    return (from < line_len) ? line[from] : 0;
}

/* Sniff du prochain MOT à partir de la position `from`. Renseigne *out_word
 * et *out_len ; retourne 1 si trouvé, 0 sinon. */
static int peek_next_word(const char* line, long from, long line_len,
                          const char** out_word, long* out_len) {
    while (from < line_len && (line[from] == ' ' || line[from] == '\t')) from++;
    if (from >= line_len) return 0;
    unsigned char c = (unsigned char)line[from];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          c == '_' || is_utf8_lead(c)))
        return 0;
    long start = from;
    while (from < line_len) {
        unsigned char cc = (unsigned char)line[from];
        if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') ||
            (cc >= '0' && cc <= '9') || cc == '_' || is_utf8_lead(cc)) {
            from++;
        } else {
            break;
        }
    }
    *out_word = line + start;
    *out_len  = from - start;
    return 1;
}

/* `brace_depth` : profondeur d'accolades threadée par le caller à travers
 * les lignes successives d'un même bloc de code. Un littéral d'init
 * `{ field = expr, ... }` s'étale sur plusieurs lignes ; on a besoin de
 * savoir qu'on est « dedans » pour colorer les noms de fields (ident à
 * gauche d'un `=`) en vert foncé membre (C_PROC), comme la grammaire
 * VSCode (brace-literal-block → variable.other.dsl-property). Chaque bloc
 * de code repart à 0 (init côté caller). */
static void render_code_line_owl(const char* line, long line_len,
                                 int* brace_depth) {
    long i = 0;
    /* État pour relier les tokens entre eux : */
    int after_decl_intro = 0;   /* dernier mot = PROCEDURE/FONCTION → prochain ident = nom déclaré (vert foncé) */
    int after_dot        = 0;   /* dernier token = '.' simple → prochain ident = membre/méthode */
    int after_double_dot = 0;   /* dernier token = '..' → prochain ident = propriété native */
    int after_container  = 0;   /* dernier mot = tableau/liste/pile/... → `de` qui suit est violet */
    int after_champ      = 0;   /* dernier mot = Champ/Field → `de` qui suit est violet (Champ de Saisie) */

    while (i < line_len) {
        unsigned char c = (unsigned char)line[i];

        /* Commentaire // jusqu'à la fin */
        if (c == '/' && i + 1 < line_len && line[i + 1] == '/') {
            printf("%s%.*s%s", C_COMMENT, (int)(line_len - i), line + i, C_RESET);
            return;
        }

        /* Chaîne "..." */
        if (c == '"') {
            long j = i + 1;
            while (j < line_len) {
                if (line[j] == '"') {
                    if (j + 1 < line_len && line[j + 1] == '"') { j += 2; continue; }
                    j++; break;
                }
                j++;
            }
            printf("%s%.*s%s", C_STRING, (int)(j - i), line + i, C_RESET);
            i = j;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }

        /* Chaîne backtick */
        if (c == '`') {
            long j = i + 1;
            while (j < line_len) {
                if (line[j] == '`') {
                    if (j + 1 < line_len && line[j + 1] == '`') { j += 2; continue; }
                    j++; break;
                }
                j++;
            }
            printf("%s%.*s%s", C_STRING, (int)(j - i), line + i, C_RESET);
            i = j;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }

        /* Markers d'interpolation [%...%] — uniquement hors string parsing,
         * affichés en évidence quand on les voit isolément dans des exemples. */
        if (c == '[' && i + 1 < line_len && line[i + 1] == '%') {
            long j = i + 2;
            while (j + 1 < line_len && !(line[j] == '%' && line[j + 1] == ']')) j++;
            if (j + 1 < line_len) j += 2;
            printf("%s%.*s%s", C_INTERP_BRK, (int)(j - i), line + i, C_RESET);
            i = j;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }

        /* Nombres */
        if (c >= '0' && c <= '9') {
            long j = i + 1;
            while (j < line_len &&
                   ((line[j] >= '0' && line[j] <= '9') ||
                    line[j] == '.' || line[j] == 'x' || line[j] == 'X' ||
                    (line[j] >= 'a' && line[j] <= 'f') ||
                    (line[j] >= 'A' && line[j] <= 'F'))) {
                j++;
            }
            printf("%s%.*s%s", C_STRING, (int)(j - i), line + i, C_RESET);
            i = j;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }

        /* Point '.' ou '..' (propriété native) — ponctuation blanche,
         * mais on met à jour l'état pour le mot suivant. */
        if (c == '.') {
            if (i + 1 < line_len && line[i + 1] == '.') {
                putchar('.'); putchar('.');
                i += 2;
                after_double_dot = 1; after_dot = 0;
                after_decl_intro = 0; after_container = 0; after_champ = 0;
            } else {
                putchar('.');
                i++;
                after_dot = 1; after_double_dot = 0;
                after_decl_intro = 0; after_container = 0; after_champ = 0;
            }
            continue;
        }

        /* Accolades : suivi de profondeur pour la coloration des membres de
         * littéral. Comptage caractère par caractère (le `{` peut clore une
         * ligne d'init, le `}` être seul ou enchaîné `}}`). On ne change pas
         * la couleur de l'accolade elle-même (ponctuation neutre). */
        if (c == '{') {
            (*brace_depth)++;
            putchar('{'); i++;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }
        if (c == '}') {
            if (*brace_depth > 0) (*brace_depth)--;
            putchar('}'); i++;
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
            continue;
        }

        /* Identifiant : a-z A-Z _ et UTF-8 multi-octet. */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '_' || is_utf8_lead(c)) {
            long j = i;
            while (j < line_len) {
                unsigned char cc = (unsigned char)line[j];
                if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') ||
                    (cc >= '0' && cc <= '9') || cc == '_' || is_utf8_lead(cc)) {
                    j++;
                } else {
                    break;
                }
            }
            long wlen = j - i;
            const char* word = line + i;

            /* Cas 1 : après '..' → propriété native — blanc neutre canon. */
            if (after_double_dot) {
                printf("%.*s", (int)wlen, word);
                i = j;
                after_decl_intro = 0; after_dot = 0;
                after_double_dot = 0; after_container = 0;
                continue;
            }

            /* Cas 2 : après '.' → membre.
             *   '(' suit → méthode → bleu (registry-known) ou vert foncé (user).
             *   sinon  → champ → vert clair (variable.other.member canon). */
            if (after_dot) {
                int is_call = (peek_nonblank(line, j, line_len) == '(');
                if (is_call) {
                    /* lookup natives autorisé en site d'appel */
                    TokKind kk = classify_word(word, wlen, /*is_call*/1, /*is_var_decl*/0);
                    if (kk == TOK_NATIVE) {
                        printf("%s%.*s%s", C_NATIVE, (int)wlen, word, C_RESET);
                    } else {
                        printf("%s%.*s%s", C_PROC, (int)wlen, word, C_RESET);
                    }
                } else {
                    printf("%s%.*s%s", C_IDENT, (int)wlen, word, C_RESET);
                }
                i = j;
                after_decl_intro = 0; after_dot = 0;
                after_double_dot = 0; after_container = 0;
                continue;
            }

            /* Cas 3 : on attend le nom d'une déclaration (PROCEDURE Xxx).
             *   - si le mot est un modifier de visibilité → on l'imprime
             *     comme KW (orange) et on reste en attente du nom réel ;
             *   - si le mot est un event canon (surClic, FinInitialisation,
             *     …) → bleu (support.function.event canon VSCode) car
             *     c'est un hook déjà câblé runtime/Qt, pas une procédure
             *     user inventée ;
             *   - sinon → c'est le nom, vert foncé canon (entity.name). */
            if (after_decl_intro) {
                if (word_in_list(word, wlen, VISIBILITY_LIST)) {
                    printf("%s%.*s%s", C_KEYWORD, (int)wlen, word, C_RESET);
                    i = j;
                    /* after_decl_intro persiste pour le prochain mot */
                    continue;
                }
                if (is_owl_event(word, wlen)) {
                    printf("%s%.*s%s", C_NATIVE, (int)wlen, word, C_RESET);
                } else {
                    printf("%s%.*s%s", C_PROC, (int)wlen, word, C_RESET);
                }
                i = j;
                after_decl_intro = 0;
                after_container  = 0;
                after_champ      = 0;
                continue;
            }

            /* Cas 3.5 : membre d'un littéral d'init `{ field = expr }`.
             *   Dans un `{}` ouvert (brace_depth > 0), un identifiant suivi
             *   (peek non-blanc) d'un `=` est un nom de field → vert foncé
             *   membre (C_PROC), AVANT la classif registry/widget qui le
             *   sortait en violet quand il collisionne avec un type/const
             *   (Couleur, Type, Trait, …). Les valeurs (RHS) gardent leur
             *   coloration normale via les cas suivants. On exige
             *   strictement `=` (pas `==`) pour ne pas attraper une
             *   comparaison ; les multilingues `{lang: ...}` (séparateur
             *   `:`) ne matchent pas non plus. */
            if (*brace_depth > 0) {
                char nb = peek_nonblank(line, j, line_len);
                int is_eq = (nb == '=');
                /* écarte `==` / `=>` éventuels : on ne veut que l'affectation
                 * de field. Recherche le `=` réel après les blancs. */
                if (is_eq) {
                    long k = j;
                    while (k < line_len && (line[k] == ' ' || line[k] == '\t')) k++;
                    if (k + 1 < line_len && line[k + 1] == '=') is_eq = 0;
                }
                if (is_eq) {
                    printf("%s%.*s%s", C_PROC, (int)wlen, word, C_RESET);
                    i = j;
                    after_decl_intro = 0; after_dot = 0;
                    after_double_dot = 0; after_container = 0;
                    after_champ = 0;
                    continue;
                }
            }

            /* Cas 4 : `de`/`of` après un container OU après `Champ`/`Field`
             *   → violet (storage.type) au lieu d'orange. `tableau DE
             *   chaînes` et `Champ DE Saisie` restent cohérents
             *   visuellement avec le type composé qu'ils forment. */
            if ((after_container || after_champ) &&
                word_in_list(word, wlen, (const char*[]){"DE","OF",NULL})) {
                printf("%s%.*s%s", C_TYPE, (int)wlen, word, C_RESET);
                i = j;
                after_container = 0;
                after_champ     = 0;
                continue;
            }

            /* Cas 4.5 : préfixe UI (FEN_/LIB_/SAI_/BTN_/...). Doit
             * passer avant classify_word car les préfixes peuvent
             * collisionner avec des noms du registry (IMG_X vs type Image).
             * Aligné sur `entity.name.dsl.owl` côté grammaire VSCode. */
            if (cli_doc_looks_like_ui_instance(word, wlen)) {
                printf("%s%.*s%s", C_PROC, (int)wlen, word, C_RESET);
                after_decl_intro = 0;
                after_container  = 0;
                after_champ      = 0;
                after_dot = 0; after_double_dot = 0;
                i = j;
                continue;
            }

            /* Cas 4.6 : event canon du runtime — bleu (support.function.event).
             * Couvre les mentions hors-déclaration : « voir surClic » dans
             * une doc, par exemple. Le lookup tape directement le catalogue
             * runtime, donc tout nouvel event ajouté au catalogue est
             * automatiquement coloré sans toucher au tokenizer. */
            if (is_owl_event(word, wlen)) {
                printf("%s%.*s%s", C_NATIVE, (int)wlen, word, C_RESET);
                after_decl_intro = 0;
                after_container  = 0;
                after_champ      = 0;
                after_dot = 0; after_double_dot = 0;
                i = j;
                continue;
            }

            /* Cas 4.7 : widget kind ou sous-bloc DSL (Bouton, Libellé,
             * Image, Fenêtre, EnTeteDocument, …) — violet (storage.type).
             * Lookup via `owl_control_kinds_table()` runtime, donc tout
             * nouveau kind ajouté à CONTROL_KINDS apparaît automatiquement
             * sans toucher au tokenizer. */
            if (is_owl_widget_kind(word, wlen)) {
                printf("%s%.*s%s", C_TYPE, (int)wlen, word, C_RESET);
                after_decl_intro = 0;
                after_container  = 0;
                after_champ      = 0;
                after_dot = 0; after_double_dot = 0;
                i = j;
                continue;
            }

            /* Cas 5 : classification standard avec contexte. */
            int is_call_site = (peek_nonblank(line, j, line_len) == '(');
            const char* nxw; long nxn;
            int is_var_decl = 0;
            if (peek_next_word(line, j, line_len, &nxw, &nxn)) {
                if (word_eq_ci(nxw, nxn, "EST") || word_eq_ci(nxw, nxn, "IS") ||
                    word_eq_ci(nxw, nxn, "SONT") || word_eq_ci(nxw, nxn, "ARE")) {
                    is_var_decl = 1;
                }
            }

            TokKind kk = classify_word(word, wlen, is_call_site, is_var_decl);
            printf("%s%.*s%s", color_for_kind(kk), (int)wlen, word, C_RESET);

            /* Mise à jour de l'état pour le mot suivant. */
            after_decl_intro = (kk == TOK_KEYWORD &&
                                kw_introduces_name(word, wlen));
            after_container  = word_in_list(word, wlen, CONTAINER_LIST);
            /* `Champ DE Saisie` : `Champ`/`Field` est un type composé du
             * registry (coloré ici via classify_word), pas un widget kind
             * de CONTROL_KINDS — il arrive donc dans cette branche, pas en
             * Cas 4.7. On arme le flag pour que le `de` suivant soit violet
             * comme après un container. */
            after_champ      = word_eq_ci(word, wlen, "Champ") ||
                               word_eq_ci(word, wlen, "Field");
            after_dot = 0; after_double_dot = 0;
            i = j;
            continue;
        }

        /* Caractère non-mot : sort tel quel ; reset l'état sauf si on est
         * encore en train d'écrire des blancs entre un PROCEDURE et son nom. */
        if (c != ' ' && c != '\t') {
            after_decl_intro = 0; after_dot = 0;
            after_double_dot = 0; after_container = 0; after_champ = 0;
        }
        putchar(line[i]); i++;
    }
}

static void render_markdown_to_terminal(const unsigned char* data, long len) {
    int in_code_block = 0;
    int code_is_owl   = 0;
    int brace_depth   = 0;   /* profondeur littéral {} dans le bloc courant */
    long i = 0;
    while (i < len) {
        long start = i;
        while (i < len && data[i] != '\n') i++;
        long line_len = i - start;
        /* Trim \r éventuel */
        while (line_len > 0 && data[start + line_len - 1] == '\r') line_len--;
        if (i < len) i++;

        const char* line = (const char*)(data + start);

        /* Fence ```[lang] */
        if (line_len >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
            if (!in_code_block) {
                /* Détecte le lang après les ``` */
                const char* lang = line + 3;
                long lang_len = line_len - 3;
                while (lang_len > 0 && (*lang == ' ' || *lang == '\t')) { lang++; lang_len--; }
                /* Tokeniser comme OWL exige un lang EXPLICITE `owl`.
                 * Fence sans lang = texte plain grisé — évite de tokeniser
                 * à tort les blocs ASCII art / pseudo-code / séquences. */
                code_is_owl = (lang_len >= 3 &&
                               (lang[0] == 'o' || lang[0] == 'O') &&
                               (lang[1] == 'w' || lang[1] == 'W') &&
                               (lang[2] == 'l' || lang[2] == 'L'));
            }
            in_code_block = !in_code_block;
            brace_depth = 0;   /* chaque bloc de code repart de zéro */
            /* Affiche la fence en discret. */
            printf("%s%.*s%s\n", C_DIM, (int)line_len, line, C_RESET);
            if (!in_code_block) code_is_owl = 0;
            continue;
        }

        /* Entête markdown */
        if (!in_code_block && line_len > 0 && line[0] == '#') {
            int depth = 0;
            while (depth < (int)line_len && line[depth] == '#') depth++;
            if (depth >= 1 && depth <= 6 &&
                depth < (int)line_len && line[depth] == ' ') {
                /* Uniformément en couleur d'accent dédiée, distinct du
                 * code OWL — un titre n'est pas une fonction native. */
                if (depth == 1) printf("\n");
                printf("%s%.*s%s\n",
                       C_MD_HEADING, (int)line_len, line, C_RESET);
                continue;
            }
        }

        /* Bloc code OWL : tokenize ; bloc non-OWL ou inconnu : grisé. */
        if (in_code_block) {
            if (code_is_owl) {
                render_code_line_owl(line, line_len, &brace_depth);
                putchar('\n');
            } else {
                printf("%s%.*s%s\n", C_DIM, (int)line_len, line, C_RESET);
            }
            continue;
        }

        /* Ligne ordinaire. */
        printf("%.*s\n", (int)line_len, line);
    }
}

/* ------------------------------------------------------------------
 * Sortie JSON
 *
 * Pas de bibliothèque externe : écriture manuelle avec échappement.
 * ------------------------------------------------------------------ */

static void json_write_string(const unsigned char* s, long len) {
    putchar('"');
    for (long i = 0; i < len; i++) {
        unsigned char c = s[i];
        switch (c) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n",  stdout); break;
            case '\r': fputs("\\r",  stdout); break;
            case '\t': fputs("\\t",  stdout); break;
            default:
                if (c < 0x20) printf("\\u%04x", c);
                else          putchar((char)c);
                break;
        }
    }
    putchar('"');
}

static void json_write_cstr(const char* s) {
    if (!s) { fputs("null", stdout); return; }
    json_write_string((const unsigned char*)s, (long)strlen(s));
}

/* ------------------------------------------------------------------
 * owl help [<commande>]
 * ------------------------------------------------------------------ */

static void print_help_top(void) {
    printf("%sOWL %s%s — langage de programmation productif\n\n",
           C_EMPH, OWL_VERSION, C_RESET);
    printf("Usage : %sowl%s %s<commande>%s [args]\n\n",
           C_CMD, C_RESET, C_ARG, C_RESET);

    printf("%sApprendre%s\n", C_SECTION, C_RESET);
    printf("  %sowl primer%s              ce qu'il faut savoir en 5 minutes\n",
           C_CMD, C_RESET);
    printf("  %sowl learn%s               liste des fiches fondamentales\n",
           C_CMD, C_RESET);
    printf("  %sowl learn%s %s<fiche>%s       lire une fiche (types, null, operateurs, ...)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl gotchas%s             pièges qui ne se devinent pas\n",
           C_CMD, C_RESET);
    printf("  %sowl cheatsheet%s          aide-mémoire de syntaxe condensé\n",
           C_CMD, C_RESET);

    printf("\n%sDécouvrir le langage%s\n", C_SECTION, C_RESET);
    printf("  %sowl list%s %s<quoi>%s         catalogues filtrables ; %sowl list%s sans argument détaille les sous-commandes\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_CMD, C_RESET);
    printf("  %sowl explain%s %s<symbole>%s   détail complet d'une fonction, d'un type, d'une constante\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl examples%s %s<symbole>%s  scénarios publics démontrant un symbole\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl search%s %s<mot>%s        recherche transversale (doc + registry + exemples)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl where%s %s<symbole>%s    reverse lookup — où ce symbole est-il utilisé\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl icons%s %s<verbe>%s     explorer le catalogue d'icones (list, search, preview)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);

    printf("\n%sDémarrer vite%s\n", C_SECTION, C_RESET);
    printf("  %sowl new%s %s<type> <Nom>%s     ossature minimale (champ, classe, fenetre, etat, projet)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl template%s             liste des squelettes prêts à coller\n",
           C_CMD, C_RESET);
    printf("  %sowl template%s %s<nom>%s       imprime un squelette (hello, classe, test, fenetre)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl template%s %s<nom>%s %s-o <fichier>%s  exporte le squelette en .owl (voir %sowl help template%s)\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_CMD, C_RESET);

    printf("\n%sExécuter%s\n", C_SECTION, C_RESET);
    printf("  %sowl%s %s<fichier.owl>%s      exécute un fichier (charge le projet voisin)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl test%s %s[chemin]%s        lance la suite de tests\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl fmt%s %s<fichier>%s        applique le formatter canon\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl translate%s %s--to en|fr <fichier>%s  traduit la surface langage vers l'autre langue\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl%s %s[dossier]%s           lance le projet (cherche un projet.owl en remontant)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl%s                     REPL (si aucun projet.owl trouvé)\n",
           C_CMD, C_RESET);

    printf("\n%sServeurs / intégrations%s\n", C_SECTION, C_RESET);
    printf("  %sowl serve%s %s<fichier.owl> [--port N] [--bind adr]%s  lance un service REST déclaré %s(défaut 127.0.0.1:8080)%s\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_DIM, C_RESET);
    printf("  %sowl openapi%s %s<fichier.owl>%s  génère le document OpenAPI 3 d'un service REST %s(JSON sur la sortie standard)%s\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_DIM, C_RESET);
    printf("  %sowl serve-llm%s %s--model <x.gguf>%s  serveur d'inférence compatible OpenAI (SSE)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl mcp%s                  serveur MCP (outils IA) sur stdin/stdout\n",
           C_CMD, C_RESET);

    printf("\n%sOutils dev%s\n", C_SECTION, C_RESET);
    printf("  %sowl --check%s %s<fichier>%s    diagnostic compile-only (sortie JSON)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
#ifndef OWL_RELEASE_DISTRIB
    printf("  %sowl --self-test-registry%s vérifie les invariants du registre\n",
           C_CMD, C_RESET);

    printf("\n%sOutillage interne (pipelines / CI — pas d'usage interactif)%s\n",
           C_SECTION, C_RESET);
    printf("  %sowl --vscode-data%s        export catalogue pour extension VSCode %s(consommé par make vscode/snapshot)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl --dump-lsp-catalog%s   régénère lsp_catalog_gen.c %s(consommé par CMakeLists)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl --check-server%s       boucle de diagnostic résidente sur stdin/stdout %s(consommé par owl-lsp)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl --self-test-cli%s      tests CLI doc %s(gate make all)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl --self-test-mcp%s      tests serveur MCP %s(gate make all)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl --self-test-net%s      tests net/SQL internes %s(gate make all)%s\n",
           C_CMD, C_RESET, C_DIM, C_RESET);
    printf("  %sowl __complete%s %s<pos> <prefix>%s  backend autocomplete shell %s(cf. scripts/owl-completion.*)%s\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_DIM, C_RESET);
#endif /* OWL_RELEASE_DISTRIB */

    printf("\nPour le détail d'une commande : %sowl help%s %s<commande>%s\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("%sFormats%s : %s--plain%s sans couleur, %s--color%s force couleur, %s--json%s structuré, %s--lang en%s anglais\n",
           C_DIM, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
    printf("%sComplétion%s : %sowl help completion%s pour installer la complétion par tabulation\n",
           C_DIM, C_RESET, C_CMD, C_RESET);
}

static void print_help_completion(void) {
    printf("%sComplétion par tabulation%s\n\n", C_SECTION, C_RESET);
    printf("La CLI %sowl%s expose un backend %sowl __complete%s qu'on branche\n",
           C_CMD, C_RESET, C_CMD, C_RESET);
    printf("au shell pour proposer les sous-commandes et les noms du registry\n");
    printf("selon le contexte (ex. %sowl explain Cha<TAB>%s liste %sChaîneDécoupe%s,\n",
           C_CMD, C_RESET, C_NATIVE, C_RESET);
    printf("%sChaineVersDate%s, etc.).\n\n", C_NATIVE, C_RESET);

    printf("%sPrérequis%s : le binaire %sowl%s (ou %sowl.exe%s) doit être\n",
           C_SECTION, C_RESET, C_CMD, C_RESET, C_CMD, C_RESET);
    printf("invocable par son nom dans le shell — soit dans le PATH, soit\n");
    printf("via un alias. Sinon le shell tombe sur la complétion par défaut\n");
    printf("(fichiers du dossier courant) car il ne reconnaît pas le binaire.\n\n");

    printf("%sBash / Git Bash%s\n", C_SECTION, C_RESET);
    printf("  source %sscripts/owl-completion.bash%s\n", C_ARG, C_RESET);
    printf("  %s# pour persister :%s\n", C_DIM, C_RESET);
    printf("  echo 'source /chemin/owl-completion.bash' >> ~/.bashrc\n\n");

    printf("%sPowerShell%s\n", C_SECTION, C_RESET);
    printf("  . %sscripts\\owl-completion.ps1%s\n", C_ARG, C_RESET);
    printf("  %s# si owl n'est pas dans le PATH, un alias suffit :%s\n",
           C_DIM, C_RESET);
    printf("  Set-Alias owl C:\\chemin\\owl.exe\n");
    printf("  %s# pour persister, ajouter au profil ($PROFILE)%s\n",
           C_DIM, C_RESET);

    printf("\nLes complétions disponibles selon la position :\n");
    printf("  %sowl <TAB>%s              sous-commandes top-level\n", C_CMD, C_RESET);
    printf("  %sowl list <TAB>%s         categories|natives|types|constants|keywords|operators|controls|events|props|globalvars|limits\n", C_CMD, C_RESET);
    printf("  %sowl learn <TAB>%s        aliases et noms de fiches\n", C_CMD, C_RESET);
    printf("  %sowl explain <TAB>%s      tous les symboles du registry\n", C_CMD, C_RESET);
    printf("  %sowl examples <TAB>%s     idem\n", C_CMD, C_RESET);
    printf("  %sowl help <TAB>%s         sous-commandes avec aide rédigée\n", C_CMD, C_RESET);
}

static void print_help_command(const char* cmd) {
    if (strcmp(cmd, "completion") == 0 || strcmp(cmd, "complete") == 0 ||
        strcmp(cmd, "tab") == 0) {
        print_help_completion();
        return;
    }
    if (strcmp(cmd, "learn") == 0) {
        printf("%sowl learn%s [<fiche>] — accède aux fiches fondamentales.\n\n",
               C_CMD, C_RESET);
        printf("Sans argument : liste les fiches embarquées dans le binaire.\n");
        printf("Avec un nom ou alias : affiche la fiche correspondante.\n\n");
        printf("%sAliases reconnus%s\n", C_SECTION, C_RESET);
        printf("  apercu, primer, overview\n");
        printf("  types, coercions\n");
        printf("  null, nullable\n");
        printf("  operateurs, operators\n");
        printf("  conventions, code\n");
        printf("  test, tests, charte\n");
        printf("  prefixes, naming\n");
        return;
    }
    if (strcmp(cmd, "primer") == 0) {
        printf("%sowl primer%s — aperçu du langage en 5 minutes.\n\n",
               C_CMD, C_RESET);
        printf("Raccourci de %sowl learn apercu%s. Imprime la fiche\n",
               C_CMD, C_RESET);
        printf("%s00_apercu_du_langage%s — premier point d'entrée pour\n",
               C_ARG, C_RESET);
        printf("découvrir la syntaxe, les types, les invariants du langage.\n");
        return;
    }
    if (strcmp(cmd, "list") == 0) {
        printf("%sowl list%s %s<quoi>%s — listes filtrables des catalogues.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Sans argument : aide locale avec toutes les sous-commandes.\n\n");
        printf("Sous-commandes acceptées : categories, natives, types,\n");
        printf("constants, keywords, operators, controls, events,\n");
        printf("props, globalvars, limits.\n");
        return;
    }
    if (strcmp(cmd, "explain") == 0) {
        printf("%sowl explain%s %s<symbole>%s — fiche complète d'un symbole.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Description, signatures, propriétés, énumérations, exemples.\n");
        printf("Multi-bucket : un même nom peut désigner native + type +\n");
        printf("event… tous sont retournés. Filtre via %s--cat <bucket>%s.\n",
               C_ARG, C_RESET);
        return;
    }
    if (strcmp(cmd, "examples") == 0) {
        printf("%sowl examples%s %s<symbole>%s — scénarios canon démontrant un symbole.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Liste les fichiers de tests/scenarios/ qui illustrent l'usage\n");
        printf("du symbole. Le body OWL est inclus tel quel — code prêt à\n");
        printf("copier/adapter.\n");
        return;
    }
    if (strcmp(cmd, "search") == 0) {
        printf("%sowl search%s %s<mot>%s — recherche transversale.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Cherche dans la doc publique embarquée, le registry (natives,\n");
        printf("types, enums, constantes) et les scénarios. Utile quand le\n");
        printf("nom canon est inconnu.\n");
        return;
    }
    if (strcmp(cmd, "where") == 0) {
        printf("%sowl where%s %s<symbole>%s — reverse lookup.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Pour une propriété DSL (Grisé, Visible…) : liste les champs\n");
        printf("qui l'acceptent. Pour un type de champ (Bouton, Corps…) :\n");
        printf("indique le contexte parent où il vit. Complémentaire à\n");
        printf("%sowl explain%s (« que suis-je ? »).\n", C_CMD, C_RESET);
        return;
    }
    if (strcmp(cmd, "icons") == 0) {
        printf("%sowl icons%s %s<verbe>%s — explorer le catalogue d'icones embarque.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Trois verbes :\n");
        printf("  %slist --set <nom> [--limit N]%s  liste les icones d'un set\n",
               C_ARG, C_RESET);
        printf("  %ssearch <mot> [<mot2> ...]%s    substring AND case+accent insensible\n",
               C_ARG, C_RESET);
        printf("  %spreview <nom> [--size N]%s     rend l'icone dans le terminal\n",
               C_ARG, C_RESET);
        printf("\nFlags : %s--ascii%s (rampe luminance pour pipes / cmd.exe legacy),\n",
               C_ARG, C_RESET);
        printf("%s--preview%s (vignettes 24px en regard des resultats de search).\n",
               C_ARG, C_RESET);
        return;
    }
    if (strcmp(cmd, "template") == 0) {
        printf("%sowl template%s [<nom>] — squelettes OWL prêts à coller.\n\n",
               C_CMD, C_RESET);
        printf("Sans argument : liste les templates embarqués avec niveau\n");
        printf("et description. Avec nom (hello, classe, test, fenetre) :\n");
        printf("imprime le code complet d'un exemple métier sur stdout.\n\n");
        printf("%s-o%s %s<fichier>%s : écrit directement le fichier en UTF-8\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        printf("(refuse d'écraser un fichier existant). À préférer à la\n");
        printf("redirection shell sous PowerShell, qui produit de l'UTF-16\n");
        printf("avec accents cassés.\n");
        return;
    }
    if (strcmp(cmd, "new") == 0) {
        printf("%sowl new%s %s<type>%s [%s<sub>%s] %s<Nom>%s — squelette générique minimal.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("Génère l'ossature d'un champ, classe, fenêtre ou état sur\n");
        printf("stdout. Types acceptés : fenetre, etat, classe, collection,\n");
        printf("champ <kind>, projet|project.\n");
        printf("Kinds champ : bouton, libelle, saisie, table, conteneur,\n");
        printf("image, liste, combo, selecteur, interrupteur, zonerepetee.\n");
        printf("Constructs menu : menuprincipal, menucontextuel, menuloupe,\n");
        printf("menucorps, optionmenu, sousmenu, menuseparateur (dans une\n");
        printf("fen\xc3\xaatre / un menu).\n");
        printf("menuloupe : menu de loupe pr\xc3\xaat \xc3\xa0 attacher \xc3\xa0 un Champ Table\n");
        printf("(propri\xc3\xa9t\xc3\xa9 MenuLoupe), options c\xc3\xa2""bl\xc3\xa9""es sur MoiM\xc3\xaame.\n");
        printf("menucorps : menu du corps d'un Champ Table (propri\xc3\xa9t\xc3\xa9\n");
        printf("MenuContextuel), options c\xc3\xa2""bl\xc3\xa9""es sur MoiM\xc3\xaame (Tout copier\n");
        printf("vers le presse-papier / Export Excel).\n\n");
        printf("%sowl new projet%s [%s<Nom>%s] (graphie EN %sowl new project%s)\n",
               C_CMD, C_RESET, C_ARG, C_RESET, C_CMD, C_RESET);
        printf("écrit un marqueur projet.owl|project.owl dans le dossier\n");
        printf("courant (bloc Projet minimal, champ Langues renseigné ;\n");
        printf("refuse d'écraser un marqueur existant). Nom par défaut =\n");
        printf("nom du dossier courant.\n\n");
        printf("Différent de %sowl template%s (exemples métier complets) :\n",
               C_CMD, C_RESET);
        printf("%sowl new%s donne juste la déclaration + props courantes.\n",
               C_CMD, C_RESET);
        return;
    }
    if (strcmp(cmd, "fmt") == 0 || strcmp(cmd, "format") == 0) {
        printf("%sowl fmt%s %s<fichier>%s — applique le formatter canon.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Normalise la casse, les accents (recherche le canon dans le\n");
        printf("registry), et l'indentation. Le fichier est modifié en\n");
        printf("place. Message de progression sur stderr.\n");
        return;
    }
    if (strcmp(cmd, "check") == 0) {
        printf("%sowl --check%s %s<fichier>%s — diagnostic compile-only.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Compile sans exécuter et retourne un JSON sur stdout :\n");
        printf("%s{\"errors\":[…], \"warnings\":[…]}%s avec line/column/message.\n",
               C_DIM, C_RESET);
        printf("Exit code POSIX : 0 si OK, 65 si erreurs.\n");
        return;
    }
    if (strcmp(cmd, "test") == 0) {
        printf("%sowl test%s [<chemin>] — lance la suite de tests.\n\n",
               C_CMD, C_RESET);
        printf("Sans argument : ramasse récursivement tous les %stest_*.owl%s\n",
               C_ARG, C_RESET);
        printf("du dossier courant. Avec chemin : limite la recherche.\n\n");
        printf("Flags : %s-v%s (verbose), %s-vv%s (super verbose), %s--filter%s\n",
               C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("%s<pattern>%s, %s--fail-fast%s.\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        return;
    }
    if (strcmp(cmd, "translate") == 0) {
        printf("%sowl translate%s %s--to en|fr%s %s<fichier.owl>%s [%s-o <fichier>%s]\n",
               C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("— traduit la surface langage d'un fichier vers la langue cible.\n\n");
        printf("Convertit les éléments à forme bilingue : mots-clés, natives,\n");
        printf("types, constantes, propriétés et valeurs d'énumération. Les\n");
        printf("identifiants définis par l'utilisateur, le contenu des chaînes\n");
        printf("et les commentaires sont préservés tels quels.\n\n");
        printf("%s--to en%s ou %s--to fr%s : langue cible (requis).\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        printf("Sans %s-o%s : sortie sur stdout. Avec %s-o%s %s<fichier>%s :\n",
               C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("écrit le résultat dans le fichier indiqué.\n\n");
        printf("Le round-trip est stable au canon : traduire vers l'autre\n");
        printf("langue puis revenir restitue la forme canonique d'origine.\n");
        return;
    }

    if (strcmp(cmd, "serve") == 0) {
        printf("%sowl serve%s %s<fichier.owl>%s [%s--port <n>%s] [%s--bind <adresse>%s]\n",
               C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("— sert un service REST déclaré dans le fichier.\n\n");
        printf("Compile le fichier, monte le service et écoute les requêtes HTTP\n");
        printf("jusqu'à l'arrêt par %sCtrl-C%s.\n\n", C_ARG, C_RESET);
        printf("Options :\n");
        printf("  %s--port%s %s<n>%s      port d'écoute (défaut 8080).\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        printf("  %s--bind%s %s<adresse>%s  interface d'écoute (défaut 127.0.0.1,\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        printf("                  local uniquement). %s--bind 0.0.0.0%s expose le\n",
               C_ARG, C_RESET);
        printf("                  service à toutes les interfaces réseau.\n\n");
        printf("%sExemple%s\n", C_SECTION, C_RESET);
        printf("  %sowl serve api.owl --port 3000%s\n", C_CMD, C_RESET);
        return;
    }
    if (strcmp(cmd, "serve-llm") == 0) {
        printf("%sowl serve-llm%s %s--model <x.gguf>%s — serveur d'inférence local.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("Expose une API HTTP compatible OpenAI : %sPOST /v1/chat/completions%s\n",
               C_ARG, C_RESET);
        printf("(streaming SSE) et %sGET /v1/models%s. Charge un modèle au format\n",
               C_ARG, C_RESET);
        printf("%s.gguf%s indiqué par %s--model%s.\n\n", C_ARG, C_RESET, C_ARG, C_RESET);
        printf("Options : %s--port%s %s<n>%s (défaut 11434), %s--host%s %s<adresse>%s\n",
               C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
        printf("(défaut 127.0.0.1).\n");
        return;
    }
    if (strcmp(cmd, "mcp") == 0) {
        printf("%sowl mcp%s — serveur d'outils IA (protocole MCP).\n\n",
               C_CMD, C_RESET);
        printf("Dialogue en JSON-RPC sur stdin/stdout : un client IA découvre\n");
        printf("et invoque les outils exposés par OWL. Pas d'argument ; le\n");
        printf("serveur reste résident jusqu'à la fermeture du flux d'entrée.\n");
        return;
    }
    if (strcmp(cmd, "openapi") == 0) {
        printf("%sowl openapi%s %s<fichier.owl>%s\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("— génère le document OpenAPI 3 d'un service REST, sur la sortie\n");
        printf("standard (JSON).\n\n");
        printf("Le document est %sdérivé%s du service : chemins, méthodes,\n",
               C_ARG, C_RESET);
        printf("paramètres, corps, réponses et schéma d'authentification sont\n");
        printf("lus depuis les blocs %sServiceREST%s / %sOpérationHTTP%s — aucune\n",
               C_ARG, C_RESET, C_ARG, C_RESET);
        printf("seconde source à maintenir. Le même document est exposé par\n");
        printf("%sowl serve%s sur la route %sGET /openapi.json%s.\n\n",
               C_CMD, C_RESET, C_ARG, C_RESET);
        printf("%sExemple%s\n", C_SECTION, C_RESET);
        printf("  %sowl openapi api.owl > openapi.json%s\n", C_CMD, C_RESET);
        return;
    }

    printf("%sowl help %s%s : aide indisponible — commande inconnue ou non documentée.\n",
           C_DIM, cmd, C_RESET);
    printf("Tapez %sowl help%s pour la liste des commandes.\n", C_CMD, C_RESET);
}

int cli_doc_help(int argc, char** argv) {
    /* argv[0] = "help" */
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "help")) return 64;

    if (opts.pos_argc == 0) {
        print_help_top();
    } else {
        print_help_command(opts.pos_argv[0]);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * owl primer
 * ------------------------------------------------------------------ */

int cli_doc_primer(int argc, char** argv) {
    /* Délègue à `learn apercu` */
    char* primer_argv[3];
    primer_argv[0] = "learn";
    primer_argv[1] = "apercu";
    int new_argc = 2;
    /* Propage les flags reçus (--plain, --json, --lang en) */
    for (int i = 1; i < argc && new_argc < 3; i++) {
        primer_argv[new_argc++] = argv[i];
    }
    (void)argc;
    return cli_doc_learn(new_argc, primer_argv);
}

/* ------------------------------------------------------------------
 * owl learn
 * ------------------------------------------------------------------ */

static int learn_list(const DocOpts* opts) {
    if (opts->json) {
        printf("{\n  \"public_documentation\": [\n");
        int first = 1;
        for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
            if (!first) printf(",\n");
            first = 0;
            printf("    {\n      \"name\": ");
            json_write_cstr(d->name);
            printf(",\n      \"path\": ");
            json_write_cstr(d->path);
            printf(",\n      \"size\": %ld\n    }", d->len);
        }
        printf("\n  ]\n}\n");
        return 0;
    }

    printf("%sFiches fondamentales disponibles%s\n", C_SECTION, C_RESET);
    printf("%sUsage%s : %sowl learn%s %s<nom>%s\n\n",
           C_DIM, C_RESET, C_NATIVE, C_RESET, C_TYPE, C_RESET);

    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        /* Trouve un alias court si dispo, sinon affiche le name brut. */
        const char* short_name = d->name;
        for (const PublicDocAlias* a = PUBLIC_DOC_ALIASES; a->alias; a++) {
            if (strcmp(a->file_stem, d->name) == 0) {
                short_name = a->alias;
                break;
            }
        }
        printf("  %s%-16s%s  %s%s%s\n",
               C_CONST, short_name, C_RESET,
               C_DIM,   d->name,    C_RESET);
    }
    printf("\n%d fiche(s) embarquée(s).\n", EMBEDDED_PUBLIC_DOCS_COUNT);
    return 0;
}

static int learn_show(const DocOpts* opts, const char* query) {
    const OwlEmbeddedDoc* d = find_public_doc(query);
    if (!d) {
        fprintf(stderr, "owl learn : fiche '%s' inconnue.\n", query);
        fprintf(stderr, "  Tapez 'owl learn' pour la liste.\n");
        return 2;
    }

    /* Bilingue (V0) : la version EN sera embarquée séparément quand
     * disponible (docs/public_documentation/en, fichiers .md anglais). Pour
     * l'instant, FR partout avec note explicite si EN demandé. */
    if (opts->lang == DOC_LANG_EN && !opts->json) {
        fprintf(stderr, "%s[FR fallback — EN translation pending]%s\n\n",
                C_DIM, C_RESET);
    }

    if (opts->json) {
        printf("{\n  \"name\": ");
        json_write_cstr(d->name);
        printf(",\n  \"path\": ");
        json_write_cstr(d->path);
        printf(",\n  \"lang\": \"fr\"");
        if (opts->lang == DOC_LANG_EN) {
            printf(",\n  \"lang_requested\": \"en\",\n  \"lang_fallback\": true");
        }
        printf(",\n  \"content\": ");
        json_write_string(d->data, d->len);
        printf("\n}\n");
        return 0;
    }

    render_markdown_to_terminal(d->data, d->len);
    return 0;
}

int cli_doc_learn(int argc, char** argv) {
    /* argv[0] = "learn" */
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "learn")) return 64;

    if (opts.pos_argc == 0) {
        return learn_list(&opts);
    }
    return learn_show(&opts, opts.pos_argv[0]);
}

/* ------------------------------------------------------------------
 * owl list — listes filtrables du registry
 * ------------------------------------------------------------------ */

/*  — forward decl : variante générique (descFR/descEN bruts) utilisée
 * par list_constants/list_types pour une desc mono-langue. Défini plus bas. */
static const char* type_to_lang(const char* descFR, const char* descEN, DocLang lang);

/*  — sélecteur bilingue pour les labels/en-têtes STATIQUES du rendu
 * CLI doc (headers de section, footers, mentions figées). `--lang en` doit
 * les rendre en EN sobre, sans mention externe (Règle 7). Distinct de
 * type_to_lang qui sélectionne des descriptions du registry. */
static const char* dl(DocLang lang, const char* fr, const char* en) {
    return (lang == DOC_LANG_EN) ? en : fr;
}

/*  — forme EN canon d'un nom de symbole du registre (type, enum ou
 * constante), ou le nom recu si le symbole est inconnu / sans alias EN.
 * Un nom de symbole n'est PAS de la prose : on ne le traduit pas, on lit sa
 * forme EN dans le registre. Sans ca, un defaut valant `Actif` deviendrait
 * « Active » par traduction litterale d'un nom propre — juste par accident. */
static const char* symbol_en(const char* name) {
    if (!name || !*name) return name;
    const OwlConstDef* c = owl_registry_const_find(name);
    if (c && c->nameEN && *c->nameEN) return c->nameEN;
    const OwlEnum* e = owl_registry_enum_find(name);
    if (e && e->nameEN && *e->nameEN) return e->nameEN;
    const OwlTypeDef* t = owl_registry_type_find(name);
    if (t && t->nameEN && *t->nameEN) return t->nameEN;
    return name;
}

/* Idem, conditionne a la langue demandee. */
static const char* symbol_lang(const char* name, DocLang lang) {
    return (lang == DOC_LANG_EN) ? symbol_en(name) : name;
}

/*  — forme d'affichage EN d'un nom de VOISIN. Les voisins du maillage
 * "Voir aussi" sont des types composes OU des natives partageant la categorie,
 * stockes en forme FR canon (find_neighbors_*). En sortie EN on lit leur alias
 * EN dans le registre (types puis natives), repli sur le nom recu si aucun
 * nameEN. symbol_en ne couvre pas les natives : d'ou ce helper dedie. */
static const char* neighbor_name_lang(const char* name, DocLang lang) {
    if (lang != DOC_LANG_EN || !name || !*name) return name;
    const OwlTypeDef* t = owl_registry_type_find(name);
    if (t && t->nameEN && *t->nameEN) return t->nameEN;
    const OwlNativeDef* n = owl_registry_find(name);
    if (n && n->nameEN && *n->nameEN) return n->nameEN;
    return name;
}

/*  — forme EN d'un nom de widget kind (« Libelle » -> « Label »,
 * « ZoneRepetee » -> « Looper »). Source : CONTROL_KINDS (ui_decl.c), seule
 * source de verite FR/ASCII/EN des kinds. Le nom recu peut etre la forme
 * accentuee ou la forme ASCII : les deux sont testees. */
static const char* control_kind_lang(const char* kind, DocLang lang) {
    if (lang != DOC_LANG_EN || !kind || !*kind) return kind;
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        if (strcmp(k->nameFR, kind) == 0 ||
            (k->nameAscii && strcmp(k->nameAscii, kind) == 0)) {
            return (k->nameEN && *k->nameEN) ? k->nameEN : kind;
        }
    }
    return kind;
}

/*  — forme EN d'un nom de propriete DSL (« Valeur » -> « Value »).
 * Le catalogue des props est indexe par kind ; on cherche la premiere
 * entree qui matche le nom, les alias EN etant identiques d'un kind a
 * l'autre pour une meme propriete. */
static const char* dsl_prop_name_lang(const char* name, DocLang lang) {
    if (lang != DOC_LANG_EN || !name || !*name) return name;
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def) continue;
        const OwlControlPropDef* d = e->def;
        if ((d->nameFR && strcmp(d->nameFR, name) == 0) ||
            (d->nameAscii && strcmp(d->nameAscii, name) == 0)) {
            return (d->nameEN && *d->nameEN) ? d->nameEN : name;
        }
    }
    return name;
}

/*  — les valeurs par defaut du registry sont stockees en forme
 * SOURCE FR ("Vrai", "Faux", "11", "{}"). En --lang en on rend la forme
 * source EN equivalente : les deux compilent, mais seule la seconde est
 * copiable telle quelle dans du code ecrit en anglais.
 *  — s'etend aux defauts qui sont des NOMS DE CONSTANTES d'enum
 * (« Actif », « dispVerticale », « largeurProportionnelle ») : meme
 * mecanisme, forme source EN, mais resolue par le registre et non par une
 * table de litteraux. Les autres defauts restent neutres (nombres, "{}",
 * "[]", chaines libres). */
static const char* default_display(const char* dft, DocLang lang) {
    if (lang != DOC_LANG_EN || !dft) return dft;
    if (strcmp(dft, "Vrai") == 0) return "True";
    if (strcmp(dft, "Faux") == 0) return "False";
    return symbol_en(dft);
}

/*  — nom du type d'une propriete DSL. owl_prop_type_name() rend la
 * forme FR (« entier », « valeur d'enum ») : c'est ce que le compilateur
 * imprime dans ses diagnostics FR, on ne le touche pas. Le rendu CLI, lui,
 * doit honorer --lang en. */
static const char* prop_type_name_lang(OwlPropType t, DocLang lang) {
    if (lang != DOC_LANG_EN) return owl_prop_type_name(t);
    switch (t) {
        case OWL_PROP_TYPE_INT:         return "integer";
        case OWL_PROP_TYPE_STRING:      return "string";
        case OWL_PROP_TYPE_BOOL:        return "boolean";
        case OWL_PROP_TYPE_VARIANT:     return "any";
        case OWL_PROP_TYPE_POSITION:    return "position (integer, integer)";
        case OWL_PROP_TYPE_COLOR:       return "color (RGB integer)";
        case OWL_PROP_TYPE_ENUM:        return "enum value";
        case OWL_PROP_TYPE_MULTILINGUE: return "multilingualString";
        case OWL_PROP_TYPE_OBJECT:      return "composite descriptor";
    }
    return owl_prop_type_name(t);
}

/*  — vocabulaire de type SANS contrepartie au registre. « tout »,
 * « rien », « varié » ne sont pas des symboles OWL : ce sont les mots du
 * rendu pour « n'importe quelle valeur », « pas de retour » et « forme
 * variable ». Même classe que prop_type_name_lang ci-dessus (une forme EN
 * de rendu), pas une traduction de contenu de registre.
 * Les entrées empruntées ailleurs dans le code, plutôt qu'inventées :
 *   pile/file/liste  → METHOD_RECEIVER_INFO (owl_methods.c), colonne alias EN
 *   Énumération      → fmt_keywords (formatter.c), partner « Enumeration »
 *   descripteur compose / valeur d'enum / couleur (entier RGB) / position
 *                    → prop_type_name_lang, mêmes libellés
 * Les entrées multi-mots sont testées AVANT tout découpage. */
static const struct { const char* fr; const char* en; } TYPE_WORD_EN[] = {
    {"couleur (entier RGB)",      "color (RGB integer)"},
    {"position (entier, entier)", "position (integer, integer)"},
    {"descripteur compose",       "composite descriptor"},
    {"constantes de langue",      "language constants"},
    {"tableau associatif",        "associative array"},
    {"valeur d'enum",             "enum value"},
    {"Énumération",               "Enumeration"},
    {"procédure",                 "procedure"},
    /* Pluriel FR rendu au singulier : la forme générique documentée du
     * conteneur est `array of <type>` (cf. PRIMITIVE_TYPES, descEN). */
    {"chaînes",                   "string"},
    {"liste",                     "list"},
    {"pile",                      "stack"},
    {"file",                      "queue"},
    {"varié",                     "varies"},
    {"rien",                      "nothing"},
    {"tout",                      "any"},
    {NULL, NULL}
};

static void type_seg_en(const char* s, long len, char* out, size_t cap);

static void type_trim(const char** s, long* len) {
    while (*len > 0 && ((*s)[0] == ' ' || (*s)[0] == '\t')) { (*s)++; (*len)--; }
    while (*len > 0 && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t')) (*len)--;
}

/* Préfixe ASCII case-insensible. Retourne la longueur consommée, 0 sinon. */
static long type_prefix_ci(const char* s, long len, const char* prefix) {
    long n = (long)strlen(prefix);
    if (len < n) return 0;
    for (long i = 0; i < n; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return n;
}

/* Forme EN d'un JETON de type. Chaîne de résolution, dans l'ordre :
 *   1. type primitif (PRIMITIVE_TYPES.nameEN)
 *   2. registre : constante / énumération / type composé (symbol_en)
 *   3. sorte de widget, avec ou sans le préfixe « Champ » que porte le
 *      receveur d'une méthode préfixe (CONTROL_KINDS.nameEN)
 *   4. vocabulaire de rendu ci-dessus
 *   5. tel quel — couvre les noms identiques FR/EN (Image, Timer,
 *      zipArchive, styleXxx…) */
static void type_word_en(const char* w, long len, char* out, size_t cap) {
    type_trim(&w, &len);
    if (len > 0 && w[len - 1] == '?') {
        char base[192];
        type_word_en(w, len - 1, base, sizeof base);
        snprintf(out, cap, "%s?", base);
        return;
    }
    char buf[192];
    if (len <= 0 || (size_t)len >= sizeof buf) {
        snprintf(out, cap, "%.*s", (int)len, w);
        return;
    }
    memcpy(buf, w, (size_t)len);
    buf[len] = '\0';

    const PrimitiveType* pt = find_primitive_type(buf);
    if (pt && pt->nameEN && *pt->nameEN) { snprintf(out, cap, "%s", pt->nameEN); return; }

    const char* sym = symbol_en(buf);
    if (sym != buf && sym && *sym) { snprintf(out, cap, "%s", sym); return; }

    const char* kindPart = (strncmp(buf, "Champ", 5) == 0 && buf[5] != '\0')
                             ? buf + 5 : buf;
    const char* kd = control_kind_lang(kindPart, DOC_LANG_EN);
    if (kd != kindPart && kd && *kd) { snprintf(out, cap, "%s", kd); return; }

    for (int i = 0; TYPE_WORD_EN[i].fr; i++) {
        if (word_eq_ci(buf, len, TYPE_WORD_EN[i].fr)) {
            snprintf(out, cap, "%s", TYPE_WORD_EN[i].en);
            return;
        }
    }
    snprintf(out, cap, "%s", buf);
}

/* Forme EN d'un SEGMENT (une alternative entre deux `|`). Gère les formes
 * conteneur et la disjonction rédigée « X ou Y » qu'on trouve dans quelques
 * types de paramètre. */
static void type_seg_en(const char* s, long len, char* out, size_t cap) {
    type_trim(&s, &len);
    for (int i = 0; TYPE_WORD_EN[i].fr; i++) {
        if (word_eq_ci(s, len, TYPE_WORD_EN[i].fr)) {
            snprintf(out, cap, "%s", TYPE_WORD_EN[i].en);
            return;
        }
    }
    for (long i = 0; i + 4 <= len; i++) {
        if (memcmp(s + i, " ou ", 4) != 0) continue;
        char a[192], b[192];
        type_seg_en(s, i, a, sizeof a);
        type_seg_en(s + i + 4, len - i - 4, b, sizeof b);
        snprintf(out, cap, "%s or %s", a, b);
        return;
    }
    long n = type_prefix_ci(s, len, "tableau associatif de ");
    if (n > 0) {
        char rest[192];
        type_seg_en(s + n, len - n, rest, sizeof rest);
        snprintf(out, cap, "associative array of %s", rest);
        return;
    }
    n = type_prefix_ci(s, len, "tableau de ");
    if (n > 0) {
        char rest[192];
        type_seg_en(s + n, len - n, rest, sizeof rest);
        snprintf(out, cap, "array of %s", rest);
        return;
    }
    type_word_en(s, len, out, cap);
}

/*  — forme EN d'une CHAÎNE DE TYPE du registre (OwlParam.type,
 * OwlSignature.returnType, OwlField.type, receveur d'un methodBinding).
 * Un nom de type n'est pas de la prose : on le lit au registre, on ne le
 * traduit pas — même geste que symbol_en sur les noms de
 * constantes. En FR la chaîne est rendue telle quelle, à l'octet près.
 * `buf` doit vivre jusqu'à l'impression. */
static const char* type_str_lang(const char* typestr, DocLang lang,
                                 char* buf, size_t cap) {
    if (lang != DOC_LANG_EN || !typestr || !*typestr) return typestr;
    size_t used = 0;
    buf[0] = '\0';
    const char* p = typestr;
    while (*p) {
        const char* seg = p;
        while (*p && *p != '|') p++;
        char en[256];
        type_seg_en(seg, p - seg, en, sizeof en);
        int n = snprintf(buf + used, cap - used, "%s%s", used ? "|" : "", en);
        if (n < 0 || (size_t)n >= cap - used) { used = cap - 1; break; }
        used += (size_t)n;
        if (*p == '|') p++;
    }
    buf[used] = '\0';
    return buf;
}

/*  — nom d'une énumération citée en contrainte (`∈ triType`) sous sa
 * forme EN canon (OwlEnum.nameEN, invariant bilingue n°3). */
static const char* enum_name_lang(const OwlEnum* e, DocLang lang) {
    if (!e) return "";
    if (lang == DOC_LANG_EN && e->nameEN && *e->nameEN) return e->nameEN;
    return e->name;
}

/* Choisit descFR ou descEN selon la langue demandée, avec fallback FR. */
static const char* pick_desc(const OwlNativeDef* d, DocLang lang) {
    if (lang == DOC_LANG_EN && d->descEN && *d->descEN) return d->descEN;
    if (d->descFR && *d->descFR) return d->descFR;
    if (d->descEN && *d->descEN) return d->descEN;
    return "";
}

/* Catégories distinctes du registry. Stocké dans un petit array
 * statique (~50 entrées max), interrogé en O(n) — suffisant. */
typedef struct {
    const char* name;
    int         count;
} CategoryStat;

static int collect_categories(CategoryStat* out, int max) {
    int n = 0;
    int total = owl_registry_count();
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        const char* c = d->category ? d->category : "(sans)";
        int found = 0;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].name, c) == 0) {
                out[j].count++;
                found = 1;
                break;
            }
        }
        if (!found && n < max) {
            out[n].name  = c;
            out[n].count = 1;
            n++;
        }
    }
    return n;
}

static int compare_cstr_ptr(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* ------------------------------------------------------------------
 * Domaine de `--cat <X>` par bucket
 * ------------------------------------------------------------------
 * `--cat` ne designe pas la meme chose selon la sous-commande : une categorie
 * de registre pour natives / types / constantes / keywords / limites, un champ
 * DSL pour events / props. Un nom hors du domaine du bucket est une question
 * non comprise, pas une reponse vide — les deux se corrigent a l'oppose, et
 * derriere un `count: 0` rien ne les distingue.
 *
 * Le domaine est enumere ici, la ou le filtre le consomme : le diagnostic
 * imprime et le verdict rendu aux canaux machine (cli_doc_list_category_is_known,
 * consomme par le serveur MCP) sortent de la meme source. Personne ne relit le
 * texte d'affichage pour savoir si une famille existe. */
/* Sous-cibles de `owl list`. Doit refleter exactement ce que cli_doc_list
 * dispatche (verrou dans cli_doc_test.c) : pas d'enums, retirees comme
 * famille navigable par  / . Alimente la completion et
 * l'enumeration des sous-commandes qui acceptent `--cat` — deduite du domaine
 * ci-dessous, pour qu'une seconde liste n'ait pas a etre tenue a jour. */
static const char* LIST_CATS[] = {
    "categories", "natives", "types", "constants",
    "keywords", "operators",
    "controls", "events", "props", "globalvars", "limits",
    NULL
};

typedef enum {
    CAT_DOMAIN_NONE = 0,     /* bucket sans `--cat` : rien a valider */
    CAT_DOMAIN_NATIVES,
    CAT_DOMAIN_TYPES,
    CAT_DOMAIN_CONSTANTS,
    CAT_DOMAIN_KEYWORDS,
    CAT_DOMAIN_LIMITS,
    CAT_DOMAIN_EVENTS,
    CAT_DOMAIN_PROPS,
} CatDomain;

#define CAT_DOMAIN_MAX 192

static bool match_widget_filter(const OwlControlKindInfo* k, const char* filter);

/* Kinds propres au catalogue de props : variantes contextuelles (cellule de
 * Table, sous-champs d'un Etat) qui n'ont pas d'entree dans CONTROL_KINDS.
 * Leur forme canon est la concatenation des formes canon des mots qui les
 * composent, et se replie sur la cle par suppression des accents — elle reste
 * donc recevable telle quelle en `--cat`. */
static const struct { const char* key; const char* canon; }
PROPS_KIND_CANON_EXTRA[] = {
    { "CelluleTable",   "CelluleTable"                        },
    { "blocEtat",       "bloc\xc3\x89tat"                     },
    { "LibelleEtat",    "Libell\xc3\xa9\xc3\x89tat"           },
    { "ImageEtat",      "Image\xc3\x89tat"                    },
    { "CodeBarresEtat", "CodeBarres\xc3\x89tat"               },
    { "ServiceREST",    "ServiceREST"                         },
    { "OperationHTTP",  "Op\xc3\xa9rationHTTP"                 },
};

/* Forme canon (accentuee) d'une cle du catalogue de props. Les cles y sont
 * stockees en ASCII (`register_dsl_props_table("Fenetre", ...)`) alors que
 * CONTROL_KINDS porte, lui, les trois formes FR / ASCII / EN — c'est pour
 * cela que le bucket `events`, qui itere CONTROL_KINDS, s'en tirait bien.
 * NULL si la cle n'a aucune forme connue. */
static const char* props_kind_canon(const char* kind) {
    if (!kind || !*kind) return NULL;
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        if (strcmp(k->nameFR, kind) == 0) return k->nameFR;
        if (k->nameAscii && strcmp(k->nameAscii, kind) == 0) return k->nameFR;
    }
    for (size_t i = 0; i < sizeof PROPS_KIND_CANON_EXTRA /
                           sizeof PROPS_KIND_CANON_EXTRA[0]; i++) {
        if (strcmp(PROPS_KIND_CANON_EXTRA[i].key, kind) == 0)
            return PROPS_KIND_CANON_EXTRA[i].canon;
    }
    return NULL;
}

/* Libelle affiche pour une cle de props. La totalite de la resolution est un
 * invariant tenu par `owl --self-test-cli`, que `make` execute : un kind
 * ajoute au catalogue sans forme canon rougit au build. Le repli sur la cle
 * n'est donc pas une tolerance, c'est le refus de rendre une chaine vide sur
 * un etat que la batterie interdit. */
static const char* props_kind_display(const char* kind) {
    const char* canon = props_kind_canon(kind);
    return canon ? canon : kind;
}

static void cat_domain_push(const char** out, int* n, int max, const char* v) {
    if (!v || !*v || *n >= max) return;
    for (int i = 0; i < *n; i++) if (strcmp(out[i], v) == 0) return;
    out[(*n)++] = v;
}

/* Familles valables du bucket, dans leur forme cle. Un kind de champ sans
 * event ni propriete reste une valeur valable : la reponse est vide, la
 * question etait comprise. */
static int cat_domain_collect(CatDomain d, const char** out, int max) {
    int n = 0;
    switch (d) {
    case CAT_DOMAIN_NATIVES:
        for (int i = 0, t = owl_registry_count(); i < t; i++)
            cat_domain_push(out, &n, max, owl_registry_at(i)->category);
        break;
    case CAT_DOMAIN_TYPES:
        for (int i = 0, t = owl_registry_types_count(); i < t; i++)
            cat_domain_push(out, &n, max, owl_registry_type_at(i)->category);
        break;
    case CAT_DOMAIN_CONSTANTS:
        for (int i = 0, t = owl_registry_consts_count(); i < t; i++)
            cat_domain_push(out, &n, max, owl_registry_const_at(i)->category);
        break;
    case CAT_DOMAIN_KEYWORDS:
        for (int i = 0, t = owl_keyword_count(); i < t; i++)
            cat_domain_push(out, &n, max, owl_keyword_at(i)->category);
        break;
    case CAT_DOMAIN_LIMITS:
        for (int i = 0, t = owl_registry_limit_count(); i < t; i++) {
            const OwlLimitDef* L = owl_registry_limit_at(i);
            if (L) cat_domain_push(out, &n, max, L->category);
        }
        break;
    case CAT_DOMAIN_EVENTS:
        for (const OwlControlKindInfo* k = owl_control_kinds_table();
             k->nameFR; k++)
            cat_domain_push(out, &n, max, k->nameFR);
        break;
    case CAT_DOMAIN_PROPS:
        /* Forme canon et non cle brute : cette liste est aussi celle que le
         * diagnostic « Champs disponibles » invite a recopier. */
        for (int i = 0, t = owl_dsl_props_count(); i < t; i++)
            cat_domain_push(out, &n, max,
                            props_kind_display(owl_dsl_prop_at(i)->kind));
        break;
    case CAT_DOMAIN_NONE:
        break;
    }
    return n;
}

/* `--cat` designe un champ DSL (events / props) et non une categorie de
 * registre : le libelle du diagnostic suit le domaine. */
static int cat_domain_is_field(CatDomain d) {
    return d == CAT_DOMAIN_EVENTS || d == CAT_DOMAIN_PROPS;
}

/* Meme comparaison que le filtre du bucket : cle exacte pour les categories
 * de registre (deja normalisees bilinguement au parse des options),
 * casse- et accent-insensible sur les formes du kind pour les champs DSL. Les
 * cles du catalogue de props restent ASCII (`Fenetre`) et seul l'affichage
 * porte la forme canon : c'est la comparaison a l'entree qui absorbe l'accent,
 * pas le stockage, et elle l'absorbe dans les deux sens. */
static int cat_domain_contains(CatDomain d, const char* cat) {
    if (!cat || !*cat) return 1;
    if (d == CAT_DOMAIN_EVENTS) {
        for (const OwlControlKindInfo* k = owl_control_kinds_table();
             k->nameFR; k++)
            if (match_widget_filter(k, cat)) return 1;
        return 0;
    }
    const char* dom[CAT_DOMAIN_MAX];
    int n = cat_domain_collect(d, dom, CAT_DOMAIN_MAX);
    for (int i = 0; i < n; i++) {
        if (d == CAT_DOMAIN_PROPS) {
            if (owl_category_eq_ci(dom[i], cat)) return 1;
        } else if (strcmp(dom[i], cat) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Diagnostic texte : nomme la valeur fautive, puis enumere les valables du
 * bucket. Renvoyer vers une commande d'aide obligerait a un second aller-retour
 * pour corriger l'appel. */
static void print_unknown_cat(const DocOpts* opts, CatDomain d,
                              const char* cat) {
    int field = cat_domain_is_field(d);
    printf("  %s%s%s %s%s%s\n", C_DIM,
           field ? dl(opts->lang, "champ inconnu :", "unknown control:")
                 : dl(opts->lang, "catégorie inconnue :", "unknown category:"),
           C_RESET, C_CONST, cat, C_RESET);

    const char* dom[CAT_DOMAIN_MAX];
    int n = cat_domain_collect(d, dom, CAT_DOMAIN_MAX);
    /* Suggestion rendue dans la langue courante, et triee sur la forme
     * AFFICHEE : trier sur la cle FR donnerait une liste anglaise en desordre
     * apparent. Les kinds de champ n'ont pas de forme de categorie. */
    if (!field)
        for (int i = 0; i < n; i++)
            dom[i] = owl_category_display(dom[i], opts->lang == DOC_LANG_EN);
    qsort(dom, (size_t)n, sizeof(const char*), compare_cstr_ptr);

    printf("\n  %s%s%s ", C_DIM,
           field ? dl(opts->lang, "Champs disponibles :", "Available controls:")
                 : dl(opts->lang, "Catégories disponibles :",
                                  "Available categories:"),
           C_RESET);
    for (int i = 0; i < n; i++)
        printf("%s%s%s%s", C_CONST, dom[i], C_RESET, (i + 1 < n) ? ", " : "");
    printf("\n");
}

/* Meme verdict en JSON : une machine ne doit pas avoir a lire une phrase pour
 * savoir que le nom n'a pas ete reconnu. Fragment insere dans l'objet deja
 * ouvert, juste avant `count`. */
static void json_emit_unknown_cat(CatDomain d, const char* cat) {
    printf(",\n  \"unknown_category\": ");
    json_write_cstr(cat);
    printf(",\n  \"valid_categories\": [");
    const char* dom[CAT_DOMAIN_MAX];
    int n = cat_domain_collect(d, dom, CAT_DOMAIN_MAX);
    qsort(dom, (size_t)n, sizeof(const char*), compare_cstr_ptr);
    for (int i = 0; i < n; i++) {
        if (i) printf(", ");
        json_write_cstr(dom[i]);
    }
    printf("]");
}

/* Nom canonique de sous-commande -> domaine. Les alias du dispatch (`fn`,
 * `consts`, `mots-cles`...) ne sont pas repris : les canaux machine passent le
 * nom canonique, et redupliquer les chaines d'alias ici recreerait la
 * divergence que ce point d'entree supprime. */
static CatDomain cat_domain_of_bucket(const char* bucket) {
    if (!bucket)                            return CAT_DOMAIN_NONE;
    if (strcmp(bucket, "natives")   == 0)   return CAT_DOMAIN_NATIVES;
    if (strcmp(bucket, "types")     == 0)   return CAT_DOMAIN_TYPES;
    if (strcmp(bucket, "constants") == 0)   return CAT_DOMAIN_CONSTANTS;
    if (strcmp(bucket, "keywords")  == 0)   return CAT_DOMAIN_KEYWORDS;
    if (strcmp(bucket, "limits")    == 0)   return CAT_DOMAIN_LIMITS;
    if (strcmp(bucket, "events")    == 0)   return CAT_DOMAIN_EVENTS;
    if (strcmp(bucket, "props")     == 0)   return CAT_DOMAIN_PROPS;
    return CAT_DOMAIN_NONE;
}

int cli_doc_list_bucket_filters_by_category(const char* bucket) {
    return cat_domain_of_bucket(bucket) != CAT_DOMAIN_NONE;
}

/* Sous-commandes sur lesquelles `--cat` a un sens, dans l'ordre de la
 * completion. Deduites du domaine : une sous-commande qui gagne des familles
 * entre dans le message sans edit. */
static void print_cat_capable_buckets(int json) {
    int first = 1;
    for (int i = 0; LIST_CATS[i]; i++) {
        if (!cli_doc_list_bucket_filters_by_category(LIST_CATS[i])) continue;
        if (json) {
            if (!first) printf(", ");
            json_write_cstr(LIST_CATS[i]);
        } else {
            fprintf(stderr, "%s%s", first ? "" : ", ", LIST_CATS[i]);
        }
        first = 0;
    }
}

/* `--cat` sur une liste qui n'est pas decoupee en familles.
 *
 * On refuse au lieu d'avertir puis de rendre la liste complete : un
 * avertissement laisse derriere lui une sortie complete, exploitable telle
 * quelle, que rien ne distingue d'un resultat filtre une fois la phrase
 * ignoree — ce qui est exactement le defaut d'origine, deplace d'un cran. Un
 * refus ne laisse rien a prendre pour un filtrage, et le code de retour rend
 * l'erreur visible d'un script comme d'un canal machine sans lire de texte.
 *
 * Le message enumere les sous-commandes qui acceptent reellement `--cat` :
 * renvoyer vers une commande d'aide obligerait a un second aller-retour pour
 * corriger l'appel. Rend 1 si l'appel a ete refuse (l'appelant sort), 0 sinon. */
static int reject_cat_unsupported(const DocOpts* opts, const char* bucket) {
    if (!opts->cat || !*opts->cat) return 0;
    if (cli_doc_list_bucket_filters_by_category(bucket)) return 0;

    if (opts->json) {
        printf("{\n  \"bucket\": ");
        json_write_cstr(bucket);
        printf(",\n  \"error\": \"category_filter_not_supported\"");
        printf(",\n  \"requested_category\": ");
        json_write_cstr(opts->cat);
        printf(",\n  \"buckets_accepting_cat\": [");
        print_cat_capable_buckets(1);
        printf("]\n}\n");
        return 1;
    }

    /* Le deux-points est dans le libelle : l'anglais ne prend pas l'espace
     * avant, le francais si. */
    fprintf(stderr, "owl list %s%s --cat '%s' %s\n", bucket,
            dl(opts->lang, " :", ":"), opts->cat,
            dl(opts->lang, "ne s'applique pas.", "does not apply."));
    fprintf(stderr, "  %s\n",
            dl(opts->lang,
               "Cette liste n'est pas découpée en catégories : le filtre "
               "resterait sans effet et la liste complète passerait pour un "
               "résultat filtré.",
               "This list is not split into categories: the filter would have "
               "no effect and the full list would pass for a filtered "
               "result."));
    fprintf(stderr, "  %s ",
            dl(opts->lang, "Listes acceptant --cat :",
                           "Lists accepting --cat:"));
    print_cat_capable_buckets(0);
    fprintf(stderr, "\n");
    return 1;
}

int cli_doc_list_category_is_known(const char* bucket, const char* cat) {
    if (!cat || !*cat) return 1;
    CatDomain d = cat_domain_of_bucket(bucket);
    if (d == CAT_DOMAIN_NONE) return 1;
    /* Meme normalisation bilingue qu'au parse de `--cat` : les deux formes
     * canoniques designent la meme categorie. */
    const OwlCategoryPair* pair = owl_category_resolve(cat);
    return cat_domain_contains(d, pair ? pair->fr : cat);
}

static int compare_cat(const void* a, const void* b) {
    const CategoryStat* ca = (const CategoryStat*)a;
    const CategoryStat* cb = (const CategoryStat*)b;
    return strcmp(ca->name, cb->name);
}

static int list_categories(const DocOpts* opts) {
    CategoryStat cats[128];
    int n = collect_categories(cats, 128);

    if (opts->json) {
        qsort(cats, (size_t)n, sizeof(CategoryStat), compare_cat);
        printf("{\n  \"categories\": [\n");
        for (int i = 0; i < n; i++) {
            printf("    { \"name\": ");
            json_write_cstr(cats[i].name);
            printf(", \"count\": %d }%s\n",
                   cats[i].count, i + 1 < n ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }

    printf("%s%s%s\n", C_SECTION,
           dl(opts->lang, "Catégories de fonctions natives",
                          "Native function categories"), C_RESET);
    printf("%s%d %s, %d %s%s\n\n",
           C_DIM, n,
           dl(opts->lang, "catégorie(s)", "category(ies)"),
           owl_registry_count(),
           dl(opts->lang, "fonction(s) au total.", "function(s) in total."),
           C_RESET);

    /* Le nom de categorie est une CLE bilingue : la commande voisine
     * `list natives --cat Tableaux --lang en` rend deja « category Arrays ».
     * Deux commandes ne peuvent pas diverger sur la meme cle. Le tri porte
     * sur la forme AFFICHEE : trier sur la cle FR rendrait une liste anglaise
     * en desordre apparent (meme raison qu'au bloc de suggestion voisin). */
    for (int i = 0; i < n; i++)
        cats[i].name = owl_category_display(cats[i].name,
                                            opts->lang == DOC_LANG_EN);
    qsort(cats, (size_t)n, sizeof(CategoryStat), compare_cat);

    for (int i = 0; i < n; i++) {
        /* Largeur comptee en graphemes (les formes FR portent des accents). */
        printf("  %s", C_CONST);
        print_padded_utf8(cats[i].name, 24);
        printf("%s %s(%d %s)%s\n",
               C_RESET, C_DIM, cats[i].count,
               dl(opts->lang, "fonctions", "functions"), C_RESET);
    }
    /* Le deux-points est dans le libellé : l'anglais ne prend pas l'espace
     * avant, le français si. */
    printf("\n%s %sowl list natives --cat%s %s<%s>%s\n",
           dl(opts->lang, "Détail d'une catégorie :", "Details of a category:"),
           C_NATIVE, C_RESET, C_TYPE,
           dl(opts->lang, "nom", "name"), C_RESET);
    return 0;
}

static int list_natives(const DocOpts* opts, const char* cat_filter) {
    int total = owl_registry_count();
    int shown = 0;

    if (opts->json) {
        printf("{\n  \"natives\": [\n");
        int first = 1;
        for (int i = 0; i < total; i++) {
            const OwlNativeDef* d = owl_registry_at(i);
            if (d->flags & OWL_NAT_INTERNAL) continue;
            if (cat_filter && (!d->category ||
                               strcmp(d->category, cat_filter) != 0)) continue;
            if (!first) printf(",\n");
            first = 0;
            printf("    {\n      \"name\": ");
            json_write_cstr(d->canonical);
            printf(",\n      \"name_ascii\": ");
            json_write_cstr(d->canonicalAscii ? d->canonicalAscii : d->canonical);
            printf(",\n      \"name_en\": ");
            json_write_cstr(d->nameEN);
            printf(",\n      \"category\": ");
            json_write_cstr(d->category);
            printf(",\n      \"desc\": ");
            json_write_cstr(pick_desc(d, opts->lang));
            printf("\n    }");
            shown++;
        }
        printf("\n  ]");
        /* Un `count: 0` ne dit pas si la catégorie est vide ou si son nom n'a
         * pas été reconnu. Un consommateur n'a aucun moyen de distinguer les
         * deux : on nomme le cas. */
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_NATIVES, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_NATIVES, cat_filter);
        printf(",\n  \"count\": %d\n}\n", shown);
        return 0;
    }

    if (cat_filter) {
        printf("%s%s %s%s%s\n",
               C_SECTION,
               dl(opts->lang, "Fonctions natives — catégorie",
                              "Native functions — category"),
               C_CONST,
               owl_category_display(cat_filter, opts->lang == DOC_LANG_EN),
               C_RESET);
    } else {
        printf("%s%s%s %s(%d %s)%s\n",
               C_SECTION,
               dl(opts->lang, "Fonctions natives", "Native functions"),
               C_RESET, C_DIM, total,
               dl(opts->lang, "au total", "total"), C_RESET);
    }
    printf("\n");

    /* Largeur max du nom pour alignement compact — comptée en graphèmes,
     * pas en bytes (sinon `ChaîneDécoupe` drift d'1 ou 2 char). */
    int wmax = 0;
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (cat_filter && (!d->category ||
                           strcmp(d->category, cat_filter) != 0)) continue;
        int w = utf8_visual_width(d->canonical);
        if (w > wmax) wmax = w;
    }
    if (wmax > 32) wmax = 32;
    if (wmax < 12) wmax = 12;

    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (cat_filter && (!d->category ||
                           strcmp(d->category, cat_filter) != 0)) continue;
        const char* desc = pick_desc(d, opts->lang);
        printf("  %s", C_NATIVE);
        print_padded_utf8(d->canonical, wmax);
        printf("%s  %s%s%s\n", C_RESET, C_DIM, desc, C_RESET);
        shown++;
    }
    if (cat_filter && shown == 0) {
        /* Deux causes distinctes derrière un « 0 résultat », et l'utilisateur
         * n'a pas à les confondre : soit le nom n'est porté par aucune entrée
         * du registre (question non comprise), soit la catégorie existe mais
         * ne contient que des entrées internes (question comprise, réponse
         * vide). Répondre « aucune fonction » au premier cas laisse croire
         * que le nom a été reconnu. */
        if (!cat_domain_contains(CAT_DOMAIN_NATIVES, cat_filter)) {
            print_unknown_cat(opts, CAT_DOMAIN_NATIVES, cat_filter);
        } else {
            printf("  %s%s%s\n", C_DIM,
                   dl(opts->lang, "(aucune fonction dans cette catégorie)",
                                  "(no function in this category)"), C_RESET);
        }
    }
    printf("\n%s%d %s%s ", C_DIM, shown,
           dl(opts->lang, "résultat(s).", "result(s)."), C_RESET);
    printf("%s : %sowl explain%s %s<%s>%s\n",
           dl(opts->lang, "Détail d'une fonction", "Details of a function"),
           C_NATIVE, C_RESET, C_TYPE,
           dl(opts->lang, "nom", "name"), C_RESET);
    return 0;
}

static int list_types(const DocOpts* opts, const char* cat_filter) {
    int total = owl_registry_types_count();

    if (opts->json) {
        printf("{\n  \"primitives\": [\n");
        int first = 1;
        /* Les deux compteurs comptent ce qu'ils accompagnent, pas le total du
         * catalogue : sous `--cat`, un compteur reste global fait conclure a
         * une reponse tronquee, et rien dans la sortie ne dement. */
        int shown_p = 0, shown_t = 0;
        /* Les primitifs n'ont pas de catégorie registry : un --cat les
         * exclut tous (le filtre porte sur les types composés). */
        if (!cat_filter)
        for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) {
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"name\": ");
            json_write_cstr(p->name);
            printf(", \"name_ascii\": "); json_write_cstr(p->nameAscii);
            printf(", \"name_en\": ");    json_write_cstr(p->nameEN);
            printf(", \"descFR\": ");     json_write_cstr(p->descFR);
            printf(", \"descEN\": ");     json_write_cstr(p->descEN);
            printf(" }");
            shown_p++;
        }
        printf("\n  ],\n  \"types\": [\n");
        int first_t = 1;
        for (int i = 0; i < total; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            if (cat_filter && (!t->category ||
                               strcmp(t->category, cat_filter) != 0)) continue;
            if (!first_t) printf(",\n");
            first_t = 0;
            printf("    { \"name\": ");
            json_write_cstr(t->canonical);
            printf(", \"name_ascii\": ");
            json_write_cstr(t->canonicalAscii ? t->canonicalAscii : t->canonical);
            printf(", \"category\": ");
            json_write_cstr(t->category);
            printf(" }");
            shown_t++;
        }
        printf("\n  ],\n  \"primitives_count\": %d,\n  \"types_count\": %d",
               shown_p, shown_t);
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_TYPES, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_TYPES, cat_filter);
        printf("\n}\n");
        return 0;
    }

    if (!cat_filter) {
        printf("%s%s%s %s(%s)%s\n\n",
               C_SECTION,
               dl(opts->lang, "Types primitifs", "Primitive types"),
               C_RESET, C_DIM,
               dl(opts->lang, "intégrés à la VM", "built into the VM"),
               C_RESET);
        for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) {
            const char* desc = (opts->lang == DOC_LANG_EN && p->descEN && *p->descEN)
                                 ? p->descEN : p->descFR;
            /*  — la colonne clé du catalogue suit la langue : un
             * anglophone écrit `x est un integer`, pas `entier`. Les deux
             * formes résolvent (invariant bilingue n°3). */
            printf("  %s", C_TYPE);
            print_padded_utf8((opts->lang == DOC_LANG_EN && p->nameEN && *p->nameEN)
                                ? p->nameEN : p->name, 12);
            printf("%s %s%s%s\n", C_RESET, C_DIM,  desc ? desc : "", C_RESET);
        }
    }

    if (cat_filter) {
        printf("\n%s%s %s%s%s\n\n",
               C_SECTION,
               dl(opts->lang, "Types composés — catégorie",
                              "Composite types — category"),
               C_CONST,
               owl_category_display(cat_filter, opts->lang == DOC_LANG_EN),
               C_RESET);
    } else {
        printf("\n%s%s%s %s(%d %s)%s\n\n",
               C_SECTION,
               dl(opts->lang, "Types composés du registry",
                              "Composite types from the registry"),
               C_RESET, C_DIM, total,
               dl(opts->lang, "au total", "total"), C_RESET);
    }

    for (int i = 0; i < total; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (cat_filter && (!t->category ||
                           strcmp(t->category, cat_filter) != 0)) continue;
        printf("  %s", C_TYPE);
        print_padded_utf8((opts->lang == DOC_LANG_EN && t->nameEN && *t->nameEN)
                            ? t->nameEN : t->canonical, 24);
        printf("%s %s(%s)%s\n",
               C_RESET,
               C_DIM,  t->category ? t->category : "(sans)", C_RESET);
    }
    if (cat_filter && !cat_domain_contains(CAT_DOMAIN_TYPES, cat_filter))
        print_unknown_cat(opts, CAT_DOMAIN_TYPES, cat_filter);
    printf("\n%s : %sowl explain%s %s<%s>%s\n",
           dl(opts->lang, "Détail d'un type", "Details of a type"),
           C_NATIVE, C_RESET, C_TYPE,
           dl(opts->lang, "nom", "name"), C_RESET);
    return 0;
}

static int list_constants(const DocOpts* opts, const char* cat_filter) {
    int total = owl_registry_consts_count();
    int shown = 0;

    if (opts->json) {
        printf("{\n  \"constants\": [\n");
        int first = 1;
        for (int i = 0; i < total; i++) {
            const OwlConstDef* c = owl_registry_const_at(i);
            if (cat_filter && (!c->category ||
                               strcmp(c->category, cat_filter) != 0)) continue;
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"name\": ");
            json_write_cstr(c->name);
            printf(", \"name_en\": ");
            json_write_cstr(c->nameEN);
            printf(", \"category\": ");
            json_write_cstr(c->category);
            printf(", \"desc\": ");
            json_write_cstr(type_to_lang(c->descFR, c->descEN, opts->lang));
            printf(" }");
            shown++;
        }
        printf("\n  ]");
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_CONSTANTS, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_CONSTANTS, cat_filter);
        printf(",\n  \"count\": %d\n}\n", shown);
        return 0;
    }

    if (cat_filter) {
        printf("%s%s %s%s%s\n\n",
               C_SECTION,
               dl(opts->lang, "Constantes — catégorie", "Constants — category"),
               C_CONST,
               owl_category_display(cat_filter, opts->lang == DOC_LANG_EN),
               C_RESET);
    } else {
        printf("%s%s%s %s(%d %s)%s\n\n",
               C_SECTION,
               dl(opts->lang, "Constantes du registry", "Registry constants"),
               C_RESET, C_DIM, total,
               dl(opts->lang, "au total", "total"), C_RESET);
    }

    for (int i = 0; i < total; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (cat_filter && (!c->category ||
                           strcmp(c->category, cat_filter) != 0)) continue;
        const char* desc = type_to_lang(c->descFR, c->descEN, opts->lang);
        printf("  %s", C_CONST);
        /* Colonne cle du catalogue : elle suit la langue, comme
         * `list types --lang en` rend `string` / `integer`. Les deux formes
         * resolvent (invariant 3). */
        print_padded_utf8((opts->lang == DOC_LANG_EN && c->nameEN && *c->nameEN)
                            ? c->nameEN : c->name, 32);
        printf("%s  %s%s%s\n", C_RESET, C_DIM, desc ? desc : "", C_RESET);
        shown++;
    }
    if (cat_filter && !cat_domain_contains(CAT_DOMAIN_CONSTANTS, cat_filter))
        print_unknown_cat(opts, CAT_DOMAIN_CONSTANTS, cat_filter);
    return 0;
}

/* ----- list keywords / operators (catalogues du langage) ----- */

/* Mots réservés du langage : reconnus par le scanner et interdits comme
 * noms de variable, mais sans fiche pédagogique propre (leur sémantique
 * relève de la portée des déclarations, hors panel keyword courant). On
 * les expose tout de même dans `list keywords` pour les rendre
 * découvrables — un utilisateur qui écrit `local est un entier` doit
 * pouvoir les retrouver ici. Aligné sur tokentype_is_reserved_keyword
 * (compiler.c) pour la portée variable. */
typedef struct {
    const char* form;
    const char* noteFR;
    const char* noteEN;
} ReservedWord;
static const ReservedWord RESERVED_WORDS[] = {
    { "local",  "portée locale (mot réservé)",  "local scope (reserved word)" },
    { "global", "portée globale (mot réservé)", "global scope (reserved word)" },
};
static const int RESERVED_WORDS_COUNT =
    (int)(sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]));

/* Forme canonique d'un mot-clé dans la langue demandée.
 *
 * Le catalogue de mots-clés ne porte que la forme FR : par construction, il
 * n'a pas le droit de dupliquer une valeur qu'une autre source rend déjà
 * (cf. owl_keyword_catalog.h). La contrepartie EN vit dans la table canon
 * bilingue du formatter — celle qui décide de la forme écrite au save et que
 * `owl translate` consomme. On la lit ici, on n'en invente pas une seconde.
 *
 * Sans contrepartie 1:1 dans la table (mots neutres, quelques mots absents de
 * la table), la forme canon reste rendue telle quelle : l'invariant bilingue
 * n'a rien à dire de plus que ce que le langage porte réellement. Un groupe de
 * synonymes n'est pas un cas sans contrepartie : ses formes portent toutes le
 * `partner` du canon opposé, l'appariement se fait de canon à canon. */
static const char* token_canon_display(TokenType t, DocLang lang) {
    const char* canon = token_type_name(t);
    if (lang != DOC_LANG_EN) return canon;
    const char* en = owl_formatter_keyword_partner(canon);
    return en ? en : canon;
}

static const char* keyword_canon_display(const OwlKeywordDef* k, DocLang lang) {
    const char* canon = k->canon_display ? k->canon_display
                                         : token_type_name(k->token);
    if (lang != DOC_LANG_EN) return canon;
    const char* en = owl_formatter_keyword_partner(canon);
    if (!en) return canon;
    /*  : un mot dont le canon EN depend du sens (PAS -> NOT + STEP) est
     * nomme par ses DEUX canons en standalone, plutot que d'en taire un. On ne
     * les COMPOSE pas (aucun « STEP IN ») : c'est la liste plate d'un mot-cle,
     * pas un composite. token_canon_display (composites) garde le canon unique. */
    const char* alt = owl_formatter_keyword_partner_alt(canon);
    if (alt) {
        static char buf[64];
        snprintf(buf, sizeof(buf), "%s / %s", en, alt);
        return buf;
    }
    return en;
}

static int list_keywords(const DocOpts* opts, const char* cat_filter) {
    int total = owl_keyword_count();

    if (opts->json) {
        printf("{\n  \"keywords\": [\n");
        int first = 1;
        /* Compte les keywords rendus, pas ceux du catalogue : sous `--cat`,
         * le total ferait lire le tableau filtre comme une reponse tronquee. */
        int shown = 0;
        for (int i = 0; i < total; i++) {
            const OwlKeywordDef* k = owl_keyword_at(i);
            if (cat_filter && (!k->category ||
                               strcmp(k->category, cat_filter) != 0)) continue;
            const char* canon = k->canon_display
                                 ? k->canon_display
                                 : token_type_name(k->token);
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"name\": ");
            json_write_cstr(canon);
            printf(", \"category\": ");
            json_write_cstr(k->category);
            printf(", \"senses_count\": %d }",
                   k->senses_count);
            shown++;
        }
        /* Réservés et composés n'ont pas de catégorie : exclus par un --cat. */
        printf("\n  ],\n  \"reserved\": [\n");
        if (!cat_filter)
        for (int i = 0; i < RESERVED_WORDS_COUNT; i++) {
            printf("    { \"name\": ");
            json_write_cstr(RESERVED_WORDS[i].form);
            printf(", \"note\": ");
            json_write_cstr(dl(opts->lang, RESERVED_WORDS[i].noteFR,
                                           RESERVED_WORDS[i].noteEN));
            printf(" }%s\n", i + 1 < RESERVED_WORDS_COUNT ? "," : "");
        }
        printf("  ]");
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_KEYWORDS, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_KEYWORDS, cat_filter);
        printf(",\n  \"count\": %d\n}\n", shown);
        return 0;
    }

    if (cat_filter) {
        printf("%s%s %s%s%s\n\n",
               C_SECTION,
               dl(opts->lang, "Keywords — catégorie", "Keywords — category"),
               C_CONST,
               owl_category_display(cat_filter, opts->lang == DOC_LANG_EN),
               C_RESET);
    } else {
        printf("%s%s%s %s(%d %s)%s\n\n",
               C_SECTION,
               dl(opts->lang, "Keywords du langage", "Language keywords"),
               C_RESET, C_DIM, total,
               dl(opts->lang, "documentés", "documented"), C_RESET);
    }

    /* Regroupage par catégorie : on itère 2 fois, d'abord pour
     * collecter les catégories distinctes (ordre rencontre), ensuite
     * pour afficher les keywords de chaque. Cap raisonnable. */
    const char* cats[32]; int ncats = 0;
    for (int i = 0; i < total; i++) {
        const OwlKeywordDef* k = owl_keyword_at(i);
        const char* c = k->category ? k->category : "(sans)";
        /* Un keyword sans categorie n'appartient a aucune famille : un `--cat`
         * l'exclut, comme dans la branche JSON voisine. */
        if (cat_filter && (!k->category || strcmp(c, cat_filter) != 0)) continue;
        int seen = 0;
        for (int j = 0; j < ncats; j++) if (strcmp(cats[j], c) == 0) { seen = 1; break; }
        if (!seen && ncats < 32) cats[ncats++] = c;
    }
    for (int cidx = 0; cidx < ncats; cidx++) {
        if (cidx > 0) printf("\n");
        printf("%s── %s ──%s\n", C_DIM,
               owl_category_display(cats[cidx], opts->lang == DOC_LANG_EN),
               C_RESET);
        for (int i = 0; i < total; i++) {
            const OwlKeywordDef* k = owl_keyword_at(i);
            const char* c = k->category ? k->category : "(sans)";
            if (strcmp(c, cats[cidx]) != 0) continue;
            printf("  %s", C_KEYWORD);
            print_padded_utf8(keyword_canon_display(k, opts->lang), 18);
            printf("%s", C_RESET);
            if (k->senses_count > 1) {
                printf(" %s(%d %s)%s",
                       C_DIM, k->senses_count,
                       dl(opts->lang, "sens", "senses"), C_RESET);
            }
            printf("\n");
        }
    }
    if (cat_filter && !cat_domain_contains(CAT_DOMAIN_KEYWORDS, cat_filter))
        print_unknown_cat(opts, CAT_DOMAIN_KEYWORDS, cat_filter);
    /* Composés et réservés n'ont pas de catégorie registry : un --cat
     * les masque (le filtre ne porte que sur les keywords catégorisés). */
    if (!cat_filter) {
        /* Composés en pied. */
        int nc = owl_keyword_composite_count();
        if (nc > 0) {
            printf("\n%s── %s ──%s\n", C_DIM,
                   dl(opts->lang, "Composés", "Compound"), C_RESET);
            for (int i = 0; i < nc; i++) {
                const OwlKeywordComposite* c = owl_keyword_composite_at(i);
                printf("  %s%s %s%s\n",
                       C_KEYWORD,
                       token_canon_display(c->t1, opts->lang),
                       token_canon_display(c->t2, opts->lang),
                       C_RESET);
            }
        }
        /* Mots réservés (interdits comme noms de variable). */
        printf("\n%s── %s ──%s\n", C_DIM,
               dl(opts->lang, "Réservés", "Reserved"), C_RESET);
        for (int i = 0; i < RESERVED_WORDS_COUNT; i++) {
            printf("  %s", C_KEYWORD);
            print_padded_utf8(RESERVED_WORDS[i].form, 18);
            printf("%s %s%s%s\n",
                   C_RESET, C_DIM,
                   dl(opts->lang, RESERVED_WORDS[i].noteFR,
                                  RESERVED_WORDS[i].noteEN), C_RESET);
        }
    }
    return 0;
}

static int list_operators(const DocOpts* opts) {
    int total = owl_operator_count();

    if (opts->json) {
        printf("{\n  \"operators\": [\n");
        for (int i = 0; i < total; i++) {
            const OwlOperatorDef* o = owl_operator_at(i);
            printf("    { \"symbol\": ");
            json_write_cstr(o->display_form);
            printf(", \"category\": ");
            json_write_cstr(o->category);
            printf(", \"senses_count\": %d", o->senses_count);
            /* Desc courte = description du sens primaire (suit --lang). */
            printf(", \"desc\": ");
            json_write_cstr(o->senses_count > 0
                ? type_to_lang(o->senses[0].descFR, o->senses[0].descEN,
                               opts->lang)
                : "");
            printf(" }%s\n", i + 1 < total ? "," : "");
        }
        printf("  ],\n  \"count\": %d\n}\n", total);
        return 0;
    }

    printf("%s%s%s %s(%d %s)%s\n\n",
           C_SECTION,
           dl(opts->lang, "Opérateurs du langage", "Language operators"),
           C_RESET, C_DIM, total,
           dl(opts->lang, "documentés", "documented"), C_RESET);

    const char* cats[32]; int ncats = 0;
    for (int i = 0; i < total; i++) {
        const OwlOperatorDef* o = owl_operator_at(i);
        const char* c = o->category ? o->category : "(sans)";
        int seen = 0;
        for (int j = 0; j < ncats; j++) if (strcmp(cats[j], c) == 0) { seen = 1; break; }
        if (!seen && ncats < 32) cats[ncats++] = c;
    }
    for (int cidx = 0; cidx < ncats; cidx++) {
        if (cidx > 0) printf("\n");
        printf("%s── %s ──%s\n", C_DIM,
               owl_category_display(cats[cidx], opts->lang == DOC_LANG_EN),
               C_RESET);
        for (int i = 0; i < total; i++) {
            const OwlOperatorDef* o = owl_operator_at(i);
            const char* c = o->category ? o->category : "(sans)";
            if (strcmp(c, cats[cidx]) != 0) continue;
            printf("  %s", C_KEYWORD);
            print_padded_utf8(o->display_form ? o->display_form : "?", 8);
            printf("%s", C_RESET);
            /* Desc courte = description du sens primaire (1 ligne). */
            const char* odesc = o->senses_count > 0
                ? type_to_lang(o->senses[0].descFR, o->senses[0].descEN,
                               opts->lang)
                : NULL;
            if (odesc && *odesc) {
                printf("  %s%s%s", C_DIM, odesc, C_RESET);
            }
            if (o->senses_count > 1) {
                printf(" %s(%d %s)%s",
                       C_DIM, o->senses_count,
                       dl(opts->lang, "sens", "senses"), C_RESET);
            }
            printf("\n");
        }
    }
    return 0;
}

/* ----- list widgets / events / props (catalogues DSL UI) ----- */

static int list_widgets(const DocOpts* opts) {
    const OwlControlKindInfo* table = owl_control_kinds_table();
    int n_widgets = 0, n_blocks = 0;
    for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
        if (k->is_widget) n_widgets++; else n_blocks++;
    }

    if (opts->json) {
        printf("{\n  \"widgets\": [\n");
        int first = 1;
        for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
            if (!k->is_widget) continue;
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"name\": ");
            json_write_cstr(k->nameFR);
            printf(", \"name_ascii\": ");
            json_write_cstr(k->nameAscii ? k->nameAscii : k->nameFR);
            printf(", \"name_en\": ");
            json_write_cstr(k->nameEN);
            printf(", \"default_property\": ");
            json_write_cstr(k->defaultProperty);
            printf(" }");
        }
        printf("\n  ],\n  \"sub_blocks\": [\n");
        first = 1;
        for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
            if (k->is_widget) continue;
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"name\": ");
            json_write_cstr(k->nameFR);
            printf(", \"name_ascii\": ");
            json_write_cstr(k->nameAscii ? k->nameAscii : k->nameFR);
            printf(", \"name_en\": ");
            json_write_cstr(k->nameEN);
            printf(" }");
        }
        printf("\n  ],\n  \"widgets_count\": %d,\n  \"sub_blocks_count\": %d\n}\n",
               n_widgets, n_blocks);
        return 0;
    }

    printf("%sChamps DSL%s %s(%d champs + %d sous-blocs)%s\n\n",
           C_SECTION, C_RESET, C_DIM, n_widgets, n_blocks, C_RESET);

    printf("%s── Champs ──%s\n", C_DIM, C_RESET);
    for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
        if (!k->is_widget) continue;
        printf("  %s", C_TYPE);
        print_padded_utf8(k->nameFR, 16);
        printf("%s", C_RESET);
        if (k->nameEN) {
            printf(" %s(EN: %s)%s", C_DIM, k->nameEN, C_RESET);
        }
        if (k->defaultProperty) {
            printf(" %s..%s%s", C_DIM, k->defaultProperty, C_RESET);
        }
        printf("\n");
    }
    if (n_blocks > 0) {
        printf("\n%s── Sous-blocs DSL ──%s\n", C_DIM, C_RESET);
        for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
            if (k->is_widget) continue;
            printf("  %s", C_TYPE);
            print_padded_utf8(k->nameFR, 16);
            printf("%s", C_RESET);
            if (k->nameEN) {
                printf(" %s(EN: %s)%s", C_DIM, k->nameEN, C_RESET);
            }
            printf("\n");
        }
    }
    printf("\nDétail : %sowl explain%s %s<champ>%s — props et events : %sowl list props%s / %sowl list events%s\n",
           C_NATIVE, C_RESET, C_TYPE, C_RESET, C_CMD, C_RESET, C_CMD, C_RESET);
    return 0;
}

/* Iterate kinds (widget + Etat catalog parallèle). Si cat_filter != NULL,
 * ne montre que ce widget (match case/accent-insensible sur nameFR/Ascii/EN). */
/* Invariant 2 : `Fenêtre`, `FENÊTRE` et `Fenetre` designent le meme champ,
 * comme `httpRequete` vaut `httpRequête` au runtime. strcasecmp ne replie que
 * l'ASCII — il laissait passer la forme minuscule accentuee par coincidence
 * d'octets, et rejetait la majuscule accentuee. */
static bool match_widget_filter(const OwlControlKindInfo* k, const char* filter) {
    if (!filter) return true;
    if (k->nameFR && owl_category_eq_ci(k->nameFR, filter)) return true;
    if (k->nameAscii && owl_category_eq_ci(k->nameAscii, filter)) return true;
    if (k->nameEN && owl_category_eq_ci(k->nameEN, filter)) return true;
    return false;
}

static int list_events(const DocOpts* opts, const char* cat_filter) {
    const OwlControlKindInfo* table = owl_control_kinds_table();
    int total = 0;

    if (opts->json) {
        printf("{\n  \"events\": [\n");
        int first = 1;
        /* Fenêtre : catalogue lifecycle parallèle (hors OwlControlKind, sans
         * payload type). Contient le cycle de vie (FinInitialisation,
         * Fermeture, ...) ET les events clavier niveau fenêtre
         * (surToucheAppuyée/Relâchée). Listé en tête comme conteneur
         * top-level.  : sans ça, la Fenêtre était invisible dans
         * `list events` alors que ses events clavier marchent au runtime. */
        if (!cat_filter || owl_category_eq_ci(cat_filter, "Fenetre")) {
            int ne = owl_window_lifecycle_catalog_count();
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_window_lifecycle_catalog_at(i);
                if (!ev || !ev->canonical) continue;
                if (!first) printf(",\n");
                first = 0;
                printf("    { \"widget\": \"Fen\xc3\xaatre\", \"name\": ");
                json_write_cstr(ev->canonical);
                printf(", \"name_en\": ");
                json_write_cstr(ev->nameEN);
                printf(", \"params\": %d }", ev->paramCount);
                total++;
            }
        }
        for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
            if (!match_widget_filter(k, cat_filter)) continue;
            OwlControlKind kind;
            if (!owl_control_kind_of_info(k, &kind)) continue;
            int ne = owl_event_catalog_count(kind);
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_event_catalog_at(kind, i);
                if (!first) printf(",\n");
                first = 0;
                printf("    { \"widget\": ");
                json_write_cstr(k->nameFR);
                printf(", \"name\": ");
                json_write_cstr(ev->canonical);
                printf(", \"name_en\": ");
                json_write_cstr(ev->nameEN);
                printf(", \"params\": %d }", ev->paramCount);
                total++;
            }
        }
        /* Etat events (catalogue parallèle, attaché conceptuellement à Etat). */
        if (!cat_filter || owl_category_eq_ci(cat_filter, "Etat")) {
            int ne = owl_etat_event_catalog_count();
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_etat_event_catalog_at(i);
                if (!first) printf(",\n");
                first = 0;
                printf("    { \"widget\": \"État\", \"name\": ");
                json_write_cstr(ev->canonical);
                printf(", \"name_en\": ");
                json_write_cstr(ev->nameEN);
                printf(", \"params\": %d }", ev->paramCount);
                total++;
            }
            int nsub = owl_etat_subcontrol_event_catalog_count();
            for (int i = 0; i < nsub; i++) {
                const OwlEventDef* ev = owl_etat_subcontrol_event_catalog_at(i);
                if (!first) printf(",\n");
                first = 0;
                printf("    { \"widget\": \"État (sous-contrôle)\", \"name\": ");
                json_write_cstr(ev->canonical);
                printf(", \"name_en\": ");
                json_write_cstr(ev->nameEN);
                printf(", \"params\": %d }", ev->paramCount);
                total++;
            }
        }
        printf("\n  ]");
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_EVENTS, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_EVENTS, cat_filter);
        printf(",\n  \"count\": %d\n}\n", total);
        return 0;
    }

    if (cat_filter) {
        printf("%sEvents — champ %s%s%s\n\n",
               C_SECTION, C_TYPE, cat_filter, C_RESET);
    } else {
        printf("%sEvents DSL%s %s(tous champs confondus)%s\n\n",
               C_SECTION, C_RESET, C_DIM, C_RESET);
    }

    /* Fenêtre : catalogue lifecycle parallèle (cf. bloc JSON ci-dessus).
     * Listée en tête car c'est le conteneur top-level. Expose le cycle de
     * vie ET les events clavier niveau fenêtre — c'est la voie canon pour
     * capter le clavier sans champ de saisie (cas d'un canvas de jeu). */
    if (!cat_filter || owl_category_eq_ci(cat_filter, "Fenetre")) {
        int ne = owl_window_lifecycle_catalog_count();
        if (ne > 0) {
            printf("%s── Fen\xc3\xaatre ──%s %s(%d event%s)%s\n",
                   C_DIM, C_RESET, C_DIM, ne, ne > 1 ? "s" : "", C_RESET);
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_window_lifecycle_catalog_at(i);
                if (!ev || !ev->canonical) continue;
                printf("  %s", C_NATIVE);
                print_padded_utf8(ev->canonical, 24);
                printf("%s", C_RESET);
                if (ev->nameEN) printf(" %s(EN: %s)%s", C_DIM, ev->nameEN, C_RESET);
                if (ev->paramCount > 0) {
                    printf(" %s[%d param%s]%s",
                           C_DIM, ev->paramCount, ev->paramCount > 1 ? "s" : "", C_RESET);
                }
                printf("\n");
                total++;
            }
            printf("\n");
        }
    }

    for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
        if (!match_widget_filter(k, cat_filter)) continue;
        OwlControlKind kind;
        if (!owl_control_kind_of_info(k, &kind)) continue;
        int ne = owl_event_catalog_count(kind);
        if (ne == 0) continue;
        printf("%s── %s ──%s %s(%d event%s)%s\n",
               C_DIM, k->nameFR, C_RESET,
               C_DIM, ne, ne > 1 ? "s" : "", C_RESET);
        for (int i = 0; i < ne; i++) {
            const OwlEventDef* ev = owl_event_catalog_at(kind, i);
            printf("  %s", C_NATIVE);
            print_padded_utf8(ev->canonical, 24);
            printf("%s", C_RESET);
            if (ev->nameEN) printf(" %s(EN: %s)%s", C_DIM, ev->nameEN, C_RESET);
            if (ev->paramCount > 0) {
                printf(" %s[%d param%s]%s",
                       C_DIM, ev->paramCount, ev->paramCount > 1 ? "s" : "", C_RESET);
            }
            printf("\n");
            total++;
        }
        printf("\n");
    }
    /* Etat events. */
    if (!cat_filter || owl_category_eq_ci(cat_filter, "Etat")) {
        int ne = owl_etat_event_catalog_count();
        if (ne > 0) {
            printf("%s── État ──%s %s(%d event%s)%s\n",
                   C_DIM, C_RESET, C_DIM, ne, ne > 1 ? "s" : "", C_RESET);
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_etat_event_catalog_at(i);
                printf("  %s", C_NATIVE);
                print_padded_utf8(ev->canonical, 24);
                printf("%s", C_RESET);
                if (ev->nameEN) printf(" %s(EN: %s)%s", C_DIM, ev->nameEN, C_RESET);
                if (ev->paramCount > 0) {
                    printf(" %s[%d param%s]%s",
                           C_DIM, ev->paramCount, ev->paramCount > 1 ? "s" : "", C_RESET);
                }
                printf("\n");
                total++;
            }
            printf("\n");
        }
        int nsub = owl_etat_subcontrol_event_catalog_count();
        if (nsub > 0) {
            printf("%s── État (sous-contrôle) ──%s %s(%d event%s)%s\n",
                   C_DIM, C_RESET, C_DIM, nsub, nsub > 1 ? "s" : "", C_RESET);
            for (int i = 0; i < nsub; i++) {
                const OwlEventDef* ev = owl_etat_subcontrol_event_catalog_at(i);
                printf("  %s", C_NATIVE);
                print_padded_utf8(ev->canonical, 24);
                printf("%s", C_RESET);
                if (ev->nameEN) printf(" %s(EN: %s)%s", C_DIM, ev->nameEN, C_RESET);
                printf("\n");
                total++;
            }
            printf("\n");
        }
    }

    if (cat_filter && !cat_domain_contains(CAT_DOMAIN_EVENTS, cat_filter)) {
        print_unknown_cat(opts, CAT_DOMAIN_EVENTS, cat_filter);
    } else if (total == 0) {
        printf("  %s(aucun event)%s\n", C_DIM, C_RESET);
    } else {
        printf("%s%d event(s) au total.%s\n", C_DIM, total, C_RESET);
    }
    return 0;
}

static int list_props(const DocOpts* opts, const char* cat_filter) {
    int total = owl_dsl_props_count();
    int shown = 0;

    if (opts->json) {
        printf("{\n  \"props\": [\n");
        int first = 1;
        for (int i = 0; i < total; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (cat_filter && !owl_category_eq_ci(e->kind, cat_filter)) continue;
            const OwlControlPropDef* d = e->def;
            if (!first) printf(",\n");
            first = 0;
            printf("    { \"widget\": ");
            json_write_cstr(props_kind_display(e->kind));
            printf(", \"name\": ");
            json_write_cstr(d->nameFR);
            printf(", \"name_ascii\": ");
            json_write_cstr(d->nameAscii ? d->nameAscii : d->nameFR);
            printf(", \"name_en\": ");
            json_write_cstr(d->nameEN);
            printf(", \"type\": ");
            json_write_cstr(prop_type_name_lang(d->type, opts->lang));
            printf(", \"enum\": ");
            json_write_cstr(symbol_lang(d->enum_name, opts->lang));
            if (d->object_type) {
                printf(", \"object_type\": ");
                json_write_cstr(d->object_type);
            }
            printf(" }");
            shown++;
        }
        printf("\n  ]");
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_PROPS, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_PROPS, cat_filter);
        printf(",\n  \"count\": %d\n}\n", shown);
        return 0;
    }

    if (cat_filter) {
        printf("%sPropriétés DSL — champ %s%s%s\n\n",
               C_SECTION, C_TYPE, cat_filter, C_RESET);
    } else {
        printf("%sPropriétés DSL%s %s(%d au total, groupées par champ)%s\n\n",
               C_SECTION, C_RESET, C_DIM, total, C_RESET);
    }

    /* Collecte ordre rencontre des kinds. */
    const char* kinds[32]; int nkinds = 0;
    for (int i = 0; i < total; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (cat_filter && !owl_category_eq_ci(e->kind, cat_filter)) continue;
        int seen = 0;
        for (int j = 0; j < nkinds; j++) if (strcmp(kinds[j], e->kind) == 0) { seen = 1; break; }
        if (!seen && nkinds < 32) kinds[nkinds++] = e->kind;
    }
    for (int ki = 0; ki < nkinds; ki++) {
        int n_in_kind = 0;
        for (int i = 0; i < total; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (strcmp(e->kind, kinds[ki]) != 0) continue;
            n_in_kind++;
        }
        printf("%s── %s ──%s %s(%d prop%s)%s\n",
               C_DIM, props_kind_display(kinds[ki]), C_RESET,
               C_DIM, n_in_kind, n_in_kind > 1 ? "s" : "", C_RESET);
        for (int i = 0; i < total; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (strcmp(e->kind, kinds[ki]) != 0) continue;
            const OwlControlPropDef* d = e->def;
            printf("  %s", C_CONST);
            print_padded_utf8(d->nameFR, 22);
            printf("%s %s%s%s",
                   C_RESET, C_DIM,
                   prop_type_name_lang(d->type, opts->lang), C_RESET);
            if (d->type == OWL_PROP_TYPE_ENUM && d->enum_name) {
                printf(" %s(%s)%s", C_DIM, symbol_lang(d->enum_name, opts->lang), C_RESET);
            }
            if (d->type == OWL_PROP_TYPE_OBJECT && d->object_type) {
                printf(" %s(%s)%s", C_DIM, d->object_type, C_RESET);
            }
            if (d->nameEN) printf(" %s[EN: %s]%s", C_DIM, d->nameEN, C_RESET);
            printf("\n");
            shown++;
        }
        printf("\n");
    }

    if (shown == 0) {
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_PROPS, cat_filter)) {
            print_unknown_cat(opts, CAT_DOMAIN_PROPS, cat_filter);
        } else if (cat_filter) {
            printf("  %s(aucune propriété pour ce champ)%s\n", C_DIM, C_RESET);
        } else {
            printf("  %s(catalogue vide)%s\n", C_DIM, C_RESET);
        }
    } else {
        printf("%s%d propriété(s) affichée(s).%s\n", C_DIM, shown, C_RESET);
    }
    return 0;
}

static int list_globalvars(const DocOpts* opts) {
    int total = owl_globalvar_catalog_count();

    if (opts->json) {
        printf("{\n  \"globalvars\": [\n");
        for (int i = 0; i < total; i++) {
            const OwlGlobalVarDef* g = owl_globalvar_catalog_at(i);
            printf("    { \"name\": ");        json_write_cstr(g->canonical);
            printf(", \"name_ascii\": ");      json_write_cstr(g->nameAscii);
            printf(", \"name_en\": ");         json_write_cstr(g->nameEN);
            printf(", \"type\": ");            json_write_cstr(g->type);
            printf(", \"read_only\": %s }%s\n",
                   g->readOnly ? "true" : "false",
                   i + 1 < total ? "," : "");
        }
        printf("  ],\n  \"count\": %d\n}\n", total);
        return 0;
    }

    printf("%sVariables globales prédéfinies%s %s(%d)%s\n\n",
           C_SECTION, C_RESET, C_DIM, total, C_RESET);
    for (int i = 0; i < total; i++) {
        const OwlGlobalVarDef* g = owl_globalvar_catalog_at(i);
        printf("  %s", C_NATIVE);
        print_padded_utf8(g->canonical, 20);
        printf("%s %s%s%s", C_RESET,
               C_TYPE, g->type ? g->type : "?", C_RESET);
        if (g->readOnly) printf(" %s(ro)%s", C_DIM, C_RESET);
        printf("\n");
    }
    printf("\nDétail : %sowl explain%s %s<nom>%s\n",
           C_NATIVE, C_RESET, C_TYPE, C_RESET);
    return 0;
}

/* list_limits : enumere les limites compile-time exposees par
 * owl_limits.def. Supporte --json, --lang, --cat <filter>. cf. . */
static int list_limits(const DocOpts* opts, const char* cat_filter) {
    int total = owl_registry_limit_count();
    int shown = 0;

    if (opts->json) {
        printf("{\n  \"limits\": [\n");
        int first = 1;
        for (int i = 0; i < total; i++) {
            const OwlLimitDef* L = owl_registry_limit_at(i);
            if (!L) continue;
            if (cat_filter && (!L->category ||
                               strcmp(L->category, cat_filter) != 0)) continue;
            if (!first) printf(",\n");
            first = 0;
            const char* desc = (opts->lang == DOC_LANG_EN && L->descEN && *L->descEN)
                                ? L->descEN : L->descFR;
            printf("    {\n      \"name\": ");      json_write_cstr(L->name);
            printf(",\n      \"value\": %d", L->value);
            printf(",\n      \"category\": ");      json_write_cstr(L->category);
            printf(",\n      \"desc\": ");          json_write_cstr(desc);
            printf("\n    }");
            shown++;
        }
        printf("\n  ]");
        if (cat_filter && !cat_domain_contains(CAT_DOMAIN_LIMITS, cat_filter))
            json_emit_unknown_cat(CAT_DOMAIN_LIMITS, cat_filter);
        printf(",\n  \"count\": %d\n}\n", shown);
        return 0;
    }

    if (cat_filter) {
        printf("%s%s %s%s%s\n",
               C_SECTION,
               dl(opts->lang, "Limites compile-time — catégorie",
                              "Compile-time limits — category"),
               C_CONST,
               owl_category_display(cat_filter, opts->lang == DOC_LANG_EN),
               C_RESET);
    } else {
        printf("%s%s%s %s(%d %s)%s\n",
               C_SECTION,
               dl(opts->lang, "Limites compile-time du compilateur OWL",
                              "Compile-time limits of the OWL compiler"),
               C_RESET, C_DIM, total,
               dl(opts->lang, "au total", "total"), C_RESET);
    }
    printf("\n");

    /* Largeur max nom pour alignement (ascii pur — strlen suffit). */
    int wmax = 0;
    for (int i = 0; i < total; i++) {
        const OwlLimitDef* L = owl_registry_limit_at(i);
        if (!L) continue;
        if (cat_filter && (!L->category ||
                           strcmp(L->category, cat_filter) != 0)) continue;
        int w = (int)strlen(L->name);
        if (w > wmax) wmax = w;
    }
    if (wmax > 32) wmax = 32;
    if (wmax < 16) wmax = 16;

    for (int i = 0; i < total; i++) {
        const OwlLimitDef* L = owl_registry_limit_at(i);
        if (!L) continue;
        if (cat_filter && (!L->category ||
                           strcmp(L->category, cat_filter) != 0)) continue;
        const char* desc = (opts->lang == DOC_LANG_EN && L->descEN && *L->descEN)
                             ? L->descEN : L->descFR;
        printf("  %s%-*s%s  %s= %-5d%s  %s%s%s\n",
               C_CONST, wmax, L->name, C_RESET,
               C_NATIVE, L->value, C_RESET,
               C_DIM, desc ? desc : "", C_RESET);
        shown++;
    }
    if (cat_filter && !cat_domain_contains(CAT_DOMAIN_LIMITS, cat_filter)) {
        print_unknown_cat(opts, CAT_DOMAIN_LIMITS, cat_filter);
    } else if (cat_filter && shown == 0) {
        printf("  %s%s%s\n", C_DIM,
               dl(opts->lang, "(aucune limite dans cette catégorie)",
                              "(no limit in this category)"), C_RESET);
    }
    printf("\n%s%d %s%s ", C_DIM, shown,
           dl(opts->lang, "résultat(s).", "result(s)."), C_RESET);
    printf("%s %sowl explain%s %s<MAX_*>%s\n",
           dl(opts->lang, "Détail :", "Details:"),
           C_NATIVE, C_RESET, C_TYPE, C_RESET);
    return 0;
}

static void print_list_help(void) {
    printf("%sowl list%s %s<quoi>%s — listes filtrables des catalogues\n\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("%sSous-commandes%s\n", C_SECTION, C_RESET);
    printf("  %sowl list categories%s        familles avec comptage\n",
           C_CMD, C_RESET);
    printf("  %sowl list natives%s %s[--cat X]%s  fonctions natives, filtre optionnel\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl list types%s             types composés (httpRequête, Cadre, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list constants%s         constantes nommées (httpGet, maskDate*, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list keywords%s          mots-clés du langage (SI, POUR, SELON, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list operators%s         opérateurs symboliques ([~~, ??*, ~=, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list controls%s          champs DSL (Fenêtre, Bouton, Saisie, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list events%s %s[--cat X]%s   events champ (surClic, surChangement, …)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl list props%s %s[--cat X]%s    propriétés DSL (groupées par champ)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("  %sowl list globalvars%s        variables globales prédéfinies (ErreurDétectée, …)\n",
           C_CMD, C_RESET);
    printf("  %sowl list limits%s %s[--cat X]%s   limites compile-time (MAX_USER_TYPES, …)\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
}

int cli_doc_list(int argc, char** argv) {
    /* argv[0] = "list" */
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "list")) return 64;

    if (opts.pos_argc == 0) {
        print_list_help();
        return 0;
    }

    const char* what = opts.pos_argv[0];

    /* Les quatre sous-commandes qui n'ont pas de familles posent le garde
     * avec leur nom canonique : le garde ne fait rien si la sous-commande en
     * gagne un jour. Une sous-commande future qui accepterait `--cat` sans
     * s'en servir rougit dans cli_doc_test.c, qui balaye les onze. */
    if (strcmp(what, "categories") == 0 || strcmp(what, "category") == 0 ||
        strcmp(what, "cats") == 0) {
        if (reject_cat_unsupported(&opts, "categories")) return 1;
        return list_categories(&opts);
    }
    if (strcmp(what, "natives") == 0 || strcmp(what, "functions") == 0 ||
        strcmp(what, "fn") == 0) {
        /* Pour `list natives`, --cat filtre par catégorie de registry
         * (HTTP, Chaîne, etc.) — sens différent du --cat de explain. */
        return list_natives(&opts, opts.cat);
    }
    if (strcmp(what, "types") == 0 || strcmp(what, "type") == 0) {
        return list_types(&opts, opts.cat);
    }
    /*  : les enums ne sont plus une categorie navigable (concept
     * interne, accessible via explain <nom> et via les references des
     * constantes / parametres). Pas de branche `list enums` -> retombe sur
     * le message "sous-commande inconnue" plus bas. */
    if (strcmp(what, "constants") == 0 || strcmp(what, "consts") == 0 ||
        strcmp(what, "constantes") == 0) {
        return list_constants(&opts, opts.cat);
    }
    if (strcmp(what, "keywords") == 0 || strcmp(what, "keyword") == 0 ||
        strcmp(what, "mots-clés") == 0 || strcmp(what, "mots-cles") == 0) {
        return list_keywords(&opts, opts.cat);
    }
    if (strcmp(what, "operators") == 0 || strcmp(what, "operator") == 0 ||
        strcmp(what, "opérateurs") == 0 || strcmp(what, "operateurs") == 0) {
        if (reject_cat_unsupported(&opts, "operators")) return 1;
        return list_operators(&opts);
    }
    if (strcmp(what, "controls") == 0 || strcmp(what, "control") == 0 ||
        strcmp(what, "champs")   == 0 || strcmp(what, "champ")   == 0) {
        if (reject_cat_unsupported(&opts, "controls")) return 1;
        return list_widgets(&opts);
    }
    if (strcmp(what, "events") == 0 || strcmp(what, "event") == 0 ||
        strcmp(what, "évents") == 0 || strcmp(what, "evenements") == 0 ||
        strcmp(what, "événements") == 0) {
        return list_events(&opts, opts.cat);
    }
    if (strcmp(what, "props") == 0 || strcmp(what, "prop") == 0 ||
        strcmp(what, "properties") == 0 || strcmp(what, "propriétés") == 0 ||
        strcmp(what, "proprietes") == 0) {
        return list_props(&opts, opts.cat);
    }
    if (strcmp(what, "globalvars") == 0 || strcmp(what, "globalvar") == 0 ||
        strcmp(what, "variables") == 0 || strcmp(what, "variable") == 0) {
        if (reject_cat_unsupported(&opts, "globalvars")) return 1;
        return list_globalvars(&opts);
    }
    if (strcmp(what, "limits") == 0 || strcmp(what, "limit") == 0 ||
        strcmp(what, "limites") == 0 || strcmp(what, "limite") == 0) {
        return list_limits(&opts, opts.cat);
    }

    fprintf(stderr, "owl list : sous-commande '%s' inconnue.\n", what);
    fprintf(stderr, "  Tapez 'owl list' pour la liste des sous-commandes.\n");
    return 1;
}

/* ==================================================================
 * Parser de scénarios embarqués
 *
 * Extrait depuis chaque tests/scenarios/<cat>/test_*.owl :
 *   - @sujet  (ligne `// @sujet : <Nom>`)
 *   - @résumé (ligne `// @résumé : <description>`)
 *   - procédures (PROCEDURE [PRIVE|PRIVEE|PRIVÉE] scenario_xxx())
 *     avec leur commentaire d'en-tête + body brut
 *
 * Stocké en cache mémoire (lazy init au premier accès). Le parser est
 * tolérant : il accepte les fixtures (avantChaqueTest, etc.) et ignore
 * tout ce qui n'est pas une PROCEDURE.
 * ================================================================== */

#define MAX_SCENARIOS    2048  /*  : ancien 256 dépassé (568 scenarios
                                  embedded au moment de ). 2048 = ~3.5x
                                  marge, ~600 KB statique acceptable. */
#define MAX_PROC_PER_SCN 32

typedef struct {
    char         name[128];        /* nom de la procédure */
    const char*  body_start;       /* pointeur dans le blob source */
    long         body_len;          /* longueur du corps incluant la déclaration */
    const char*  doc_start;        /* lignes // au-dessus, ou NULL */
    long         doc_len;
    int          is_private;        /* 1 si PROCEDURE PRIVE/PRIVEE/PRIVÉE */
    int          is_fixture;        /* 1 si avantChaqueTest / apresChaqueTest / etc. */
} ParsedProcedure;

typedef struct {
    char            sujet[128];    /* @sujet (vide si pas trouvé) */
    char            resume[512];   /* @résumé */
    const OwlEmbeddedDoc* source;  /* backref */
    ParsedProcedure procedures[MAX_PROC_PER_SCN];
    int             procedure_count;
} ParsedScenario;

static ParsedScenario g_scenarios[MAX_SCENARIOS];
static int            g_scenarios_count = 0;
static int            g_scenarios_initialized = 0;

/* --- Helpers --------------------------------------------------- */

/* Lit la ligne courante à partir de p, retourne sa longueur (sans \n).
 * Avance p à la ligne suivante via *next. */
static long read_line(const char* p, const char* end, const char** next) {
    const char* q = p;
    while (q < end && *q != '\n') q++;
    long len = q - p;
    *next = (q < end) ? q + 1 : end;
    /* Trim \r en fin de ligne (CRLF tolerance) */
    while (len > 0 && p[len - 1] == '\r') len--;
    return len;
}

/* Skip whitespace en début de ligne, retourne le premier char non-blanc
 * + sa longueur effective. */
static const char* line_ltrim(const char* p, long* len) {
    while (*len > 0 && (*p == ' ' || *p == '\t')) { p++; (*len)--; }
    return p;
}

static int line_starts_with(const char* p, long len, const char* prefix) {
    long plen = (long)strlen(prefix);
    if (plen > len) return 0;
    return strncmp(p, prefix, (size_t)plen) == 0;
}

/* Récupère la valeur après "@sujet :" ou "@résumé :" dans un commentaire.
 * Retourne 1 si trouvé, 0 sinon. */
static int extract_meta(const char* line, long len,
                        const char* tag, char* out, size_t out_size) {
    const char* p = line;
    p = line_ltrim(p, &len);
    if (!line_starts_with(p, len, "//")) return 0;
    p += 2; len -= 2;
    p = line_ltrim(p, &len);
    long tag_len = (long)strlen(tag);
    if (len < tag_len) return 0;
    if (strncmp(p, tag, (size_t)tag_len) != 0) return 0;
    p += tag_len; len -= tag_len;
    p = line_ltrim(p, &len);
    if (len > 0 && *p == ':') { p++; len--; }
    p = line_ltrim(p, &len);
    /* Copy out */
    size_t n = (size_t)len;
    if (n + 1 > out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    /* Trim trailing whitespace */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return 1;
}

/* Delta de profondeur de blocs apporté par une ligne de scénario : +1 si elle
 * ouvre un bloc fermé par FIN, -1 si elle le ferme, 0 sinon.
 *
 * La règle n'est pas réimplémentée ici : elle vient de `prescan_depth_delta`
 * (prescan.h), source de vérité unique depuis . Elle seule fait le peek
 * qui distingue les deux formes légales SANS FIN — `SI c ALORS instr` et
 * `CONSTANTE K = 3` — de leurs formes bloc. Un `SI` mono-ligne compté comme
 * ouvrant empêche la profondeur de retomber à zéro : le corps du scénario avale
 * alors les déclarations suivantes.
 *
 * Seul le PREMIER token de la ligne est jugé, comme le fait le détecteur GO
 * (main.c) pour `AVEC` : c'est la POSITION qui discrimine. `SINON SI c ALORS`
 * est une clause elif (un seul FIN pour toute la structure) et non un bloc
 * supplémentaire ; `p est une Procedure` est une annotation de type, pas un
 * handler. Juger tous les tokens de la ligne compterait les deux en trop.
 *
 * `AVEC` reste traité ici : `prescan_depth_delta` l'exclut volontairement (le
 * même mot-clé sert de modificateur au milieu d'une instruction, `POUR TOUT v
 * DE t AVEC <cond>`), son discriminant est justement la position en tête de
 * ligne, connue d'ici seulement.
 *
 * La ligne est recopiée dans un tampon zero-terminé : les blobs de scénario
 * embarqués ne le sont pas (cf. embedded_docs.h) et le Scanner s'arrête sur
 * '\0'. Le terminateur tient lieu de fin de ligne — `prescan_depth_delta`
 * traite TOKEN_EOF comme TOKEN_NEWLINE dans son peek. */
int cli_doc_scenario_line_depth_delta(const char* line, long len) {
    char buf[1024];
    const char* p = line_ltrim(line, &len);
    if (len <= 0) return 0;
    size_t n = (size_t)len;
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';

    Scanner sc;
    scanner_init(&sc, buf);
    Token tok = scanner_scan_token(&sc);
    if (tok.type == TOKEN_AVEC) return +1;
    return prescan_depth_delta(tok, &sc);
}

/* Mots-clés d'introduction d'une déclaration de procédure. Une seule entrée
 * par mot : la comparaison passe par `word_eq_ci` (casse + accent
 * insensibles, règle 6.1/6.2), donc `PROCÉDURE`, `Procedure` et `procedure`
 * sont couverts par « PROCEDURE ». Miroir de la table `keywords` de
 * scanner.c (procedure / fonction / function). */
static const char* SCN_PROC_KEYS[] = { "PROCEDURE", "FONCTION", "FUNCTION", NULL };

/* Modificateurs tolérés entre le mot-clé et le nom déclaré. Grammaire canon :
 * `PROCEDURE [<accès>] [GLOBALE] [ABSTRAITE] <Nom>(...)`. `restricted` = 1
 * quand la visibilité sort le scénario de la vitrine publique.
 *
 * La table couvre les DEUX graphies de chaque accès : le canon standalone
 * masculin (`PRIVÉ`, marqueur de bloc) et le canon inline féminin accordé
 * avec « procédure » (`PRIVÉE`, `PUBLIQUE`, `PROTÉGÉE`) que le formatter
 * impose sur une déclaration de méthode, plus les formes EN invariables.
 * Miroir strict de scanner.c : les mots absents du scanner (RESTREINT,
 * VIRTUELLE, INTERNE) ne sont PAS des mots-clés OWL et n'ont rien à faire
 * ici — une déclaration qui les emploie ne compile pas. */
typedef struct { const char* kw; int restricted; } ScnProcMod;
static const ScnProcMod SCN_PROC_MODS[] = {
    { "PUBLIC",    0 }, { "PUBLIQUE",  0 },
    { "PRIVE",     1 }, { "PRIVEE",    1 }, { "PRIVATE",   1 },
    { "PROTEGE",   1 }, { "PROTEGEE",  1 }, { "PROTECTED", 1 },
    { "GLOBALE",   0 }, { "ABSTRAITE", 0 }, { "ABSTRACT",  0 },
    { NULL,        0 }
};

/* Longueur du mot commençant en `p`. Les octets ≥ 0x80 comptent comme
 * caractères de mot : un identifiant OWL peut être accentué
 * (`aprèsChaqueTest`), et le tronquer au 1er accent produit un faux nom. */
static long scn_word_len(const char* p, long len) {
    long n = 0;
    while (n < len) {
        unsigned char c = (unsigned char)p[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) n++;
        else break;
    }
    return n;
}

/* Détecte une déclaration PROCEDURE et en extrait le nom + le flag de
 * visibilité restreinte. Retourne 1 si trouvé, 0 sinon.
 *
 * Non statique : `--self-test-cli` sonde directement cette fonction pour
 * verrouiller toutes les graphies canon, y compris celles qu'aucun scénario
 * du corpus embarqué n'emploie. */
int cli_doc_scenario_parse_decl(const char* line, long len,
                                char* name_out, size_t name_size,
                                int* is_private_out) {
    const char* p = line_ltrim(line, &len);
    *is_private_out = 0;

    /* Mot-clé d'introduction, suivi d'un blanc. */
    long wlen = scn_word_len(p, len);
    if (wlen == 0 || !word_in_list(p, wlen, SCN_PROC_KEYS)) return 0;
    p += wlen; len -= wlen;
    if (len <= 0 || (*p != ' ' && *p != '\t')) return 0;
    p = line_ltrim(p, &len);

    /* Modificateurs optionnels, dans n'importe quel ordre. */
    for (;;) {
        wlen = scn_word_len(p, len);
        if (wlen == 0) break;
        const ScnProcMod* hit = NULL;
        for (int k = 0; SCN_PROC_MODS[k].kw; k++) {
            if (word_eq_ci(p, wlen, SCN_PROC_MODS[k].kw)) {
                hit = &SCN_PROC_MODS[k];
                break;
            }
        }
        if (!hit) break;
        if (hit->restricted) *is_private_out = 1;
        p += wlen; len -= wlen;
        p = line_ltrim(p, &len);
    }

    /* Nom déclaré : le mot restant, jusqu'à '(' ou blanc. */
    wlen = scn_word_len(p, len);
    if (wlen == 0) return 0;
    size_t n = (size_t)wlen;
    if (n + 1 > name_size) n = name_size - 1;
    memcpy(name_out, p, n);
    name_out[n] = '\0';
    return 1;
}

/* Fixtures du runner : casse + accent insensibles comme tout identifiant
 * OWL (`aprèsChaqueTest` == `apresChaqueTest`). */
static int is_fixture_name(const char* name) {
    static const char* NAMES[] = {
        "avantToutTest",   "beforeAllTests",
        "apresToutTest",   "afterAllTests",
        "avantChaqueTest", "beforeEachTest",
        "apresChaqueTest", "afterEachTest",
        NULL
    };
    return word_in_list(name, (long)strlen(name), NAMES);
}

/* --- Parser principal d'un blob de scénario ------------------- */

static void parse_scenario_blob(const OwlEmbeddedDoc* src, ParsedScenario* out) {
    memset(out, 0, sizeof(*out));
    out->source = src;

    const char* p   = (const char*)src->data;
    const char* end = p + src->len;

    /* Buffer pour le commentaire en cours d'accumulation (lignes
     * commençant par // accumulées au-dessus d'une procédure). */
    const char* doc_start = NULL;
    long        doc_len   = 0;

    while (p < end) {
        const char* next;
        long len = read_line(p, end, &next);
        const char* trimmed = line_ltrim(p, &len);

        /* Métadonnées de header */
        if (extract_meta(p, len, "@sujet", out->sujet, sizeof out->sujet)) {
            p = next; continue;
        }
        if (extract_meta(p, len, "@r\xc3\xa9""sum\xc3\xa9", out->resume, sizeof out->resume)) {
            p = next; continue;
        }
        if (extract_meta(p, len, "@resume", out->resume, sizeof out->resume)) {
            p = next; continue;
        }

        /* Accumulation des lignes de commentaire pour la doc d'une proc. */
        if (line_starts_with(trimmed, len - (trimmed - p), "//") &&
            !line_starts_with(trimmed, len - (trimmed - p), "// @")) {
            if (doc_start == NULL) doc_start = p;
            doc_len = next - doc_start;
            p = next; continue;
        }

        /* Ligne vide : reset doc accumulator. */
        if (len == 0 || (len == 1 && *p == '\r')) {
            /* Ne pas reset si la ligne d'après est aussi un comment.
             * Pour simplicité, on reset. La majorité des scénarios bien
             * formés ont leur doc collée à la proc. */
            doc_start = NULL;
            doc_len = 0;
            p = next; continue;
        }

        /* PROCEDURE ? */
        char name[128];
        int  is_priv = 0;
        if (cli_doc_scenario_parse_decl(p, len, name, sizeof name, &is_priv)) {
            if (out->procedure_count >= MAX_PROC_PER_SCN) { p = next; continue; }
            ParsedProcedure* proc = &out->procedures[out->procedure_count++];

            strncpy(proc->name, name, sizeof(proc->name) - 1);
            proc->name[sizeof(proc->name) - 1] = '\0';
            proc->is_private = is_priv;
            proc->is_fixture = is_fixture_name(name);
            proc->body_start = p;
            proc->doc_start = doc_start;
            proc->doc_len   = doc_len;
            doc_start = NULL; doc_len = 0;

            /* Cherche le FIN matchant : depth=1 dès PROCEDURE. */
            int depth = 1;
            const char* q = next;
            while (q < end && depth > 0) {
                const char* qnext;
                long qlen = read_line(q, end, &qnext);
                depth += cli_doc_scenario_line_depth_delta(q, qlen);
                q = qnext;
            }
            proc->body_len = q - proc->body_start;
            p = q;
            continue;
        }

        /* Tout le reste : reset doc. */
        doc_start = NULL; doc_len = 0;
        p = next;
    }
}

static void ensure_scenarios_parsed(void) {
    if (g_scenarios_initialized) return;
    g_scenarios_initialized = 1;
    int idx = 0;
    for (const OwlEmbeddedDoc* d = EMBEDDED_SCENARIOS;
         d->name && idx < MAX_SCENARIOS; d++) {
        parse_scenario_blob(d, &g_scenarios[idx]);
        idx++;
    }
    g_scenarios_count = idx;
}

/* Matching @sujet : insensible accent et casse. Retourne 1 si le sujet
 * du scénario égale (au sens accent/casse-insensible) le nom recherché. */
static int ascii_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Égalité token terminé/nom : strict, ascii-strict, puis fallback
 * accent/casse-insensible (invariants 6.1/6.2 : un @sujet "httpRequête"
 * matche "HTTPRequete" et réciproquement, via word_eq_ci qui
 * strip_accents + lower). */
static int token_str_eq(const char* tok, long len,
                        const char* name, const char* name_ascii) {
    if (strcmp(tok, name) == 0) return 1;
    if (name_ascii && strcmp(tok, name_ascii) == 0) return 1;
    if (ascii_eq_ci(tok, name)) return 1;
    if (name_ascii && ascii_eq_ci(tok, name_ascii)) return 1;
    if (word_eq_ci(tok, len, name)) return 1;
    if (name_ascii && word_eq_ci(tok, len, name_ascii)) return 1;
    return 0;
}

/* Match un token nettoyé, avec repli sur le suffixe d'une forme pointée
 * `Type.Méthode`. Un @sujet de scénario de type est souvent écrit en forme
 * qualifiée (`Tenseur.TrancheCanaux`) alors que la native est cherchée par
 * son nom de méthode nu (`TrancheCanaux`) — on matche donc aussi la partie
 * après le dernier point. La forme qualifiée complète reste couverte par le
 * match plein. */
static int token_core_matches(const char* tok, long len,
                              const char* name, const char* name_ascii) {
    if (token_str_eq(tok, len, name, name_ascii)) return 1;
    const char* dot = strrchr(tok, '.');
    if (dot && dot != tok && dot[1] != '\0') {
        const char* suf = dot + 1;
        long slen = len - (long)(suf - tok);
        if (slen > 0 && token_str_eq(suf, slen, name, name_ascii)) return 1;
    }
    return 0;
}

/* Compare un token brut (slice [p, p+len)) au nom recherché. Le token est
 * trimmé (whitespace + chevrons tolérés : ` ' " et parenthèse fermante),
 * puis débarrassé d'une éventuelle description libre introduite par un
 * tiret cadratin (`Tenseur.Convolution2D — convolution 2D`). Retourne 1 si
 * match (strict, ascii-strict, accent/casse-insensible, ou suffixe pointé). */
static int sujet_token_matches(const char* p, long len,
                               const char* name, const char* name_ascii) {
    /* Trim leading whitespace + ponctuation typique. */
    while (len > 0 && (*p == ' ' || *p == '\t' || *p == '`' ||
                       *p == '\'' || *p == '"')) {
        p++; len--;
    }
    while (len > 0) {
        char c = p[len - 1];
        if (c == ' ' || c == '\t' || c == '`' || c == '\'' ||
            c == '"' || c == ')') {
            len--;
        } else break;
    }
    if (len <= 0) return 0;
    /* Buffer local pour terminer le token. */
    char tok[160];
    if (len >= (long)sizeof(tok)) len = (long)sizeof(tok) - 1;
    memcpy(tok, p, (size_t)len);
    tok[len] = '\0';
    /* Coupe une description libre introduite par un tiret cadratin `—`
     * (U+2014, octets E2 80 94). Le symbole vit avant le tiret. */
    for (char* q = tok; q[0] && q[1] && q[2]; q++) {
        if ((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x80 &&
            (unsigned char)q[2] == 0x94) {
            *q = '\0';
            len = (long)(q - tok);
            /* Re-trim l'espace laissé avant le tiret. */
            while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) {
                tok[--len] = '\0';
            }
            break;
        }
    }
    if (len <= 0) return 0;
    if (token_core_matches(tok, len, name, name_ascii)) return 1;
    /* Tag composite `Symbole (précision)` : un scénario peut préciser son
     * cas d'usage entre parenthèses (`Tenseur.ProduitMatriciel (Matrice ×
     * Vecteur)`). On matche alors sur le préfixe avant la parenthèse pour
     * que `owl examples <Symbole>` retrouve toute la famille sans perdre
     * la précision dans le tag. */
    char* paren = strstr(tok, " (");
    if (paren && paren > tok) {
        long plen = paren - tok;
        char pref[160];
        memcpy(pref, tok, (size_t)plen);
        pref[plen] = '\0';
        if (token_core_matches(pref, plen, name, name_ascii)) return 1;
    }
    return 0;
}

/* Match un segment (déjà découpé sur virgule). D'abord le segment entier
 * (préserve un @sujet passé tel quel comme argument), puis, s'il contient un
 * slash, chacune de ses sous-formes. Le slash sépare les graphies bilingues
 * ou les variantes (`IoU / SuppressionNonMax`, `Tenseur.X / Tenseur.Y`). */
static int sujet_segment_matches(const char* p, long len,
                                 const char* name, const char* name_ascii) {
    if (sujet_token_matches(p, len, name, name_ascii)) return 1;
    const char* end = p + len;
    int has_slash = 0;
    for (const char* q = p; q < end; q++) {
        if (*q == '/') { has_slash = 1; break; }
    }
    if (!has_slash) return 0;
    const char* s = p;
    while (s < end) {
        const char* slash = s;
        while (slash < end && *slash != '/') slash++;
        if (sujet_token_matches(s, slash - s, name, name_ascii)) return 1;
        if (slash >= end) break;
        s = slash + 1;
    }
    return 0;
}

/* Le `@sujet` d'un scénario peut être :
 *   - un nom canon unique : `@sujet : Applique`
 *   - une liste séparée par virgule pour les scénarios transverses :
 *     `@sujet : ChaîneVersJSON, JSONVersChaîne, JSONSérialise`
 *   - des graphies bilingues/variantes séparées par slash :
 *     `@sujet : IoU / SuppressionNonMax` ou `Tenseur.X / Tenseur.Y`
 *   - une description libre après un tiret cadratin.
 * On match si AU MOINS une des formes (trimmée) égale le nom recherché. */
static int sujet_matches(const ParsedScenario* sc,
                         const char* name, const char* name_ascii) {
    if (!sc->sujet[0]) return 0;
    const char* p = sc->sujet;
    const char* end = p + strlen(p);
    while (p < end) {
        const char* comma = p;
        while (comma < end && *comma != ',') comma++;
        if (sujet_segment_matches(p, comma - p, name, name_ascii)) return 1;
        if (comma >= end) break;
        p = comma + 1;
    }
    return 0;
}

/* ==================================================================
 * Rendering des scénarios
 * ================================================================== */

static void print_doc_block(const char* doc_start, long doc_len) {
    /* Imprime un bloc de commentaires en filtrant les "//" en début. */
    const char* p = doc_start;
    const char* end = p + doc_len;
    while (p < end) {
        const char* next;
        long len = read_line(p, end, &next);
        const char* t = line_ltrim(p, &len);
        if (line_starts_with(t, len, "//")) {
            t += 2; len -= 2;
            t = line_ltrim(t, &len);
        }
        printf("    %s%.*s%s\n", C_DIM, (int)len, t, C_RESET);
        p = next;
    }
}

/* Calcule la plage [out_start, out_start+out_len) du CORPS d'un exemple
 * débarrassé de son wrapper de procédure. Réplique fidèlement la logique
 * `stripProc` du viewer HTML (docs/reference/_owl-doc-template.html) :
 *
 *   1. retire la 1ère ligne si elle commence par PROCEDURE|FONCTION|FUNCTION
 *   2. retire les lignes vides de queue
 *   3. retire la dernière ligne si elle est `FIN`/`END` seul
 *   4. retire les lignes vides de tête
 *
 * Ne pop QUE la dernière ligne en (3) → un FIN de bloc interne (SI/POUR)
 * n'est jamais touché, seul le FIN terminal du wrapper l'est. Le
 * dé-indent par ligne est appliqué au moment du rendu (cf.
 * proc_line_deindent), pas ici — cette fonction ne fait que borner.
 *
 * Non statique : cf. cli_doc_scenario_parse_decl (verrou --self-test-cli). */
void cli_doc_scenario_strip_wrapper(const char* body_start, long body_len,
                                    const char** out_start, long* out_len) {
    const char* start = body_start;
    const char* end   = body_start + body_len;

    /* (1) 1ère ligne PROCEDURE|FONCTION|FUNCTION ? Le mot est isolé par
     * scn_word_len puis comparé casse + accent insensible, ce qui couvre
     * toutes les graphies tolérées (`PROCÉDURE`, `Function`, ...) et donne
     * la frontière de mot gratuitement (`PROCEDUREX` ne matche pas). */
    {
        const char* next;
        long len = read_line(start, end, &next);
        const char* t = line_ltrim(start, &len);
        long wlen = scn_word_len(t, len);
        if (wlen > 0 && word_in_list(t, wlen, SCN_PROC_KEYS)) start = next;
    }

    /* Recalcule les bornes de lignes restantes pour (2)/(3)/(4). On
     * travaille en repérant la dernière ligne via un scan arrière. */

    /* (2) retire les \n de queue jusqu'à la dernière ligne non vide. */
    /* (3) si cette dernière ligne est `FIN`/`END` seul, la retirer. */
    {
        /* Trouve la fin du dernier contenu non-blanc. On scanne depuis
         * `end` vers `start`. */
        const char* q = end;
        /* Retire blancs de queue (espaces, tabs, \n, \r). */
        while (q > start && (q[-1] == '\n' || q[-1] == '\r' ||
                             q[-1] == ' '  || q[-1] == '\t')) q--;
        /* q pointe juste après le dernier char non-blanc. Trouve le
         * début de cette dernière ligne. */
        const char* line_start = q;
        while (line_start > start && line_start[-1] != '\n') line_start--;
        long llen = (long)(q - line_start);
        const char* lt = line_ltrim(line_start, &llen);
        /* trim droite déjà fait (q est sur le dernier non-blanc). */
        if (word_eq_ci(lt, llen, "FIN") || word_eq_ci(lt, llen, "END")) {
            /* C'est `FIN` seul → la dernière ligne (le FIN terminal du
             * wrapper) est retirée. Nouvelle fin = début de cette ligne. */
            end = line_start;
        } else {
            /* Pas de FIN terminal : on garde jusqu'à q (sans blancs de
             * queue superflus, mais on préserve le contenu). */
            end = q;
        }
    }

    /* (4) retire les lignes vides de tête. */
    while (start < end) {
        const char* next;
        long len = read_line(start, end, &next);
        const char* t = line_ltrim(start, &len);
        (void)t;
        if (len != 0) break;       /* ligne non vide → stop */
        start = next;
    }

    if (start > end) start = end;
    *out_start = start;
    *out_len   = (long)(end - start);
}

/* Dé-indente une ligne d'un niveau, façon viewer : si la ligne commence
 * par un tab → retire 1 tab ; sinon retire 1 à 4 espaces de tête.
 * Renvoie les bornes ajustées (le contenu n'est pas copié). */
static void proc_line_deindent(const char* p, long len,
                               const char** out_p, long* out_len) {
    if (len > 0 && p[0] == '\t') {
        *out_p = p + 1;
        *out_len = len - 1;
        return;
    }
    long n = 0;
    while (n < 4 && n < len && p[n] == ' ') n++;
    *out_p = p + n;
    *out_len = len - n;
}

static void print_proc_body(const ParsedProcedure* proc) {
    /* Affiche le body débarrassé du wrapper PROCEDURE/FIN, chaque ligne
     * dé-indentée d'un niveau puis passée au tokenizer OWL — mêmes règles
     * de coloration que les blocs ```owl ``` du markdown. */
    const char* p;
    long n;
    cli_doc_scenario_strip_wrapper(proc->body_start, proc->body_len, &p, &n);
    const char* end = p + n;
    int brace_depth = 0;
    while (p < end) {
        const char* next;
        long len = read_line(p, end, &next);
        const char* dp;
        long dlen;
        proc_line_deindent(p, len, &dp, &dlen);
        printf("    ");
        render_code_line_owl(dp, dlen, &brace_depth);
        putchar('\n');
        p = next;
    }
}

/* Émet en JSON le corps d'un exemple débarrassé de son wrapper et
 * dé-indenté ligne par ligne (consommé par `owl examples --json` → MCP
 * owl_examples). Réutilise les mêmes bornes que le rendu texte pour une
 * cohérence stricte. Écrit la chaîne JSON complète (guillemets inclus). */
static void json_write_proc_body_stripped(const char* body_start,
                                           long body_len) {
    const char* p;
    long n;
    cli_doc_scenario_strip_wrapper(body_start, body_len, &p, &n);
    const char* end = p + n;
    putchar('"');
    int first = 1;
    while (p < end) {
        const char* next;
        long len = read_line(p, end, &next);
        const char* dp;
        long dlen;
        proc_line_deindent(p, len, &dp, &dlen);
        if (!first) fputs("\\n", stdout);
        first = 0;
        for (long i = 0; i < dlen; i++) {
            unsigned char c = (unsigned char)dp[i];
            switch (c) {
                case '"':  fputs("\\\"", stdout); break;
                case '\\': fputs("\\\\", stdout); break;
                case '\t': fputs("\\t",  stdout); break;
                default:
                    if (c < 0x20) printf("\\u%04x", c);
                    else          putchar((char)c);
                    break;
            }
        }
        p = next;
    }
    putchar('"');
}

static void render_scenario_public(const ParsedScenario* sc) {
    /* Affiche les procédures publiques (non-PRIVEE, non-fixture). */
    int shown = 0;
    for (int i = 0; i < sc->procedure_count; i++) {
        const ParsedProcedure* proc = &sc->procedures[i];
        if (proc->is_private || proc->is_fixture) continue;
        if (shown > 0) printf("\n");
        /* Vert foncé canon (entity.name.function) — cohérent avec la
         * coloration des noms de PROCEDURE dans le body. */
        printf("  %s► %s%s\n", C_PROC, proc->name, C_RESET);
        if (proc->doc_start && proc->doc_len > 0) {
            print_doc_block(proc->doc_start, proc->doc_len);
            printf("\n");
        }
        print_proc_body(proc);
        shown++;
    }
}

/* ==================================================================
 * owl explain <symbole>
 * ================================================================== */

/* Forward decl — défini juste après le bloc méthodes. */
static const char* type_to_lang(const char* descFR, const char* descEN, DocLang lang);

/* Pour une native donnée et un type receiver, retourne le binding
 * "préféré" à afficher dans la liste des méthodes fluent. Préférence :
 *
 *   --lang fr : on préfère un methodName qui n'est PAS exactement
 *               l'alias EN canonique de la native (d->nameEN). Sinon,
 *               le premier binding matchant.
 *   --lang en : inverse — on préfère explicitement nameEN s'il est
 *               présent en binding ; sinon le premier.
 *
 * Renvoie NULL si la native n'a aucun binding sur ce receiverType. */
static const OwlMethodBinding* preferred_method_binding(
    const OwlNativeDef* d, const char* type_name, DocLang lang) {
    if (!d || !d->methodBindings) return NULL;
    const OwlMethodBinding* first_match = NULL;
    const OwlMethodBinding* lang_match  = NULL;
    for (const OwlMethodBinding* mb = d->methodBindings;
         mb->receiverType && mb->methodName; mb++) {
        if (!word_eq_ci(mb->receiverType, (long)strlen(mb->receiverType),
                        type_name))
            continue;
        if (!first_match) first_match = mb;
        int is_en = (d->nameEN && strcmp(mb->methodName, d->nameEN) == 0);
        if (lang == DOC_LANG_FR && !is_en && !lang_match) lang_match = mb;
        if (lang == DOC_LANG_EN &&  is_en && !lang_match) lang_match = mb;
    }
    return lang_match ? lang_match : first_match;
}

/* Itère le registry et imprime les méthodes fluent (.NomMéthode) que
 * type_name reçoit en receiver. Une SEULE ligne par native, même si
 * plusieurs alias (FR/EN/court) y existent. Le LLM qui rédige du code
 * a besoin du chaînage `obj.Méthode().Autre()` sans bruit.
 *
 * Comparaison case + accent insensible (cf. invariant 6.1/6.2).
 * Imprime rien si aucune méthode n'est enregistrée pour ce type. */
static int print_methods_for_type(const char* type_name, DocLang lang) {
    if (!type_name || !*type_name) return 0;
    int total = owl_registry_count();
    int wmax = 0;
    /* Première passe : largeur du pad. Une seule entrée par native. */
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        const OwlMethodBinding* mb = preferred_method_binding(d, type_name, lang);
        if (!mb) continue;
        int w = (int)strlen(mb->methodName) + 1; /* +1 pour '.' */
        if (w > wmax) wmax = w;
    }
    if (wmax == 0) return 0;
    if (wmax < 14) wmax = 14;
    if (wmax > 28) wmax = 28;

    /* Compte les méthodes en première passe pour le count du titre. */
    int n_methods = 0;
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (preferred_method_binding(d, type_name, lang)) n_methods++;
    }
    printf("\n%s%s%s %s(%d)%s\n",
           C_SECTION, dl(lang, "Méthodes", "Methods"),
           C_RESET, C_DIM, n_methods, C_RESET);
    int found = 0;
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        const OwlMethodBinding* mb = preferred_method_binding(d, type_name, lang);
        if (!mb) continue;
        char dotname[64];
        snprintf(dotname, sizeof dotname, ".%s", mb->methodName);
        int pad = wmax - (int)strlen(dotname);
        if (pad < 0) pad = 0;
        printf("  %s%s%s%*s  %s→ %s%s",
               C_NATIVE, dotname, C_RESET,
               pad, "",
               C_DIM, d->canonical, C_RESET);
        const char* desc = type_to_lang(d->descFR, d->descEN, lang);
        if (desc && *desc) {
            /* Tronque à la première phrase pour rester compact — la
             * fiche complète est dans owl_explain de la native. */
            size_t n = 0;
            while (desc[n]) {
                if (desc[n] == '.' &&
                    (desc[n + 1] == '\0' || desc[n + 1] == ' ' ||
                     desc[n + 1] == '\n')) {
                    n++; break;
                }
                n++;
            }
            printf("\n      %s%.*s%s", C_DIM, (int)n, desc, C_RESET);
        }
        printf("\n");
        found++;
    }
    return found;
}

static const char* type_to_lang(const char* descFR, const char* descEN, DocLang lang) {
    if (lang == DOC_LANG_EN && descEN && *descEN) return descEN;
    if (descFR && *descFR) return descFR;
    if (descEN && *descEN) return descEN;
    return "";
}

static void print_scenarios_for_symbol(const char* name, const char* name_ascii,
                                       DocLang lang) {
    ensure_scenarios_parsed();
    int n_files = 0;
    int n_procs = 0;
    int en = (lang == DOC_LANG_EN);
    for (int i = 0; i < g_scenarios_count; i++) {
        if (!sujet_matches(&g_scenarios[i], name, name_ascii)) continue;
        if (n_files == 0) {
            printf("\n%s%s%s\n", C_SECTION,
                   dl(lang, "Exemples", "Examples"), C_RESET);
        }
        n_files++;
        /*  — le corpus de scénarios est rédigé en OWL canon FR
         * (résumé + commentaires + code, articles `un`/`une` inclus). En
         * `--lang en` on ne dumpe pas ces corps (aucune traduction dispo) :
         * on annonce leur présence et on renvoie vers la langue source. */
        if (!en) {
            if (g_scenarios[i].resume[0]) {
                printf("  %s%s%s\n", C_DIM, g_scenarios[i].resume, C_RESET);
            }
            render_scenario_public(&g_scenarios[i]);
        }
        for (int p = 0; p < g_scenarios[i].procedure_count; p++) {
            if (!g_scenarios[i].procedures[p].is_private &&
                !g_scenarios[i].procedures[p].is_fixture) n_procs++;
        }
    }
    if (n_files == 0) {
        printf("\n%s%s%s\n", C_DIM,
               dl(lang, "Aucun scénario public n'illustre ce symbole.",
                        "No public scenario illustrates this symbol."), C_RESET);
    } else if (en) {
        printf("\n%s%d public scenario(s) in %d file(s) "
               "(source corpus in French: run with --lang fr).%s\n",
               C_DIM, n_procs, n_files, C_RESET);
    } else {
        printf("\n%s%d scénario(s) public(s) dans %d fichier(s).%s\n",
               C_DIM, n_procs, n_files, C_RESET);
    }
}

/* ==================================================================
 * Maillage — section "Voir aussi" jointe a la fin d'explain.
 *
 * Politique anti-flood :
 *   - Cap dur N=3 fiches max, seuil score>=2 (occurrences dans des
 *     blocs ```owl uniquement — la prose markdown est ignoree).
 *   - Cap dur N=5 voisins du registry, seulement pour les natives
 *     (autres natives de la meme categorie, exclus soi-meme).
 *   - Au-dela : indicateur "(+N autres)".
 *   - Symbole envahissant (>3*MAX fondations matchees) : on degrade en
 *     ligne courte "present dans N fiches — owl search X" plutot que
 *     d'enumerer.
 *
 * Le scoring compte les occurrences word-bounded dans les blocs ```owl
 * — case+accent insensible via word_eq_ci. La fiche qui contient le
 * symbole une seule fois (souvent en passing reference) tombe sous le
 * seuil et n'est pas listee : ca filtre le bruit a la racine.
 * ================================================================== */

#define RELATED_MIN_SCORE          2
#define RELATED_MAX_PUBLIC_DOCS    3
#define RELATED_MAX_NEIGHBORS      5
/* Widget -> natives : la surface d'un widget compte facilement ~15 natives
 * qui le manipulent (le cas Table). On liste plus large que les voisins de
 * categorie (cap 5) pour couvrir l'API du widget ; le rendu passe par la
 * meme ligne "voisins"/"neighbors" et traduit via neighbor_name_lang. */
#define RELATED_MAX_WIDGET_NEIGHBORS 24
/* Dimension du tampon neighbors : max des deux caps ci-dessus. */
#define RELATED_NEIGHBORS_STORAGE  RELATED_MAX_WIDGET_NEIGHBORS
#define RELATED_PUBLIC_DOC_BUF     64
/* "Utilisé par" (reverse lookup type composé → consommateurs) : au-delà de
 * ce cap on n'énumère plus, on renvoie vers la famille (catégorie du type). */
#define RELATED_MAX_USEDBY         6
#define RELATED_USEDBY_BUF         512

typedef struct {
    const char* name;
    int score;
} RelatedPublicDoc;

typedef struct {
    const char* name;
    int         is_type;   /* 1 = type composé, 0 = native */
} RelatedUsedBy;

typedef enum {
    REL_KIND_NATIVE,
    REL_KIND_TYPE,
    REL_KIND_WIDGET,
    REL_KIND_DSL_PROP,
    REL_KIND_ENUM,
    REL_KIND_CONST,
    REL_KIND_PRIMITIVE
} RelatedKind;

typedef struct {
    const char*       canon;
    const char*       canon_ascii;
    RelatedKind       kind;
    int               where_count;            /* -1 = non applicable */
    int               examples_count;
    RelatedPublicDoc public_docs[RELATED_MAX_PUBLIC_DOCS];
    int               public_docs_n;
    int               public_docs_total;
    const char*       neighbors[RELATED_NEIGHBORS_STORAGE];
    int               neighbors_n;
    int               neighbors_total;
    /* "Utilisé par" — uniquement pour les types composés (has_used_by=1).
     * Si used_by_total <= cap : used_by_n entrées listées, used_by_family NULL.
     * Sinon : used_by_n=0 et used_by_family = catégorie du type (renvoi). */
    int               has_used_by;
    RelatedUsedBy     used_by[RELATED_MAX_USEDBY];
    int               used_by_n;
    int               used_by_total;
    const char*       used_by_family;
} RelatedInfo;

static int is_word_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* Compte les occurrences word-bounded de `symbol` dans une plage. */
static int symbol_occurrences(const char* range, long range_len,
                              const char* symbol) {
    int n = 0;
    long i = 0;
    while (i < range_len) {
        if (!is_word_byte((unsigned char)range[i])) { i++; continue; }
        long s = i;
        while (i < range_len && is_word_byte((unsigned char)range[i])) i++;
        if (word_eq_ci(range + s, i - s, symbol)) n++;
    }
    return n;
}

/* Scan markdown : on ne compte que dans les blocs ```owl ... ```.
 * Une mention isolee en prose ne suffit pas a relier la fiche au
 * symbole — on cherche les fiches qui le *demontrent* en code. */
static int count_owl_block_occurrences(const unsigned char* data, long len,
                                       const char* symbol) {
    int n = 0;
    int in_block = 0;
    long i = 0;
    while (i < len) {
        long line_start = i;
        while (i < len && data[i] != '\n') i++;
        long line_len = i - line_start;
        if (i < len) i++;
        const char* line = (const char*)(data + line_start);

        long lt = 0;
        while (lt < line_len && (line[lt] == ' ' || line[lt] == '\t')) lt++;
        if (line_len - lt >= 3 &&
            line[lt] == '`' && line[lt+1] == '`' && line[lt+2] == '`') {
            if (!in_block) {
                long pi = lt + 3;
                while (pi < line_len && (line[pi] == ' ' || line[pi] == '\t')) pi++;
                long li_start = pi;
                while (pi < line_len && is_word_byte((unsigned char)line[pi])) pi++;
                long li_len = pi - li_start;
                if (li_len == 3 &&
                    (line[li_start]   == 'o' || line[li_start]   == 'O') &&
                    (line[li_start+1] == 'w' || line[li_start+1] == 'W') &&
                    (line[li_start+2] == 'l' || line[li_start+2] == 'L')) {
                    in_block = 1;
                }
            } else {
                in_block = 0;
            }
            continue;
        }
        if (in_block) n += symbol_occurrences(line, line_len, symbol);
    }
    return n;
}

static int find_related_public_docs(const char* symbol,
                                    RelatedPublicDoc* out, int max_out,
                                    int* out_total) {
    RelatedPublicDoc buf[RELATED_PUBLIC_DOC_BUF];
    int nb = 0;
    int total = 0;
    for (int i = 0; i < EMBEDDED_PUBLIC_DOCS_COUNT; i++) {
        const OwlEmbeddedDoc* d = &EMBEDDED_PUBLIC_DOCS[i];
        int score = count_owl_block_occurrences(d->data, d->len, symbol);
        if (score < RELATED_MIN_SCORE) continue;
        total++;
        if (nb < RELATED_PUBLIC_DOC_BUF) {
            buf[nb].name = d->name;
            buf[nb].score = score;
            nb++;
        }
    }
    /* Insertion-sort descendant par score — petit n, robuste. */
    for (int i = 1; i < nb; i++) {
        RelatedPublicDoc t = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j].score < t.score) {
            buf[j+1] = buf[j]; j--;
        }
        buf[j+1] = t;
    }
    int n_out = nb < max_out ? nb : max_out;
    for (int i = 0; i < n_out; i++) out[i] = buf[i];
    if (out_total) *out_total = total;
    return n_out;
}

/* ==================================================================
 *  — table de cross-references declaratives fiche->guide.
 *
 * Source unique, curee, sparse : epingle des liens symbole->guide voulus que
 * l'heuristique d'occurrences (count_owl_block_occurrences) ne peut pas trouver
 * — typiquement un guide qui oriente en prose vers un symbole sans jamais le
 * nommer dans un bloc de code. Le declaratif PRIME et est garanti present ;
 * l'heuristique reste le fallback (elle n'est pas remplacee).
 *
 * key_kind reserve la desambiguisation ST3 : aujourd'hui toutes les entrees
 * sont des symboles du registre (native/type/enum/constante), resolus par nom.
 * OWL_XREF_DSL_PROP est reserve pour epingler des fiches de propriete DSL (dont
 * le nom peut entrer en collision avec une native homonyme) — la resolution de
 * validite les cherchera alors dans le catalogue de props, pas dans le registre.
 *
 * Un self-test (registry_self_test rule 62) valide les DEUX bouts de chaque
 * entree : le symbole existe ET le stem de guide est embarque. Un lien mort est
 * refuse au build (violation_count > 0).
 * ================================================================== */

typedef enum {
    OWL_XREF_SYMBOL,     /* native / type / enum / constante — resolu par nom */
    OWL_XREF_DSL_PROP    /* propriete DSL — reserve ST3 (catalogue de props) */
} OwlCrossRefKeyKind;

typedef struct {
    OwlCrossRefKeyKind key_kind;
    const char*        key;   /* fiche source : nom canonique du symbole */
    const char*        stem;  /* fiche cible : stem du guide public (NN_*) */
} OwlCrossRefEntry;

static const OwlCrossRefEntry OWL_CROSSREF_TABLE[] = {
    /* Le guide des operateurs oriente en prose vers le module regex juste apres
     * les operateurs de comparaison, sans jamais nommer la native dans un bloc
     * de code -> l'heuristique ne peut structurellement pas la trouver. */
    { OWL_XREF_SYMBOL, "RegexRemplace", "05_operateurs" },
    /* Le guide des connecteurs SQL documente les champs de connexion
     * (Provider/Source/...) que SQLConnexionOuvre consomme, mais sans bloc de
     * code nommant la native -> aucun lien heuristique aujourd'hui. */
    { OWL_XREF_SYMBOL, "SQLConnexionOuvre", "14_sql_connecteurs" },
    /* Propriete DSL du bloc `est un Projet` : pas de fiche `explain` autonome
     * (champ du type composite Projet), sa fiche navigable vit dans le viewer
     * (DSLPROPS). Le guide des prefixes decrit la convention de nommage que la
     * propriete active -> lien fiche-propriete->guide. */
    { OWL_XREF_DSL_PROP, "ActiveAvertissementConventionDeNommage",
      "12_prefixes_de_nommage" },
};

#define OWL_CROSSREF_TABLE_COUNT \
    ((int)(sizeof(OWL_CROSSREF_TABLE) / sizeof(OWL_CROSSREF_TABLE[0])))

/* Score sentinelle des liens epingles : ce n'est pas un comptage d'occurrences
 * (le lien est curee, pas heuristique). Valeur haute -> reste en tete si un
 * consommateur retriait par score. */
#define RELATED_DECLARATIVE_SCORE  9999

int cli_doc_crossref_count(void) { return OWL_CROSSREF_TABLE_COUNT; }

int cli_doc_crossref_at(int i, const char** out_symbol, const char** out_stem) {
    if (i < 0 || i >= OWL_CROSSREF_TABLE_COUNT) return 0;
    if (out_symbol) *out_symbol = OWL_CROSSREF_TABLE[i].key;
    if (out_stem)   *out_stem   = OWL_CROSSREF_TABLE[i].stem;
    return 1;
}

/* Vrai si `stem` correspond au name d'un guide embarque. */
static int crossref_stem_embedded(const char* stem) {
    if (!stem) return 0;
    for (int i = 0; i < EMBEDDED_PUBLIC_DOCS_COUNT; i++) {
        if (strcmp(EMBEDDED_PUBLIC_DOCS[i].name, stem) == 0) return 1;
    }
    return 0;
}

/* Vrai si `name` resout comme propriete DSL : un champ (name / nameAccented /
 * nameEN) d'un type reifie du registre, case+accent insensible. Sert a valider
 * les entrees OWL_XREF_DSL_PROP, dont la cle n'est pas un symbole du registre
 * mais un champ d'un type porteur (ex. Projet.ActiveAvertissementConventionDeNommage). */
static int crossref_dsl_prop_exists(const char* name) {
    if (!name) return 0;
    long len = (long)strlen(name);
    for (int i = 0, nt = owl_registry_types_count(); i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->fields) continue;
        for (const OwlField* f = t->fields; f->name; f++) {
            if (f->name        && word_eq_ci(name, len, f->name))        return 1;
            if (f->nameAccented && word_eq_ci(name, len, f->nameAccented)) return 1;
            if (f->nameEN      && word_eq_ci(name, len, f->nameEN))      return 1;
        }
    }
    return 0;
}

/* Vrai si `symbol` resout dans un registre (native/type/enum/constante) OU
 * comme propriete DSL (champ d'un type porteur), case+accent insensible via les
 * lookups canon. L'union couvre les deux key_kind de la table de cross-ref :
 * OWL_XREF_SYMBOL (registre) et OWL_XREF_DSL_PROP (catalogue de props). */
static int crossref_symbol_exists(const char* symbol) {
    if (!symbol) return 0;
    return owl_registry_find(symbol) != NULL
        || owl_registry_type_find(symbol) != NULL
        || owl_registry_enum_find(symbol) != NULL
        || owl_registry_const_find(symbol) != NULL
        || crossref_dsl_prop_exists(symbol);
}

int cli_doc_crossref_entry_valid(const char* symbol, const char* stem,
                                 int* out_symbol_ok, int* out_stem_ok) {
    int sym_ok  = crossref_symbol_exists(symbol);
    int stem_ok = crossref_stem_embedded(stem);
    if (out_symbol_ok) *out_symbol_ok = sym_ok;
    if (out_stem_ok)   *out_stem_ok   = stem_ok;
    return sym_ok && stem_ok;
}

/* Fusionne les liens declaratifs fiche->guide de `canon` dans le tableau
 * `docs` (deja rempli par l'heuristique, `*n` entrees, cap `cap`). Le
 * declaratif est epingle en tete, dedupe par stem, garanti present : si le cap
 * est sature, il evince l'entree heuristique la moins bien classee (qui reste
 * comptee dans *total et bascule en "extra"). */
static void pin_declarative_public_docs(const char* canon,
                                        RelatedPublicDoc* docs, int* n,
                                        int* total, int cap) {
    if (!canon) return;
    long canon_len = (long)strlen(canon);
    for (int e = 0; e < OWL_CROSSREF_TABLE_COUNT; e++) {
        const OwlCrossRefEntry* ce = &OWL_CROSSREF_TABLE[e];
        if (!word_eq_ci(canon, canon_len, ce->key)) continue;
        int present = 0;
        for (int i = 0; i < *n; i++) {
            if (strcmp(docs[i].name, ce->stem) == 0) { present = 1; break; }
        }
        if (present) continue;
        if (*n < cap) {
            for (int i = *n; i > 0; i--) docs[i] = docs[i - 1];
            (*n)++;
        } else {
            for (int i = cap - 1; i > 0; i--) docs[i] = docs[i - 1];
        }
        docs[0].name  = ce->stem;
        docs[0].score = RELATED_DECLARATIVE_SCORE;
        if (total) (*total)++;
    }
}

static int find_neighbors_native(const OwlNativeDef* self,
                                 const char** out, int max_out,
                                 int* out_total) {
    if (!self || !self->category) {
        if (out_total) *out_total = 0;
        return 0;
    }
    int total = 0;
    int n_out = 0;
    int n_natives = owl_registry_count();
    for (int i = 0; i < n_natives; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || d == self || !d->category) continue;
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (strcmp(d->category, self->category) != 0) continue;
        total++;
        if (n_out < max_out) out[n_out++] = d->canonical;
    }
    if (out_total) *out_total = total;
    return n_out;
}

/* Voisins d'un type composé SANS native homonyme (IAModèle, LLM,
 * OptionsEntraînement…). Même critère que find_neighbors_native : partage
 * de catégorie. Les types à native homonyme (Tenseur) passent par le chemin
 * REL_KIND_NATIVE (dominant) et n'atteignent jamais cette fonction. On ignore
 * les entrées primitiveLiteral (durée…) : pas de vrais types composés. */
static int find_neighbors_type(const OwlTypeDef* self,
                               const char** out, int max_out,
                               int* out_total) {
    if (!self || !self->category) {
        if (out_total) *out_total = 0;
        return 0;
    }
    int total = 0;
    int n_out = 0;
    int n_types = owl_registry_types_count();
    for (int i = 0; i < n_types; i++) {
        const OwlTypeDef* d = owl_registry_type_at(i);
        if (!d || d == self || !d->category) continue;
        if (d->primitiveLiteral) continue;
        if (strcmp(d->category, self->category) != 0) continue;
        total++;
        if (n_out < max_out) out[n_out++] = d->canonical;
    }
    if (out_total) *out_total = total;
    return n_out;
}

/*  — un segment de type de param (deja splitte sur '|') reference-t-il
 * le widget ? On trim, on retire un modificateur widget de tete optionnel
 * ("Champ"/"Field"/"Control" — les formes canon du modificateur, cf.
 * compiler.c token_is_champ_modifier) puis on compare le reste au nom du
 * widget (canon FR / ASCII / EN, accent+casse insensible via word_eq_ci). */
static int seg_refs_widget(const char* seg, long len,
                           const char* canon, const char* canonAscii,
                           const char* nameEN) {
    while (len > 0 && (seg[0] == ' ' || seg[0] == '\t')) { seg++; len--; }
    while (len > 0 && (seg[len-1] == ' ' || seg[len-1] == '\t')) len--;
    if (len <= 0) return 0;
    static const char* const mods[] = { "Champ", "Field", "Control" };
    for (int m = 0; m < 3; m++) {
        long ml = (long)strlen(mods[m]);
        if (len > ml && (seg[ml] == ' ' || seg[ml] == '\t') &&
            word_eq_ci(seg, ml, mods[m])) {
            const char* rest = seg + ml + 1;
            long rlen = len - ml - 1;
            while (rlen > 0 && (rest[0] == ' ' || rest[0] == '\t')) { rest++; rlen--; }
            seg = rest; len = rlen;
            break;
        }
    }
    if (len <= 0) return 0;
    if (canon      && word_eq_ci(seg, len, canon))      return 1;
    if (canonAscii && word_eq_ci(seg, len, canonAscii)) return 1;
    if (nameEN     && word_eq_ci(seg, len, nameEN))     return 1;
    return 0;
}

/* Un type de param (chaine registry, ex "Champ Table" ou "Champ Table|chaîne")
 * reference-t-il le widget ? Split sur '|', trim, delegue a seg_refs_widget. */
static int param_type_refs_widget(const char* typestr,
                                  const char* canon, const char* canonAscii,
                                  const char* nameEN) {
    if (!typestr) return 0;
    const char* p = typestr;
    while (*p) {
        const char* seg = p;
        while (*p && *p != '|') p++;
        if (seg_refs_widget(seg, p - seg, canon, canonAscii, nameEN)) return 1;
        if (*p == '|') p++;
    }
    return 0;
}

/*  — voisins d'un WIDGET : natives dont un param (ou le type de
 * retour) est type sur ce widget (ex. le 1er param "Champ Table" des ~14
 * natives Table). Reverse-lookup type-based, miroir de compute_type_usedby
 * cote types composes. Rendu sur la meme ligne "voisins"/"neighbors" que les
 * autres buckets -> traduction FR->EN automatique via neighbor_name_lang.
 * Les Tableau* (param type "tableau") ne matchent pas -> exclus naturellement. */
static int find_neighbors_widget(const OwlControlKindInfo* self,
                                 const char** out, int max_out,
                                 int* out_total) {
    if (out_total) *out_total = 0;
    if (!self) return 0;
    const char* canon      = self->nameFR;
    const char* canonAscii = self->nameAscii;
    const char* nameEN     = self->nameEN;
    int total = 0, n_out = 0;
    int nn = owl_registry_count();
    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || (d->flags & OWL_NAT_INTERNAL) || !d->signatures) continue;
        int matched = 0;
        for (const OwlSignature* s = d->signatures;
             !(s->arity == 0 && s->params == NULL &&
               s->returnType == NULL && s->returnDesc == NULL); s++) {
            if (param_type_refs_widget(s->returnType, canon, canonAscii, nameEN)) {
                matched = 1; break;
            }
            if (s->params) {
                for (const OwlParam* p = s->params;
                     p->name && (s->arity < 0 || (p - s->params) < s->arity);
                     p++) {
                    if (param_type_refs_widget(p->type, canon, canonAscii, nameEN)) {
                        matched = 1; break;
                    }
                }
            }
            if (matched) break;
        }
        if (!matched) continue;
        total++;
        if (n_out < max_out) out[n_out++] = d->canonical;
    }
    if (out_total) *out_total = total;
    return n_out;
}

static int count_examples_for(const char* name, const char* name_ascii) {
    ensure_scenarios_parsed();
    int n = 0;
    for (int i = 0; i < g_scenarios_count; i++) {
        if (sujet_matches(&g_scenarios[i], name, name_ascii)) n++;
    }
    return n;
}

/* ==================================================================
 *  — émetteurs JSON pour le navigateur de doc HTML.
 * Réutilisent le parser de scénarios + le maillage voisins du CLI
 * (source unique). Appelés par main.c (dump_doc_html), émettent des
 * fragments `,"examples":[…]` / `,"neighbors":[…]` à insérer dans un
 * objet symbole déjà ouvert.
 * ================================================================== */

/* Nettoie un bloc de lignes `//` en texte plat et l'émet en JSON string. */
static void doc_emit_clean_doc_json(const char* p, long len) {
    char buf[1024];
    int o = 0, need_space = 0;
    long i = 0;
    while (i < len && o < (int)sizeof(buf) - 1) {
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i + 1 < len && p[i] == '/' && p[i + 1] == '/') i += 2;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        if (need_space && o < (int)sizeof(buf) - 1) buf[o++] = ' ';
        while (i < len && p[i] != '\n' && o < (int)sizeof(buf) - 1) buf[o++] = p[i++];
        while (i < len && p[i] != '\n') i++;   /* drain si buffer plein */
        if (i < len) i++;                        /* skip \n */
        need_space = 1;
    }
    buf[o] = '\0';
    json_write_cstr(buf);
}

void cli_doc_emit_examples_json(const char* name, const char* name_ascii) {
    ensure_scenarios_parsed();
    int started = 0;
    for (int i = 0; i < g_scenarios_count; i++) {
        const ParsedScenario* sc = &g_scenarios[i];
        if (!sujet_matches(sc, name, name_ascii)) continue;
        for (int p = 0; p < sc->procedure_count; p++) {
            const ParsedProcedure* proc = &sc->procedures[p];
            if (proc->is_private || proc->is_fixture) continue;
            if (!started) { printf(",\"examples\":["); started = 1; }
            else printf(",");
            printf("{\"title\":");
            json_write_cstr(proc->name);
            printf(",\"sub\":");
            if (proc->doc_start && proc->doc_len > 0)
                doc_emit_clean_doc_json(proc->doc_start, proc->doc_len);
            else
                json_write_cstr(sc->resume[0] ? sc->resume : "");
            printf(",\"code\":");
            json_write_string((const unsigned char*)proc->body_start, proc->body_len);
            printf("}");
        }
    }
    if (started) printf("]");
}

/*  — parite viewer : emet les liens fiche->guide (declaratifs +
 * heuristique fusionnes) comme `,"public_documentation":[<stem>, ...]`, meme
 * source que le "Voir aussi" CLI. Rien emis si aucun lien. */
void cli_doc_emit_public_docs_json(const char* canon, const char* canon_ascii) {
    (void)canon_ascii;
    RelatedPublicDoc docs[RELATED_MAX_PUBLIC_DOCS];
    int total = 0;
    int n = find_related_public_docs(canon, docs, RELATED_MAX_PUBLIC_DOCS, &total);
    pin_declarative_public_docs(canon, docs, &n, &total, RELATED_MAX_PUBLIC_DOCS);
    if (n <= 0) return;
    printf(",\"public_documentation\":[");
    for (int i = 0; i < n; i++) {
        if (i) printf(",");
        json_write_cstr(docs[i].name);
    }
    printf("]");
}

/*  — liens declaratifs fiche-propriete->guide pour une propriete DSL
 * (bloc `est un Projet`, etc.). Emis dans l'entree DSLPROPS de dump_doc_html au
 * meme titre que cli_doc_emit_public_docs_json le fait pour les natives/types.
 * Match la cle de la table sur le nom FR OU EN de la propriete (case+accent
 * insensible). N'emet rien si aucune entree ne correspond. */
void cli_doc_emit_dsl_prop_public_docs_json(const char* propNameFR,
                                            const char* propNameEN) {
    int first = 1;
    for (int e = 0; e < OWL_CROSSREF_TABLE_COUNT; e++) {
        const OwlCrossRefEntry* ce = &OWL_CROSSREF_TABLE[e];
        if (ce->key_kind != OWL_XREF_DSL_PROP) continue;
        int match = 0;
        if (propNameFR && word_eq_ci(propNameFR, (long)strlen(propNameFR), ce->key))
            match = 1;
        if (!match && propNameEN
            && word_eq_ci(propNameEN, (long)strlen(propNameEN), ce->key))
            match = 1;
        if (!match) continue;
        if (first) { printf(",\"public_documentation\":["); first = 0; }
        else printf(",");
        json_write_cstr(ce->stem);
    }
    if (!first) printf("]");
}

void cli_doc_emit_neighbors_json(const OwlNativeDef* d) {
    const char* nb[RELATED_MAX_NEIGHBORS];
    int total = 0;
    int n = find_neighbors_native(d, nb, RELATED_MAX_NEIGHBORS, &total);
    if (n <= 0) return;
    printf(",\"neighbors\":[");
    for (int i = 0; i < n; i++) {
        if (i) printf(",");
        json_write_cstr(nb[i]);
    }
    printf("]");
}

/* Forward declaration : find_prop_users est defini plus bas (avec les
 * helpers de reverse lookup). compute_related s'en sert pour calculer
 * where_count en cas de DSL_PROP. */
static int find_prop_users(const char* prop_name,
                           const OwlControlPropDef** out_def,
                           const char* kinds_out[], int max_kinds);

/* Vrai si `typestr` (forme registry d'un type de param/retour/field, ex.
 * "sqlRequête" ou "chaîne|entier") reference le type nomme. Le type registry
 * est une CHAINE (pas un pointeur) : on split sur '|', on trim, et on compare
 * chaque segment au canon / ascii / EN via word_eq_ci (accent+casse insensible).
 * Les formes conteneur ("tableau de X") ne matchent pas par design — on relie
 * les fiches par usage direct du type, pas par containment. */
static int type_str_refs(const char* typestr, const char* canon,
                         const char* canonAscii, const char* nameEN) {
    if (!typestr) return 0;
    const char* p = typestr;
    while (*p) {
        const char* seg = p;
        while (*p && *p != '|') p++;
        long len = p - seg;
        while (len > 0 && (seg[0] == ' ' || seg[0] == '\t')) { seg++; len--; }
        while (len > 0 && (seg[len-1] == ' ' || seg[len-1] == '\t')) len--;
        if (len > 0) {
            if (canon      && word_eq_ci(seg, len, canon))      return 1;
            if (canonAscii && word_eq_ci(seg, len, canonAscii)) return 1;
            if (nameEN     && word_eq_ci(seg, len, nameEN))     return 1;
        }
        if (*p == '|') p++;
    }
    return 0;
}

/* Reverse lookup d'un type composé : natives (params + type de retour) et
 * autres types composés (fields) qui le consomment. Dédup par identité de
 * chaine canon (interned-stable). Ignore les natives INTERNAL, comme
 * enum_compute_usedby. Remplit les champs used_by* de `out`. */
static void compute_type_usedby(const OwlTypeDef* self, RelatedInfo* out) {
    if (!self) return;
    out->has_used_by = 1;

    const char* canon      = self->canonical;
    const char* canonAscii = self->canonicalAscii;
    const char* nameEN     = self->nameEN;

    const char* found[RELATED_USEDBY_BUF];
    int         is_type_buf[RELATED_USEDBY_BUF];
    int         total = 0;

    /* Natives : signatures[].returnType + params[].type. */
    int nn = owl_registry_count();
    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || (d->flags & OWL_NAT_INTERNAL) || !d->signatures) continue;
        int matched = 0;
        for (const OwlSignature* s = d->signatures;
             !(s->arity == 0 && s->params == NULL &&
               s->returnType == NULL && s->returnDesc == NULL); s++) {
            if (type_str_refs(s->returnType, canon, canonAscii, nameEN)) { matched = 1; break; }
            if (s->params) {
                for (const OwlParam* p = s->params;
                     p->name && (s->arity < 0 || (p - s->params) < s->arity);
                     p++) {
                    if (type_str_refs(p->type, canon, canonAscii, nameEN)) { matched = 1; break; }
                }
            }
            if (matched) break;
        }
        if (!matched) continue;
        int dup = 0;
        for (int k = 0; k < total; k++) if (found[k] == d->canonical) { dup = 1; break; }
        if (!dup && total < RELATED_USEDBY_BUF) {
            found[total] = d->canonical; is_type_buf[total] = 0; total++;
        }
    }

    /* Autres types composés : fields[].type (self exclu). */
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || t == self || !t->fields) continue;
        int matched = 0;
        for (const OwlField* f = t->fields; f->name; f++) {
            if (type_str_refs(f->type, canon, canonAscii, nameEN)) { matched = 1; break; }
        }
        if (!matched) continue;
        int dup = 0;
        for (int k = 0; k < total; k++) if (found[k] == t->canonical) { dup = 1; break; }
        if (!dup && total < RELATED_USEDBY_BUF) {
            found[total] = t->canonical; is_type_buf[total] = 1; total++;
        }
    }

    out->used_by_total = total;
    if (total > RELATED_MAX_USEDBY) {
        /* Explosion : on n'énumère pas, on renvoie vers la famille. */
        out->used_by_n = 0;
        out->used_by_family = self->category;
    } else {
        for (int i = 0; i < total; i++) {
            out->used_by[i].name    = found[i];
            out->used_by[i].is_type = is_type_buf[i];
        }
        out->used_by_n = total;
        out->used_by_family = NULL;
    }
}

/*  — fragment JSON "utilisé par" pour la fiche HTML d'un type
 * composé. Réutilise compute_type_usedby (source unique) via un RelatedInfo
 * jetable. Rien émis si aucun consommateur. */
void cli_doc_emit_usedby_json(const OwlTypeDef* t) {
    if (!t || t->primitiveLiteral) return;
    RelatedInfo ri;
    memset(&ri, 0, sizeof(ri));
    compute_type_usedby(t, &ri);
    if (ri.used_by_total <= 0) return;
    printf(",\"used_by\":[");
    for (int i = 0; i < ri.used_by_n; i++) {
        if (i) printf(",");
        printf("{\"name\":");
        json_write_cstr(ri.used_by[i].name);
        printf(",\"kind\":\"%s\"}", ri.used_by[i].is_type ? "type" : "native");
    }
    printf("],\"used_by_total\":%d,\"used_by_family\":", ri.used_by_total);
    if (ri.used_by_family) json_write_cstr(ri.used_by_family);
    else printf("null");
}

/* ------------------------------------------------------------------
 *  — diff "Nouveautés" : fiches présentes maintenant, absentes
 * de la release précédente. Cœur logique pur, sans I/O ni registry (les
 * deux ensembles sont fournis par l'appelant). Le type reste local à ce
 * .c : le self-test (cli_doc_test.c) déclare son propre OwlDocFicheId +
 * extern, et un typedef identique dans cli_doc.h entrerait en collision
 * (redéfinition interdite en C99) dans la TU du test qui inclut le header.
 * ------------------------------------------------------------------ */

typedef struct {
    const char* kind;
    const char* name;
} OwlDocFicheId;

static int novelty_cmp(const void* a, const void* b) {
    const OwlDocFicheId* fa = (const OwlDocFicheId*)a;
    const OwlDocFicheId* fb = (const OwlDocFicheId*)b;
    int c = strcmp(fa->kind, fb->kind);
    if (c != 0) return c;
    return strcmp(fa->name, fb->name);
}

int owl_doc_novelties_diff(const OwlDocFicheId* baseline, int nb,
                           const OwlDocFicheId* current, int nc,
                           OwlDocFicheId* out, int out_cap) {
    if (out == NULL || out_cap <= 0 || current == NULL || nc <= 0) return 0;
    if (nb < 0) nb = 0;

    /* Tri appliqué sur l'ensemble des nouveautés AVANT le plafonnement à
     * out_cap → collecte dans un tampon dédié (les nouveautés peuvent
     * dépasser out_cap). Aliasing des pointeurs de `current`, pas de copie
     * de chaîne. */
    OwlDocFicheId* tmp = (OwlDocFicheId*)malloc((size_t)nc * sizeof(OwlDocFicheId));
    if (tmp == NULL) return 0;

    int count = 0;
    for (int i = 0; i < nc; i++) {
        int found = 0;
        for (int j = 0; j < nb; j++) {
            if (strcmp(current[i].kind, baseline[j].kind) == 0 &&
                strcmp(current[i].name, baseline[j].name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) tmp[count++] = current[i];
    }

    if (count > 1) qsort(tmp, (size_t)count, sizeof(OwlDocFicheId), novelty_cmp);

    int n = (count < out_cap) ? count : out_cap;
    for (int i = 0; i < n; i++) out[i] = tmp[i];

    free(tmp);
    return n;
}

/* `kind_data` typé selon kind :
 *   REL_KIND_NATIVE   : const OwlNativeDef*   (utilise pour voisins)
 *   REL_KIND_DSL_PROP : const OwlControlPropDef* (utilise pour where_count)
 *   autres            : NULL accepte */
static void compute_related(RelatedInfo* out,
                            const char* canon,
                            const char* canon_ascii,
                            RelatedKind kind,
                            const void* kind_data) {
    memset(out, 0, sizeof(*out));
    out->canon = canon;
    out->canon_ascii = canon_ascii;
    out->kind = kind;
    out->where_count = -1;

    out->public_docs_n = find_related_public_docs(
        canon, out->public_docs, RELATED_MAX_PUBLIC_DOCS,
        &out->public_docs_total);
    /*  — epingle les liens declaratifs par-dessus l'heuristique. */
    pin_declarative_public_docs(canon, out->public_docs, &out->public_docs_n,
                                &out->public_docs_total, RELATED_MAX_PUBLIC_DOCS);

    out->examples_count = count_examples_for(canon, canon_ascii);

    if (kind == REL_KIND_NATIVE) {
        out->neighbors_n = find_neighbors_native(
            (const OwlNativeDef*)kind_data,
            out->neighbors, RELATED_MAX_NEIGHBORS,
            &out->neighbors_total);
    }

    /* Types composés sans native homonyme : voisins par catégorie. Les types
     * qui ont une native homonyme sont classés REL_KIND_NATIVE (dominant), donc
     * jamais ici — leurs voisins restent ceux de la native (pas de régression). */
    if (kind == REL_KIND_TYPE && kind_data) {
        out->neighbors_n = find_neighbors_type(
            (const OwlTypeDef*)kind_data,
            out->neighbors, RELATED_MAX_NEIGHBORS,
            &out->neighbors_total);
    }

    /*  — widget : voisins = natives qui manipulent ce widget
     * (reverse-lookup type-based sur les params typés "Champ <Widget>"). */
    if (kind == REL_KIND_WIDGET && kind_data) {
        out->neighbors_n = find_neighbors_widget(
            (const OwlControlKindInfo*)kind_data,
            out->neighbors, RELATED_MAX_WIDGET_NEIGHBORS,
            &out->neighbors_total);
    }

    if (kind == REL_KIND_DSL_PROP && kind_data) {
        const OwlControlPropDef* d = (const OwlControlPropDef*)kind_data;
        const char* kinds_tmp[64];
        const OwlControlPropDef* unused = NULL;
        out->where_count = find_prop_users(d->nameFR, &unused, kinds_tmp, 64);
    }

    /* "Utilisé par" — types composés uniquement. On résout le type par nom
     * plutôt que via `kind` : un même symbole peut être multi-bucket (ex.
     * Police = type + propriété DSL) et le kind dominant n'est pas TYPE.
     * On saute les entrées registry qui décrivent un littéral primitif
     * (primitiveLiteral, ex. durée) — ce ne sont pas de vrais types composés. */
    {
        const OwlTypeDef* ty = (kind == REL_KIND_TYPE && kind_data)
                                 ? (const OwlTypeDef*)kind_data
                                 : owl_registry_type_find(canon);
        if (ty && !ty->primitiveLiteral) compute_type_usedby(ty, out);
    }
}

/* "Voir aussi" en mode texte. No-op si tout est vide. */
static void print_related_text(const RelatedInfo* r, DocLang lang) {
    int has_where = r->where_count > 0;
    int has_ex    = r->examples_count > 0;
    int has_fnd   = r->public_docs_n > 0;
    int has_nbr   = r->neighbors_n > 0;
    int has_usedby = r->has_used_by &&
                     (r->used_by_n > 0 ||
                      (r->used_by_family && r->used_by_total > 0));
    if (!has_where && !has_ex && !has_fnd && !has_nbr && !has_usedby) return;

    printf("\n%s%s%s\n", C_SECTION, dl(lang, "Voir aussi", "See also"), C_RESET);

    if (has_where) {
        printf("  %s%s%s %d %s%s %s ..%s%s%s "
               "%s(owl where %s)%s\n",
               C_DIM, dl(lang, "where      ", "where      "), C_RESET,
               r->where_count,
               dl(lang, "champ", "field"),
               r->where_count > 1 ? "s" : "",
               dl(lang, "acceptent", "accept"),
               C_CONST, r->canon, C_RESET,
               C_DIM, r->canon, C_RESET);
    }

    if (has_ex) {
        printf("  %s%s%s %d %s%s "
               "%s(owl examples %s)%s\n",
               C_DIM, dl(lang, "examples   ", "examples   "), C_RESET,
               r->examples_count,
               dl(lang, "scénario", "scenario"),
               r->examples_count > 1 ? "s" : "",
               C_DIM, r->canon, C_RESET);
    }

    if (has_fnd) {
        /* Flood : > 3*MAX fondations → ligne courte sans énumération. */
        if (r->public_docs_total > RELATED_MAX_PUBLIC_DOCS * 3) {
            printf("  %s%s%s %s %d %s "
                   "%s(owl search %s)%s\n",
                   C_DIM, dl(lang, "fiches     ", "docs       "), C_RESET,
                   dl(lang, "présent dans", "found in"),
                   r->public_docs_total,
                   dl(lang, "fiches", "docs"),
                   C_DIM, r->canon, C_RESET);
        } else {
            printf("  %s%s%s ", C_DIM,
                   dl(lang, "fiches     ", "docs       "), C_RESET);
            for (int i = 0; i < r->public_docs_n; i++) {
                if (i > 0) printf(", ");
                printf("%s%s%s", C_CMD, r->public_docs[i].name, C_RESET);
            }
            int extra = r->public_docs_total - r->public_docs_n;
            if (extra > 0) printf(" %s(+%d %s)%s", C_DIM, extra,
                                  dl(lang, "autres", "more"), C_RESET);
            printf("\n");
        }
    }

    if (has_nbr) {
        printf("  %s%s%s ", C_DIM,
               dl(lang, "voisins    ", "neighbors  "), C_RESET);
        for (int i = 0; i < r->neighbors_n; i++) {
            if (i > 0) printf(", ");
            printf("%s%s%s", C_NATIVE,
                   neighbor_name_lang(r->neighbors[i], lang), C_RESET);
        }
        int extra = r->neighbors_total - r->neighbors_n;
        if (extra > 0) printf(" %s(+%d %s)%s", C_DIM, extra,
                              dl(lang, "autres", "more"), C_RESET);
        printf("\n");
    }

    if (has_usedby) {
        printf("  %s%s%s ", C_DIM,
               dl(lang, "utilisé par", "used by    "), C_RESET);
        if (r->used_by_family) {
            /* Explosion : renvoi vers la famille (catégorie du type). */
            printf("%d %s %s%s%s %s(owl list %s)%s\n",
                   r->used_by_total,
                   dl(lang, "fonctions — famille", "functions — family"),
                   C_CONST, r->used_by_family, C_RESET,
                   C_DIM, r->used_by_family, C_RESET);
        } else {
            for (int i = 0; i < r->used_by_n; i++) {
                if (i > 0) printf(", ");
                printf("%s%s%s %s(%s)%s",
                       r->used_by[i].is_type ? C_TYPE : C_NATIVE,
                       r->used_by[i].name, C_RESET,
                       C_DIM, r->used_by[i].is_type ? "type" : "native", C_RESET);
            }
            printf("\n");
        }
    }
}

/* "Voir aussi" en JSON — emis comme un champ `related` (sans virgule
 * de tete : le caller insere la virgule s'il y a un champ avant). */
static void print_related_json(const RelatedInfo* r) {
    printf("\"related\": {");
    printf("\n    \"where_count\": %d", r->where_count);
    printf(",\n    \"examples_count\": %d", r->examples_count);

    printf(",\n    \"public_documentation\": [");
    for (int i = 0; i < r->public_docs_n; i++) {
        if (i > 0) printf(",");
        printf("\n      {\"name\": ");
        json_write_cstr(r->public_docs[i].name);
        printf(", \"score\": %d}", r->public_docs[i].score);
    }
    printf("\n    ]");
    int extra_f = r->public_docs_total - r->public_docs_n;
    printf(",\n    \"public_documentation_more\": %d", extra_f < 0 ? 0 : extra_f);

    printf(",\n    \"neighbors\": [");
    for (int i = 0; i < r->neighbors_n; i++) {
        if (i > 0) printf(",");
        printf("\n      ");
        json_write_cstr(r->neighbors[i]);
    }
    printf("\n    ]");
    int extra_n = r->neighbors_total - r->neighbors_n;
    printf(",\n    \"neighbors_more\": %d", extra_n < 0 ? 0 : extra_n);

    /* "Utilisé par" — émis uniquement pour les types composés (has_used_by).
     * Absent des autres kinds (natives, primitives, enums…) pour ne pas
     * polluer leur JSON ni entrer en collision avec le used_by des enums. */
    if (r->has_used_by) {
        if (r->used_by_n == 0) {
            printf(",\n    \"used_by\": []");
        } else {
            printf(",\n    \"used_by\": [");
            for (int i = 0; i < r->used_by_n; i++) {
                if (i > 0) printf(",");
                printf("\n      {\"name\": ");
                json_write_cstr(r->used_by[i].name);
                printf(", \"kind\": \"%s\"}", r->used_by[i].is_type ? "type" : "native");
            }
            printf("\n    ]");
        }
        printf(",\n    \"used_by_total\": %d", r->used_by_total);
        printf(",\n    \"used_by_family\": ");
        if (r->used_by_family) json_write_cstr(r->used_by_family);
        else printf("null");
    }

    printf("\n  }");
}

/* ==================================================================
 * Bucket KEYWORD — fiches des mots-clés du langage.
 *
 * Source de vérité unique : la table KEYWORDS[] du scanner (mot →
 * TokenType) + le catalogue owl_keyword_catalog (TokenType → fiche
 * pédagogique). Les aliases FR/EN sont récupérés dynamiquement via
 * scanner_keyword_at() — jamais dupliqués dans le catalogue.
 *
 * Composés (PAS DANS, SORTIR SELON...) : pas de TokenType propre, le
 * scanner émet 2 tokens consécutifs. Lookup dédié par paire (t1, t2).
 * ================================================================== */

/* Récupère tous les aliases (FR + EN) d'un TokenType via le scanner.
 * Remplit out_buf en concaténant les aliases séparés par ", ". */
static void collect_keyword_aliases(TokenType t, char* out_buf, size_t buf_size) {
    out_buf[0] = '\0';
    size_t written = 0;
    int first = 1;
    for (int i = 0; ; i++) {
        TokenType type;
        const char* alias = scanner_keyword_at(i, &type);
        if (!alias) break;
        if (type != t) continue;
        size_t alen = strlen(alias);
        size_t need = alen + (first ? 0 : 2);
        if (written + need + 1 >= buf_size) break;
        if (!first) {
            memcpy(out_buf + written, ", ", 2);
            written += 2;
        }
        memcpy(out_buf + written, alias, alen);
        written += alen;
        out_buf[written] = '\0';
        first = 0;
    }
}

static void print_keyword_sense_text(const OwlKeywordSense* s,
                                     int multi, int index, DocLang lang) {
    const char* label = (lang == DOC_LANG_EN && s->label_en && *s->label_en)
                          ? s->label_en : s->label_fr;
    const char* desc  = (lang == DOC_LANG_EN && s->descEN && *s->descEN)
                          ? s->descEN : s->descFR;
    if (multi) {
        printf("\n%sSens %d — %s%s\n", C_SECTION, index + 1, label, C_RESET);
    }
    printf("  %s%s%s %s%s%s\n",
           C_DIM, dl(lang, "Syntaxe :", "Syntax:"), C_RESET,
           C_KEYWORD, s->syntax, C_RESET);
    if (desc && *desc) {
        printf("  %s\n", desc);
    }
    if (s->example && *s->example) {
        printf("\n  %sExemple%s\n", C_DIM, C_RESET);
        /* render_code_line_owl ligne par ligne, indenté à 6 colonnes. */
        const char* p = s->example;
        int brace_depth = 0;
        while (*p) {
            printf("      ");
            const char* line_start = p;
            while (*p && *p != '\n') p++;
            render_code_line_owl(line_start, p - line_start, &brace_depth);
            putchar('\n');
            if (*p == '\n') p++;
        }
    }
}

/* Liste des kinds qui exposent un event donné. Itère tous les widgets,
 * le catalogue Etat et le catalogue sous-contrôle. Retourne le nombre
 * d'owners trouvés (capé à max) et populate `out` avec leurs labels +
 * pointer vers OwlEventDef. */
typedef struct {
    const char* kindLabel;       /* "Bouton", "État", "État (sous-contrôle)" */
    const OwlEventDef* def;
} EventOwner;

static int find_event_owners(const char* name, EventOwner* out, int max) {
    int n = 0;
    const OwlControlKindInfo* table = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = table; k->nameFR && n < max; k++) {
        OwlControlKind kind;
        if (!owl_control_kind_of_info(k, &kind)) continue;
        const OwlEventDef* ev = owl_event_catalog_find(kind, name);
        if (ev) {
            out[n].kindLabel = k->nameFR;
            out[n].def = ev;
            n++;
        }
    }
    if (n < max) {
        const OwlEventDef* ev = owl_etat_event_catalog_find(name);
        if (ev) {
            out[n].kindLabel = "État";
            out[n].def = ev;
            n++;
        }
    }
    if (n < max) {
        const OwlEventDef* ev = owl_etat_subcontrol_event_catalog_find(name);
        if (ev) {
            out[n].kindLabel = "État (sous-contrôle)";
            out[n].def = ev;
            n++;
        }
    }
    /*  — la Fenêtre expose ses events via un catalogue lifecycle
     * parallèle (hors OwlControlKind). `explain` doit le consulter aussi,
     * sinon les events clavier (surToucheAppuyée...) n'affichent jamais
     * « Fenêtre » dans « Exposé par ». */
    if (n < max) {
        const OwlEventDef* ev = owl_window_lifecycle_catalog_find(name);
        if (ev) {
            out[n].kindLabel = "Fenêtre";
            out[n].def = ev;
            n++;
        }
    }
    return n;
}

/* Recherche d'une builtin property par nom (case + accent insensible).
 * Collecte tous les kinds qui exposent cette prop. Retourne le nombre
 * d'owners trouvés (capé à max) et popule `out_owners` avec leur kindName
 * et la prop canon (premier match, identique pour les doublons inter-kinds).
 * `out_prop` reçoit la première prop trouvée (utile pour les descs). */
typedef struct {
    const char* kindName;
    const BuiltinKindProp* prop;
} BuiltinPropOwner;

typedef struct {
    const char* searchName;     /* nom à matcher (UTF-8 brut) */
    BuiltinPropOwner* owners;
    int max;
    int count;
} BuiltinPropSearchCtx;

static void builtin_prop_search_cb(const char* kindName,
                                    const BuiltinKindProp* p, void* ctx) {
    BuiltinPropSearchCtx* c = (BuiltinPropSearchCtx*)ctx;
    if (c->count >= c->max) return;

    char qry[64], buf[64];
    int qry_n = scanner_strip_accents(c->searchName, (int)strlen(c->searchName),
                                       qry, (int)sizeof qry);
    /* lowercase qry */
    for (int i = 0; i < qry_n; i++) qry[i] = (char)tolower((unsigned char)qry[i]);
    qry[qry_n] = '\0';

    /* Compare contre nameFR + nameEN après accent-strip + lowercase. */
    const char* candidates[2] = { p->nameFR, p->nameEN };
    for (int ci = 0; ci < 2; ci++) {
        if (!candidates[ci]) continue;
        int n = scanner_strip_accents(candidates[ci], (int)strlen(candidates[ci]),
                                       buf, (int)sizeof buf);
        if (n != qry_n) continue;
        bool match = true;
        for (int i = 0; i < n; i++) {
            if ((char)tolower((unsigned char)buf[i]) != qry[i]) { match = false; break; }
        }
        if (match) {
            c->owners[c->count].kindName = kindName;
            c->owners[c->count].prop = p;
            c->count++;
            return;
        }
    }
}

static int find_builtin_prop_owners(const char* name,
                                     BuiltinPropOwner* out, int max) {
    BuiltinPropSearchCtx ctx = { name, out, max, 0 };
    compiler_iter_builtin_props_full(builtin_prop_search_cb, &ctx);
    return ctx.count;
}

static int explain_builtin_prop(const char* name,
                                 const BuiltinPropOwner* owners, int n,
                                 const DocOpts* opts) {
    (void)name;
    const BuiltinKindProp* canon = owners[0].prop;
    const char* desc = (opts->lang == DOC_LANG_EN && canon->descEN && *canon->descEN)
                         ? canon->descEN : canon->descFR;

    if (opts->json) {
        printf("{\n  \"name\": ");      json_write_cstr(canon->nameFR);
        printf(",\n  \"name_en\": ");   json_write_cstr(canon->nameEN);
        printf(",\n  \"type\": ");      json_write_cstr(canon->valueType);
        printf(",\n  \"desc\": ");      json_write_cstr(desc);
        printf(",\n  \"owners\": [");
        for (int i = 0; i < n; i++) {
            if (i > 0) printf(",");
            printf(" ");
            json_write_cstr(owners[i].kindName);
        }
        printf(" ]\n}\n");
        return 0;
    }

    printf("%s.%s%s", C_NATIVE, canon->nameFR, C_RESET);
    if (canon->nameEN && strcmp(canon->nameEN, canon->nameFR) != 0) {
        printf(" %s/ .%s%s", C_DIM, canon->nameEN, C_RESET);
    }
    printf("\n  %spropriété sur primitif · %s%s%s\n\n",
           C_DIM, C_TYPE, canon->valueType ? canon->valueType : "?", C_RESET);
    if (desc && *desc) printf("  %s\n\n", desc);

    printf("%sExposée par%s\n", C_SECTION, C_RESET);
    /* Dédup les kinds (PROPS_COLLECTION est partagé file/pile/liste). */
    const char* shown[16];
    int n_shown = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < n_shown; j++) {
            if (strcmp(shown[j], owners[i].kindName) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (n_shown < 16) shown[n_shown++] = owners[i].kindName;
        printf("  %s%s%s\n", C_TYPE, owners[i].kindName, C_RESET);
    }
    printf("\n%sSyntaxe : %svar.%s%s%s ou %svar..%s%s%s\n",
           C_DIM, C_RESET, C_NATIVE, canon->nameFR, C_RESET,
           C_NATIVE, canon->nameFR, C_RESET, C_DIM);
    return 0;
}

/* Recherche des method bindings qui matchent un nom. Itère le registry
 * complet ; pour chaque native qui a methodBindings, scanne la table et
 * collecte les owners (native parente + receiver type). Dédup par couple
 * (native, receiver) — une native peut avoir 2 bindings sur le même
 * receiver (alias FR/EN/accentué) qu'on consolide. */
typedef struct {
    const OwlNativeDef* native;    /* native parente, ou NULL pour méthode pure */
    const char* receiverType;       /* "chaine", "UUID", "Chrono"… */
    int arity;                      /* arité incl. receveur ; -1 variadic ; 0 inconnu */
} MethodOwner;

/*  — filtre de receveur pour la forme qualifiée `Type.Méthode`.
 * Match bilingue + accent/casse-insensible : compare directement contre le
 * nom canon du receveur, puis résout le filtre via le registry des types
 * pour couvrir les alias EN (chaine/string, IAModele/AIModel…). */
static bool method_receiver_matches(const char* filter, const char* receiverType) {
    if (!filter) return true;  /* pas de filtre = accepte tout */
    long flen = (long)strlen(filter);
    if (word_eq_ci(filter, flen, receiverType)) return true;
    const OwlTypeDef* td = owl_registry_type_find(filter);
    if (td && td->canonical &&
        word_eq_ci(td->canonical, (long)strlen(td->canonical), receiverType))
        return true;
    return false;
}

static int find_method_bindings(const char* name, const char* recvFilter,
                                MethodOwner* out, int max) {
    int n = 0;
    int total = owl_registry_count();
    char qry_buf[64];
    int qry_n = scanner_strip_accents(name, (int)strlen(name),
                                       qry_buf, (int)sizeof qry_buf);
    for (int i = 0; i < qry_n; i++) qry_buf[i] = (char)tolower((unsigned char)qry_buf[i]);
    qry_buf[qry_n] = '\0';

    /* Source 1 : OwlNativeDef::methodBindings (déclaratif sur la def). */
    for (int i = 0; i < total && n < max; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->methodBindings) continue;
        for (const OwlMethodBinding* mb = d->methodBindings;
             mb->receiverType && mb->methodName; mb++) {
            /* Compare methodName accent-stripped + lower contre qry. */
            char mb_buf[64];
            int mb_n = scanner_strip_accents(mb->methodName,
                                              (int)strlen(mb->methodName),
                                              mb_buf, (int)sizeof mb_buf);
            if (mb_n != qry_n) continue;
            bool match = true;
            for (int k = 0; k < mb_n; k++) {
                if ((char)tolower((unsigned char)mb_buf[k]) != qry_buf[k]) {
                    match = false; break;
                }
            }
            if (!match) continue;
            if (!method_receiver_matches(recvFilter, mb->receiverType)) continue;

            /* Dédup par (native, receiver). */
            bool dup = false;
            for (int s = 0; s < n; s++) {
                if (out[s].native == d &&
                    strcmp(out[s].receiverType, mb->receiverType) == 0) {
                    dup = true; break;
                }
            }
            if (dup) continue;
            out[n].native = d;
            out[n].receiverType = mb->receiverType;
            out[n].arity = 0;  /* les bindings déclaratifs ne portent pas l'arité */
            n++;
            if (n >= max) break;
        }
    }

    /* Source 2 : method_catalog (méthodes enregistrées via
     * owl_register_method depuis les stdlib_*_init). Résout NativeFn →
     * OwlNativeDef par scan du registry. */
    int mcc = method_catalog_count();
    for (int i = 0; i < mcc && n < max; i++) {
        const char* typeName = NULL;
        const MethodEntry* me = method_catalog_at(i, &typeName);
        if (!me || !me->name || !typeName) continue;

        char mb_buf[64];
        int mb_n = scanner_strip_accents(me->name, (int)strlen(me->name),
                                          mb_buf, (int)sizeof mb_buf);
        if (mb_n != qry_n) continue;
        bool match = true;
        for (int k = 0; k < mb_n; k++) {
            if ((char)tolower((unsigned char)mb_buf[k]) != qry_buf[k]) {
                match = false; break;
            }
        }
        if (!match) continue;
        if (!method_receiver_matches(recvFilter, typeName)) continue;

        /* Résout NativeFn → OwlNativeDef en scannant le registry. La
         * native est typiquement marquée OWL_NAT_INTERNAL si elle n'est
         * pas exposée comme global ; sinon c'est la forme préfixée
         * publique (ChaîneVersDate, etc.). Peut rester NULL : les méthodes
         * PURES (owl_register_method sans native globale — Chat/Génère côté
         * LLM, Infère côté IAModèle) n'ont pas de def parente.  les
         * conserve avec native=NULL pour les rendre explicables (receveur +
         * signature), au lieu de les jeter silencieusement. */
        const OwlNativeDef* parent = NULL;
        for (int j = 0; j < total; j++) {
            const OwlNativeDef* d = owl_registry_at(j);
            if (d && d->impl == me->function) {
                /* Préférence : si plusieurs natives partagent le pointeur,
                 * on prend la première non-INTERNAL pour avoir une fiche
                 * user-facing. Sinon on garde la première. */
                if (!parent || (parent->flags & OWL_NAT_INTERNAL)) {
                    parent = d;
                }
            }
        }

        /* Dédup par (native, receiver) — typeName du catalog est déjà
         * le canonical FR du receiver. Pour les méthodes pures (parent
         * NULL) c'est le couple (NULL, receiver) qui dédup. */
        bool dup = false;
        for (int s = 0; s < n; s++) {
            if (out[s].native == parent &&
                strcmp(out[s].receiverType, typeName) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        out[n].native = parent;
        out[n].receiverType = typeName;
        out[n].arity = me->arity;
        n++;
    }
    return n;
}

static int explain_method(const char* name, const MethodOwner* owners, int n,
                           const DocOpts* opts) {
    if (opts->json) {
        printf("{\n  \"name\": "); json_write_cstr(name);
        printf(",\n  \"forms\": [");
        for (int i = 0; i < n; i++) {
            if (i > 0) printf(",");
            printf("\n    { \"receiver\": ");
            json_write_cstr(owners[i].receiverType);
            printf(", \"native\": ");
            if (owners[i].native) {
                json_write_cstr(owners[i].native->canonical);
                printf(", \"category\": ");
                json_write_cstr(owners[i].native->category);
            } else {
                /* Méthode pure : pas de native globale ni de catégorie. */
                printf("null");
                printf(", \"arity\": %d", owners[i].arity);
            }
            printf(" }");
        }
        printf("\n  ]\n}\n");
        return 0;
    }

    printf("%s.%s()%s\n", C_NATIVE, name, C_RESET);
    if (opts->lang == DOC_LANG_EN) {
        printf("  %smethod form — %d receiver%s%s\n\n",
               C_DIM, n, n > 1 ? "s" : "", C_RESET);
    } else {
        printf("  %sforme méthode — %d receveur%s%s\n\n",
               C_DIM, n, n > 1 ? "s" : "", C_RESET);
    }

    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owners[i].native;
        /*  — le receveur est un NOM DE TYPE (ou la sorte de widget
         * préfixée « Champ ») : forme EN canon lue au registre. */
        char rcv[128];
        const char* recv = type_str_lang(owners[i].receiverType, opts->lang,
                                         rcv, sizeof rcv);
        if (!d) {
            /*  — méthode pure : pas de native parente, donc pas de
             * desc à afficher. On rend le receveur + la signature (arité,
             * receveur exclu). Arité -1 = variadique, 0 = inconnue. */
            printf("  %s<%s>%s.%s%s%s(", C_TYPE, recv, C_RESET,
                   C_NATIVE, name, C_RESET);
            int params = owners[i].arity;
            if (params < 0) {
                printf("%s…%s", C_DIM, C_RESET);
            } else {
                for (int a = 1; a < params; a++) {
                    printf("%s%sarg%d%s", a > 1 ? ", " : "", C_DIM, a, C_RESET);
                }
            }
            printf(")\n");
            continue;
        }
        printf("  %s<%s>%s.%s%s%s()  %s≡%s  %s%s%s\n",
               C_TYPE, recv, C_RESET,
               C_NATIVE, name, C_RESET,
               C_DIM, C_RESET,
               C_NATIVE, d->canonical, C_RESET);
        if (d->category) {
            printf("    %s%s%s", C_DIM, d->category, C_RESET);
        }
        if (d->descFR && *d->descFR) {
            const char* desc = (opts->lang == DOC_LANG_EN && d->descEN && *d->descEN)
                                 ? d->descEN : d->descFR;
            /* Première ligne courte de la desc — la fiche complète est
             * accessible via `owl explain <canonical>`. */
            char first[140];
            int j = 0;
            while (desc[j] && desc[j] != '\n' && desc[j] != '.' && j < 130) {
                first[j] = desc[j]; j++;
            }
            first[j] = '\0';
            printf(" %s· %s%s", C_DIM, first, C_RESET);
        }
        printf("\n");
    }
    printf("\n%s%s : %sowl explain%s %s<canonical>%s\n",
           C_DIM,
           dl(opts->lang, "Détail d'une forme", "Details of a form"),
           C_NATIVE, C_RESET, C_TYPE, C_RESET);
    return 0;
}

static int explain_globalvar(const OwlGlobalVarDef* g, const DocOpts* opts) {
    const char* desc = (opts->lang == DOC_LANG_EN && g->descEN && *g->descEN)
                         ? g->descEN : g->descFR;

    if (opts->json) {
        printf("{\n  \"name\": ");        json_write_cstr(g->canonical);
        printf(",\n  \"name_ascii\": ");  json_write_cstr(g->nameAscii);
        printf(",\n  \"name_en\": ");     json_write_cstr(g->nameEN);
        printf(",\n  \"type\": ");        json_write_cstr(g->type);
        printf(",\n  \"read_only\": %s", g->readOnly ? "true" : "false");
        printf(",\n  \"desc\": ");        json_write_cstr(desc);
        printf("\n}\n");
        return 0;
    }

    printf("%s%s%s", C_NATIVE, g->canonical, C_RESET);
    if (g->nameAscii && strcmp(g->nameAscii, g->canonical) != 0) {
        printf(" %s(%s)%s", C_DIM, g->nameAscii, C_RESET);
    }
    if (g->nameEN && strcmp(g->nameEN, g->canonical) != 0 &&
        (!g->nameAscii || strcmp(g->nameEN, g->nameAscii) != 0)) {
        printf(" %s/ %s%s", C_DIM, g->nameEN, C_RESET);
    }
    printf("\n  %svariable globale prédéfinie · %s%s%s",
           C_DIM, C_TYPE, g->type ? g->type : "?", C_RESET);
    if (g->readOnly) printf(" %s· lecture seule%s", C_DIM, C_RESET);
    printf("\n\n");
    if (desc && *desc) printf("  %s\n", desc);
    return 0;
}

/* Voisins d'un event : autres events du premier widget owner. Permet
 * d'orienter l'utilisateur vers les events sœurs (surClic → surDoubleClic,
 * surEntrée, surSortie…). Cap à 6 affichés + comptage du reste. */
typedef struct {
    const char* names[8];
    int count;
    int total_others;
} EventNeighbors;

static void find_event_neighbors(const EventOwner* owners, int n_owners,
                                  const char* searched,
                                  EventNeighbors* out) {
    out->count = 0;
    out->total_others = 0;
    if (n_owners == 0) return;

    const OwlControlKindInfo* table = owl_control_kinds_table();
    /* Trouve le premier owner qui est un widget kind (pas État). */
    const OwlControlKindInfo* picked = NULL;
    for (const OwlControlKindInfo* k = table; k->nameFR; k++) {
        if (strcmp(k->nameFR, owners[0].kindLabel) == 0) {
            picked = k;
            break;
        }
    }
    if (!picked) {
        /* Si c'est État ou sous-contrôle, on itère le catalogue
         * correspondant. */
        if (strcmp(owners[0].kindLabel, "État") == 0) {
            int ne = owl_etat_event_catalog_count();
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_etat_event_catalog_at(i);
                if (strcasecmp(ev->canonical, searched) == 0) continue;
                if (out->count < 8) out->names[out->count++] = ev->canonical;
                else out->total_others++;
            }
        } else if (strcmp(owners[0].kindLabel, "État (sous-contrôle)") == 0) {
            int ne = owl_etat_subcontrol_event_catalog_count();
            for (int i = 0; i < ne; i++) {
                const OwlEventDef* ev = owl_etat_subcontrol_event_catalog_at(i);
                if (strcasecmp(ev->canonical, searched) == 0) continue;
                if (out->count < 8) out->names[out->count++] = ev->canonical;
                else out->total_others++;
            }
        }
        return;
    }
    OwlControlKind kind;
    if (!owl_control_kind_of_info(picked, &kind)) return;
    int ne = owl_event_catalog_count(kind);
    for (int i = 0; i < ne; i++) {
        const OwlEventDef* ev = owl_event_catalog_at(kind, i);
        if (!ev) continue;
        if (strcasecmp(ev->canonical, searched) == 0) continue;
        if (out->count < 8) out->names[out->count++] = ev->canonical;
        else out->total_others++;
    }
}

static int explain_event(const char* name, const EventOwner* owners, int n,
                         const DocOpts* opts, const RelatedInfo* related) {
    (void)related;
    const OwlEventDef* canon = owners[0].def;
    const char* lang_desc = (opts->lang == DOC_LANG_EN && canon->descEN && *canon->descEN)
                              ? canon->descEN : canon->descFR;

    if (opts->json) {
        printf("{\n  \"name\": "); json_write_cstr(canon->canonical);
        printf(",\n  \"name_en\": "); json_write_cstr(canon->nameEN);
        printf(",\n  \"desc\": "); json_write_cstr(lang_desc);
        printf(",\n  \"params\": [");
        for (int i = 0; i < canon->paramCount; i++) {
            if (i > 0) printf(",");
            printf("\n    { \"name\": ");
            json_write_cstr(canon->params[i].name);
            printf(", \"type\": ");
            json_write_cstr(canon->params[i].typeName);
            printf(", \"desc\": ");
            json_write_cstr(opts->lang == DOC_LANG_EN
                ? canon->params[i].descEN : canon->params[i].descFR);
            printf(" }");
        }
        printf("\n  ],\n  \"owners\": [");
        for (int i = 0; i < n; i++) {
            if (i > 0) printf(",");
            printf(" ");
            json_write_cstr(owners[i].kindLabel);
        }
        printf(" ]\n}\n");
        return 0;
    }

    /* Texte : header + desc + params + liste des widgets supports. */
    printf("%s%s%s", C_NATIVE, canon->canonical, C_RESET);
    if (canon->nameEN && strcmp(canon->nameEN, canon->canonical) != 0) {
        printf(" %s/ %s%s", C_DIM, canon->nameEN, C_RESET);
    }
    /*  — meme classe de pluriel en dur que « %d champ%s ». */
    printf("\n  %sevent — %s %d %s%s%s\n\n",
           C_DIM, dl(opts->lang, "exposé par", "exposed by"), n,
           dl(opts->lang, "champ", "field"), n > 1 ? "s" : "", C_RESET);

    if (lang_desc && *lang_desc) {
        printf("  %s\n\n", lang_desc);
    }

    if (canon->paramCount > 0) {
        printf("%sParamètres%s\n", C_SECTION, C_RESET);
        for (int i = 0; i < canon->paramCount; i++) {
            const OwlEventParam* p = &canon->params[i];
            printf("  %s%s%s %s:%s %s%s%s",
                   C_ARG, p->name, C_RESET,
                   C_DIM, C_RESET,
                   C_TYPE, p->typeName ? p->typeName : "?", C_RESET);
            const char* pdesc = (opts->lang == DOC_LANG_EN && p->descEN && *p->descEN)
                                  ? p->descEN : p->descFR;
            if (pdesc && *pdesc) printf("  %s%s%s", C_DIM, pdesc, C_RESET);
            printf("\n");
        }
        printf("\n");
    }

    printf("%s%s%s\n", C_SECTION, dl(opts->lang, "Exposé par", "Exposed by"),
           C_RESET);
    for (int i = 0; i < n; i++) {
        printf("  %s%s%s", C_TYPE,
               control_kind_lang(owners[i].kindLabel, opts->lang), C_RESET);
        /* Si l'event a une desc différente sur un autre widget, on indique. */
        if (i > 0 && owners[i].def != canon &&
            owners[i].def->descFR && canon->descFR &&
            strcmp(owners[i].def->descFR, canon->descFR) != 0) {
            printf(" %s%s%s", C_DIM,
                   dl(opts->lang, "(desc spécifique)", "(specific description)"),
                   C_RESET);
        }
        printf("\n");
    }
    /* Voisins : autres events du premier widget owner. */
    EventNeighbors nb;
    find_event_neighbors(owners, n, canon->canonical, &nb);
    if (nb.count > 0) {
        printf("\n%sVoisins (%s%s%s)%s\n",
               C_SECTION, C_TYPE, owners[0].kindLabel, C_SECTION, C_RESET);
        for (int i = 0; i < nb.count; i++) {
            printf("  %s%s%s%s",
                   C_NATIVE, nb.names[i], C_RESET,
                   i + 1 < nb.count ? ", " : "");
        }
        if (nb.total_others > 0) {
            printf(" %s(+%d autres)%s", C_DIM, nb.total_others, C_RESET);
        }
        printf("\n");
    }

    printf("\n%s%s %sowl explain%s %s%s%s\n",
           C_DIM, dl(opts->lang, "Détail d'un champ :", "Control detail:"),
           C_NATIVE, C_RESET, C_TYPE,
           dl(opts->lang, "<champ>", "<control>"), C_RESET);
    (void)name;
    return 0;
}

static int explain_keyword(const OwlKeywordDef* k, const DocOpts* opts,
                           const RelatedInfo* related) {
    /* Aliases dynamiques via scanner. */
    char aliases[256];
    collect_keyword_aliases(k->token, aliases, sizeof aliases);

    if (opts->json) {
        printf("{\n  \"kind\": \"keyword\"");
        printf(",\n  \"category\": ");  json_write_cstr(k->category);
        printf(",\n  \"aliases\": ");   json_write_cstr(aliases);
        printf(",\n  \"senses\": [");
        for (int i = 0; i < k->senses_count; i++) {
            const OwlKeywordSense* s = &k->senses[i];
            if (i > 0) printf(",");
            printf("\n    {\n      \"label_fr\": "); json_write_cstr(s->label_fr);
            printf(",\n      \"label_en\": ");      json_write_cstr(s->label_en);
            printf(",\n      \"syntax\": ");        json_write_cstr(s->syntax);
            /*  — desc mono-langue (--lang) ; label_fr/label_en gardés. */
            printf(",\n      \"desc\": ");          json_write_cstr(type_to_lang(s->descFR, s->descEN, opts->lang));
            printf(",\n      \"example\": ");       json_write_cstr(s->example);
            printf("\n    }");
        }
        printf("\n  ]");
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    /* Header : forme canon. Préférence : canon_display si fourni
     * (ex. "À" pour TOKEN_A — le scanner stocke "A" ASCII pour la
     * normalisation, mais la forme normative utilisateur a l'accent),
     * sinon token_type_name qui renvoie la chaîne du switch (TOKEN_SI
     * → "SI"). */
    const char* canon = k->canon_display ? k->canon_display
                                          : token_type_name(k->token);
    printf("%s%s%s %s%s%s\n",
           C_KEYWORD, canon ? canon : "?", C_RESET, C_DIM,
           dl(opts->lang, "(mot-clé)", "(keyword)"), C_RESET);
    printf("  %s%s%s\n", C_DIM,
           owl_category_display(k->category ? k->category : "",
                                opts->lang == DOC_LANG_EN), C_RESET);
    if (aliases[0]) {
        printf("  %sAliases :%s %s\n", C_DIM, C_RESET, aliases);
    }

    int multi = k->senses_count > 1;
    if (!multi) {
        printf("\n");
        print_keyword_sense_text(&k->senses[0], 0, 0, opts->lang);
    } else {
        for (int i = 0; i < k->senses_count; i++) {
            print_keyword_sense_text(&k->senses[i], 1, i, opts->lang);
        }
    }
    return 0;
}

static int explain_keyword_composite(const OwlKeywordComposite* c,
                                     const char* w1, const char* w2,
                                     const DocOpts* opts,
                                     const RelatedInfo* related) {
    if (opts->json) {
        printf("{\n  \"kind\": \"keyword_composite\"");
        printf(",\n  \"category\": "); json_write_cstr(c->category);
        printf(",\n  \"words\": [");
        printf("\n    "); json_write_cstr(w1);
        printf(",\n    "); json_write_cstr(w2);
        printf("\n  ]");
        printf(",\n  \"sense\": ");
        printf("{\n    \"label_fr\": "); json_write_cstr(c->sense->label_fr);
        printf(",\n    \"label_en\": "); json_write_cstr(c->sense->label_en);
        printf(",\n    \"syntax\": ");   json_write_cstr(c->sense->syntax);
        /*  — desc mono-langue (--lang) ; label_fr/label_en gardés. */
        printf(",\n    \"desc\": ");     json_write_cstr(type_to_lang(c->sense->descFR, c->sense->descEN, opts->lang));
        printf(",\n    \"example\": ");  json_write_cstr(c->sense->example);
        printf("\n  }");
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    printf("%s%s %s%s %s%s%s\n",
           C_KEYWORD, w1, w2, C_RESET, C_DIM,
           dl(opts->lang, "(mot-clé composé)", "(compound keyword)"), C_RESET);
    printf("  %s%s%s\n\n", C_DIM,
           owl_category_display(c->category ? c->category : "",
                                opts->lang == DOC_LANG_EN), C_RESET);
    print_keyword_sense_text(c->sense, 0, 0, opts->lang);
    return 0;
}

/* ==================================================================
 * Bucket OPERATOR — fiches des opérateurs symboliques du langage.
 *
 * Source d'identité : TokenType + display_form (chaîne brute, ex.
 * "[~~" ou "??*"). Le lookup se fait par symbole exact, pas via le
 * scanner KEYWORDS (les opérateurs ne sont pas des mots).
 *
 * Réutilise OwlKeywordSense + print_keyword_sense_text pour cohérence
 * d'affichage avec les keywords.
 * ================================================================== */

static int explain_operator(const OwlOperatorDef* o, const DocOpts* opts,
                            const RelatedInfo* related) {
    if (opts->json) {
        printf("{\n  \"kind\": \"operator\"");
        printf(",\n  \"display_form\": ");
        json_write_cstr(o->display_form);
        printf(",\n  \"category\": ");
        json_write_cstr(o->category);
        printf(",\n  \"senses\": [");
        for (int i = 0; i < o->senses_count; i++) {
            const OwlKeywordSense* s = &o->senses[i];
            if (i > 0) printf(",");
            printf("\n    {\n      \"label_fr\": "); json_write_cstr(s->label_fr);
            printf(",\n      \"label_en\": ");      json_write_cstr(s->label_en);
            printf(",\n      \"syntax\": ");        json_write_cstr(s->syntax);
            /*  — desc mono-langue (--lang) ; label_fr/label_en gardés. */
            printf(",\n      \"desc\": ");          json_write_cstr(type_to_lang(s->descFR, s->descEN, opts->lang));
            printf(",\n      \"example\": ");       json_write_cstr(s->example);
            printf("\n    }");
        }
        printf("\n  ]");
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    /* Header : symbole brut en couleur keyword + étiquette "opérateur". */
    printf("%s%s%s %s(opérateur)%s\n",
           C_KEYWORD, o->display_form ? o->display_form : "?",
           C_RESET, C_DIM, C_RESET);
    printf("  %s%s%s\n", C_DIM,
           o->category ? owl_category_display(o->category, opts->lang == DOC_LANG_EN) : "",
           C_RESET);

    int multi = o->senses_count > 1;
    if (!multi) {
        printf("\n");
        print_keyword_sense_text(&o->senses[0], 0, 0, opts->lang);
    } else {
        for (int i = 0; i < o->senses_count; i++) {
            print_keyword_sense_text(&o->senses[i], 1, i, opts->lang);
        }
    }
    return 0;
}

static int explain_native(const OwlNativeDef* d, const DocOpts* opts,
                          const RelatedInfo* related) {
    if (opts->json) {
        printf("{\n  \"kind\": \"native\"");
        printf(",\n  \"name\": ");           json_write_cstr(d->canonical);
        printf(",\n  \"name_ascii\": ");     json_write_cstr(d->canonicalAscii ? d->canonicalAscii : d->canonical);
        printf(",\n  \"name_en\": ");        json_write_cstr(d->nameEN);
        /*  — le JSON doit honorer --lang comme le fait deja la sortie
         * plain (owl_category_display) : sans ca la fiche d'aide du viewer, qui
         * consomme ce JSON, restait « Fichiers » en anglais. */
        printf(",\n  \"category\": ");
        json_write_cstr(d->category
            ? owl_category_display(d->category, opts->lang == DOC_LANG_EN)
            : NULL);
        printf(",\n  \"family\": ");         json_write_cstr(d->family);
        printf(",\n  \"desc\": ");           json_write_cstr(type_to_lang(d->descFR, d->descEN, opts->lang));
        /*  — mono-langue : desc_fr/desc_en retirés (la desc suit --lang).
         * Le nom dans l'autre langue (name_en) reste. */

        /*  — signatures structurées (forme appelable). Même
         * sentinelle de fin que le mode texte : {arity==0, params==NULL,
         * returnType==NULL, returnDesc==NULL}. Param contraint à un enum :
         * allowedOptions prioritaire sur allowedBase (cf. dump_doc_html). */
        if (d->signatures) {
            printf(",\n  \"signatures\": [");
            int first_sig = 1;
            for (const OwlSignature* s = d->signatures;
                 !(s->arity == 0 && s->params == NULL &&
                   s->returnType == NULL && s->returnDesc == NULL); s++) {
                if (!first_sig) printf(",");
                first_sig = 0;
                printf("\n    {\n      \"params\": [");
                int first_p = 1;
                if (s->params) {
                    /*  : borner a l'arite — plusieurs signatures
                     * peuvent partager le meme tableau de params (prefixe
                     * partage). Variadique (arity<0) : pas de borne. */
                    for (const OwlParam* p = s->params;
                         p->name && (s->arity < 0 || (p - s->params) < s->arity);
                         p++) {
                        if (!first_p) printf(",");
                        first_p = 0;
                        printf("\n        {\"name\": ");
                        json_write_cstr(p->name);
                        /*  — type sous sa forme EN canon (type_str_lang,
                         * meme source que le plain). desc mono-langue FR : omise
                         * en --lang en plutot que de fuiter du FR (idem plain). */
                        if (p->type) {
                            char ptbuf[256];
                            printf(", \"type\": ");
                            json_write_cstr(type_str_lang(p->type, opts->lang,
                                                          ptbuf, sizeof ptbuf));
                        }
                        if (p->desc && opts->lang != DOC_LANG_EN) {
                            printf(", \"desc\": "); json_write_cstr(p->desc);
                        }
                        const OwlEnum* en = p->allowedOptions ? p->allowedOptions
                                                              : p->allowedBase;
                        if (en) { printf(", \"enum\": "); json_write_cstr(en->name); }
                        printf("}");
                    }
                }
                if (first_p) printf("]");          /* params vides : [] sur une ligne */
                else         printf("\n      ]");
                /*  — return_type traduit (type_str_lang) ; return_desc
                 * mono-langue FR : null en --lang en (coherent avec le plain qui
                 * la supprime au lieu de fuiter du FR). */
                {
                    char rtbuf[256];
                    printf(",\n      \"return_type\": ");
                    json_write_cstr(type_str_lang(s->returnType, opts->lang,
                                                  rtbuf, sizeof rtbuf));
                }
                printf(",\n      \"return_desc\": ");
                json_write_cstr(opts->lang == DOC_LANG_EN ? NULL : s->returnDesc);
                if (s->arity < 0) printf(",\n      \"variadic\": true");
                printf("\n    }");
            }
            printf("\n  ]");
        }

        /*  — formes méthode déclarées (d->methodBindings : paires
         * receiver/nom FR+EN). Une entrée par binding, cohérent avec
         * l'itération du mode texte (section "Forme méthode"). */
        if (d->methodBindings) {
            printf(",\n  \"methods\": [");
            int first_m = 1;
            for (const OwlMethodBinding* mb = d->methodBindings;
                 mb->receiverType && mb->methodName; mb++) {
                if (!first_m) printf(",");
                first_m = 0;
                printf("\n    {\"receiver\": ");
                json_write_cstr(mb->receiverType);
                printf(", \"name\": ");
                json_write_cstr(mb->methodName);
                printf(", \"arity\": %d}", mb->arity);
            }
            printf("\n  ]");
        }

        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    printf("%s%s%s", C_NATIVE, d->canonical, C_RESET);
    if (d->canonicalAscii && strcmp(d->canonicalAscii, d->canonical) != 0) {
        printf(" %s(%s)%s", C_DIM, d->canonicalAscii, C_RESET);
    }
    /* nameEN affiché seulement s'il diffère du canonical ET du canonicalAscii —
     * éviter le "Trace / Trace" disgracieux quand FR==EN. */
    if (d->nameEN && *d->nameEN &&
        strcmp(d->nameEN, d->canonical) != 0 &&
        (!d->canonicalAscii || strcmp(d->nameEN, d->canonicalAscii) != 0)) {
        printf(" %s/ %s%s", C_DIM, d->nameEN, C_RESET);
    }
    printf("\n");
    /* Aliases legacy : noms historiques resolus vers cette native par le
     * registry (cf. champ aliases dans OwlNativeDef). On les affiche en
     * sous-ligne (ex : `owl explain fTailleFichier` pointe vers fTaille). */
    if (d->aliases && d->aliases[0]) {
        printf("  %sAliases :%s ", C_DIM, C_RESET);
        for (const char* const* a = d->aliases; *a; a++) {
            if (a != d->aliases) printf(", ");
            printf("%s", *a);
        }
        printf("\n");
    }
    printf("  %s%s%s", C_DIM,
           d->category ? owl_category_display(d->category, opts->lang == DOC_LANG_EN)
                       : dl(opts->lang, "(sans catégorie)", "(no category)"),
           C_RESET);
    if (d->family && *d->family) printf(" %s· %s%s", C_DIM, d->family, C_RESET);
    printf("\n\n");

    const char* desc = type_to_lang(d->descFR, d->descEN, opts->lang);
    if (desc && *desc) {
        printf("  %s\n", desc);
    }

    /* Signatures (forme appelable + arguments + retour). Comptage en
     * première passe pour le count du titre — utile quand une native a
     * plusieurs variantes par arité. */
    if (d->signatures) {
        int n_sigs = 0;
        for (const OwlSignature* s = d->signatures;
             !(s->arity == 0 && s->params == NULL &&
               s->returnType == NULL && s->returnDesc == NULL); s++) {
            n_sigs++;
        }
        /*  — en EN, header « Call signatures » plutôt que
         * « Signatures » nu : le mot « Signatures » est identique dans les
         * deux langues, mais la digue linguistique le rejette (elle ne peut
         * distinguer un « Signatures » EN légitime d'une fuite FR). « Call
         * signatures » lève l'ambiguïté et reste de l'anglais sobre. */
        printf("\n%s%s%s %s(%d)%s\n",
               C_SECTION, dl(opts->lang, "Signatures", "Call signatures"),
               C_RESET, C_DIM, n_sigs, C_RESET);
        int sig_count = 0;
        for (const OwlSignature* s = d->signatures;
             !(s->arity == 0 && s->params == NULL &&
               s->returnType == NULL && s->returnDesc == NULL); s++) {
            sig_count++;
            printf("  %s%s%s(", C_NATIVE, d->canonical, C_RESET);
            int p_count = 0;
            if (s->params) {
                for (const OwlParam* p = s->params;
                     p->name && (s->arity < 0 || (p - s->params) < s->arity);
                     p++) {
                    if (p_count > 0) printf(", ");
                    /*  — le NOM du paramètre reste FR : OwlParam ne
                     * porte pas de champ nameEN. Le TYPE, lui, est lu au
                     * registre sous sa forme EN canon. */
                    printf("%s%s%s", C_ARG, p->name, C_RESET);
                    if (p->type && *p->type) {
                        char tbuf[256];
                        printf(" %s:%s %s%s%s",
                               C_DIM, C_RESET, C_TYPE,
                               type_str_lang(p->type, opts->lang, tbuf, sizeof tbuf),
                               C_RESET);
                    }
                    /* Param contraint à un enum (allowedBase) : signale la
                     * famille de valeurs, comme pour les fields composés. */
                    if (p->allowedBase) {
                        printf(" %s∈ %s%s", C_DIM,
                               enum_name_lang(p->allowedBase, opts->lang), C_RESET);
                    }
                    p_count++;
                }
            }
            if (s->arity < 0) {
                if (p_count > 0) printf(", ");
                printf("%s...%s", C_DIM, C_RESET);
            }
            printf(")");
            if (s->returnType && *s->returnType) {
                char rbuf[256];
                printf(" %s→%s %s%s%s",
                       C_DIM, C_RESET, C_TYPE,
                       type_str_lang(s->returnType, opts->lang, rbuf, sizeof rbuf),
                       C_RESET);
            }
            printf("\n");

            /* Détail des paramètres (description) sur lignes suivantes.
             *  — OwlParam.desc / OwlSignature.returnDesc sont
             * mono-langue (FR canon, pas de champ EN au registry). En
             * `--lang en` on ne les imprime pas plutôt que de fuiter du FR ;
             * les noms/types/retours structurés restent visibles. */
            if (opts->lang != DOC_LANG_EN) {
                if (s->params) {
                    for (const OwlParam* p = s->params;
                         p->name && (s->arity < 0 || (p - s->params) < s->arity);
                         p++) {
                        if (p->desc && *p->desc) {
                            printf("      %s%s%s  %s%s%s\n",
                                   C_ARG, p->name, C_RESET,
                                   C_DIM, p->desc, C_RESET);
                        }
                    }
                }
                if (s->returnDesc && *s->returnDesc) {
                    printf("      %s→%s   %s%s%s\n",
                           C_DIM, C_RESET, C_DIM, s->returnDesc, C_RESET);
                }
            }
        }
        if (sig_count == 0) {
            printf("  %s%s%s\n", C_DIM,
                   dl(opts->lang, "(aucune signature déclarée)",
                                  "(no declared signature)"), C_RESET);
        }
    }

    /* Méthodes prefix associées */
    if (d->methodBindings) {
        printf("\n%s%s%s\n", C_SECTION,
               dl(opts->lang, "Forme méthode", "Method form"), C_RESET);
        for (const OwlMethodBinding* mb = d->methodBindings;
             mb->receiverType && mb->methodName; mb++) {
            char rcv[128];
            printf("  %s%s%s.%s%s%s",
                   C_TYPE, type_str_lang(mb->receiverType, opts->lang,
                                         rcv, sizeof rcv), C_RESET,
                   C_NATIVE, mb->methodName, C_RESET);
            if (mb->arity >= 0) printf(" %s(%s %d)%s", C_DIM,
                                       dl(opts->lang, "arité", "arity"),
                                       mb->arity, C_RESET);
            printf("\n");
        }
    }

    print_scenarios_for_symbol(d->canonical,
                               d->canonicalAscii ? d->canonicalAscii : NULL,
                               opts->lang);
    return 0;
}

/* Nom d'affichage d'un champ selon la langue : forme EN canon si demandee
 * et disponible (invariant bilingue n°3), sinon forme FR accentuee. */
static const char* field_display_name(const OwlField* f, DocLang lang) {
    if (lang == DOC_LANG_EN && f->nameEN && *f->nameEN) return f->nameEN;
    return f->nameAccented ? f->nameAccented : f->name;
}

/*  — accesseur d'affichage d'une propriete DSL dans la liste
 * Properties d'un kind. En --lang en la forme EN est adjointe a la forme
 * canon FR, comme la section Events du MEME ecran affiche deja
 * « Filtre()  / Filter » : sans elle, `explain Report --lang en` listait
 * « ..MargeHaut » alors que le lecteur anglophone ecrit « ..TopMargin ».
 * Meme condition de declenchement que l'en-tete de `explain <propriete>`
 * (l'alias EN n'apparait qu'en mode EN : en FR l'utilisateur tape la
 * forme canon). */
static void dsl_prop_accessor(const OwlControlPropDef* def, DocLang lang,
                              char* buf, size_t n) {
    if (lang == DOC_LANG_EN && def->nameEN && *def->nameEN &&
        strcmp(def->nameEN, def->nameFR) != 0 &&
        (!def->nameAscii || strcmp(def->nameEN, def->nameAscii) != 0)) {
        snprintf(buf, n, "..%s / ..%s", def->nameFR, def->nameEN);
    } else {
        snprintf(buf, n, "..%s", def->nameFR);
    }
}

static int explain_type(const OwlTypeDef* t, const DocOpts* opts,
                        const RelatedInfo* related) {
    if (opts->json) {
        printf("{\n  \"kind\": \"type\"");
        printf(",\n  \"name\": ");           json_write_cstr(t->canonical);
        printf(",\n  \"name_ascii\": ");     json_write_cstr(t->canonicalAscii ? t->canonicalAscii : t->canonical);
        printf(",\n  \"name_en\": ");        json_write_cstr(t->nameEN);
        printf(",\n  \"category\": ");       json_write_cstr(t->category);
        printf(",\n  \"desc\": ");           json_write_cstr(type_to_lang(t->descFR, t->descEN, opts->lang));
        /* coercible : true si le type accepte `var est un T = <expr>` via
         * __InstanceCoerce. Consomme par MCP / IDE pour proposer la
         * coerce en autocomplete sans deviner. */
        printf(",\n  \"coercible\": %s", t->coercible ? "true" : "false");
        printf(",\n  \"fields\": [\n");
        int first = 1;
        if (t->fields) {
            for (const OwlField* f = t->fields; f->name; f++) {
                if (!first) printf(",\n");
                first = 0;
                printf("    {\n      \"name\": "); json_write_cstr(f->name);
                if (f->nameAccented) { printf(",\n      \"name_accented\": "); json_write_cstr(f->nameAccented); }
                if (f->nameEN)       { printf(",\n      \"name_en\": ");       json_write_cstr(f->nameEN); }
                printf(",\n      \"type\": ");        json_write_cstr(f->type);
                printf(",\n      \"default\": ");     json_write_cstr(default_display(f->defaultValue, opts->lang));
                printf(",\n      \"desc\": ");        json_write_cstr(type_to_lang(f->descFR, f->descEN, opts->lang));
                if (f->allowedBase)    { printf(",\n      \"enum_base\": ");    json_write_cstr(f->allowedBase->name); }
                if (f->allowedOptions) { printf(",\n      \"enum_options\": "); json_write_cstr(f->allowedOptions->name); }
                printf("\n    }");
            }
        }
        printf("\n  ]");

        /*  — méthodes fluent : natives ayant un methodBinding sur
         * ce type (même sélection que print_methods_for_type : une entrée
         * par native, nom lang-aware via preferred_method_binding). */
        {
            int total = owl_registry_count();
            int first_m = 1;
            for (int i = 0; i < total; i++) {
                const OwlNativeDef* d = owl_registry_at(i);
                if (d->flags & OWL_NAT_INTERNAL) continue;
                const OwlMethodBinding* mb =
                    preferred_method_binding(d, t->canonical, opts->lang);
                if (!mb) continue;
                if (first_m) printf(",\n  \"methods\": [");
                else         printf(",");
                first_m = 0;
                printf("\n    {\"receiver\": ");
                json_write_cstr(mb->receiverType);
                printf(", \"name\": ");
                json_write_cstr(mb->methodName);
                printf(", \"native\": ");
                json_write_cstr(d->canonical);
                printf("}");
            }
            if (!first_m) printf("\n  ]");
        }

        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    printf("%s%s%s %s(%s)%s\n", C_TYPE, t->canonical, C_RESET, C_DIM,
           dl(opts->lang, "type composé", "composite type"), C_RESET);
    if (t->canonicalAscii && strcmp(t->canonicalAscii, t->canonical) != 0) {
        printf("  %sASCII : %s%s\n", C_DIM, t->canonicalAscii, C_RESET);
    }
    printf("  %s%s%s\n", C_DIM,
           t->category ? owl_category_display(t->category, opts->lang == DOC_LANG_EN)
                       : dl(opts->lang, "(sans catégorie)", "(no category)"),
           C_RESET);

    /* Mention coerce-able : `var est un T = <expr>` accepte une coercion
     * declaration-pilote vers ce type. Le parser est enregistre via
     * owl_register_instance_coerce ; le compilo emet __InstanceCoerce
     * automatiquement. Si t->coercible=false, aucune coerce — l'user
     * doit assigner explicitement le bon type. */
    if (t->coercible) {
        const char* canonAscii = t->canonicalAscii ? t->canonicalAscii : t->canonical;
        if (opts->lang == DOC_LANG_EN) {
            printf("  %sCoerce-able%s — `%svar is a %s = <expr>%s` parses "
                   "<expr> automatically\n",
                   C_CONST, C_RESET, C_DIM, canonAscii, C_RESET);
        } else {
            printf("  %sCoerce-able%s — `%svar est un %s = <expr>%s` parse "
                   "<expr> automatiquement\n",
                   C_CONST, C_RESET, C_DIM, canonAscii, C_RESET);
        }
    }
    printf("\n");

    const char* desc = type_to_lang(t->descFR, t->descEN, opts->lang);
    if (desc && *desc) printf("  %s\n\n", desc);

    if (t->fields) {
        int n_fields = 0;
        for (const OwlField* f = t->fields; f->name; f++) n_fields++;
        printf("%s%s%s %s(%d)%s\n",
               C_SECTION, dl(opts->lang, "Propriétés", "Properties"),
               C_RESET, C_DIM, n_fields, C_RESET);
        /* Largeur max du nom pour alignement */
        int wmax = 0;
        for (const OwlField* f = t->fields; f->name; f++) {
            const char* display = field_display_name(f, opts->lang);
            int w = (int)strlen(display);
            if (w > wmax) wmax = w;
        }
        if (wmax < 12) wmax = 12;
        if (wmax > 28) wmax = 28;

        for (const OwlField* f = t->fields; f->name; f++) {
            const char* display = field_display_name(f, opts->lang);
            const char* fdesc   = type_to_lang(f->descFR, f->descEN, opts->lang);
            char ftbuf[256];
            printf("  %s%-*s%s  %s%s%s",
                   C_CONST, wmax, display, C_RESET,
                   C_TYPE,  f->type
                              ? type_str_lang(f->type, opts->lang, ftbuf, sizeof ftbuf)
                              : "", C_RESET);
            if (f->defaultValue && *f->defaultValue && strcmp(f->defaultValue, "\"\"") != 0) {
                printf(" %s= %s%s", C_DIM,
                       default_display(f->defaultValue, opts->lang), C_RESET);
            }
            if (f->allowedBase) {
                printf(" %s∈ %s%s", C_DIM,
                       enum_name_lang(f->allowedBase, opts->lang), C_RESET);
            }
            printf("\n");
            if (fdesc && *fdesc) {
                printf("      %s%s%s\n", C_DIM, fdesc, C_RESET);
            }
        }
    }

    /* Méthodes fluent : natives ayant un methodBinding sur ce type. */
    print_methods_for_type(t->canonical, opts->lang);
    if (t->canonicalAscii && strcmp(t->canonicalAscii, t->canonical) != 0) {
        /* Le receiverType côté registry est typiquement ASCII (sans
         * accents) — on retente sur canonicalAscii si différent pour
         * couvrir les types accentués (httpRequête → httpRequete). */
        /* Note : le résultat est cumulatif visuellement — mais comme la
         * comparaison word_eq_ci est accent-insensible, le 2e appel ne
         * trouvera RIEN de plus que le 1er. Pas de doublon. */
    }

    print_scenarios_for_symbol(t->canonical,
                               t->canonicalAscii ? t->canonicalAscii : NULL,
                               opts->lang);
    return 0;
}

/*  — reverse lookup "Utilisé par" pour une énumération.
 *
 * Une enum peut contraindre soit un paramètre de native (OwlParam.
 * allowedBase/allowedOptions), soit un champ de type composé (OwlField.
 * allowedBase/allowedOptions). On itère les deux catalogues et on
 * collecte les NOMS canoniques (natives + types) qui la consomment,
 * dédupliqués (un même symbole listé une fois même s'il a plusieurs
 * params/champs sur la même enum). L'identité enum se compare par
 * pointeur : tous les sites pointent vers le même OwlEnum const du
 * registry. */
typedef struct {
    const char* names[256];
    int         is_type[256];   /* 1 = type composé, 0 = native */
    int         count;
} EnumUsedBy;

static void enum_usedby_add(EnumUsedBy* ub, const char* name, int is_type) {
    if (!name) return;
    for (int i = 0; i < ub->count; i++) {
        if (ub->names[i] == name) return;   /* canon strings interned-stable */
    }
    if (ub->count >= 256) return;
    ub->names[ub->count]   = name;
    ub->is_type[ub->count] = is_type;
    ub->count++;
}

static void enum_compute_usedby(const OwlEnum* e, EnumUsedBy* ub) {
    ub->count = 0;
    if (!e) return;

    /* Natives : signatures[].params[].allowedBase/allowedOptions. */
    int nn = owl_registry_count();
    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || (d->flags & OWL_NAT_INTERNAL) || !d->signatures) continue;
        int matched = 0;
        for (const OwlSignature* s = d->signatures;
             !(s->arity == 0 && s->params == NULL &&
               s->returnType == NULL && s->returnDesc == NULL); s++) {
            if (!s->params) continue;
            /*  : ne scanner que les params actifs de cette arite
             * (prefixe partage) ; variadique (arity<0) = tous. */
            for (const OwlParam* p = s->params;
                 p->name && (s->arity < 0 || (p - s->params) < s->arity);
                 p++) {
                if (p->allowedBase == e || p->allowedOptions == e) { matched = 1; break; }
            }
            if (matched) break;
        }
        if (matched) enum_usedby_add(ub, d->canonical, 0);
    }

    /* Types composés : fields[].allowedBase/allowedOptions. */
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->fields) continue;
        int matched = 0;
        for (const OwlField* f = t->fields; f->name; f++) {
            if (f->allowedBase == e || f->allowedOptions == e) { matched = 1; break; }
        }
        if (matched) enum_usedby_add(ub, t->canonical, 1);
    }
}

/* Section texte "Utilisé par" (omise si vide). */
static void print_enum_usedby_text(const EnumUsedBy* ub) {
    if (ub->count == 0) return;
    printf("\n%sUtilisé par%s %s(%d)%s\n",
           C_SECTION, C_RESET, C_DIM, ub->count, C_RESET);
    for (int i = 0; i < ub->count; i++) {
        printf("  %s%s%s %s(%s)%s\n",
               ub->is_type[i] ? C_TYPE : C_NATIVE, ub->names[i], C_RESET,
               C_DIM, ub->is_type[i] ? "type" : "native", C_RESET);
    }
}

/* Champ JSON "used_by" (omis si vide ; pas de virgule de tête,
 * le caller l'insère). */
static void print_enum_usedby_json(const EnumUsedBy* ub) {
    printf("\"used_by\": [");
    for (int i = 0; i < ub->count; i++) {
        if (i > 0) printf(",");
        printf("\n    {\"name\": ");
        json_write_cstr(ub->names[i]);
        printf(", \"kind\": \"%s\"}", ub->is_type[i] ? "type" : "native");
    }
    printf("\n  ]");
}

/*  — dedup a l'AFFICHAGE des valeurs d'un enum. Un enum peut porter
 * dans `.values` des alias ASCII sans-accent qui doublent leur jumelle
 * accentuee (ex `GrisFonce` double `GrisFoncé` dans l'enum couleur). Ces alias
 * sont load-bearing : ils alimentent la wordlist TextMate accent-sensible via
 * `--vscode-data`, donc on ne les retire PAS de `.values`. Mais `owl explain`
 * ne doit pas les lister deux fois. Retourne 1 si values[idx] a une forme
 * accent-strippee (casse-insensible) deja apparue a un indice anterieur — donc
 * a masquer au rendu. La forme canon accentuee precede l'alias ASCII dans le
 * tableau, c'est donc elle qui reste. O(n²), n petit. */
static int enum_value_is_display_dup(const char* const* values, int idx) {
    for (int j = 0; j < idx; j++) {
        if (word_eq_ci(values[idx], (long)strlen(values[idx]), values[j]))
            return 1;
    }
    return 0;
}

static int explain_enum(const OwlEnum* e, const DocOpts* opts,
                        const RelatedInfo* related) {
    EnumUsedBy ub;
    enum_compute_usedby(e, &ub);
    if (opts->json) {
        printf("{\n  \"kind\": \"enum\"");
        printf(",\n  \"name\": ");        json_write_cstr(e->name);
        printf(",\n  \"name_ascii\": ");  json_write_cstr(e->nameAscii ? e->nameAscii : e->name);
        printf(",\n  \"name_en\": ");     json_write_cstr(e->nameEN);
        printf(",\n  \"category\": ");    json_write_cstr(e->category);
        printf(",\n  \"combinable\": %s", e->combinable ? "true" : "false");
        printf(",\n  \"desc\": ");        json_write_cstr(type_to_lang(e->descFR, e->descEN, opts->lang));
        printf(",\n  \"values\": [");
        if (e->values) {
            int first = 1;
            for (int i = 0; e->values[i]; i++) {
                if (enum_value_is_display_dup(e->values, i)) continue;
                if (!first) printf(",");
                first = 0;
                printf("\n    ");
                json_write_cstr(e->values[i]);
            }
        }
        printf("\n  ]");
        if (ub.count) { printf(",\n  "); print_enum_usedby_json(&ub); }
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    printf("%s%s%s %s(%s%s)%s\n",
           C_TYPE, e->name, C_RESET,
           C_DIM, dl(opts->lang, "énumération", "enumeration"),
           e->combinable ? ", combinable" : "", C_RESET);
    /* La categorie est AFFICHEE localisee (comme les chemins list/cat et le
     * detail native/type/const) ; le filtre `owl list --cat <cat>` reste sur
     * la forme canonique FR — separation display vs filtre. */
    printf("  %s%s%s\n\n", C_DIM,
           e->category ? owl_category_display(e->category, opts->lang == DOC_LANG_EN)
                       : dl(opts->lang, "(sans catégorie)", "(uncategorized)"),
           C_RESET);

    const char* desc = type_to_lang(e->descFR, e->descEN, opts->lang);
    if (desc && *desc) printf("  %s\n\n", desc);

    if (e->values) {
        int n_values = 0;
        for (int i = 0; e->values[i]; i++)
            if (!enum_value_is_display_dup(e->values, i)) n_values++;
        printf("%s%s%s %s(%d)%s\n",
               C_SECTION, dl(opts->lang, "Valeurs", "Values"), C_RESET,
               C_DIM, n_values, C_RESET);
        /*  — les membres sont des SYMBOLES : en --lang en on affiche
         * leur forme EN du registre a cote du canon FR, comme la section
         * Events le fait pour les noms d'evenements. Pas de traduction : un
         * nom de constante qui n'a pas d'alias EN reste tel quel. */
        for (int i = 0; e->values[i]; i++) {
            if (enum_value_is_display_dup(e->values, i)) continue;
            const char* v = e->values[i];
            const char* ven = symbol_lang(v, opts->lang);
            if (ven != v && strcmp(ven, v) != 0) {
                printf("  %s%s%s %s/ %s%s\n", C_CONST, v, C_RESET,
                       C_DIM, ven, C_RESET);
            } else {
                printf("  %s%s%s\n", C_CONST, v, C_RESET);
            }
        }
    }
    print_enum_usedby_text(&ub);
    return 0;
}

/* Lookup case+accent insensible d'un widget kind par nom.
 * Pour les widget kinds *runtime* (is_widget=true), out_kind reçoit la
 * valeur d'OwlControlKind correspondante. Pour les sous-blocs DSL
 * (is_widget=false : Etat, EnTeteDocument, Corps, BasDePage, …),
 * out_kind est mis à -1 — l'appelant doit router sur le catalogue
 * approprié via le name de l'info.
 *
 * Retourne info (pointeur dans CONTROL_KINDS) si trouvé, NULL sinon. */
static const OwlControlKindInfo*
find_widget_kind(const char* name, int* out_kind) {
    if (!name || !*name) return NULL;
    long nlen = (long)strlen(name);
    /* Itération directe sur la table : permet de matcher les sous-blocs
     * (is_widget=false) que `owl_control_kind_from_token` rejette. */
    const OwlControlKindInfo* t = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = t; k && k->nameFR; k++) {
        int hit =
            (k->nameFR    && word_eq_ci(name, nlen, k->nameFR))    ||
            (k->nameAscii && word_eq_ci(name, nlen, k->nameAscii)) ||
            (k->nameEN    && word_eq_ci(name, nlen, k->nameEN));
        if (!hit) continue;
        if (out_kind) {
            if (k->is_widget) {
                OwlControlKind kind = OWL_CONTROL_LABEL;
                if (owl_control_kind_of_info(k, &kind))
                    *out_kind = (int)kind;
                else
                    *out_kind = -1;
            } else {
                *out_kind = -1;  /* sous-bloc DSL : pas de OwlControlKind */
            }
        }
        return k;
    }
    return NULL;
}

/* Source du catalogue d'events pour le rendu `explain` d'un kind.
 *
 * Les kinds non-widget ne sont pas indexés par OwlControlKind : chacun
 * route vers un catalogue parallèle distinct — ou n'a aucun event. On ne
 * peut donc pas router « tout non-widget vers Etat » (Colonne / les blocs
 * de menu n'ont RIEN à voir avec un rapport). */
typedef enum {
    KIND_EV_WIDGET,       /* owl_event_catalog(kind) — widgets réels */
    KIND_EV_ETAT,         /* owl_etat_event_catalog — Etat + ses sous-blocs */
    KIND_EV_MENU_OPTION,  /* owl_menu_option_event_catalog — OptionMenu */
    KIND_EV_NONE          /* non-widget sans event (Colonne, MenuPrincipal…) */
} KindEventSource;

/* Retourne le contexte parent d'un widget kind sous forme de texte
 * lisible (« fait partie d'un État » / « part of a Report »), ou NULL si
 * le kind est top-level.
 *
 * Le texte EN cite les noms de kind sous leur forme canon EN (Report,
 * Table, MainMenu, Submenu) : citer « Etat » dans une phrase anglaise
 * donnerait au lecteur un mot introuvable dans son propre code.
 *
 * Convention OWL :
 *   - Etat est top-level (un rapport ne s'imbrique pas)
 *   - EnTeteDocument / HautDePage / Corps / BasDePage / FinDeDocument /
 *     Iteration → enfants directs d'un Etat
 *   - colonne → enfant direct d'une Table
 *   - Fenêtre est top-level
 *   - Tous les autres widgets (Bouton, Libellé, Saisie, ...) → peuvent
 *     vivre dans Fenêtre OU Conteneur OU ZoneRépétée
 */
static int kind_name_eq(const char* a, const char* b);

static const char* widget_kind_parent_context(const OwlControlKindInfo* info,
                                              int lang) {
    int en = (lang == DOC_LANG_EN);
    if (!info || !info->nameFR) return NULL;
    if (!info->is_widget) {
        /* Sous-bloc DSL. Chaque famille a son parent structurel propre —
         * on ne route pas « tout non-widget vers Etat ». */
        if (kind_name_eq(info->nameFR, "Etat"))
            return NULL;                       /* top-level */
        if (kind_name_eq(info->nameFR, "colonne"))
            return en ? "part of a Table" : "fait partie d'une Table";
        /* Famille menu : le menu principal se déclare dans une Fenêtre,
         * les sous-blocs s'imbriquent dans un SousMenu. */
        if (kind_name_eq(info->nameFR, "MenuPrincipal"))
            return en ? "can be declared inside a Window"
                      : "peut être déclaré dans une Fenêtre";
        if (kind_name_eq(info->nameFR, "SousMenu"))
            return en ? "part of a MainMenu or a Submenu"
                      : "fait partie d'un MenuPrincipal ou d'un SousMenu";
        if (kind_name_eq(info->nameFR, "OptionMenu"))
            return en ? "part of a Submenu" : "fait partie d'un SousMenu";
        if (kind_name_eq(info->nameFR, "MenuSéparateur"))
            return en ? "part of a Submenu" : "fait partie d'un SousMenu";
        /* Sous-blocs structurels d'un Etat (EnTeteDocument, Corps, ...). */
        return en ? "part of a Report" : "fait partie d'un État";
    }
    /* Widget runtime — Fenêtre n'a pas de parent. */
    if (word_eq_ci(info->nameFR, (long)strlen(info->nameFR), "Fenêtre"))
        return NULL;
    return en ? "can be declared inside Window, Container or Looper"
              : "peut être déclaré dans Fenêtre, Conteneur ou ZoneRépétée";
}

/* Compare un nom de kind du registry (ex. "Bouton") avec une chaîne
 * cible — case+accent insensible. */
static int kind_name_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    return word_eq_ci(a, (long)strlen(a), b);
}

/* Classe un kind vers son catalogue d'events. Les non-widget sont routés
 * par famille (nom canon FR, résolu par find_widget_kind quelle que soit
 * la langue d'entrée) — pas de « tout non-widget = Etat » : Colonne et les
 * blocs de menu ne sont pas des rapports. */
static KindEventSource kind_event_source(const OwlControlKindInfo* info) {
    if (!info) return KIND_EV_NONE;
    if (info->is_widget) return KIND_EV_WIDGET;
    if (kind_name_eq(info->nameFR, "OptionMenu")) return KIND_EV_MENU_OPTION;
    /* Famille Etat : le rapport lui-même + ses sous-blocs structurels.
     * Ces kinds partagent le catalogue lifecycle Etat (Filtre/Lecture/
     * Fermeture). Colonne, MenuPrincipal, SousMenu, MenuSéparateur en sont
     * exclus → aucun event affiché. */
    static const char* const ETAT_FAMILY[] = {
        "Etat", "EnTeteDocument", "HautDePage", "Corps",
        "BasDePage", "FinDeDocument", "Iteration", NULL
    };
    for (int i = 0; ETAT_FAMILY[i]; i++)
        if (kind_name_eq(info->nameFR, ETAT_FAMILY[i])) return KIND_EV_ETAT;
    return KIND_EV_NONE;
}

static int kind_event_count(KindEventSource src, OwlControlKind kind) {
    switch (src) {
        case KIND_EV_WIDGET:      return owl_event_catalog_count(kind);
        case KIND_EV_ETAT:        return owl_etat_event_catalog_count();
        case KIND_EV_MENU_OPTION: return owl_menu_option_event_catalog_count();
        case KIND_EV_NONE:        return 0;
    }
    return 0;
}

static const OwlEventDef* kind_event_at(KindEventSource src,
                                        OwlControlKind kind, int i) {
    switch (src) {
        case KIND_EV_WIDGET:      return owl_event_catalog_at(kind, i);
        case KIND_EV_ETAT:        return owl_etat_event_catalog_at(i);
        case KIND_EV_MENU_OPTION: return owl_menu_option_event_catalog_at(i);
        case KIND_EV_NONE:        return NULL;
    }
    return NULL;
}

static int explain_widget(const OwlControlKindInfo* info, int kind_or_neg1,
                          const DocOpts* opts, const RelatedInfo* related) {
    /* Quand kind_or_neg1 == -1, on est sur un sous-bloc DSL (Etat & co) :
     * pas indexable dans owl_event_catalog. On route vers le catalogue
     * Etat séparé. Le caller (cli_doc_explain) passe -1 dans ce cas. */
    KindEventSource evsrc = kind_event_source(info);
    OwlControlKind kind = (OwlControlKind)(kind_or_neg1 >= 0 ? kind_or_neg1 : 0);
    if (opts->json) {
        printf("{\n  \"kind\": \"widget\"");
        printf(",\n  \"name\": ");      json_write_cstr(info->nameFR);
        printf(",\n  \"name_ascii\": "); json_write_cstr(info->nameAscii);
        printf(",\n  \"name_en\": ");   json_write_cstr(info->nameEN);
        printf(",\n  \"is_widget\": %s", info->is_widget ? "true" : "false");
        /* Propriete par defaut visee par `WIDGET = X` (affectation directe).
         * Cf OwlControlKindInfo.defaultProperty (ui_decl.h). null = widget
         * compose qui refuse l'affectation directe au compile-time. */
        if (info->defaultProperty) {
            printf(",\n  \"default_property\": ");
            json_write_cstr(info->defaultProperty);
        } else {
            printf(",\n  \"default_property\": null");
        }

        /* Propriétés DSL */
        printf(",\n  \"properties\": [");
        int n = owl_dsl_props_count();
        int first = 1;
        for (int i = 0; i < n; i++) {
            const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
            if (!e || !e->kind || !e->def) continue;
            const char* canon = info->nameAscii ? info->nameAscii : info->nameFR;
            if (!kind_name_eq(e->kind, canon)) continue;
            const OwlControlPropDef* def = e->def;
            if (!first) printf(",");
            first = 0;
            printf("\n    {\n      \"name\": ");
            json_write_cstr(def->nameFR);
            if (def->nameAscii) {
                printf(",\n      \"name_ascii\": "); json_write_cstr(def->nameAscii);
            }
            if (def->nameEN) {
                printf(",\n      \"name_en\": ");    json_write_cstr(def->nameEN);
            }
            printf(",\n      \"type\": ");       json_write_cstr(prop_type_name_lang(def->type, opts->lang));
            if (def->enum_name) {
                printf(",\n      \"enum\": "); json_write_cstr(symbol_lang(def->enum_name, opts->lang));
            }
            if (def->object_type) {
                printf(",\n      \"object_type\": "); json_write_cstr(def->object_type);
            }
            printf(",\n      \"desc\": ");
            json_write_cstr(type_to_lang(def->descFR, def->descEN, opts->lang));
            printf("\n    }");
        }
        printf("\n  ]");

        /* Events — route sur le catalogue Etat séparé si sous-bloc. */
        printf(",\n  \"events\": [");
        int ec = kind_event_count(evsrc, kind);
        first = 1;
        for (int i = 0; i < ec; i++) {
            const OwlEventDef* ev = kind_event_at(evsrc, kind, i);
            if (!ev) continue;
            if (!first) printf(",");
            first = 0;
            printf("\n    {\n      \"name\": "); json_write_cstr(ev->canonical);
            if (ev->nameEN) { printf(",\n      \"name_en\": "); json_write_cstr(ev->nameEN); }
            printf(",\n      \"desc\": ");
            json_write_cstr(type_to_lang(ev->descFR, ev->descEN, opts->lang));
            printf(",\n      \"params\": [");
            for (int j = 0; j < ev->paramCount && ev->params; j++) {
                if (j > 0) printf(",");
                printf("\n        {\"name\": ");
                json_write_cstr(ev->params[j].name);
                printf(", \"type\": ");
                json_write_cstr(ev->params[j].typeName);
                printf(", \"desc\": ");
                json_write_cstr(type_to_lang(ev->params[j].descFR,
                                              ev->params[j].descEN, opts->lang));
                printf("}");
            }
            printf("\n      ]\n    }");
        }
        printf("\n  ]");
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    /* Sortie texte — format aligné sur les natives :
     *   Fenêtre (Fenetre) / Window
     *     widget
     * La forme ASCII et l'EN ne s'affichent que si elles diffèrent du nameFR. */
    printf("%s%s%s", C_TYPE, info->nameFR, C_RESET);
    if (info->nameAscii && strcmp(info->nameAscii, info->nameFR) != 0) {
        printf(" %s(%s)%s", C_DIM, info->nameAscii, C_RESET);
    }
    if (info->nameEN && strcmp(info->nameEN, info->nameFR) != 0 &&
        (!info->nameAscii || strcmp(info->nameEN, info->nameAscii) != 0)) {
        printf(" %s/ %s%s", C_DIM, info->nameEN, C_RESET);
    }
    printf("\n  %s%s%s",
           C_DIM,
           opts->lang == DOC_LANG_EN ? (info->is_widget ? "control" : "sub-block")
                                     : (info->is_widget ? "champ"   : "sous-bloc"),
           C_RESET);
    {
        const char* parent_ctx = widget_kind_parent_context(info, opts->lang);
        if (parent_ctx) printf(" %s— %s%s", C_DIM, parent_ctx, C_RESET);
    }
    printf("\n");

    /* Propriete par defaut : affectation directe `WIDGET = X` cible cette
     * prop. NULL = widget compose qui refuse l'affectation scalaire. */
    if (info->is_widget) {
        if (info->defaultProperty) {
            /*  — le nom du widget et celui de la prop sont des
             * SYMBOLES : forme EN du catalogue / du registre, pas une
             * traduction. L'exemple doit etre copiable tel quel. */
            const char* kindName = (opts->lang == DOC_LANG_EN && info->nameEN)
                                     ? info->nameEN : info->nameFR;
            const char* propName = dsl_prop_name_lang(info->defaultProperty,
                                                      opts->lang);
            printf("  %s%s%s ..%s — `%s = X` %s `%s..%s = X`\n",
                   C_DIM, dl(opts->lang, "Propriété par défaut :",
                             "Default property:"), C_RESET,
                   propName, kindName,
                   dl(opts->lang, "équivaut à", "is equivalent to"),
                   kindName, propName);
        } else {
            printf("  %s%s%s\n",
                   C_DIM, dl(opts->lang, "Affectation directe non supportée.",
                             "Direct assignment is not supported."), C_RESET);
        }
    }
    /*  — la fiche Table ne disait rien de la gestion des colonnes
     * (support #308). Renvoi explicite : chaque colonne est un sous-bloc
     * Colonne, sa fiche documente ses propriétés. Le renvoi cite la commande
     * d'exploration pour rester copiable tel quel. */
    if (info->is_widget &&
        word_eq_ci(info->nameFR, (long)strlen(info->nameFR), "Table")) {
        printf("  %s%s%s\n",
               C_DIM,
               dl(opts->lang,
                  "Colonnes : chaque colonne est un sous-bloc Colonne "
                  "(Libellé, Type, Largeur). Détail : owl explain Colonne.",
                  "Columns: each column is a Column sub-block "
                  "(Caption, Type, Width). Details: owl explain Column."),
               C_RESET);
        /* La police/couleur globale du champ (TBL..Police, TBL..CouleurTexte)
         * sert de base au rendu des cellules et en-têtes (posée sur la vue) ;
         * le styleTable surcharge ensuite par sous-élément. Les deux coexistent. */
        printf("  %s%s%s\n",
               C_DIM,
               dl(opts->lang,
                  "Police et couleur : une police/couleur globale du champ "
                  "(TAB..Police, TAB..CouleurTexte) sert de base au rendu des "
                  "cellules et en-têtes ; le style détaillé la surcharge par "
                  "sous-élément (Style..TitreColonne..Police, "
                  "Style..LibelléLignes..Couleur, Style..LigneSélectionnée..Police…). "
                  "Les deux coexistent. Sous-éléments : owl explain styleTable.",
                  "Font and color: a global field font/color (TAB..Police, "
                  "TAB..CouleurTexte) serves as the base for rendering cells and "
                  "headers; the detailed style overrides it per sub-element "
                  "(Style..ColumnTitle..Font, Style..RowLabel..Color, "
                  "Style..SelectedRow..Font…). Both coexist. "
                  "Sub-elements: owl explain styleTable."),
               C_RESET);
    }
    printf("\n");

    /* Propriétés DSL — itère l'index plat, filtre sur ce kind. */
    int n = owl_dsl_props_count();
    int prop_shown = 0;
    int n_props_total = 0;
    /* Largeur max du nom pour alignement + comptage du total. */
    int wmax = 0;
    const char* canon = info->nameAscii ? info->nameAscii : info->nameFR;
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->kind || !e->def) continue;
        if (!kind_name_eq(e->kind, canon)) continue;
        n_props_total++;
        char acc[96];
        dsl_prop_accessor(e->def, opts->lang, acc, sizeof acc);
        int w = (int)strlen(acc);
        if (w > wmax) wmax = w;
    }
    /* Bornes exprimees sur l'accesseur complet (".." inclus). Le plafond
     * s'ouvre en EN, ou l'accesseur porte les deux formes. */
    if (wmax < 14) wmax = 14;
    {
        int cap = (opts->lang == DOC_LANG_EN) ? 44 : 26;
        if (wmax > cap) wmax = cap;
    }

    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->kind || !e->def) continue;
        if (!kind_name_eq(e->kind, canon)) continue;
        if (!prop_shown) {
            printf("%s%s%s %s(%d)%s\n",
                   C_SECTION, dl(opts->lang, "Propriétés", "Properties"),
                   C_RESET, C_DIM, n_props_total, C_RESET);
        }
        const OwlControlPropDef* def = e->def;
        /* Affiche le nom préfixé `..` (forme d'accès canonique en code
         * OWL : `WIDGET..Propriété`) en blanc neutre — colle visuellement
         * à la syntaxe d'usage. */
        char accessor[96];
        dsl_prop_accessor(def, opts->lang, accessor, sizeof accessor);
        printf("  ");
        print_padded_utf8(accessor, wmax);
        printf("  %s%s%s",
               C_TYPE,
               prop_type_name_lang(def->type, opts->lang), C_RESET);
        if (def->enum_name) {
            printf(" %s∈ %s%s", C_DIM, symbol_lang(def->enum_name, opts->lang), C_RESET);
        }
        if (def->object_type) {
            printf(" %s→ %s%s", C_DIM, symbol_lang(def->object_type, opts->lang), C_RESET);
        }
        printf("\n");
        const char* d = type_to_lang(def->descFR, def->descEN, opts->lang);
        if (d && *d) printf("      %s%s%s\n", C_DIM, d, C_RESET);
        prop_shown++;
    }
    if (prop_shown) printf("\n");

    /* Events — route sur le catalogue Etat séparé si sous-bloc. */
    int ec = kind_event_count(evsrc, kind);
    if (ec > 0) {
        printf("%sEvents%s %s(%d)%s\n",
               C_SECTION, C_RESET, C_DIM, ec, C_RESET);
        for (int i = 0; i < ec; i++) {
            const OwlEventDef* ev = kind_event_at(evsrc, kind, i);
            if (!ev) continue;
            /* Signature : surNom(p1 : type1, p2 : type2, ...) */
            printf("  %s%s%s%s(%s", C_NATIVE, ev->canonical, C_RESET, C_DIM, C_RESET);
            for (int j = 0; j < ev->paramCount && ev->params; j++) {
                if (j > 0) printf(", ");
                char etbuf[256];
                printf("%s%s%s : %s%s%s",
                       C_IDENT, ev->params[j].name, C_RESET,
                       C_TYPE,
                       type_str_lang(ev->params[j].typeName
                                       ? ev->params[j].typeName : "tout",
                                     opts->lang, etbuf, sizeof etbuf),
                       C_RESET);
            }
            printf("%s)%s", C_DIM, C_RESET);
            if (ev->nameEN) {
                printf("  %s/ %s%s", C_DIM, ev->nameEN, C_RESET);
            }
            printf("\n");
            const char* d = type_to_lang(ev->descFR, ev->descEN, opts->lang);
            if (d && *d) printf("      %s%s%s\n", C_DIM, d, C_RESET);
        }
    }

    /* Méthodes fluent — un widget peut aussi recevoir des natives (ex
     * ListeAjoute sur Liste). On tente sur le nom ASCII canon. */
    print_methods_for_type(canon, opts->lang);

    return 0;
}

/* Reverse lookup : itère l'index plat des propriétés DSL et collecte
 * tous les kinds qui acceptent la propriété `prop_name`. Le premier
 * `def` trouvé est exposé via *out_def (type / enum / desc — devraient
 * être identiques sur tous les kinds qui partagent la même prop).
 * Retourne le nombre de kinds collectés.
 *
 * Les kinds rendus sont en forme canon (props_kind_display) et non en cle
 * brute : ce que `where` et `explain` affichent est ce que l'appelant
 * recopie. Meme resolution que `list props`, pas une seconde
 * table. La dedup porte donc sur la forme rendue, ce qui couvre au passage
 * deux cles distinctes qui partageraient un meme canon. */
static int find_prop_users(const char* prop_name,
                           const OwlControlPropDef** out_def,
                           const char* kinds_out[], int max_kinds) {
    if (out_def) *out_def = NULL;
    if (!prop_name || !*prop_name) return 0;
    int n = owl_dsl_props_count();
    int n_kinds = 0;
    long pn = (long)strlen(prop_name);
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->kind) continue;
        const OwlControlPropDef* d = e->def;
        int match =
            (d->nameFR    && word_eq_ci(prop_name, pn, d->nameFR))    ||
            (d->nameAscii && word_eq_ci(prop_name, pn, d->nameAscii)) ||
            (d->nameEN    && word_eq_ci(prop_name, pn, d->nameEN));
        if (!match) continue;
        if (out_def && !*out_def) *out_def = d;
        /* Dédup par kind (le même kind peut apparaître plusieurs fois si
         * une prop est répétée — paranoïa). */
        const char* shown = props_kind_display(e->kind);
        int dup = 0;
        for (int k = 0; k < n_kinds; k++) {
            if (strcmp(kinds_out[k], shown) == 0) { dup = 1; break; }
        }
        if (!dup && n_kinds < max_kinds) {
            kinds_out[n_kinds++] = shown;
        }
    }
    return n_kinds;
}

/* Affiche la fiche d'une propriété DSL : type attendu, enum si
 * applicable. La description et la liste des widgets dépendent du flag
 * `show_users` :
 *   - 0 (depuis `explain`) : pas de description (trop souvent contextuelle
 *     à un seul widget — ex. « Fenetre desactivee » pour ..Grisé qui sert
 *     en fait à 10 widgets) ni de liste utilisateurs. Le user explore
 *     d'abord le type ; pour les utilisateurs il fait `owl uses`.
 *   - 1 (depuis `uses`) : liste des widgets qui acceptent cette prop. */
/* Rend le defaut d'une prop DSL en chaine lisible ( Veine Y).
 * Retourne NULL si UNSET (pas de defaut peuple -> ligne masquee). */
static const char* dsl_prop_default_human(const OwlPropDefault* d,
                                          char* buf, size_t n, DocLang lang) {
    switch (d->kind) {
        case OWL_PROP_DEFAULT_BOOL: return default_display(d->i ? "Vrai" : "Faux", lang);
        case OWL_PROP_DEFAULT_INT:  snprintf(buf, n, "%d", d->i); return buf;
        case OWL_PROP_DEFAULT_AUTO: return "auto";
        case OWL_PROP_DEFAULT_NULL: return "null";
        /*  — un defaut ENUM est un NOM DE CONSTANTE : forme EN du
         * registre en --lang en (default_display), pas la chaine brute. */
        case OWL_PROP_DEFAULT_ENUM:   return d->s ? default_display(d->s, lang) : "";
        case OWL_PROP_DEFAULT_STRING: return d->s ? d->s : "";
        case OWL_PROP_DEFAULT_UNSET:
        default: return NULL;
    }
}

static int explain_dsl_prop(const OwlControlPropDef* def,
                             const char* kinds[], int n_kinds,
                             const DocOpts* opts, int show_users,
                             const RelatedInfo* related) {
    if (opts->json) {
        printf("{\n  \"kind\": \"property\"");
        printf(",\n  \"name\": "); json_write_cstr(def->nameFR);
        if (def->nameAscii) { printf(",\n  \"name_ascii\": "); json_write_cstr(def->nameAscii); }
        if (def->nameEN)    { printf(",\n  \"name_en\": ");    json_write_cstr(def->nameEN); }
        printf(",\n  \"type\": "); json_write_cstr(prop_type_name_lang(def->type, opts->lang));
        if (def->enum_name) { printf(",\n  \"enum\": "); json_write_cstr(symbol_lang(def->enum_name, opts->lang)); }
        if (def->object_type) { printf(",\n  \"object_type\": "); json_write_cstr(symbol_lang(def->object_type, opts->lang)); }
        /* defaut canon : valeur typee + kind (preserve unset/null/auto). */
        printf(",\n  \"default_value\": ");
        switch (def->dft.kind) {
            case OWL_PROP_DEFAULT_BOOL:   printf(def->dft.i ? "true" : "false"); break;
            case OWL_PROP_DEFAULT_INT:    printf("%d", def->dft.i); break;
            case OWL_PROP_DEFAULT_AUTO:   printf("\"auto\""); break;
            case OWL_PROP_DEFAULT_ENUM:
            case OWL_PROP_DEFAULT_STRING: json_write_cstr(def->dft.s ? def->dft.s : ""); break;
            case OWL_PROP_DEFAULT_NULL:
            case OWL_PROP_DEFAULT_UNSET:
            default:                      printf("null"); break;
        }
        printf(",\n  \"default_kind\": ");
        json_write_cstr(owl_prop_default_kind_name(def->dft.kind));
        if (show_users) {
            printf(",\n  \"users\": [");
            for (int i = 0; i < n_kinds; i++) {
                if (i > 0) printf(",");
                printf("\n    "); json_write_cstr(kinds[i]);
            }
            printf("\n  ]");
        }
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    /* Header : nom préfixé `..`, en blanc (pas violet). L'ASCII apparaît
     * uniquement s'il diffère du canon ; l'alias EN reste discret et seul
     * en mode --lang en (en FR le LLM tape la forme canon). */
    printf("..%s", def->nameFR);
    if (def->nameAscii && strcmp(def->nameAscii, def->nameFR) != 0) {
        printf(" %s(..%s)%s", C_DIM, def->nameAscii, C_RESET);
    }
    if (opts->lang == DOC_LANG_EN &&
        def->nameEN && strcmp(def->nameEN, def->nameFR) != 0 &&
        (!def->nameAscii || strcmp(def->nameEN, def->nameAscii) != 0)) {
        printf(" %s/ ..%s%s", C_DIM, def->nameEN, C_RESET);
    }
    printf("\n  %s%s%s\n\n", C_DIM, dl(opts->lang, "propriété", "property"),
           C_RESET);

    /* Type attendu + contrainte enum si applicable. */
    printf("  %s%s%s %s%s%s",
           C_DIM, dl(opts->lang, "Type :", "Type:"), C_RESET,
           C_TYPE, prop_type_name_lang(def->type, opts->lang), C_RESET);
    if (def->enum_name) {
        printf(" %s∈ %s%s", C_DIM, symbol_lang(def->enum_name, opts->lang), C_RESET);
    }
    if (def->object_type) {
        printf(" %s→ %s%s", C_DIM, symbol_lang(def->object_type, opts->lang), C_RESET);
    }
    printf("\n");

    /* Defaut canon — masque si non peuple (UNSET). */
    {
        char dbuf[32];
        const char* dft = dsl_prop_default_human(&def->dft, dbuf, sizeof dbuf,
                                                 opts->lang);
        if (dft) printf("  %s%s%s %s%s%s\n", C_DIM,
                        dl(opts->lang, "Défaut :", "Default:"), C_RESET,
                        C_CONST, dft, C_RESET);
    }

    /* Liste des utilisateurs — uniquement depuis `uses`. */
    /*  — bloc entierement non localise jusqu'ici, pluriel en dur
     * inclus (meme classe que « %d champ%s » traitee par ). Les
     * kinds sont des noms de widgets : forme EN du catalogue, pas une
     * traduction. */
    if (show_users && n_kinds > 0) {
        printf("\n%s%s%s %s(%d %s%s)%s\n",
               C_SECTION, dl(opts->lang, "Acceptée par", "Accepted by"), C_RESET,
               C_DIM, n_kinds,
               dl(opts->lang, "champ", "field"), n_kinds > 1 ? "s" : "",
               C_RESET);
        for (int i = 0; i < n_kinds; i++) {
            printf("  %s%s%s\n", C_TYPE,
                   control_kind_lang(kinds[i], opts->lang), C_RESET);
        }
    }
    return 0;
}

static int explain_limit(const OwlLimitDef* L, const DocOpts* opts) {
    if (opts->json) {
        printf("{\n  \"kind\": \"limit\"");
        printf(",\n  \"name\": ");     json_write_cstr(L->name);
        printf(",\n  \"value\": %d", L->value);
        printf(",\n  \"category\": "); json_write_cstr(L->category);
        const char* desc = (opts->lang == DOC_LANG_EN && L->descEN && *L->descEN)
                            ? L->descEN : L->descFR;
        printf(",\n  \"desc\": ");     json_write_cstr(desc);
        printf("\n}\n");
        return 0;
    }
    printf("%s%s%s %s%s%s\n",
           C_CONST, L->name, C_RESET, C_DIM,
           dl(opts->lang, "(limite compile-time)", "(compile-time limit)"),
           C_RESET);
    printf("  %s%s%s\n", C_DIM,
           L->category ? owl_category_display(L->category,
                                              opts->lang == DOC_LANG_EN)
                       : dl(opts->lang, "(sans catégorie)", "(no category)"),
           C_RESET);
    printf("  %s%s %s%d%s\n\n",
           C_DIM, dl(opts->lang, "Valeur :", "Value:"), C_NATIVE, L->value, C_RESET);
    const char* desc = (opts->lang == DOC_LANG_EN && L->descEN && *L->descEN)
                        ? L->descEN : L->descFR;
    if (desc && *desc) printf("  %s\n", desc);
    return 0;
}

/*  — fiche d'un code d'erreur du registry owl_errors.def.
 * Champs disponibles : name + category + templateFR + templateEN. Pas de
 * description riche (ce sont des messages d'erreur, pas des fonctions à
 * explorer). Les placeholders {var} dans le template sont rendus tels
 * quels — ils font partie de la doc du template, c'est l'appelant qui
 * substitue à runtime. */
/*  — reconnait l'alias numerique d'un code d'erreur : prefixe E ou W
 * (case-insensitive) suivi d'au moins un chiffre, rien d'autre. */
static int is_error_num_format(const char* s) {
    if (!s) return 0;
    char p = *s;
    if (p != 'E' && p != 'e' && p != 'W' && p != 'w') return 0;
    s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static int explain_error(const OwlErrorDef* e, const DocOpts* opts) {
    const char* tpl = (opts->lang == DOC_LANG_EN && e->templateEN && *e->templateEN)
                        ? e->templateEN : e->templateFR;
    const char* num = owl_registry_error_num(owl_registry_error_code_of(e));
    if (opts->json) {
        printf("{\n  \"kind\": \"error\"");
        printf(",\n  \"name\": ");     json_write_cstr(e->name);
        printf(",\n  \"num\": ");      json_write_cstr(num ? num : "");
        printf(",\n  \"category\": "); json_write_cstr(e->category);
        printf(",\n  \"template\": "); json_write_cstr(tpl ? tpl : "");
        printf(",\n  \"lang\": \"%s\"",
               opts->lang == DOC_LANG_EN ? "en" : "fr");
        printf("\n}\n");
        return 0;
    }
    const char* errLabel = dl(opts->lang, "(code d'erreur)", "(error code)");
    if (num)
        printf("%s%s%s %s(%s)%s %s%s%s\n",
               C_CONST, e->name, C_RESET, C_DIM, num, C_RESET,
               C_DIM, errLabel, C_RESET);
    else
        printf("%s%s%s %s%s%s\n",
               C_CONST, e->name, C_RESET, C_DIM, errLabel, C_RESET);
    printf("  %s%s%s\n\n",
           C_DIM, e->category ? e->category
                              : dl(opts->lang, "(sans catégorie)", "(no category)"),
           C_RESET);
    if (tpl && *tpl) printf("  %s\n", tpl);
    return 0;
}

static int explain_const(const OwlConstDef* c, const DocOpts* opts,
                         const RelatedInfo* related) {
    /* Enum d'appartenance : permet de retrouver l'énumération source depuis une
     * de ses valeurs (les enums ne sont plus une catégorie navigable, ). */
    const OwlEnum* owner = owl_registry_enum_owner_of_const(c->name);
    /*  — une constante hérite du reverse lookup de son enum
     * d'appartenance (httpGet ∈ httpMéthodes → utilisé par httpRequête). */
    EnumUsedBy ub;
    enum_compute_usedby(owner, &ub);
    if (opts->json) {
        printf("{\n  \"kind\": \"constant\"");
        printf(",\n  \"name\": ");     json_write_cstr(c->name);
        printf(",\n  \"name_en\": ");  json_write_cstr(c->nameEN);
        printf(",\n  \"category\": "); json_write_cstr(c->category);
        printf(",\n  \"desc\": ");     json_write_cstr(type_to_lang(c->descFR, c->descEN, opts->lang));
        if (owner) { printf(",\n  \"enum\": "); json_write_cstr(owner->name); }
        if (ub.count) { printf(",\n  "); print_enum_usedby_json(&ub); }
        if (related) { printf(",\n  "); print_related_json(related); }
        printf("\n}\n");
        return 0;
    }

    /*  — en --lang en, l'alias EN de la constante est adjoint au nom
     * canon FR, comme la fiche de propriete DSL le fait deja
     * (« ..Grisé / ..Grayed »). Le nom canon reste affiche : c'est la forme
     * du registre, celle que `owl explain` prend en entree. */
    printf("%s%s%s", C_CONST, c->name, C_RESET);
    if (opts->lang == DOC_LANG_EN && c->nameEN && strcmp(c->nameEN, c->name) != 0) {
        printf(" %s/ %s%s", C_DIM, c->nameEN, C_RESET);
    }
    printf(" %s(%s)%s\n", C_DIM, dl(opts->lang, "constante", "constant"), C_RESET);
    printf("  %s%s%s\n", C_DIM,
           c->category ? owl_category_display(c->category, opts->lang == DOC_LANG_EN)
                       : dl(opts->lang, "(sans catégorie)", "(no category)"),
           C_RESET);
    if (owner) printf("  %s∈%s %s%s%s\n", C_DIM, C_RESET, C_TYPE,
                      symbol_lang(owner->name, opts->lang), C_RESET);
    printf("\n");
    const char* desc = type_to_lang(c->descFR, c->descEN, opts->lang);
    if (desc && *desc) printf("  %s\n", desc);
    print_enum_usedby_text(&ub);
    return 0;
}

/*  — Rend la métadonnée de littéral d'un type primitif (durée) DANS
 * la section primitif, au lieu d'une section [Type composé] autonome au label
 * faux. Documente la syntaxe du littéral `{ Heure = 2 }` + les composantes de
 * construction (fields du catalogue). Texte uniquement : le bucket primitif
 * n'émet pas de JSON (cohérent avec les autres primitifs). */
static void print_primitive_literal(const OwlTypeDef* t, const DocOpts* opts) {
    if (!t || !t->fields) return;
    /* Description riche du type (documente la construction du littéral). */
    const char* desc = type_to_lang(t->descFR, t->descEN, opts->lang);
    if (desc && *desc) printf("\n  %s\n", desc);

    int n_fields = 0;
    for (const OwlField* f = t->fields; f->name; f++) n_fields++;
    printf("\n%sComposantes du littéral%s %s(%d)%s\n",
           C_SECTION, C_RESET, C_DIM, n_fields, C_RESET);

    int wmax = 0;
    for (const OwlField* f = t->fields; f->name; f++) {
        const char* display = field_display_name(f, opts->lang);
        int w = (int)strlen(display);
        if (w > wmax) wmax = w;
    }
    if (wmax < 12) wmax = 12;
    if (wmax > 28) wmax = 28;

    for (const OwlField* f = t->fields; f->name; f++) {
        const char* display = field_display_name(f, opts->lang);
        const char* fdesc   = type_to_lang(f->descFR, f->descEN, opts->lang);
        printf("  %s%-*s%s  %s%s%s",
               C_CONST, wmax, display, C_RESET,
               C_TYPE,  f->type ? f->type : "", C_RESET);
        if (f->defaultValue && *f->defaultValue && strcmp(f->defaultValue, "\"\"") != 0) {
            printf(" %s= %s%s", C_DIM,
                   default_display(f->defaultValue, opts->lang), C_RESET);
        }
        printf("\n");
        if (fdesc && *fdesc) {
            printf("      %s%s%s\n", C_DIM, fdesc, C_RESET);
        }
    }
}

/*  — troisieme etage du chemin not-found : les formes heritees de fin
 * de ligne (rc / cr) ne sont plus des symboles du langage depuis ,
 * mais restent RESOLUBLES — le code compile et le formateur les reecrit en
 * CRLF a l'enregistrement. Interroger le catalogue au moment precis ou son
 * symbole vient d'etre reecrit est l'instant ou l'utilisateur apprend la
 * bascule ; sans cet indice l'outil se tait.
 *
 * Deliberement un indice, PAS une entree : rien n'est reinjecte dans le
 * registre (owl list, --vscode-data, completion LSP restent muets) et le rc
 * du chemin inconnu ne bouge pas (2) — un script qui teste l'existence d'un
 * symbole doit continuer a recevoir non.
 *
 * Le nom cite est celui que l'utilisateur a tape (RC pour rc, CR pour cR),
 * jamais CRLF qu'il n'a pas ecrit — meme principe que le warning de masquage
 * W_SEM_VAR_SHADOWS_EOL_LEGACY. Les cles sont ASCII pur : toupper suffit. */
static void print_eol_legacy_hint(const char* name, DocLang lang) {
    const char* canonical = owl_formatter_eol_legacy_canonical(name);
    if (canonical == NULL) return;

    char legacy[16];
    size_t n = strlen(name);
    if (n >= sizeof(legacy)) return;
    for (size_t i = 0; i < n; i++) {
        legacy[i] = (char)toupper((unsigned char)name[i]);
    }
    legacy[n] = '\0';

    if (lang == DOC_LANG_EN) {
        fprintf(stderr,
                "  %s: legacy form, equals %s. Rewritten to %s on save.\n"
                "  For a plain end of line, use LF.\n",
                legacy, canonical, canonical);
    } else {
        fprintf(stderr,
                "  %s : forme héritée, équivaut à %s. Réécrite en %s à "
                "l'enregistrement.\n"
                "  Pour une fin de ligne simple, utiliser LF.\n",
                legacy, canonical, canonical);
    }
}

int cli_doc_explain(int argc, char** argv) {
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "explain")) return 64;

    if (opts.pos_argc == 0) {
        fprintf(stderr, "owl explain : nom de symbole requis.\n");
        fprintf(stderr, "  Tapez 'owl list natives' pour voir les noms disponibles.\n");
        return 1;
    }
    const char* name = opts.pos_argv[0];

    /* Filtre par bucket via --cat. Vide = tous les buckets, non reconnu
     * = erreur explicite (mieux que silencieusement ignorer). */
    BucketMask mask = parse_explain_cat(opts.cat);
    if (opts.cat && mask == 0) {
        fprintf(stderr, "owl explain : --cat '%s' n'est pas un bucket reconnu.\n",
                opts.cat);
        fprintf(stderr, "  Valeurs : all | primitives | types | composés | natives | enums | constants | keywords | limits | errors\n");
        return 64;
    }

    /* Composé keyword (2 mots positionnels — ex. "PAS DANS", "SORTIR
     * SELON"). Exclusif : si match, on affiche la fiche composé et stop. */
    if ((mask & BUCKET_KEYWORD) && opts.pos_argc >= 2) {
        const OwlKeywordComposite* comp =
            owl_keyword_composite_find_by_words(opts.pos_argv[0], opts.pos_argv[1]);
        if (comp) {
            RelatedInfo rc; memset(&rc, 0, sizeof rc);
            char joined[64];
            snprintf(joined, sizeof joined, "%s %s",
                     opts.pos_argv[0], opts.pos_argv[1]);
            (void)joined;  /* maillage Voir aussi à brancher en Phase 1+. */
            return explain_keyword_composite(comp,
                                              opts.pos_argv[0],
                                              opts.pos_argv[1],
                                              &opts, NULL);
        }
    }

    /* On regarde dans TOUS les buckets : un nom peut désigner à la fois
     * une fonction native et un type composé (invariant règle 6.5 —
     * `HTTPRequête()` = constructeur, `req est un httpRequête` = type).
     * Sortir tous les matches dans l'ordre primitive → type → widget →
     * fonction → enum → const, séparés visuellement. */
    const PrimitiveType* pt = (mask & BUCKET_PRIMITIVE) ? find_primitive_type(name) : NULL;
    const OwlNativeDef* d   = (mask & BUCKET_NATIVE)    ? owl_registry_find(name)      : NULL;
    const OwlTypeDef*   t   = (mask & BUCKET_TYPE)      ? owl_registry_type_find(name) : NULL;
    const OwlEnum*      ee  = (mask & BUCKET_ENUM)      ? owl_registry_enum_find(name) : NULL;
    const OwlConstDef*  c   = (mask & BUCKET_CONST)     ? owl_registry_const_find_bilingual(name) : NULL;
    const OwlLimitDef*  lim = (mask & BUCKET_LIMIT)     ? owl_registry_limit_find(name) : NULL;
    /*  — bucket errors : lookup case-insensitive sur le name
     * du registry owl_errors.def.  — un alias numerique de la
     * forme [EW]\d+ (E0042, W0003) resout via le lookup inverse. */
    const OwlErrorDef*  err = NULL;
    if (mask & BUCKET_ERROR) {
        if (is_error_num_format(name)) err = owl_registry_error_by_num(name);
        if (!err)                      err = owl_registry_error_find(name);
    }
    int w_kind = -1;
    const OwlControlKindInfo* w_info = (mask & BUCKET_WIDGET)
        ? find_widget_kind(name, &w_kind) : NULL;

    /* Propriété DSL — fiche sans reverse lookup. Le strip `..` permet
     * d'accepter `owl explain ..Visible` aussi bien que `Visible`. */
    const char* prop_query = name;
    if (prop_query[0] == '.' && prop_query[1] == '.') prop_query += 2;
    const OwlControlPropDef* pp_def = NULL;
    const char* pp_kinds[64];
    int pp_n = 0;
    if (mask & BUCKET_DSL_PROP) {
        pp_n = find_prop_users(prop_query, &pp_def, pp_kinds, 64);
    }

    /* Keyword du langage — délègue au catalogue (source : scanner +
     * owl_keyword_catalog). Si le mot n'est pas un keyword OU si le
     * TokenType n'a pas (encore) de fiche rédigée, renvoie NULL. */
    const OwlKeywordDef* kw = (mask & BUCKET_KEYWORD)
        ? owl_keyword_find_by_word(name) : NULL;

    /* Opérateur symbolique — lookup direct par display_form sur la
     * chaîne brute (ex. "[~~", "??*"). Indépendant du scanner. */
    const OwlOperatorDef* op = (mask & BUCKET_OPERATOR)
        ? owl_operator_find_by_symbol(name) : NULL;

    /* Event DSL — recherche dans tous les widgets + catalogues Etat. */
    EventOwner ev_owners[32];
    int ev_n = (mask & BUCKET_EVENT)
        ? find_event_owners(name, ev_owners, 32) : 0;

    /* Variable globale prédéfinie (ErreurDétectée, …). */
    const OwlGlobalVarDef* gv = (mask & BUCKET_GLOBALVAR)
        ? owl_globalvar_catalog_find(name) : NULL;

    /* Propriété builtin sur primitif (.Année, .EnHeures, .Vide, …). */
    BuiltinPropOwner bp_owners[16];
    int bp_n = (mask & BUCKET_BUILTIN_PROP)
        ? find_builtin_prop_owners(name, bp_owners, 16) : 0;

    /* Forme méthode d'une native (s.VersDate(), id.VersChaine(), …) —
     * itère le registry et collecte les natives qui exposent ce nom comme
     * method binding sur un type récepteur.
     *
     *  — forme qualifiée `Type.Méthode` (un seul point, pas `..` de
     * propriété, pas `..Prop` en tête) : on scinde et on filtre les
     * receveurs. `LLM.Charge` cible LLM sans lister IAModèle. */
    const char* method_query = name;
    const char* method_recv_filter = NULL;
    char qual_type[64] = {0};
    char qual_meth[64] = {0};
    {
        const char* dot = strchr(name, '.');
        if (dot && dot != name && dot[-1] != '.' &&
            dot[1] != '\0' && dot[1] != '.') {
            size_t tlen = (size_t)(dot - name);
            size_t mlen = strlen(dot + 1);
            if (tlen > 0 && tlen < sizeof qual_type && mlen < sizeof qual_meth) {
                memcpy(qual_type, name, tlen);  qual_type[tlen] = '\0';
                memcpy(qual_meth, dot + 1, mlen); qual_meth[mlen] = '\0';
                method_query = qual_meth;
                method_recv_filter = qual_type;
            }
        }
    }
    MethodOwner mt_owners[32];
    int mt_n = (mask & BUCKET_METHOD)
        ? find_method_bindings(method_query, method_recv_filter, mt_owners, 32) : 0;

    if (!pt && !d && !t && !ee && !c && !lim && !err && !w_info && !pp_def && !kw && !op && !ev_n && !gv && !bp_n && !mt_n) {
        if (opts.cat) {
            fprintf(stderr, "owl explain : '%s' introuvable dans la catégorie '%s'.\n",
                    name, opts.cat);
            fprintf(stderr, "  Tentez sans --cat pour chercher dans tous les buckets.\n");
            print_eol_legacy_hint(name, opts.lang);
        } else {
            /* Bloc bilingue (invariant 3) : ces trois lignes etaient FR en dur.
             * L'indice ci-dessous suit opts.lang — les laisser FR produisait un
             * ilot anglais au milieu d'un bloc francais, pire que l'unilingue.
             * Branches explicites plutot que dl() en format : le format reste
             * litteral, seul emplacement du fichier ou dl() porterait un %s. */
            if (opts.lang == DOC_LANG_EN) {
                fprintf(stderr, "owl explain: unknown symbol '%s'.\n", name);
                fprintf(stderr, "  Not found in natives / types / enums / constants / errors.\n");
                print_eol_legacy_hint(name, opts.lang);
                fprintf(stderr, "  Try 'owl search %s' for a broader search.\n", name);
            } else {
                fprintf(stderr, "owl explain : symbole '%s' inconnu.\n", name);
                fprintf(stderr, "  Pas trouvé dans natives / types / enums / constantes / erreurs.\n");
                print_eol_legacy_hint(name, opts.lang);
                fprintf(stderr, "  Essayez 'owl search %s' pour une recherche large.\n", name);
            }
        }
        return 2;
    }

    /*  — durée : l'entrée composite est en réalité la métadonnée du
     * littéral d'un type PRIMITIF. On la retire du bucket composé (plus de
     * section [Type composé] ni de label faux, plus de double) et on la
     * documente DANS la section primitif via print_primitive_literal. */
    const OwlTypeDef* litForPrimitive = NULL;
    if (t && t->primitiveLiteral && pt) {
        litForPrimitive = t;
        t = NULL;
    }

    /* Compteur de buckets matchés — quand >1, on affiche un séparateur
     * entre chaque section pour bien marquer la disambiguation. */
    int nbuckets = (pt ? 1 : 0) + (d ? 1 : 0) + (t ? 1 : 0) +
                   (ee ? 1 : 0) + (c ? 1 : 0) + (w_info ? 1 : 0) +
                   (pp_def ? 1 : 0) + (kw ? 1 : 0) + (op ? 1 : 0) +
                   (ev_n ? 1 : 0) + (gv ? 1 : 0) + (bp_n ? 1 : 0) +
                   (mt_n ? 1 : 0) + (lim ? 1 : 0) + (err ? 1 : 0);
    int first = 1;

    /* Calcul du RelatedInfo (maillage "Voir aussi") — un seul, base sur
     * le bucket dominant. Ordre de priorite : NATIVE > DSL_PROP > TYPE >
     * WIDGET > ENUM > CONST > PRIMITIVE. La native est dominante car
     * elle apporte les voisins de categorie (utile pour exploration
     * laterale). DSL_PROP avant TYPE car la prop apporte where_count. */
    RelatedInfo rinfo;
    int has_related = 0;
    if (d) {
        compute_related(&rinfo, d->canonical, d->canonicalAscii,
                        REL_KIND_NATIVE, d);
        has_related = 1;
    } else if (pp_def) {
        compute_related(&rinfo, pp_def->nameFR, pp_def->nameAscii,
                        REL_KIND_DSL_PROP, pp_def);
        has_related = 1;
    } else if (t) {
        compute_related(&rinfo, t->canonical, t->canonicalAscii,
                        REL_KIND_TYPE, t);
        has_related = 1;
    } else if (w_info) {
        compute_related(&rinfo, w_info->nameFR, w_info->nameAscii,
                        REL_KIND_WIDGET, w_info);
        has_related = 1;
    } else if (ee) {
        compute_related(&rinfo, ee->name, NULL, REL_KIND_ENUM, ee);
        has_related = 1;
    } else if (c) {
        compute_related(&rinfo, c->name, NULL, REL_KIND_CONST, c);
        has_related = 1;
    } else if (pt) {
        compute_related(&rinfo, pt->name, pt->nameAscii,
                        REL_KIND_PRIMITIVE, NULL);
        has_related = 1;
    } else if (kw) {
        /* Keyword : on prend le label du premier sense comme canon
         * pour le maillage (les keywords n'ont pas de canonical
         * accent/ASCII distinct — on s'aligne sur l'argument utilisateur). */
        compute_related(&rinfo, name, NULL, REL_KIND_PRIMITIVE, NULL);
        has_related = 1;
    }
    /* Le JSON related est emis par chaque bucket ; le texte une seule
     * fois apres le dernier bucket. */
    const RelatedInfo* rptr = has_related ? &rinfo : NULL;

    if (pt) {
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Type primitif", "Primitive type"), C_RESET);
        const char* desc = (opts.lang == DOC_LANG_EN && pt->descEN && *pt->descEN)
                             ? pt->descEN : pt->descFR;
        /* Évite les "tableau (tableau) / array" disgracieux quand les
         * formes ne diffèrent pas (pas d'accent, ou EN identique au FR). */
        printf("%s%s%s", C_TYPE, pt->name, C_RESET);
        if (pt->nameAscii && strcmp(pt->nameAscii, pt->name) != 0) {
            printf(" %s(%s)%s", C_DIM, pt->nameAscii, C_RESET);
        }
        if (pt->nameEN && *pt->nameEN &&
            strcmp(pt->nameEN, pt->name) != 0 &&
            strcmp(pt->nameEN, pt->nameAscii) != 0) {
            printf(" %s/ %s%s", C_DIM, pt->nameEN, C_RESET);
        }
        printf("\n  %s%s%s\n\n", C_DIM,
               dl(opts.lang, "Type primitif intégré à la VM",
                             "Built-in primitive type (VM)"), C_RESET);
        printf("  %s\n", desc ? desc : "");

        /* Méthodes fluent sur ce primitif. Le receiverType côté registry
         * est typiquement ASCII (« chaine » et non « chaîne »), mais
         * word_eq_ci normalise — on passe le canonique. */
        print_methods_for_type(pt->name, opts.lang);

        /*  — durée : littéral + composantes de construction rendus
         * dans la section primitif (une seule section cohérente). */
        if (litForPrimitive && !opts.json) {
            print_primitive_literal(litForPrimitive, &opts);
        }
    }

    /* Ordre : type d'abord (souvent ce que cherche l'utilisateur quand il
     * tape `httpRequête`), puis fonction, puis enum, puis constante. */
    if (t) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Type composé", "Composite type"), C_RESET);
        explain_type(t, &opts, rptr);
    }
    if (d) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Fonction native", "Native function"), C_RESET);
        explain_native(d, &opts, rptr);
    }
    if (w_info) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Champ DSL", "DSL control"), C_RESET);
        explain_widget(w_info, w_kind, &opts, rptr);
    }
    if (pp_def) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s ..%s]%s\n",
                   C_SECTION, dl(opts.lang, "Propriété", "Property"),
                   pp_def->nameFR, C_RESET);
        /* show_users=0 : `explain` ne fait pas le reverse lookup,
         * c'est le job d'`owl where`. */
        explain_dsl_prop(pp_def, pp_kinds, pp_n, &opts,
                         /*show_users*/ 0, rptr);
    }
    if (ee) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Énumération", "Enumeration"), C_RESET);
        explain_enum(ee, &opts, rptr);
    }
    if (c) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Constante", "Constant"), C_RESET);
        explain_const(c, &opts, rptr);
    }
    if (lim) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Limite compile-time", "Compile-time limit"),
                   C_RESET);
        explain_limit(lim, &opts);
    }
    if (err) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Code d'erreur", "Error code"), C_RESET);
        explain_error(err, &opts);
    }
    if (kw) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Mot-clé", "Keyword"), C_RESET);
        explain_keyword(kw, &opts, rptr);
    }
    if (op) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Opérateur", "Operator"), C_RESET);
        explain_operator(op, &opts, rptr);
    }
    if (ev_n) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[Event]%s\n", C_SECTION, C_RESET);
        explain_event(name, ev_owners, ev_n, &opts, rptr);
    }
    if (gv) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Variable globale", "Global variable"), C_RESET);
        explain_globalvar(gv, &opts);
    }
    if (bp_n) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Propriété builtin", "Built-in property"),
                   C_RESET);
        explain_builtin_prop(name, bp_owners, bp_n, &opts);
    }
    if (mt_n) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        first = 0;
        if (nbuckets > 1)
            printf("%s[%s]%s\n", C_SECTION,
                   dl(opts.lang, "Forme méthode", "Method form"), C_RESET);
        explain_method(method_query, mt_owners, mt_n, &opts);
        /*  — joindre les scénarios d'une forme méthode. Les buckets
         * native (`d`) et type composé (`t`) impriment déjà leur section
         * "Exemples" ; on n'ajoute la nôtre que si aucun d'eux n'a matché,
         * pour éviter une seconde section. On matche sur le nom fourni par
         * l'utilisateur : nu (`TrancheCanaux`) ou qualifié (`Tenseur.X`),
         * `sujet_matches` couvrant les deux graphies du @sujet. */
        if (!opts.json && !d && !t) {
            print_scenarios_for_symbol(name, NULL, opts.lang);
        }
    }

    /* Maillage : section "Voir aussi" en mode texte, une fois en fin de
     * sortie. En JSON, chaque bucket a deja son champ `related` injecte. */
    if (!opts.json && has_related) {
        print_related_text(rptr, opts.lang);
    }
    return 0;
}

/* ==================================================================
 * owl examples <symbole>
 * ================================================================== */

int cli_doc_examples(int argc, char** argv) {
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "examples")) return 64;

    if (opts.pos_argc == 0) {
        fprintf(stderr, "owl examples : nom de symbole requis.\n");
        return 1;
    }
    const char* user_input = opts.pos_argv[0];

    ensure_scenarios_parsed();

    /* Résout le symbole via le registry pour récupérer la forme canon
     * accentuée (la forme que les scénarios utilisent dans @sujet). */
    const char* name       = user_input;
    const char* name_ascii = NULL;
    const OwlNativeDef* d = owl_registry_find(user_input);
    if (d) {
        name       = d->canonical;
        name_ascii = d->canonicalAscii;
    } else {
        const OwlTypeDef* t = owl_registry_type_find(user_input);
        if (t) {
            name       = t->canonical;
            name_ascii = t->canonicalAscii;
        }
    }

    if (opts.json) {
        printf("{\n  \"symbol\": ");
        json_write_cstr(name);
        printf(",\n  \"scenarios\": [");
        int first = 1;
        for (int i = 0; i < g_scenarios_count; i++) {
            if (!sujet_matches(&g_scenarios[i], name, name_ascii)) continue;
            for (int p = 0; p < g_scenarios[i].procedure_count; p++) {
                const ParsedProcedure* pr = &g_scenarios[i].procedures[p];
                if (pr->is_private || pr->is_fixture) continue;
                if (!first) printf(",");
                first = 0;
                printf("\n    {\n      \"name\": "); json_write_cstr(pr->name);
                printf(",\n      \"file\": "); json_write_cstr(g_scenarios[i].source->path);
                printf(",\n      \"sujet\": "); json_write_cstr(g_scenarios[i].sujet);
                printf(",\n      \"body\": ");
                json_write_proc_body_stripped(pr->body_start, pr->body_len);
                printf("\n    }");
            }
        }
        printf("\n  ]\n}\n");
        return 0;
    }

    /* `print_scenarios_for_symbol` imprime déjà sa propre section
     * "Exemples" — pas de header redondant ici. */
    print_scenarios_for_symbol(name, name_ascii, opts.lang);
    return 0;
}

/* ==================================================================
 * owl search <mot>
 *
 * Recherche transversale : fondations + registry + scénarios.
 * Sortie groupée par catégorie de hit.
 * ================================================================== */

/* Recherche insensible casse d'un substring dans une chaîne. */
static int substr_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn) {
            char a = haystack[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == nn) return 1;
    }
    return 0;
}

/*  — comparaisons insensibles a la casse (meme convention ASCII que
 * substr_ci : minuscule a-z, PAS de strip d'accents — l'insensibilite accent
 * vient des champs *Ascii des entrees). */
static int streq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}
static int startswith_ci(const char* hay, const char* pre) {
    if (!hay || !pre || !*pre) return 0;
    while (*pre) {
        char x = *hay, y = *pre;
        if (!x) return 0;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        hay++; pre++;
    }
    return 1;
}

/* Recherche dans un blob binaire avec retour de la première ligne qui matche. */
static int substr_in_blob_ci(const unsigned char* data, long len,
                             const char* needle, char* line_out, size_t line_size) {
    if (!data || !needle || !*needle) return 0;
    size_t nn = strlen(needle);
    long line_start = 0;
    for (long i = 0; i + (long)nn <= len; i++) {
        if (data[i] == '\n') line_start = i + 1;
        long k = 0;
        while (k < (long)nn) {
            char a = (char)data[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == (long)nn) {
            /* Trouvé. Extrait la ligne. */
            long le = i;
            while (le < len && data[le] != '\n') le++;
            long n = le - line_start;
            if (n + 1 > (long)line_size) n = (long)line_size - 1;
            memcpy(line_out, data + line_start, (size_t)n);
            line_out[n] = '\0';
            /* trim */
            while (n > 0 && (line_out[n - 1] == '\r' ||
                              line_out[n - 1] == ' ' ||
                              line_out[n - 1] == '\t')) {
                line_out[--n] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

/* Tokenisation du needle de `owl search` sur les espaces.
 *
 * Une requête multi-mots (`"JSON chaine"`) est traitée comme un OU de
 * tokens avec score : une entrée matche si AU MOINS un token apparaît
 * dans ses champs indexés, et son rang final = nombre de tokens
 * distincts trouvés. Les meilleurs matchs sortent en premier. Une
 * requête mono-mot retombe sur l'ancien comportement (score binaire).
 * Tokens vides (espaces multiples) ignorés. Borne à 8 tokens pour
 * rester sur un buffer statique ; surnuméraires ignorés. Retourne le
 * nombre de tokens retenus. */
#define SEARCH_MAX_TOKENS 8
#define SEARCH_TOKEN_LEN  64
static int tokenize_needle(const char* needle, char tokens[][SEARCH_TOKEN_LEN]) {
    int nt = 0;
    const char* p = needle;
    while (*p && nt < SEARCH_MAX_TOKENS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != ' ' && *p != '\t' && k < SEARCH_TOKEN_LEN - 1) {
            tokens[nt][k++] = *p++;
        }
        tokens[nt][k] = '\0';
        /* Saute le reste d'un token trop long pour ne pas le scinder. */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (k > 0) nt++;
    }
    return nt;
}

/* Nombre de tokens distincts présents (substr_ci) dans au moins un des `nf`
 * champs. Sémantique OR avec score : 0 = aucun match (entrée à écarter),
 * sinon valeur ∈ [1..nt] qui sert au classement (les meilleurs matchs
 * d'abord). Cas mono-token : retourne 0 ou 1 → ensemble matché identique
 * au comportement AND historique. Les champs NULL sont ignorés. */
static int count_tokens_in_fields(const char* const* fields, int nf,
                                  char tokens[][SEARCH_TOKEN_LEN], int nt) {
    int hits = 0;
    for (int t = 0; t < nt; t++) {
        int found = 0;
        for (int f = 0; f < nf && !found; f++) {
            if (fields[f] && substr_ci(fields[f], tokens[t])) found = 1;
        }
        if (found) hits++;
    }
    return hits;
}

/*  (support #216) — palier de pertinence d'une entree pour `tokens`,
 * calcule sur ses champs de NOM (canonical/ascii/nameEN, PAS la desc) :
 *   4 = un champ nom EGALE un token          (match exact)
 *   3 = un champ nom COMMENCE PAR un token   (prefixe)
 *   2 = un champ nom CONTIENT un token       (sous-chaine)
 *   0 = aucun nom ne matche (le caller retombe alors sur le palier 1 = desc-only)
 * On garde le MEILLEUR palier atteint (tokens x champs). Sert de clé de tri
 * SECONDAIRE apres le nombre de tokens matchés (relevance multi-mots). */
static int entry_name_tier(const char* const* nameFields, int nnf,
                           char tokens[][SEARCH_TOKEN_LEN], int nt) {
    int best = 0;
    for (int t = 0; t < nt; t++) {
        for (int f = 0; f < nnf; f++) {
            const char* fld = nameFields[f];
            if (!fld) continue;
            if (streq_ci(fld, tokens[t]))            { if (best < 4) best = 4; }
            else if (startswith_ci(fld, tokens[t]))  { if (best < 3) best = 3; }
            else if (substr_ci(fld, tokens[t]))      { if (best < 2) best = 2; }
        }
    }
    return best;
}

/*  — resultats de `owl search` classes par pertinence PURE (liste
 * plate, toutes categories confondues). La categorie n'est plus qu'un tag ;
 * le browse par famille passe par les filtres. Cle de tri : nb de tokens
 * matchés > position dans le nom (exact>prefixe>sous-chaine>desc-only) >
 * occurrences dans la desc (tie-break faible) > ordre d'insertion (stable). */
typedef struct {
    char name[96];
    char hint[256];
    const char* kind;    /* tag lang-resolu affiché entre crochets */
    const char* color;   /* couleur ANSI du nom (code par categorie) */
    int  pass;           /* nb de tokens de la requete matchés */
    int  tier;           /* 4 exact / 3 prefixe / 2 sous-chaine / 1 desc-only */
    int  occ;            /* occurrences dans la desc (tie-break) */
    int  order;          /* ordre d'insertion (tie-break stable) */
    int  is_doc;         /*  : hit categorie DOCUMENTATION (guide/fondation) */
} SearchHit;

static SearchHit g_search_hits[512];
static int g_search_hit_count = 0;

/* Occurrences (case-insensible, ASCII) de `needle` dans `hay`. Tie-break
 * FAIBLE : le nom prime, la desc ne departage que les ex aequo. */
static int count_ci_occurrences(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return 0;
    int n = 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t k = 0;
        for (; k < nl && p[k]; k++) {
            char a = p[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == nl) n++;
    }
    return n;
}
static int desc_occurrences(const char* desc, char tokens[][SEARCH_TOKEN_LEN], int nt) {
    int n = 0;
    for (int t = 0; t < nt; t++) n += count_ci_occurrences(desc, tokens[t]);
    return n;
}
static void search_add_hit(const char* name, const char* hint, const char* kind,
                           const char* color, int pass, int tier, int occ) {
    int cap = (int)(sizeof g_search_hits / sizeof g_search_hits[0]);
    if (g_search_hit_count >= cap) return;
    SearchHit* h = &g_search_hits[g_search_hit_count];
    snprintf(h->name, sizeof h->name, "%s", name ? name : "");
    snprintf(h->hint, sizeof h->hint, "%s", hint ? hint : "");
    h->kind = kind; h->color = color;
    h->pass = pass; h->tier = tier; h->occ = occ;
    h->order = g_search_hit_count;
    h->is_doc = 0;
    g_search_hit_count++;
}
static int search_hit_cmp(const void* pa, const void* pb) {
    const SearchHit* a = (const SearchHit*)pa;
    const SearchHit* b = (const SearchHit*)pb;
    /*  (support #355) — documentation-first : les hits guide/fondation
     * remontent AVANT tout autre hit (cle de tri primaire, inconditionnelle cote
     * CLI puisque `owl search` n'a pas de notion de filtre). A l'interieur de
     * chaque groupe (doc / non-doc), l'ordre existant est preserve tel quel. */
    if (a->is_doc != b->is_doc) return b->is_doc - a->is_doc;
    if (a->pass != b->pass) return b->pass - a->pass;   /* + de mots matchés */
    if (a->tier != b->tier) return b->tier - a->tier;   /* exact>prefixe>sous-chaine>desc */
    int la = (int)strlen(a->name), lb = (int)strlen(b->name);
    if (la != lb) return la - lb;                        /* nom court = match plus proche */
    if (a->occ  != b->occ)  return b->occ  - a->occ;    /* tie-break : occurrences desc */
    return a->order - b->order;                          /* stable */
}

/* Variante blob : compte les tokens distincts présents dans le blob.
 * `line_out` reçoit la ligne du PREMIER token matché (hint d'affichage)
 * — on garde le 1er token plutôt que le mieux classé pour rester
 * proche de la requête utilisateur. Retourne 0..nt. */
static int count_tokens_in_blob_ci(const unsigned char* data, long len,
                                   char tokens[][SEARCH_TOKEN_LEN], int nt,
                                   char* line_out, size_t line_size) {
    char hint[256] = "";
    int got_hint = 0;
    int hits = 0;
    for (int t = 0; t < nt; t++) {
        char tmp[256];
        if (substr_in_blob_ci(data, len, tokens[t], tmp, sizeof tmp)) {
            hits++;
            if (!got_hint) {
                size_t n = strlen(tmp);
                if (n + 1 > sizeof hint) n = sizeof hint - 1;
                memcpy(hint, tmp, n);
                hint[n] = '\0';
                got_hint = 1;
            }
        }
    }
    if (line_out && line_size) {
        size_t n = strlen(hint);
        if (n + 1 > line_size) n = line_size - 1;
        memcpy(line_out, hint, n);
        line_out[n] = '\0';
    }
    return hits;
}

/* ==================================================================
 * owl where <symbole> — reverse lookup
 *
 * Question type : « où ce symbole est-il utilisé ? » plutôt que « que
 * suis-je ? » (la 2e étant le rôle d'`owl explain`).
 *
 * Trois cas :
 *  - Propriété DSL (Grisé, Visible, Titre…) → widgets qui l'acceptent
 *  - Widget kind / sous-bloc → contexte parent où il vit
 *  - Autres (type, native, enum, constante) → message d'orientation
 * ================================================================== */

int cli_doc_where(int argc, char** argv) {
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "where")) return 64;

    if (opts.pos_argc == 0) {
        fprintf(stderr, "owl where : nom de symbole requis.\n");
        fprintf(stderr, "  Exemples :\n");
        fprintf(stderr, "    owl where Grisé        (propriété → champs utilisateurs)\n");
        fprintf(stderr, "    owl where Libellé      (champ → contexte parent)\n");
        fprintf(stderr, "    owl where ..Visible    (préfixe d'accès accepté)\n");
        return 1;
    }
    const char* name = opts.pos_argv[0];

    /* Strip un préfixe `..` éventuel pour accepter `owl where ..Grisé`. */
    const char* query = name;
    if (query[0] == '.' && query[1] == '.') query += 2;

    /* Lookup parallèle dans les deux buckets reverse-utiles. */
    const OwlControlPropDef* prop_def = NULL;
    const char* prop_kinds[64];
    int prop_n = find_prop_users(query, &prop_def, prop_kinds, 64);

    int w_kind = -1;
    const OwlControlKindInfo* w_info = find_widget_kind(query, &w_kind);

    int nbuckets = (prop_def ? 1 : 0) + (w_info ? 1 : 0);
    if (nbuckets == 0) {
        fprintf(stderr,
            "owl where : '%s' n'est ni une propriété DSL ni un type de champ.\n"
            "  `uses` répond à « qui m'utilise ? ». Pour la fiche d'un type,\n"
            "  d'une fonction, d'une enum ou d'une constante, utilise\n"
            "  `owl explain %s`.\n", name, name);
        return 2;
    }

    /* JSON : on émet un objet avec les deux sections quand applicable. */
    if (opts.json) {
        printf("{");
        int j_first = 1;
        if (prop_def) {
            printf("\n  \"property\": ");
            /* Réutilise explain_dsl_prop en JSON pour cohérence ;
             * on capture difficilement la sortie nested ici, donc on
             * fait un mini schéma manuel ad hoc. */
            printf("{\n    \"name\": ");
            json_write_cstr(prop_def->nameFR);
            printf(",\n    \"type\": ");
            json_write_cstr(prop_type_name_lang(prop_def->type, opts.lang));
            if (prop_def->enum_name) {
                printf(",\n    \"enum\": ");
                json_write_cstr(symbol_lang(prop_def->enum_name, opts.lang));
            }
            if (prop_def->object_type) {
                printf(",\n    \"object_type\": ");
                json_write_cstr(prop_def->object_type);
            }
            printf(",\n    \"users\": [");
            for (int i = 0; i < prop_n; i++) {
                if (i > 0) printf(",");
                printf("\n      "); json_write_cstr(prop_kinds[i]);
            }
            printf("\n    ]\n  }");
            j_first = 0;
        }
        if (w_info) {
            if (!j_first) printf(",");
            printf("\n  \"widget\": {");
            printf("\n    \"name\": ");      json_write_cstr(w_info->nameFR);
            if (w_info->nameAscii) {
                printf(",\n    \"name_ascii\": ");
                json_write_cstr(w_info->nameAscii);
            }
            printf(",\n    \"is_widget\": %s",
                   w_info->is_widget ? "true" : "false");
            const char* ctx = widget_kind_parent_context(w_info, opts.lang);
            printf(",\n    \"parent_context\": ");
            json_write_cstr(ctx ? ctx : "top-level");
            printf("\n  }");
        }
        printf("\n}\n");
        return 0;
    }

    /* Sortie texte multi-bucket — étiquettes uniquement si plusieurs. */
    int first = 1;
    if (prop_def) {
        if (nbuckets > 1)
            printf("%s[%s ..%s]%s\n",
                   C_SECTION, dl(opts.lang, "Propriété", "Property"),
                   dsl_prop_name_lang(prop_def->nameFR, opts.lang), C_RESET);
        first = 0;
        explain_dsl_prop(prop_def, prop_kinds, prop_n, &opts,
                         /*show_users*/ 1, /*related*/ NULL);
    }
    if (w_info) {
        if (!first) printf("\n%s%s%s\n",
                           C_DIM,
                           "────────────────────────────────────────",
                           C_RESET);
        /* Étiquette adaptée :
         *   - is_widget=true → « Widget Bouton »
         *   - Etat lui-même  → « Rapport Etat » (top-level d'impression)
         *   - autres sous-blocs (Corps, EnTeteDocument, …) → « Sous-bloc Etat » */
        int en = (opts.lang == DOC_LANG_EN);
        if (nbuckets > 1) {
            const char* label = en ? "Control" : "Champ";
            if (!w_info->is_widget) {
                if (word_eq_ci(w_info->nameFR, (long)strlen(w_info->nameFR),
                               "Etat"))
                    label = en ? "Report" : "Rapport";
                else
                    label = en ? "Sub-block" : "Sous-bloc";
            }
            printf("%s[%s %s]%s\n",
                   C_SECTION, label,
                   en && w_info->nameEN ? w_info->nameEN : w_info->nameFR,
                   C_RESET);
        }
        printf("%s%s%s", C_TYPE, w_info->nameFR, C_RESET);
        if (w_info->nameEN && strcmp(w_info->nameEN, w_info->nameFR) != 0) {
            printf(" %s/ %s%s", C_DIM, w_info->nameEN, C_RESET);
        }
        printf("\n  %s%s%s\n\n",
               C_DIM,
               en ? (w_info->is_widget ? "control" : "sub-block")
                  : (w_info->is_widget ? "champ"   : "sous-bloc"),
               C_RESET);
        /* Contexte parent en liste verticale, types en violet — plus
         * lisible que la phrase plate « peut être déclaré dans X, Y, Z ».
         * Les noms cites suivent la langue demandee : un lecteur EN doit
         * lire Report / Window / Looper, pas État / Fenêtre / ZoneRépétée. */
        printf("%s%s%s\n", C_SECTION,
               en ? "Parent context" : "Contexte parent", C_RESET);
        if (!w_info->is_widget) {
            if (word_eq_ci(w_info->nameFR, (long)strlen(w_info->nameFR),
                           "Etat")) {
                printf("  %stop-level (%s%s %s%s%s%s)%s\n",
                       C_DIM, en ? "triggered by" : "déclenché par", C_RESET,
                       C_NATIVE, en ? "iPrintReport" : "iImprimeEtat",
                       C_RESET, C_DIM, C_RESET);
            } else if (word_eq_ci(w_info->nameFR,
                                  (long)strlen(w_info->nameFR), "colonne")) {
                printf("  %sTable%s\n", C_TYPE, C_RESET);
            } else {
                printf("  %s%s%s\n", C_TYPE, en ? "Report" : "État", C_RESET);
            }
        } else if (word_eq_ci(w_info->nameFR, (long)strlen(w_info->nameFR),
                              "Fenêtre")) {
            printf("  %stop-level (%s%s %s%s%s%s)%s\n",
                   C_DIM, en ? "triggered by" : "déclenché par", C_RESET,
                   C_NATIVE, en ? "Open" : "Ouvre", C_RESET, C_DIM, C_RESET);
        } else {
            /* Widgets runtime — peuvent vivre dans plusieurs conteneurs. */
            printf("  %s%s%s\n", C_TYPE, en ? "Window"    : "Fenêtre", C_RESET);
            printf("  %s%s%s\n", C_TYPE, en ? "Container" : "Conteneur", C_RESET);
            printf("  %s%s%s\n", C_TYPE, en ? "Looper"    : "ZoneRépétée", C_RESET);
        }
    }
    return 0;
}

/* ==================================================================
 * owl template [<nom>] — squelettes prêts à l'emploi
 *
 * Les templates vivent dans le dossier `docs/templates/` et sont embarques au
 * build dans EMBEDDED_TEMPLATES. Chaque fichier a un header `// @nom:`
 * `// @description:` `// @niveau:` qu'on parse pour la liste. Quand
 * on imprime le code, ces lignes meta sont strippees pour qu'on puisse
 * rediriger vers un fichier directement utilisable. Export propre (UTF-8,
 * anti-ecrasement) recommande via -o plutot que la redirection shell :
 *     owl template hello -o MonScript.owl
 * ================================================================== */

typedef struct {
    char name[64];
    char description[256];
    char level[32];
} TemplateMeta;

/* Parse le header meta d'un template. Retourne le nombre d'octets a
 * skipper pour obtenir le debut du code (apres les lignes meta + la
 * ligne vide qui suit). */
static long parse_template_meta(const unsigned char* data, long len,
                                 TemplateMeta* out) {
    memset(out, 0, sizeof(*out));
    long i = 0;
    long last_meta_end = 0;
    while (i < len) {
        long line_start = i;
        while (i < len && data[i] != '\n') i++;
        long line_len = i - line_start;
        if (i < len) i++;
        const char* line = (const char*)(data + line_start);

        long lt = 0;
        while (lt < line_len && (line[lt] == ' ' || line[lt] == '\t')) lt++;

        /* Ligne vide → on continue tant qu'on n'a pas vu de meta yet ;
         * apres meta → on consomme la ligne vide separant header et
         * corps. */
        if (lt >= line_len) {
            if (last_meta_end > 0) return i;  /* premiere vide post-meta */
            continue;
        }
        /* Doit commencer par "//" pour etre un commentaire meta. */
        if (lt + 2 > line_len || line[lt] != '/' || line[lt+1] != '/') {
            return last_meta_end;  /* premiere ligne de code → stop */
        }
        long ci = lt + 2;
        while (ci < line_len && (line[ci] == ' ' || line[ci] == '\t')) ci++;
        if (ci >= line_len || line[ci] != '@') {
            return last_meta_end;
        }
        ci++;  /* skip '@' */
        long key_start = ci;
        while (ci < line_len &&
               ((line[ci] >= 'a' && line[ci] <= 'z') ||
                (line[ci] >= 'A' && line[ci] <= 'Z'))) ci++;
        long key_len = ci - key_start;
        if (ci >= line_len || line[ci] != ':') return last_meta_end;
        ci++;  /* skip ':' */
        while (ci < line_len && (line[ci] == ' ' || line[ci] == '\t')) ci++;
        /* Trim trailing \r ou espaces sur la valeur. */
        long val_end = line_len;
        while (val_end > ci && (line[val_end-1] == ' ' || line[val_end-1] == '\t'
                                  || line[val_end-1] == '\r')) val_end--;
        long val_len = val_end - ci;

        /* Dispatch sur le nom du tag. */
        char* dst = NULL;
        size_t dst_size = 0;
        if (key_len == 3 && word_eq_ci(line + key_start, key_len, "nom")) {
            dst = out->name; dst_size = sizeof(out->name);
        } else if (key_len == 11 &&
                   word_eq_ci(line + key_start, key_len, "description")) {
            dst = out->description; dst_size = sizeof(out->description);
        } else if (key_len == 6 &&
                   word_eq_ci(line + key_start, key_len, "niveau")) {
            dst = out->level; dst_size = sizeof(out->level);
        }
        if (dst && val_len > 0) {
            size_t n = (size_t)val_len < dst_size - 1
                       ? (size_t)val_len : dst_size - 1;
            memcpy(dst, line + ci, n);
            dst[n] = '\0';
        }
        last_meta_end = i;
    }
    return last_meta_end;
}

/* Imprime le corps du template avec coloration OWL. `body` vient de
 * owl_template_emitted_body (EOL '\n' nu, deja canonique). */
static void print_template_body(const char* body) {
    int brace_depth = 0;
    const char* p = body;
    while (*p) {
        const char* eol = strchr(p, '\n');
        long line_len = eol ? (long)(eol - p) : (long)strlen(p);
        render_code_line_owl(p, line_len, &brace_depth);
        putchar('\n');
        p = eol ? eol + 1 : p + line_len;
    }
}

/*  : ecrit le corps du template dans un fichier, en UTF-8 sans BOM —
 * owl maitrise l'encodage, ce qui contourne la redirection shell (PowerShell
 * `>` produit de l'UTF-16 + accents casses). Retour 0 OK, -1 echec
 * d'ouverture.
 *
 *  : EOL natif plateforme via le helper commun (mode texte), pour que
 * `-o` et la redirection stdout donnent le meme fichier. `body` n'a que des
 * '\n' nus : pas de "\r\r\n" possible a la traduction. */
static int write_template_body_to_file(const char* path, const char* body) {
    FILE* f = owl_fopen_generated_source(path);
    if (!f) return -1;
    const char* p = body;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        fwrite(p, 1, line_len, f);
        fputc('\n', f);  /* traduit en CRLF sous Windows par le mode texte */
        p = eol ? eol + 1 : p + line_len;
    }
    fclose(f);
    return 0;
}

/* Corps émis d'un template embarqué, dans une chaîne allouée : header meta
 * strippé, '\r' de fin de ligne strippés, et contenu canonisé par le
 * formateur — exactement le contenu que `owl template <nom>` imprime,
 * exporte (-o) et sérialise (--json).
 *
 * La canonisation est faite ici, sur la SORTIE, et pas dans les sources de
 * `docs/public_templates/` : ces fichiers sont intouchables sans ticket dev
 * explicite. Le formateur est aussi ce qui rend l'alignement du
 * '=' juste sans padding en dur.
 *
 * Point de vérité unique, partagé avec le self-test (règle 38) qui vérifie
 * l'idempotence de ce contenu. Caller free(). */
char* owl_template_emitted_body(const OwlEmbeddedDoc* t) {
    if (!t || !t->data || t->len <= 0) return NULL;
    TemplateMeta meta;
    long body_start = parse_template_meta(t->data, t->len, &meta);
    if (body_start < 0 || body_start > t->len) body_start = 0;

    /* Borne haute : le corps sans les '\r' tient toujours dans la taille
     * restante (+1 pour le NUL). */
    size_t cap = (size_t)(t->len - body_start) + 2;
    char* body = (char*)malloc(cap);
    if (!body) return NULL;
    size_t o = 0;
    long i = body_start;
    while (i < t->len) {
        long line_start = i;
        while (i < t->len && t->data[i] != '\n') i++;
        long line_len = i - line_start;
        if (line_len > 0 && t->data[line_start + line_len - 1] == '\r') line_len--;
        if (i < t->len) i++;
        memcpy(body + o, t->data + line_start, (size_t)line_len);
        o += (size_t)line_len;
        body[o++] = '\n';
    }
    body[o] = '\0';

    /* Repli sur le corps brut si le formateur refuse la source : `owl
     * template` doit toujours rendre quelque chose. Le self-test attrape le
     * cas (template_fmt_failed). */
    char* canon = owl_format(body);
    if (canon == NULL) return body;
    free(body);
    return canon;
}

static int cmp_template_meta(const void* a, const void* b) {
    /* Ordre : basique → intermédiaire → avancé, puis alpha sur nom.
     * Les niveaux inconnus tombent en fin. */
    const TemplateMeta* ma = (const TemplateMeta*)a;
    const TemplateMeta* mb = (const TemplateMeta*)b;
    int wa = 99, wb = 99;
    if (word_eq_ci(ma->level, (long)strlen(ma->level), "basique"))       wa = 0;
    else if (word_eq_ci(ma->level, (long)strlen(ma->level), "intermédiaire")) wa = 1;
    else if (word_eq_ci(ma->level, (long)strlen(ma->level), "avancé"))   wa = 2;
    if (word_eq_ci(mb->level, (long)strlen(mb->level), "basique"))       wb = 0;
    else if (word_eq_ci(mb->level, (long)strlen(mb->level), "intermédiaire")) wb = 1;
    else if (word_eq_ci(mb->level, (long)strlen(mb->level), "avancé"))   wb = 2;
    if (wa != wb) return wa - wb;
    return strcmp(ma->name, mb->name);
}

int cli_doc_template(int argc, char** argv) {
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "template")) return 64;

    /* Mode liste : sans argument positionnel. */
    if (opts.pos_argc == 0) {
        TemplateMeta metas[64];
        const OwlEmbeddedDoc* docs[64];
        int n = 0;
        for (int i = 0; i < EMBEDDED_TEMPLATES_COUNT && n < 64; i++) {
            const OwlEmbeddedDoc* d = &EMBEDDED_TEMPLATES[i];
            parse_template_meta(d->data, d->len, &metas[n]);
            /* Fallback nom = stem si le header n'a pas @nom. */
            if (!metas[n].name[0]) {
                size_t k = strlen(d->name);
                if (k >= sizeof(metas[n].name)) k = sizeof(metas[n].name) - 1;
                memcpy(metas[n].name, d->name, k);
                metas[n].name[k] = '\0';
            }
            docs[n] = d;
            n++;
        }
        /* Tri par niveau puis nom. On ne re-trie pas docs[] avec metas[]
         * vu qu'on ne reaffiche pas le doc, juste les metas. */
        qsort(metas, (size_t)n, sizeof(TemplateMeta), cmp_template_meta);

        if (opts.json) {
            printf("{\n  \"templates\": [");
            for (int i = 0; i < n; i++) {
                if (i > 0) printf(",");
                printf("\n    {\n      \"name\": ");
                json_write_cstr(metas[i].name);
                printf(",\n      \"description\": ");
                json_write_cstr(metas[i].description);
                printf(",\n      \"level\": ");
                json_write_cstr(metas[i].level);
                printf("\n    }");
            }
            printf("\n  ],\n  \"count\": %d\n}\n", n);
            return 0;
        }

        printf("%sTemplates disponibles%s %s(%d)%s\n",
               C_SECTION, C_RESET, C_DIM, n, C_RESET);
        int wmax = 0;
        for (int i = 0; i < n; i++) {
            int w = (int)strlen(metas[i].name);
            if (w > wmax) wmax = w;
        }
        if (wmax < 10) wmax = 10;
        for (int i = 0; i < n; i++) {
            printf("  %s%-*s%s  %s%-14s%s  %s%s%s\n",
                   C_CMD, wmax, metas[i].name, C_RESET,
                   C_DIM, metas[i].level[0] ? metas[i].level : "—", C_RESET,
                   C_DIM, metas[i].description, C_RESET);
        }
        printf("\n  %sowl template <nom>%s pour imprimer le code.\n",
               C_CMD, C_RESET);
        printf("  %sowl template <nom> -o <fichier>%s pour exporter un .owl "
               "(UTF-8, n'écrase pas) — détails : %sowl help template%s.\n",
               C_CMD, C_RESET, C_CMD, C_RESET);
        return 0;
    }

    /* Mode show : argument positionnel = nom du template. */
    const char* wanted = opts.pos_argv[0];
    long wanted_len = (long)strlen(wanted);
    const OwlEmbeddedDoc* hit = NULL;
    TemplateMeta meta;
    memset(&meta, 0, sizeof(meta));
    for (int i = 0; i < EMBEDDED_TEMPLATES_COUNT; i++) {
        const OwlEmbeddedDoc* d = &EMBEDDED_TEMPLATES[i];
        TemplateMeta m;
        parse_template_meta(d->data, d->len, &m);
        const char* canon = m.name[0] ? m.name : d->name;
        if (word_eq_ci(wanted, wanted_len, canon) ||
            word_eq_ci(wanted, wanted_len, d->name)) {
            hit = d;
            meta = m;
            break;
        }
    }

    if (!hit) {
        fprintf(stderr, "owl template : '%s' inconnu.\n", wanted);
        fprintf(stderr, "  Tapez 'owl template' pour la liste.\n");
        return 2;
    }

    /* Corps émis unique pour les trois sorties : header meta strippé, EOL nu,
     * canonique au sens du formateur. */
    char* body = owl_template_emitted_body(hit);
    if (body == NULL) {
        fprintf(stderr, "owl template : '%s' est vide.\n", wanted);
        return 2;
    }

    /*  : sortie fichier directe (UTF-8) au lieu de la redirection
     * shell. Prioritaire sur --json/texte. Anti-ecrasement : refuse si le
     * fichier existe deja. */
    if (opts.out) {
        FILE* exist = fopen(opts.out, "rb");
        if (exist) {
            fclose(exist);
            fprintf(stderr, "owl template : '%s' existe déjà, refus d'écraser. "
                            "Choisir un autre nom ou supprimer le fichier.\n",
                    opts.out);
            free(body);
            return 2;
        }
        if (write_template_body_to_file(opts.out, body) != 0) {
            fprintf(stderr, "owl template : impossible d'écrire '%s'.\n",
                    opts.out);
            free(body);
            return 2;
        }
        const char* canon = meta.name[0] ? meta.name : hit->name;
        fprintf(stderr, "owl template : '%s' écrit (%s, UTF-8).\n",
                opts.out, canon);
        free(body);
        return 0;
    }

    if (opts.json) {
        const char* canon = meta.name[0] ? meta.name : hit->name;
        printf("{\n  \"name\": ");        json_write_cstr(canon);
        printf(",\n  \"description\": "); json_write_cstr(meta.description);
        printf(",\n  \"level\": ");       json_write_cstr(meta.level);
        printf(",\n  \"code\": ");
        json_write_string((const unsigned char*)body, (long)strlen(body));
        printf("\n}\n");
        free(body);
        return 0;
    }

    /* Texte : on imprime le code, lignes meta strippees. */
    print_template_body(body);
    free(body);
    /* Indice sur stderr uniquement : stdout peut etre redirige vers un .owl
     * (owl template hello > f.owl), ne pas le polluer. */
    fprintf(stderr, "\n(astuce : 'owl template %s -o <fichier>' exporte un "
                    ".owl directement — voir 'owl help template')\n",
            meta.name[0] ? meta.name : hit->name);
    return 0;
}

int cli_doc_search(int argc, char** argv) {
    char* scratch[16];
    DocOpts opts;
    parse_doc_opts(argc > 1 ? argc - 1 : 0,
                   argc > 1 ? argv + 1 : NULL,
                   &opts, scratch);

    if (reject_bad_flag(&opts, "search")) return 64;

    if (opts.pos_argc == 0) {
        fprintf(stderr, "owl search : mot-clé requis.\n");
        fprintf(stderr, "  Exemple : owl search date\n");
        return 1;
    }
    const char* needle = opts.pos_argv[0];

    /* Needle tokenisé : match = OU avec score (cf. tokenize_needle). */
    char tokens[SEARCH_MAX_TOKENS][SEARCH_TOKEN_LEN];
    int n_tokens = tokenize_needle(needle, tokens);
    if (n_tokens == 0) {
        /* Needle tout-espaces : rien à chercher. */
        if (opts.json) { printf("{\n  \"query\": "); json_write_cstr(needle);
            printf(",\n  \"total\": 0\n}\n"); return 0; }
        printf("%sAucun résultat.%s\n", C_DIM, C_RESET);
        return 0;
    }

    /* Compteurs par catégorie pour le JSON et le bilan. */
    int n_found = 0, n_natives = 0, n_types = 0, n_enums = 0, n_consts = 0, n_scen = 0, n_limits = 0, n_widgets = 0, n_prim = 0, n_props = 0;

    /* Pour ranker en OR : on parcourt en passes de score décroissant
     * (n_tokens → 1) et n'affiche dans chaque passe que les entrées dont
     * le score == passe courante. Coût total = O(n_tokens × N_entrées),
     * borné par SEARCH_MAX_TOKENS=8 — négligeable face au registry. */
    if (opts.json) {
        printf("{\n  \"query\": "); json_write_cstr(needle);
        printf(",\n  \"public_documentation\": [");
        int first_f = 1;
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
                char line[256];
                if (count_tokens_in_blob_ci(d->data, d->len, tokens, n_tokens, line, sizeof line) != pass) continue;
                const char* nmf[] = { d->name };
                int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_f) printf(",");
                first_f = 0;
                printf("\n    { \"name\": "); json_write_cstr(d->name);
                printf(", \"hint\": "); json_write_cstr(line); printf(" }");
                n_found++;
            }
          }
        }
        printf("\n  ],\n  \"natives\": [");
        int first_n = 1;
        int total = owl_registry_count();
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < total; i++) {
                const OwlNativeDef* d = owl_registry_at(i);
                const char* nf[] = { d->canonical, d->canonicalAscii, d->nameEN,
                                     d->descFR, d->descEN };
                if (count_tokens_in_fields(nf, 5, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { d->canonical, d->canonicalAscii, d->nameEN };
                int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_n) printf(",");
                first_n = 0;
                printf("\n    "); json_write_cstr(d->canonical);
                n_natives++;
            }
          }
        }
        printf("\n  ],\n  \"primitives\": [");
        int first_p = 1;
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {
            for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) {
                const char* pf[] = { p->name, p->nameAscii, p->nameEN, p->descFR, p->descEN };
                if (count_tokens_in_fields(pf, 5, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { p->name, p->nameAscii, p->nameEN };
                int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_p) printf(",");
                first_p = 0;
                printf("\n    "); json_write_cstr(p->name);
                n_prim++;
            }
          }
        }
        printf("\n  ],\n  \"types\": [");
        int first_t = 1;
        int ttotal = owl_registry_types_count();
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < ttotal; i++) {
                const OwlTypeDef* t = owl_registry_type_at(i);
                char tAscii[128];   /*  #2 */
                scanner_strip_accents(t->canonical, (int)strlen(t->canonical), tAscii, (int)sizeof tAscii);
                const char* tf[] = { t->canonical, tAscii, t->nameEN, t->descFR, t->descEN };
                if (count_tokens_in_fields(tf, 5, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { t->canonical, tAscii, t->nameEN };
                int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_t) printf(",");
                first_t = 0;
                printf("\n    "); json_write_cstr(t->canonical);
                n_types++;
            }
          }
        }
        printf("\n  ],\n  \"enums\": [");
        int first_e = 1;
        int etotal = owl_registry_enums_count();
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < etotal; i++) {
                const OwlEnum* e = owl_registry_enum_at(i);
                const char* ef[] = { e->name, e->descFR, e->descEN };
                if (count_tokens_in_fields(ef, 3, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { e->name };
                int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_e) printf(",");
                first_e = 0;
                printf("\n    "); json_write_cstr(e->name);
                n_enums++;
            }
          }
        }
        printf("\n  ],\n  \"constants\": [");
        int first_c = 1;
        int ctotal = owl_registry_consts_count();
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < ctotal; i++) {
                const OwlConstDef* c = owl_registry_const_at(i);
                const char* cf[] = { c->name, c->nameEN, c->descFR, c->descEN };
                if (count_tokens_in_fields(cf, 4, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { c->name, c->nameEN };
                int tier = entry_name_tier(nmf, 2, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_c) printf(",");
                first_c = 0;
                printf("\n    "); json_write_cstr(c->name);
                n_consts++;
            }
          }
        }
        printf("\n  ],\n  \"limits\": [");
        int first_l = 1;
        int ltotal = owl_registry_limit_count();
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < ltotal; i++) {
                const OwlLimitDef* L = owl_registry_limit_at(i);
                if (!L) continue;
                const char* lf[] = { L->name, L->descFR, L->descEN };
                if (count_tokens_in_fields(lf, 3, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { L->name };
                int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_l) printf(",");
                first_l = 0;
                printf("\n    "); json_write_cstr(L->name);
                n_limits++;
            }
          }
        }
        printf("\n  ],\n  \"scenarios\": [");
        int first_s = 1;
        ensure_scenarios_parsed();
        for (int pass = n_tokens; pass >= 1; pass--) {
            for (int i = 0; i < g_scenarios_count; i++) {
                char line[256];
                int score = 0;
                for (int t = 0; t < n_tokens; t++) {
                    if (substr_ci(g_scenarios[i].sujet, tokens[t]) ||
                        substr_ci(g_scenarios[i].resume, tokens[t]) ||
                        substr_in_blob_ci(g_scenarios[i].source->data,
                                          g_scenarios[i].source->len,
                                          tokens[t], line, sizeof line)) {
                        score++;
                    }
                }
                if (score == pass) {
                    if (!first_s) printf(",");
                    first_s = 0;
                    printf("\n    "); json_write_cstr(g_scenarios[i].source->path);
                    n_scen++;
                }
            }
        }
        printf("\n  ],\n  \"widgets\": [");
        int first_w = 1;
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
                const char* wf[] = { k->nameFR, k->nameAscii ? k->nameAscii : k->nameFR,
                                     k->nameEN, k->descFR, k->descEN };
                if (count_tokens_in_fields(wf, 5, tokens, n_tokens) != pass) continue;
                const char* nmf[] = { k->nameFR, k->nameAscii ? k->nameAscii : k->nameFR, k->nameEN };
                int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
                if (tier != tp) continue;
                if (!first_w) printf(",");
                first_w = 0;
                printf("\n    "); json_write_cstr(k->nameFR);
                n_widgets++;
            }
          }
        }
        printf("\n  ],\n  \"properties\": [");
        int first_pr = 1;
        for (int pass = n_tokens; pass >= 1; pass--) {
          for (int tp = 4; tp >= 1; tp--) {   /*  */
            for (int i = 0; i < ttotal; i++) {
                const OwlTypeDef* t = owl_registry_type_at(i);
                if (!t->fields) continue;
                for (const OwlField* f = t->fields; f->name; f++) {
                    const char* ff[] = { f->name, f->nameAccented, f->nameEN,
                                         f->descFR, f->descEN };
                    if (count_tokens_in_fields(ff, 5, tokens, n_tokens) != pass) continue;
                    const char* nmf[] = { f->name, f->nameAccented, f->nameEN };
                    int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
                    if (tier != tp) continue;
                    if (!first_pr) printf(",");
                    first_pr = 0;
                    printf("\n    { \"type\": "); json_write_cstr(t->canonical);
                    printf(", \"name\": ");
                    json_write_cstr(f->nameAccented ? f->nameAccented : f->name);
                    if (f->nameEN) { printf(", \"name_en\": "); json_write_cstr(f->nameEN); }
                    printf(" }");
                    n_props++;
                }
            }
          }
        }
        printf("\n  ],\n  \"total\": %d\n}\n",
               n_found + n_natives + n_types + n_enums + n_consts + n_scen + n_limits + n_widgets + n_prim + n_props);
        return 0;
    }

    /* --- Sortie texte --- */
    printf("%sRecherche de %s'%s'%s%s\n\n",
           C_SECTION, C_CONST, needle, C_RESET, C_RESET);

    /* Collecte : chaque categorie alimente g_search_hits ; le tri
     * par pertinence pure + le rendu a plat viennent ensuite. Fondations
     * (guides) : match sur le stem + le corps du guide. */
    g_search_hit_count = 0;
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        char line[256];
        int pass = count_tokens_in_blob_ci(d->data, d->len, tokens, n_tokens, line, sizeof line);
        if (pass < 1) continue;
        const char* nmf[] = { d->name };
        int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
        int idx_before = g_search_hit_count;
        search_add_hit(d->name, line, dl(opts.lang, "fondation", "guide"), C_CONST,
                       pass, tier, desc_occurrences(line, tokens, n_tokens));
        if (g_search_hit_count > idx_before) g_search_hits[idx_before].is_doc = 1;
        n_found++;
    }

    /* Fonctions natives */
    int total = owl_registry_count();
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (d->flags & OWL_NAT_INTERNAL) continue;
        const char* desc = type_to_lang(d->descFR, d->descEN, opts.lang);
        const char* nf[] = { d->canonical, d->canonicalAscii, d->nameEN, desc };
        int pass = count_tokens_in_fields(nf, 4, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { d->canonical, d->canonicalAscii, d->nameEN };
        int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(d->canonical, desc, dl(opts.lang, "native", "native"), C_NATIVE,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_natives++;
    }

    /* Types primitifs — briques VM documentees (chaîne, entier…). */
    for (const PrimitiveType* p = PRIMITIVE_TYPES; p->name; p++) {
        const char* desc = type_to_lang(p->descFR, p->descEN, opts.lang);
        const char* pf[] = { p->name, p->nameAscii, p->nameEN, desc };
        int pass = count_tokens_in_fields(pf, 4, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { p->name, p->nameAscii, p->nameEN };
        int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(p->name, desc, dl(opts.lang, "type primitif", "primitive"), C_TYPE,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_prim++;
    }

    /* Types composés (registry) */
    int ttotal = owl_registry_types_count();
    for (int i = 0; i < ttotal; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        const char* desc = type_to_lang(t->descFR, t->descEN, opts.lang);
        char tAscii[128];   /* sans accent -> "chaine" matche "chaîne" */
        scanner_strip_accents(t->canonical, (int)strlen(t->canonical), tAscii, (int)sizeof tAscii);
        const char* tf[] = { t->canonical, tAscii, t->nameEN, desc };
        int pass = count_tokens_in_fields(tf, 4, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { t->canonical, tAscii, t->nameEN };
        int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(t->canonical, desc, dl(opts.lang, "type", "type"), C_TYPE,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_types++;
    }

    /* Propriétés (champs) des types composés — indexées individuellement pour
     * qu'une prop dont seul le nom ou la description (FR OU EN) porte le terme
     * soit trouvable. Le match desc couvre descFR ET descEN indépendamment de
     * la langue de sortie : un terme EN-only reste trouvable en sortie FR. Le
     * hit affiche `Type..Prop` (les deux côtés lang-aware) et se tague
     * [propriété] pour distinguer la prop de son type parent. */
    for (int i = 0; i < ttotal; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t->fields) continue;
        for (const OwlField* f = t->fields; f->name; f++) {
            const char* fdesc = type_to_lang(f->descFR, f->descEN, opts.lang);
            const char* ff[] = { f->name, f->nameAccented, f->nameEN,
                                 f->descFR, f->descEN };
            int fpass = count_tokens_in_fields(ff, 5, tokens, n_tokens);
            if (fpass < 1) continue;
            const char* nmf[] = { f->name, f->nameAccented, f->nameEN };
            int ftier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!ftier) ftier = 1;
            const char* pname = (opts.lang == DOC_LANG_EN && f->nameEN) ? f->nameEN
                              : (f->nameAccented ? f->nameAccented : f->name);
            const char* tname = (opts.lang == DOC_LANG_EN && t->nameEN) ? t->nameEN
                                                                        : t->canonical;
            char label[96];
            snprintf(label, sizeof label, "%s..%s", tname, pname);
            search_add_hit(label, fdesc, dl(opts.lang, "propriété", "property"),
                           C_TYPE, fpass, ftier,
                           desc_occurrences(fdesc, tokens, n_tokens));
            n_props++;
        }
    }

    /* Champs / widgets DSL — parité avec `owl explain`, sans filtrer sur
     * is_widget (outil de découverte plat). */
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        const char* desc = type_to_lang(k->descFR, k->descEN, opts.lang);
        const char* wf[] = { k->nameFR, k->nameAscii ? k->nameAscii : k->nameFR,
                             k->nameEN, desc };
        int pass = count_tokens_in_fields(wf, 4, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { k->nameFR, k->nameAscii ? k->nameAscii : k->nameFR, k->nameEN };
        int tier = entry_name_tier(nmf, 3, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(k->nameFR, desc ? desc : "", dl(opts.lang, "champ", "widget"), C_TYPE,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_widgets++;
    }

    /* Énumérations */
    int etotal = owl_registry_enums_count();
    for (int i = 0; i < etotal; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        const char* desc = type_to_lang(e->descFR, e->descEN, opts.lang);
        const char* ef[] = { e->name, desc };
        int pass = count_tokens_in_fields(ef, 2, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { e->name };
        int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(e->name, desc, dl(opts.lang, "énum", "enum"), C_TYPE,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_enums++;
    }

    /* Constantes */
    int ctotal = owl_registry_consts_count();
    for (int i = 0; i < ctotal; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        const char* desc = type_to_lang(c->descFR, c->descEN, opts.lang);
        const char* cf[] = { c->name, c->nameEN, desc };
        int pass = count_tokens_in_fields(cf, 3, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { c->name, c->nameEN };
        int tier = entry_name_tier(nmf, 2, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(c->name, desc, dl(opts.lang, "const", "const"), C_CONST,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_consts++;
    }

    /* Limites compile-time */
    int ltotal = owl_registry_limit_count();
    for (int i = 0; i < ltotal; i++) {
        const OwlLimitDef* L = owl_registry_limit_at(i);
        if (!L) continue;
        const char* desc = type_to_lang(L->descFR, L->descEN, opts.lang);
        const char* lf[] = { L->name, desc };
        int pass = count_tokens_in_fields(lf, 2, tokens, n_tokens);
        if (pass < 1) continue;
        const char* nmf[] = { L->name };
        int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
        search_add_hit(L->name, desc, dl(opts.lang, "limite", "limit"), C_CONST,
                       pass, tier, desc_occurrences(desc, tokens, n_tokens));
        n_limits++;
    }

    /* Scénarios (tests documentés) — match méta (sujet/resume) + corps ; le
     * hint affiche le resume si méta, sinon la 1re ligne body matchée. */
    ensure_scenarios_parsed();
    for (int i = 0; i < g_scenarios_count; i++) {
        char line[256] = "";
        int score = 0, in_meta = 0, got_body_line = 0;
        for (int t = 0; t < n_tokens; t++) {
            char tline[256] = "";
            int m = substr_ci(g_scenarios[i].sujet, tokens[t]) ||
                    substr_ci(g_scenarios[i].resume, tokens[t]);
            int b = substr_in_blob_ci(g_scenarios[i].source->data,
                                      g_scenarios[i].source->len,
                                      tokens[t], tline, sizeof tline);
            if (m || b) {
                score++;
                if (m) in_meta = 1;
                if (b && !got_body_line) { memcpy(line, tline, sizeof line); got_body_line = 1; }
            }
        }
        if (score < 1) continue;
        const char* sujet = g_scenarios[i].sujet[0] ? g_scenarios[i].sujet
                                                     : g_scenarios[i].source->name;
        const char* nmf[] = { sujet };
        int tier = entry_name_tier(nmf, 1, tokens, n_tokens); if (!tier) tier = 1;
        const char* hint = in_meta ? g_scenarios[i].resume : line;
        search_add_hit(sujet, hint, dl(opts.lang, "scénario", "scenario"), C_NATIVE,
                       score, tier, desc_occurrences(hint, tokens, n_tokens));
        n_scen++;
    }

    /* Tri par pertinence PURE puis rendu a plat : le nom prime,
     * toutes categories confondues, chaque ligne taguée [categorie]. */
    qsort(g_search_hits, (size_t)g_search_hit_count, sizeof g_search_hits[0], search_hit_cmp);

    int total_hits = g_search_hit_count;
    if (total_hits == 0) {
        printf("%sAucun résultat.%s\n", C_DIM, C_RESET);
    } else {
        for (int i = 0; i < g_search_hit_count; i++) {
            const SearchHit* h = &g_search_hits[i];
            printf("  %s%-26s%s  %s[%s]%s  %s%s%s\n",
                   h->color, h->name, C_RESET,
                   C_SECTION, h->kind, C_RESET,
                   C_DIM, h->hint, C_RESET);
        }
        printf("\n%s%d résultat(s) : %d fondation(s), %d native(s), %d primitif(s), %d type(s), %d propriété(s), %d champ(s), %d enum(s), %d const(s), %d limite(s), %d scénario(s).%s\n",
               C_DIM, total_hits, n_found, n_natives, n_prim, n_types, n_props, n_widgets, n_enums, n_consts, n_limits, n_scen, C_RESET);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * owl __complete — backend tab completion shell
 * ------------------------------------------------------------------ */

/* Vrai si `s` commence par `prefix`, case ET accent insensible.
 * Le passage par `scanner_strip_accents` aligne sur l'invariant runtime
 * OWL : un user qui tape `Chaine` (sans accent) obtient à la fois les
 * formes `ChaineVersEntier` et `ChaîneVersEntier` en complétion.
 * Prefix vide → tout matche. */
static int starts_with_ci(const char* s, const char* prefix) {
    if (!prefix || !*prefix) return 1;
    if (!s) return 0;
    char sb[256], pb[256];
    int sn = scanner_strip_accents(s,      (int)strlen(s),      sb, (int)sizeof sb);
    int pn = scanner_strip_accents(prefix, (int)strlen(prefix), pb, (int)sizeof pb);
    if (sn < pn) return 0;
    for (int i = 0; i < pn; i++) {
        char cs = sb[i], cp = pb[i];
        if (cs >= 'A' && cs <= 'Z') cs = (char)(cs + 32);
        if (cp >= 'A' && cp <= 'Z') cp = (char)(cp + 32);
        if (cs != cp) return 0;
    }
    return 1;
}

static void emit_completion(const char* candidate, const char* prefix) {
    if (candidate && starts_with_ci(candidate, prefix)) {
        printf("%s\n", candidate);
    }
}

/* Top-level : sous-commandes appelables directement.
 * Liste à garder cohérente avec le dispatch dans main.c.  */
static const char* TOP_LEVEL_CMDS[] = {
    "help", "primer", "learn", "gotchas", "cheatsheet",
    "list", "explain", "examples", "search", "where", "icons", "template",
    "test", "fmt", "translate", "repl",
    NULL
};

/* Sous-verbes de `owl icons`. */
static const char* ICONS_VERBS[] = {
    "list", "search", "preview", NULL
};

/* Sous-commandes pour lesquelles `owl help <X>` a du contenu rédigé. */
static const char* HELP_TARGETS[] = {
    "learn", "primer", "completion",
    "list", "explain", "examples", "search", "where", "icons",
    "template", "new",
    "fmt", "check", "test", "translate",
    NULL
};

int cli_doc_complete(int argc, char** argv) {
    /* argv[0] = "__complete"
     * argv[1] = position (sous-commande en cours, ou "" pour top-level)
     * argv[2] = prefix (optionnel, peut être vide) */
    const char* position = (argc >= 2) ? argv[1] : "";
    const char* prefix   = (argc >= 3) ? argv[2] : "";

    /* Top-level : liste les sous-commandes. */
    if (!position || !*position || strcmp(position, "owl") == 0) {
        for (int i = 0; TOP_LEVEL_CMDS[i]; i++) {
            emit_completion(TOP_LEVEL_CMDS[i], prefix);
        }
        return 0;
    }

    if (strcmp(position, "list") == 0) {
        for (int i = 0; LIST_CATS[i]; i++) emit_completion(LIST_CATS[i], prefix);
        return 0;
    }

    if (strcmp(position, "icons") == 0) {
        for (int i = 0; ICONS_VERBS[i]; i++) emit_completion(ICONS_VERBS[i], prefix);
        return 0;
    }

    if (strcmp(position, "help") == 0) {
        for (int i = 0; HELP_TARGETS[i]; i++) emit_completion(HELP_TARGETS[i], prefix);
        return 0;
    }

    if (strcmp(position, "learn") == 0 || strcmp(position, "primer") == 0) {
        /* Aliases + noms de fichiers fondation. */
        for (const PublicDocAlias* a = PUBLIC_DOC_ALIASES; a->alias; a++) {
            emit_completion(a->alias, prefix);
        }
        for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
            emit_completion(d->name, prefix);
        }
        return 0;
    }

    if (strcmp(position, "template") == 0) {
        /* Noms canon des templates embarques. */
        TemplateMeta m;
        for (int i = 0; i < EMBEDDED_TEMPLATES_COUNT; i++) {
            const OwlEmbeddedDoc* d = &EMBEDDED_TEMPLATES[i];
            parse_template_meta(d->data, d->len, &m);
            const char* canon = m.name[0] ? m.name : d->name;
            emit_completion(canon, prefix);
        }
        return 0;
    }

    if (strcmp(position, "explain") == 0 || strcmp(position, "examples") == 0 ||
        strcmp(position, "where") == 0) {
        /* Tous les symboles du registry : natives, types, enums, constantes.
         * Émet la forme canonique + canonicalAscii quand elle diffère pour
         * permettre la complétion sans accent. */
        int total = owl_registry_count();
        for (int i = 0; i < total; i++) {
            const OwlNativeDef* d = owl_registry_at(i);
            emit_completion(d->canonical, prefix);
            if (d->canonicalAscii && strcmp(d->canonicalAscii, d->canonical) != 0)
                emit_completion(d->canonicalAscii, prefix);
        }
        int ttotal = owl_registry_types_count();
        for (int i = 0; i < ttotal; i++) {
            const OwlTypeDef* t = owl_registry_type_at(i);
            emit_completion(t->canonical, prefix);
        }
        int etotal = owl_registry_enums_count();
        for (int i = 0; i < etotal; i++) {
            const OwlEnum* e = owl_registry_enum_at(i);
            emit_completion(e->name, prefix);
        }
        int ctotal = owl_registry_consts_count();
        for (int i = 0; i < ctotal; i++) {
            const OwlConstDef* c = owl_registry_const_at(i);
            emit_completion(c->name, prefix);
        }
        return 0;
    }

    /* Position inconnue : rien à proposer, le shell remplira. */
    return 0;
}

/* ================================================================
 * Gotchas & Cheatsheet — fiches dédiées, mêmes mécaniques que primer.
 * Déléguent à `learn <alias>` : la fiche embarquée est rendue par
 * render_markdown_to_terminal, les flags (--plain, --json, --lang) sont
 * propagés tels quels.
 * ================================================================ */

static int delegate_to_learn(const char* alias, int argc, char** argv) {
    char* learn_argv[3];
    learn_argv[0] = "learn";
    learn_argv[1] = (char*)alias;
    int new_argc = 2;
    for (int i = 1; i < argc && new_argc < 3; i++) {
        learn_argv[new_argc++] = argv[i];
    }
    return cli_doc_learn(new_argc, learn_argv);
}

int cli_doc_gotchas(int argc, char** argv) {
    return delegate_to_learn("gotchas", argc, argv);
}

int cli_doc_cheatsheet(int argc, char** argv) {
    return delegate_to_learn("cheatsheet", argc, argv);
}

/* ================================================================
 * owl icons — exploration du catalogue d'icones Iconify embarque.
 *
 * Trois verbes : `list`, `search`, `preview`. Confort dev pur — pas
 * de decision architecturale, le catalogue resolveur est fige par
 * . On reutilise l'API publique exposee par
 * stdlib_icones.c (`owl_icones_catalog_find` qui ne pollue pas le
 * canal erreur native vs `owl_icones_namespace_lookup` qui materialise
 * un ObjIcone et emet une erreur si miss).
 *
 * Rendu visuel `preview` :
 *   - Defaut : Unicode demi-blocs (U+2580 `▀` + U+2584 `▄`) avec ANSI
 *     24-bit foreground/background. Marche dans tous les terminaux
 *     modernes (Windows Terminal, iTerm2, gnome-terminal, etc.).
 *     L'init ANSI VT100 sous Windows est faite par platform.c au boot
 *     d'owl.exe — pas a faire ici.
 *   - `--ascii` : rampe luminance ` .:-=+*#%@` (10 niveaux). Marche
 *     partout (cmd.exe legacy, MCP, pipe vers fichier). Stable pour
 *     les snapshots de test.
 * ================================================================ */
#include "icons_catalog.h"
#include "icons_canon.h"     /*  — mapping FR -> EN bilingue */
#include "owl_stdlib.h"      /* owl_icones_namespace_lookup, rasterize */
#include "object.h"          /* IS_ICONE, AS_ICONE, ObjIcone */

/* ---------------- Helpers communs ---------------- */

/* ============================================================
 *  — Categorisation OWL-native du catalogue.
 *
 * Les 12 sets Iconify (material-symbols, mdi, fluent, etc.) sont
 * des marques externes (Google, Microsoft, etc.) sans interet
 * semantique pour le user OWL. La regle 7 OWL impose de ne pas
 * exposer de marques tierces dans les surfaces user-facing.
 *
 * On regroupe les 12 sets sous 6 categories OWL-native, exposees
 * dans les outputs CLI (`list`, `search`, `preview`). Les noms de
 * sets Iconify restent acceptes en input `--set` comme alias
 * (compat) mais le display utilise les categories.
 *
 * `colored = true` : la categorie contient des icones colorees
 * natives. Utilise par preview pour preferer ces sets quand le nom
 * existe dans plusieurs categories (preview UX colored-first).
 * ============================================================ */
typedef struct {
    const char* owl_name;     /* "ui", "marques", "emoji", etc. */
    const char* desc;
    int8_t      set_ids[8];   /* set_id Iconify membres ; -1 = sentinel fin */
    bool        colored;      /* hint pour preview colored-prio */
} OwlIconCategory;

static const OwlIconCategory OWL_ICON_CATEGORIES[] = {
    /* set_id : 0=material-symbols, 1=mdi, 2=fluent, 3=vscode-icons,
     *          4=logos, 5=devicon, 6=fluent-emoji-flat, 7=carbon,
     *          8=game-icons, 9=simple-icons, 10=line-md, 11=flat-color-icons */
    { "ui",         "interface generique mono",  {0, 1, 2, 7, 10, -1, -1, -1}, false },
    { "fichiers",   "types de fichiers color",   {3, -1, -1, -1, -1, -1, -1, -1}, true  },
    { "marques",    "logos services langages",   {4, 5, 9, -1, -1, -1, -1, -1}, true  },
    { "emoji",      "emoji plat couleur",        {6, -1, -1, -1, -1, -1, -1, -1}, true  },
    { "jeu",        "fantasy RPG",               {8, -1, -1, -1, -1, -1, -1, -1}, false },
    { "classiques", "aplats colores classiques", {11, -1, -1, -1, -1, -1, -1, -1}, true  },
};
#define OWL_ICON_CATEGORIES_COUNT \
    ((int)(sizeof(OWL_ICON_CATEGORIES) / sizeof(OWL_ICON_CATEGORIES[0])))

/* Itération publique de la table des catégories d'icones. Permet aux
 * self-tests de scanner les labels `desc` user-facing (regle 7) sans
 * dupliquer la table hors de cli_doc.c. Chaque out NULL = pas demandé.
 * Renvoie 0 si i hors borne. */
int cli_doc_icon_category_count(void) {
    return OWL_ICON_CATEGORIES_COUNT;
}

int cli_doc_icon_category_at(int i, const char** out_owl_name,
                                    const char** out_desc) {
    if (i < 0 || i >= OWL_ICON_CATEGORIES_COUNT) return 0;
    if (out_owl_name) *out_owl_name = OWL_ICON_CATEGORIES[i].owl_name;
    if (out_desc)     *out_desc     = OWL_ICON_CATEGORIES[i].desc;
    return 1;
}

/* Retourne le nom de categorie OWL pour un set_id donne, ou "?" si
 * set_id non mappe (ne devrait pas arriver, le catalogue est figé
 * a la generation). */
static const char* icons_set_id_to_owl_category(int set_id) {
    for (int c = 0; c < OWL_ICON_CATEGORIES_COUNT; c++) {
        const int8_t* ids = OWL_ICON_CATEGORIES[c].set_ids;
        for (int i = 0; i < 8 && ids[i] >= 0; i++) {
            if (ids[i] == set_id) return OWL_ICON_CATEGORIES[c].owl_name;
        }
    }
    return "?";
}

/* Lookup categorie OWL par nom (case-insensible). NULL si pas trouve. */
static const OwlIconCategory* icons_find_owl_category(const char* name);

/* Comparaison case-insensitive ASCII (sets sont en kebab-case). */
static int icons_streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Index du set par nom (case-insensitive), ou -1 si inconnu. */
static int icons_set_id_of(const char* name) {
    if (name == NULL) return -1;
    for (size_t i = 0; i < OWL_ICONS_SET_COUNT; i++) {
        if (OWL_ICONS_SET_NAMES[i] == NULL) break;
        if (icons_streq_ci(OWL_ICONS_SET_NAMES[i], name)) return (int)i;
    }
    return -1;
}

/*  — Lookup categorie OWL par nom (case-insensible).
 * Definition de la forward declaration plus haut. */
static const OwlIconCategory* icons_find_owl_category(const char* name) {
    if (name == NULL) return NULL;
    for (int c = 0; c < OWL_ICON_CATEGORIES_COUNT; c++) {
        if (icons_streq_ci(OWL_ICON_CATEGORIES[c].owl_name, name)) {
            return &OWL_ICON_CATEGORIES[c];
        }
    }
    return NULL;
}

/*  — Comptage entries par categorie OWL (agrege les sets). */
static const size_t* icons_set_counts(void);  /* forward */
static size_t icons_owl_category_count(const OwlIconCategory* cat) {
    const size_t* cnt = icons_set_counts();
    size_t total = 0;
    for (int i = 0; i < 8 && cat->set_ids[i] >= 0; i++) {
        total += cnt[cat->set_ids[i]];
    }
    return total;
}

/* Comptage des entries par set_id. Resultat fige a la 1ere demande
 * (catalogue static const). */
static const size_t* icons_set_counts(void) {
    static size_t counts[16] = {0};
    static bool   done = false;
    if (done) return counts;
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        uint8_t sid = OWL_ICONS_CATALOG[i].set_id;
        if (sid < 16) counts[sid]++;
    }
    done = true;
    return counts;
}

/* Levenshtein simple borne (utilise localement pour did-you-mean sur
 * les 12 noms de set). On ne factorise pas avec stdlib_icones.c — sa
 * version est static + specialisee 47k entries avec normalize. */
static int icons_lev3(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 32 || lb > 32) return 4;
    if (la == 0) return lb;
    if (lb == 0) return la;
    int prev[33], cur[33];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        char ca = a[i-1];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        for (int j = 1; j <= lb; j++) {
            char cb = b[j-1];
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            int cost = (ca == cb) ? 0 : 1;
            int v = prev[j] + 1;
            int w = cur[j-1] + 1;
            if (w < v) v = w;
            w = prev[j-1] + cost;
            if (w < v) v = w;
            cur[j] = v;
        }
        for (int j = 0; j <= lb; j++) prev[j] = cur[j];
    }
    return prev[lb];
}

/* did-you-mean parmi les 12 sets : meilleur match avec distance <= 2
 * (les noms sont courts, on tolere 2 erreurs maxi). */
static const char* icons_set_did_you_mean(const char* name) {
    const char* best = NULL;
    int best_d = 3;
    for (size_t i = 0; i < OWL_ICONS_SET_COUNT; i++) {
        const char* s = OWL_ICONS_SET_NAMES[i];
        if (s == NULL) break;
        int d = icons_lev3(name, s);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

/* Recherche le premier index dans OWL_ICONS_CATALOG ou commence un set.
 * Le tableau est trie par (set_id, name) — on parcourt lineairement
 * (~47k iterations triviales, quelques ms ; pas besoin d'index secondaire
 * pour un usage CLI). Retourne SIZE_MAX si introuvable. */
static size_t icons_first_index_of_set(int set_id) {
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        if (OWL_ICONS_CATALOG[i].set_id == set_id) return i;
    }
    return (size_t)-1;
}

/* Convertit kebab-case Iconify (`file-type-pdf2`) en forme OWL DSL
 * (`file_type_pdf2`) pour le snippet de code dans `preview`. Le user
 * tape la forme underscore dans `.owl`. Ecrit dans `out` (cap inclut
 * '\0'). */
static void icons_kebab_to_owl(const char* src, char* out, size_t cap) {
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < cap) {
        out[i] = (src[i] == '-') ? '_' : src[i];
        i++;
    }
    out[i] = '\0';
}

/* ---------------- Aide top ---------------- */

static void print_icons_help(void) {
    printf("%sowl icons%s %s<verbe>%s [options] — explorer le catalogue d'icones embarque\n\n",
           C_CMD, C_RESET, C_ARG, C_RESET);
    printf("%sVerbes%s\n", C_SECTION, C_RESET);
    printf("  %sowl icons list%s %s--set <categorie>%s [%s--limit N%s] [%s--preview%s]\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
    printf("        liste les icones d'une categorie. Defaut --limit 50.\n");
    printf("        Sans --set : refuse et affiche les 6 categories disponibles.\n");
    printf("        Avec --preview : vignettes 16px en regard (--limit 10).\n");
    printf("  %sowl icons search%s %s<mot>%s [%s<mot2>%s ...] [%s--preview%s]\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
    printf("        recherche substring AND, FR/EN bilingue, case+accent insensible.\n");
    printf("        Defaut top 50. Avec --preview : vignettes 16px (--limit 10).\n");
    printf("  %sowl icons preview%s %s<nom>%s [%s--size N%s]\n",
           C_CMD, C_RESET, C_ARG, C_RESET, C_ARG, C_RESET);
    printf("        rend l'icone dans le terminal (Unicode demi-blocs + ANSI 24-bit).\n");
    printf("        Defaut taille 24 (legal : 16..128). Alias court : -size N.\n");
    printf("        Si l'icone existe en plusieurs categories, prefere la coloree.\n");
    printf("\n%sCategories%s (par defaut 6) : ui, fichiers, marques, emoji, jeu, classiques.\n",
           C_SECTION, C_RESET);
    printf("%sFooter preview%s : snippet de code OWL pret-a-coller + indication\n",
           C_SECTION, C_RESET);
    printf("`couleur_modifiable : oui|non` (heuristique sur la presence de `currentColor`).\n");
}

/* ---------------- list ---------------- */

/*  — Affiche les 6 categories OWL avec leur compteur agrege.
 * Sortie destinee a stderr quand c'est un message d'aide post-erreur,
 * stdout en mode --json. */
static void print_set_index(FILE* out) {
    /* Alignement colonne nom. */
    int wmax = 0;
    for (int c = 0; c < OWL_ICON_CATEGORIES_COUNT; c++) {
        int l = (int)strlen(OWL_ICON_CATEGORIES[c].owl_name);
        if (l > wmax) wmax = l;
    }
    for (int c = 0; c < OWL_ICON_CATEGORIES_COUNT; c++) {
        size_t n = icons_owl_category_count(&OWL_ICON_CATEGORIES[c]);
        fprintf(out, "  %-*s  %6zu icones   (%s)\n",
                wmax, OWL_ICON_CATEGORIES[c].owl_name, n,
                OWL_ICON_CATEGORIES[c].desc);
    }
}

static int icons_cmd_list(int argc, char** argv) {
    /* Parse --set <nom>, --limit N, --preview a la main. parse_doc_opts gere
     * deja --plain/--json/--lang ; --set/--limit/--preview sont specifiques. */
    const char* set_name = NULL;
    int  limit  = 50;        /* defaut */
    bool got_limit = false;
    bool preview = false;    /*  : mini preview 16px */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
            set_name = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
            got_limit = true;
        } else if (strcmp(argv[i], "--preview") == 0) {
            preview = true;
            if (!got_limit) limit = 10;  /* adapte pour mini previews */
        }
    }
    (void)got_limit;

    if (set_name == NULL) {
        printf("owl icons list : option --set <nom> requise.\n");
        printf("  Listing complet de 47k icones inutile — choisir une categorie :\n\n");
        print_set_index(stdout);
        printf("\n  Ex : owl icons list --set ui --limit 20\n");
        return 1;
    }

    /*  — Accepter categorie OWL (prioritaire) OU nom de set
     * Iconify (compat). Categorie = iter sur plusieurs sets aggreges. */
    const OwlIconCategory* cat = icons_find_owl_category(set_name);
    int single_sid = -1;
    if (cat == NULL) {
        single_sid = icons_set_id_of(set_name);
        if (single_sid < 0) {
            printf("owl icons list : categorie '%s' inconnue.\n", set_name);
            const char* sugg = icons_set_did_you_mean(set_name);
            if (sugg) printf("  Voulez-vous dire : %s ?\n", sugg);
            printf("\n  Categories disponibles :\n");
            print_set_index(stdout);
            return 1;
        }
    }

    /* Liste des set_ids a iterer : soit ceux d'une categorie, soit un
     * unique set Iconify. */
    int8_t sids[8];
    int    nsids = 0;
    if (cat != NULL) {
        for (int i = 0; i < 8 && cat->set_ids[i] >= 0; i++) {
            sids[nsids++] = cat->set_ids[i];
        }
    } else {
        sids[0] = (int8_t)single_sid;
        nsids = 1;
    }

    int shown = 0;
    bool preview_ok = preview;
    for (int s = 0; s < nsids && (limit <= 0 || shown < limit); s++) {
        size_t first = icons_first_index_of_set(sids[s]);
        if (first == (size_t)-1) continue;
        for (size_t i = first; i < OWL_ICONS_CATALOG_COUNT; i++) {
            if (OWL_ICONS_CATALOG[i].set_id != (uint8_t)sids[s]) break;
            if (limit > 0 && shown >= limit) break;
            printf("%s\n", OWL_ICONS_CATALOG[i].name);
            if (preview_ok) {
                extern int icons_render_unicode(const char*, int);
                int rc = icons_render_unicode(OWL_ICONS_CATALOG[i].name, 16);
                if (rc != 0) {
                    preview_ok = false;
                    fprintf(stderr,
                            "%s(rendu visuel indisponible — passe sans vignettes)%s\n",
                            C_DIM, C_RESET);
                }
            }
            shown++;
        }
    }
    /* Footer compteur. */
    size_t total = (cat != NULL)
        ? icons_owl_category_count(cat)
        : icons_set_counts()[single_sid];
    if (limit > 0 && (size_t)shown < total) {
        fprintf(stderr, "%s%d / %zu affichees%s (--limit %d, --limit 0 pour tout)\n",
                C_DIM, shown, total, C_RESET, limit);
    } else {
        fprintf(stderr, "%s%d icones%s\n", C_DIM, shown, C_RESET);
    }
    return 0;
}

/* ---------------- search ---------------- */

/* substring insensible : transforme chaque caractere ASCII de `hay`
 * en lowercase a la volee ; `needle` deja en lowercase. */
static bool icons_contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (size_t i = 0; hay[i] != '\0'; i++) {
        size_t j = 0;
        while (j < nl && hay[i + j] != '\0') {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != needle[j]) break;
            j++;
        }
        if (j == nl) return true;
    }
    return false;
}

static int icons_cmd_search(int argc, char** argv) {
    /* Collecte les mots positionnels + flag --preview / --limit. */
    char* words[16];
    int   nwords = 0;
    bool  preview = false;
    int   limit = 50;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--preview") == 0) {
            preview = true;
            if (limit == 50) limit = 10;  /* defaut adapte pour preview */
        } else if (strcmp(a, "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (a[0] != '-') {
            if (nwords < 16) words[nwords++] = (char*)a;
        }
    }

    if (nwords == 0) {
        printf("owl icons search : au moins un mot requis.\n");
        printf("  Ex : owl icons search save\n");
        printf("       owl icons search file pdf   (semantique AND)\n");
        printf("       owl icons search save --preview   (vignettes 16px)\n");
        return 1;
    }

    /* Pre-lowercase des needles (ASCII only — kebab-case Iconify). */
    char lw[16][64];
    for (int k = 0; k < nwords; k++) {
        size_t l = strlen(words[k]);
        if (l >= 64) l = 63;
        for (size_t i = 0; i < l; i++) {
            char c = words[k][i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            lw[k][i] = c;
        }
        lw[k][l] = '\0';
    }

    /*  — Expansion bilingue FR -> [EN+] par needle.
     *
     * Si la needle (normalisee accents+casse) matche un terme FR du
     * mapping `tools/icons_canon_fr_en/mapping.def`, ses tokens EN
     * synonymes deviennent des candidates additionnelles. Le matching
     * AND/OR : un name catalogue valide si pour CHAQUE needle, AU
     * MOINS UN candidate la couvre.
     *
     * Ex : `owl icons search maison rouge` =>
     *   needle 0 = "maison" -> candidates = [maison, home, house]
     *   needle 1 = "rouge"  -> candidates = [rouge, red, crimson]
     *   match = name contient (maison|home|house) AND (rouge|red|crimson)
     *
     * Si le terme n'est pas dans le mapping FR, la candidate reste seule
     * (= ancien comportement, fallback EN direct).
     */
    const char* cands[16][24];
    int cand_count[16];
    int bilingual_hits = 0;
    for (int k = 0; k < nwords; k++) {
        cand_count[k] = 1;
        cands[k][0] = lw[k];

        char fr_norm[64];
        owl_icones_canon_normalize_fr(lw[k], fr_norm, sizeof(fr_norm));

        const char* const* en_list = NULL;
        int en_count = 0;
        if (owl_icones_fr_to_en(fr_norm, &en_list, &en_count)) {
            bilingual_hits++;
            for (int j = 0; j < en_count && cand_count[k] < 24; j++) {
                cands[k][cand_count[k]++] = en_list[j];
            }
        }
    }

    /* Pour `--preview`, on a besoin du rasterizer. Si absent, fallback
     * en mode liste compact + warning. */
    bool preview_ok = preview;
    /* On testera per-icone : si la 1ere rasterize echoue, on bascule. */

    int shown = 0;
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        if (limit > 0 && shown >= limit) break;
        const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
        bool match = true;
        for (int k = 0; k < nwords && match; k++) {
            bool any = false;
            for (int j = 0; j < cand_count[k]; j++) {
                if (icons_contains_ci(e->name, cands[k][j])) {
                    any = true; break;
                }
            }
            if (!any) match = false;
        }
        if (!match) continue;
        /*  : affiche categorie OWL au lieu du set Iconify brut. */
        const char* cat_name = icons_set_id_to_owl_category(e->set_id);
        printf("%s/%s\n", cat_name, e->name);
        if (preview_ok) {
            /* Mini-thumbnail Unicode demi-bloc 24px. Pour ne pas surcharger
             * j'extrais l'impl dans la passe `preview` ci-dessous : on appelle
             * directement le helper. */
            extern int icons_render_unicode(const char*, int);
            int rc = icons_render_unicode(e->name, 16);  /*  : 24 -> 16 */
            if (rc != 0) {
                preview_ok = false;
                fprintf(stderr, "%s(rendu visuel indisponible — passe sans vignettes)%s\n",
                        C_DIM, C_RESET);
            }
        }
        shown++;
    }

    fprintf(stderr, "%s%d resultat(s) (--limit %d)%s%s.%s\n",
            C_DIM, shown, limit,
            (bilingual_hits > 0 ? " — expansion FR/EN" : ""),
            "",
            C_RESET);
    return 0;
}

/* ---------------- preview ---------------- */

/* Renders RGBA en demi-blocs Unicode + ANSI 24-bit.
 *
 *  : bufferisation single fwrite pour eviter le freeze
 * ligne-par-ligne sur Windows (stdout line-buffered + cout par appel
 * non-negligeable). On construit toute la chaine en RAM puis on flush
 * d'un coup. Pour 64x64 : ~64 * 32 * 32 chars = 64 KB max. Stack
 * insuffisant pour le worst-case, on alloue dynamique.
 */
static void icons_render_unicode_rgba(const unsigned char* rgba, int w, int h) {
    /* Pire cas par cellule : "\x1b[38;2;255;255;255;48;2;255;255;255m▀"
     * = 30 chars + 3 UTF-8 = 33. + 4 chars (\x1b[0m\n) en fin de ligne. */
    size_t cap = (size_t)w * (size_t)((h + 1) / 2) * 36 + 64;
    char*  buf = (char*)malloc(cap);
    if (buf == NULL) {
        /* OOM fallback : rendu direct (lent) */
        for (int y = 0; y < h; y += 2) {
            printf("\n");
        }
        return;
    }
    size_t n = 0;
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x++) {
            const unsigned char* top = &rgba[(y * w + x) * 4];
            int has_bot = (y + 1 < h);
            const unsigned char* bot = has_bot
                ? &rgba[((y + 1) * w + x) * 4] : NULL;
            int tr = top[0], tg = top[1], tb = top[2], ta = top[3];
            int br = has_bot ? bot[0] : 0;
            int bg = has_bot ? bot[1] : 0;
            int bb = has_bot ? bot[2] : 0;
            int ba = has_bot ? bot[3] : 0;
            int written;
            if (ta < 16 && ba < 16) {
                written = snprintf(buf + n, cap - n, "\x1b[0m ");
            } else if (ta >= 16 && ba >= 16) {
                written = snprintf(buf + n, cap - n,
                    "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm\xe2\x96\x80",
                    tr, tg, tb, br, bg, bb);
            } else if (ta >= 16) {
                written = snprintf(buf + n, cap - n,
                    "\x1b[49m\x1b[38;2;%d;%d;%dm\xe2\x96\x80",
                    tr, tg, tb);
            } else {
                written = snprintf(buf + n, cap - n,
                    "\x1b[49m\x1b[38;2;%d;%d;%dm\xe2\x96\x84",
                    br, bg, bb);
            }
            if (written > 0 && (size_t)written < cap - n) n += (size_t)written;
        }
        int eol = snprintf(buf + n, cap - n, "\x1b[0m\n");
        if (eol > 0 && (size_t)eol < cap - n) n += (size_t)eol;
    }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    free(buf);
}

/* Helper exporte (utilise par icons_cmd_search en mode --preview).
 * Retourne 0 sur succes, !=0 si le rasterizer n'est pas dispo ou si
 * l'icone est absente. Sortie sur stdout. */
int icons_render_unicode(const char* name, int target_size) {
    Value icn = owl_icones_namespace_lookup(name);
    if (!IS_ICONE(icn) || !AS_ICONE(icn)->valide) return 2;
    unsigned char* rgba = NULL;
    int w = 0, h = 0;
    if (!owl_icone_rasterize_value(icn, target_size, &rgba, &w, &h)) return 1;
    icons_render_unicode_rgba(rgba, w, h);
    free(rgba);
    return 0;
}

static int icons_cmd_preview(int argc, char** argv) {
    const char* name = NULL;
    int  size = 24;  /*  : default 24 (ajustement post-test dev) */
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        /*  : `-size` accepte comme alias court de `--size`. */
        if ((strcmp(a, "--size") == 0 || strcmp(a, "-size") == 0)
                && i + 1 < argc) {
            size = atoi(argv[++i]);
        } else if (a[0] != '-' && name == NULL) {
            name = a;
        } else if (a[0] == '-') {
            printf("owl icons preview : flag inconnu '%s'.\n", a);
            printf("  Flags valides : --size N (ou -size N), 16..128.\n");
            return 1;
        }
    }
    if (name == NULL) {
        printf("owl icons preview : nom d'icone requis.\n");
        printf("  Ex : owl icons preview save\n");
        printf("       owl icons preview file-type-pdf2 --size 64\n");
        return 1;
    }
    if (size < 16 || size > 128) {
        printf("owl icons preview : --size %d hors borne legale [16..128].\n",
               size);
        return 1;
    }

    /*  — Preview colored-first : si l'icone existe dans une
     * categorie colored (fichiers, marques, emoji, classiques), preferer
     * cette version pour le preview. Le user voit la version la plus
     * representative visuellement. Le runtime `icones.X` continue
     * d'utiliser sa priorite (material-symbols-first) pour la coherence
     * UI dans le code OWL. */
    const OwlIconCatalogEntry* entry = NULL;
    int found_colored = 0;
    int found_any = 0;
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
        if (!icons_streq_ci(e->name, name)) continue;
        if (!found_any) { entry = e; found_any = 1; }
        /* Check si ce set est dans une categorie colored. */
        for (int c = 0; c < OWL_ICON_CATEGORIES_COUNT && !found_colored; c++) {
            if (!OWL_ICON_CATEGORIES[c].colored) continue;
            const int8_t* ids = OWL_ICON_CATEGORIES[c].set_ids;
            for (int k = 0; k < 8 && ids[k] >= 0; k++) {
                if (ids[k] == e->set_id) {
                    entry = e;
                    found_colored = 1;
                    break;
                }
            }
        }
        if (found_colored) break;
    }
    if (entry == NULL) {
        /* Fallback : essayer le lookup normalise (case+accent insensible) si
         * le scan strict ci-dessus a echoue (peu probable mais robuste). */
        entry = owl_icones_catalog_find(name);
    }
    if (entry == NULL) {
        printf("owl icons preview : icone introuvable '%s'.\n", name);
        const char* sugg = owl_icones_namespace_did_you_mean(name);
        if (sugg) printf("  Voulez-vous dire : %s ?\n", sugg);
        else      printf("  Essayer : owl icons search <mot>\n");
        return 2;
    }

    /*  — Materialisation directe sans repasser par le lookup
     * case-insensible (qui retomberait sur la priorite mono-first du
     * resolver runtime). Ainsi preview affiche bien la version colored
     * selectionnee ci-dessus. */
    Value icn = owl_icones_materialize_entry(entry);
    if (!IS_ICONE(icn) || !AS_ICONE(icn)->valide) {
        fprintf(stderr, "owl icons preview : materialisation KO pour '%s'.\n",
                name);
        return 2;
    }

    unsigned char* rgba = NULL;
    int w = 0, h = 0;
    if (!owl_icone_rasterize_value(icn, size, &rgba, &w, &h)) {
        fprintf(stderr,
                "owl icons preview : rendu visuel indisponible "
                "(rasterizer Qt6Svg absent dans cette build).\n");
        return 1;
    }
    icons_render_unicode_rgba(rgba, w, h);
    free(rgba);

    /* Footer : snippet de code OWL + couleur_modifiable + categorie. */
    char owlname[128];
    icons_kebab_to_owl(entry->name, owlname, sizeof(owlname));
    const char* cat_name = icons_set_id_to_owl_category(entry->set_id);
    printf("\n");
    printf("  %sicone%s ICN_demo est une %sIcone%s = icones.%s\n",
           C_DIM, C_RESET, C_TYPE, C_RESET, owlname);
    printf("  %scategorie : %s%s   %scouleur_modifiable : %s%s\n",
           C_DIM, cat_name, C_RESET,
           C_DIM, entry->couleur_modifiable ? "oui" : "non", C_RESET);
    return 0;
}

/* ---------------- dispatch ---------------- */

int cli_doc_icons(int argc, char** argv) {
    /* argv[0] = "icons" */
    if (argc < 2) {
        print_icons_help();
        return 0;
    }
    const char* verb = argv[1];
    if (strcmp(verb, "list") == 0)    return icons_cmd_list(argc - 2, argv + 2);
    if (strcmp(verb, "search") == 0)  return icons_cmd_search(argc - 2, argv + 2);
    if (strcmp(verb, "preview") == 0) return icons_cmd_preview(argc - 2, argv + 2);
    if (strcmp(verb, "--help") == 0 || strcmp(verb, "-h") == 0 ||
        strcmp(verb, "help") == 0) {
        print_icons_help();
        return 0;
    }
    fprintf(stderr, "owl icons : verbe inconnu '%s'.\n", verb);
    fprintf(stderr, "  Verbes valides : list, search, preview.\n");
    fprintf(stderr, "  Tapez 'owl icons' pour l'aide.\n");
    return 1;
}
