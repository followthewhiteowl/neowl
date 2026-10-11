// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test fmt-stability ( + extension cross-fichier ).
 *
 * Porte la categorie `fmt-stability` de run_compiler_tests.sh en in-process.
 * Pour chaque `tests/compiler/fmt-stability/<name>.owl` (hors `*.expected.owl`),
 * formate via `owl_format(src)` et compare a `<name>.expected.owl`
 * apres normalisation CRLF->LF des deux cotes. Le formatter applique
 * toujours la normalisation whitespace (espace autour des operateurs,
 * pas d'espace interne aux parentheses, 1 espace apres virgule).
 *
 * Motivation : la version bash spawnait owl.exe + mktemp + cp + rm par
 * fixture (26 fixtures, ~15s sous MSYS2). In-process : un seul boot, le
 * formatter est appele directement. Aucun changement de semantique de test.
 *
 * Extension  : si un sous-dossier contient `projet.owl`, on traite
 * chacun de ses `*.owl` (hors expected/projet) avec un OwlFormatContext
 * construit a partir des autres fichiers du sous-dossier (cross-fichier).
 *
 * Extension  : le harnais ne se tait plus sur un appariement manquant.
 * Trois regles, dans cet ordre :
 *   1. Tout `<name>.owl` (hors `projet.owl`) DOIT avoir son `<name>.expected.owl`.
 *      Son absence est un ECHEC, plus un SKIP. C'est ce SKIP indistinguable qui
 *      a laisse deux `.expected.owl` orphelins simuler une couverture.
 *   2. Un fichier qui n'est la que pour fournir des symboles au voisinage
 *      (contexte cross-fichier) se DECLARE par son nom : `<name>.companion.owl`.
 *      Meme axe que `.expected.owl` — le role du fichier est dans son nom, donc
 *      lisible par un humain qui liste le dossier, et pas dans une liste
 *      out-of-band qui derive. Il est compte et affiche, jamais silencieux.
 *   3. Tout `<name>.expected.owl` DOIT avoir son entree `<name>.owl`. Un
 *      snapshot orphelin est un ECHEC (il ne teste rien et ment sur la couverture).
 * Le harnais affiche le nombre de cas reellement executes et ECHOUE si ce
 * nombre est nul (corpus vide / dossier absent) : un controle doit prouver
 * qu'il a examine quelque chose.
 *
 * Extension  : une seconde section `fmt-eol-parity` travaille en
 * memoire et en octets bruts, SANS strip_cr. Elle couvre ce que la
 * comparaison de fixtures ne peut structurellement pas voir (parite du
 * verdict `fmt --check` entre LF et CRLF, preservation de l'EOL du
 * fichier). Voir son bloc de commentaire dedie plus bas.
 *
 * Branche sur `owl --self-test-fmt-stability` (cf src/main.c), gate `make all`.
 * Retour : 0 si tout passe, sinon le nombre d'echecs. */

#include "formatter.h"
#include "prescan.h"
#include "project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;
/* Compagnons declares (`*.companion.owl`) : comptes et affiches, jamais
 * silencieux — un fichier ecarte du corpus doit se voir dans le bilan. */
static int g_companions = 0;

#define FMT_DIR "tests/compiler/fmt-stability"
#define COMPANION_SUFFIX ".companion.owl"
#define EXPECTED_SUFFIX  ".expected.owl"

/* `name` se termine-t-il par `suffix` ? */
static bool has_suffix(const char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

/* Un `<stem>.expected.owl` sans `<stem>.owl` ne teste rien : il ne sera jamais
 * apparie, donc jamais formate, donc jamais compare. C'est le cas qui a survecu
 * des mois en silence. */
static void check_orphan_expected(const char* dir, const char* name) {
    size_t len = strlen(name);
    size_t stem = len - strlen(EXPECTED_SUFFIX);
    char inPath[1024];
    snprintf(inPath, sizeof inPath, "%s/%.*s.owl", dir, (int)stem, name);
    if (file_exists(inPath)) return;
    g_fail++;
    fprintf(stderr, "  ORPHELIN %s/%s : snapshot sans entree — aucun %.*s.owl "
                    "a formater, cette fixture ne teste rien (ecrire l'entree "
                    "ou supprimer le snapshot)\n",
            dir, name, (int)stem, name);
}

/* Message unique pour une entree sans snapshot : ECHEC, et il nomme la sortie
 * legitime (se declarer compagnon) pour qu'aucun oubli ne puisse s'y cacher. */
static void report_missing_expected(const char* dir, const char* name) {
    size_t stem = strlen(name) - 4;
    g_fail++;
    fprintf(stderr, "  MANQUE %s/%s : aucun %.*s.expected.owl — poser le "
                    "snapshot, ou renommer en %.*s" COMPANION_SUFFIX " si ce "
                    "fichier n'existe que pour fournir des symboles au "
                    "voisinage\n",
            dir, name, (int)stem, name, (int)stem, name);
}

/* Lit un fichier entier (NULL si absent). Caller free(). */
static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Strip les '\r' in-place (CRLF -> LF). Le formatter ecrit CRLF sous
 * Windows, les snapshots sont versionnes en LF — on neutralise des deux
 * cotes pour comparer le contenu, pas le line-ending. */
static void strip_cr(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) {
        if (*r != '\r') *w++ = *r;
    }
    *w = '\0';
}

/* Traite une fixture `<name>.owl` du dossier racine FMT_DIR (intra-fichier). */
static void run_one(const char* name) {
    size_t len = strlen(name);

    char inPath[1024];
    snprintf(inPath, sizeof inPath, "%s/%s", FMT_DIR, name);

    /* expected = <stem>.expected.owl (stem = name sans le ".owl" final) */
    char expPath[1024];
    snprintf(expPath, sizeof expPath, "%s/%.*s.expected.owl",
             FMT_DIR, (int)(len - 4), name);

    char* src = slurp(inPath);
    char* exp = slurp(expPath);
    if (!src) { free(exp); return; }
    if (!exp) {
        report_missing_expected(FMT_DIR, name);
        free(src);
        return;
    }

    char* out = owl_format(src);
    if (!out) {
        g_fail++;
        fprintf(stderr, "  FAIL fmt-stability/%s: owl_format a renvoye NULL\n", name);
        free(src); free(exp);
        return;
    }

    strip_cr(out);
    strip_cr(exp);

    if (strcmp(out, exp) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL fmt-stability/%s (formatter veut modifier — "
                        "regression d'indentation/casse)\n", name);
    }

    free(out); free(src); free(exp);
}

/*  — construit un OwlFormatContext en pre-scannant tous les
 * fichiers `.owl` du sous-dossier `subDir` SAUF `targetBasename`. Reproduit
 * la logique de fmt_build_context (main.c) pour les tests : les declarations
 * des fichiers voisins alimentent la canonisation de casse de la cible. */
static OwlFormatContext* build_subproject_context(const char* subDir,
                                                  const char* targetBasename) {
    Project project;
    project_init(&project);
    /* Scan recursif (le sous-dossier contient projet.owl). */
    if (!project_discover_scoped(&project, subDir, true, true)) {
        project_free(&project);
        return NULL;
    }

    SymbolTable* symbols = symtable_create();
    for (int i = 0; i < project.fileCount; i++) {
        /* Skip la cible elle-meme. Compare sur le basename pour eviter les
         * differences de separateurs `/` vs `\` selon project_discover. */
        const char* path = project.files[i].path;
        const char* base = path;
        for (const char* q = path; *q; q++) {
            if (*q == '/' || *q == '\\') base = q + 1;
        }
        if (strcmp(base, targetBasename) == 0) continue;
        prescan_file(symbols, project.files[i].source, i);
    }

    OwlFormatContext* ctx = owl_format_context_new();
    for (int i = 0; i < symbols->count; i++) {
        SymbolKind k = symbols->symbols[i].kind;
        if (k == SYM_CLASS || k == SYM_STRUCT || k == SYM_ENUM) {
            if (symtable_type_is_ambiguous(symbols, symbols->symbols[i].name))
                continue;
            owl_format_context_add_type(ctx, symbols->symbols[i].name);
        } else if (k == SYM_VARIABLE || k == SYM_CONSTANT ||
                   k == SYM_PROCEDURE || k == SYM_FUNCTION ||
                   k == SYM_WINDOW /*  */) {
            if (symtable_global_is_ambiguous(symbols, symbols->symbols[i].name))
                continue;
            owl_format_context_add_global(ctx, symbols->symbols[i].name);
            /*  : callables -> procGlobals (cf. formatter_context.c). */
            if (k == SYM_PROCEDURE || k == SYM_FUNCTION)
                owl_format_context_add_proc(ctx, symbols->symbols[i].name);
        }
    }
    for (int i = 0; i < symbols->memberCount; i++) {
        ProjectMember* m = &symbols->members[i];
        if (symtable_member_is_ambiguous(symbols, m->ownerType, m->name))
            continue;
        SymbolKind ownerKind = SYM_CLASS;
        for (int j = 0; j < symbols->count; j++) {
            if (strcasecmp(symbols->symbols[j].name, m->ownerType) == 0 &&
                (symbols->symbols[j].kind == SYM_CLASS ||
                 symbols->symbols[j].kind == SYM_STRUCT ||
                 symbols->symbols[j].kind == SYM_ENUM)) {
                ownerKind = symbols->symbols[j].kind; break;
            }
        }
        if (ownerKind == SYM_ENUM) {
            owl_format_context_add_enum_member(ctx, m->ownerType, m->name);
        } else {
            owl_format_context_add_member(ctx, m->ownerType, m->name,
                                          m->declType[0] ? m->declType : NULL);
        }
    }
    /*  : seed les methodes de classe comme membres (cf. note dans
     * formatter_context.c::owl_format_build_context). Le harness reproduit la
     * construction du contexte — il doit refleter la meme logique. */
    for (int i = 0; i < symbols->classMethodCount; i++) {
        const char* cls = symbols->classMethods[i].className;
        const char* mname = symbols->classMethods[i].sig.name;
        if (!cls[0] || !mname[0]) continue;
        owl_format_context_add_member(ctx, cls, mname, NULL);
    }

    symtable_destroy(symbols);
    project_free(&project);
    return ctx;
}

/* Traite une fixture `<name>.owl` d'un sous-dossier projet (cross-fichier). */
static void run_one_in_subproject(const char* subDir, const char* name) {
    size_t len = strlen(name);

    char inPath[1024];
    snprintf(inPath, sizeof inPath, "%s/%s", subDir, name);

    char expPath[1024];
    snprintf(expPath, sizeof expPath, "%s/%.*s.expected.owl",
             subDir, (int)(len - 4), name);

    char* src = slurp(inPath);
    char* exp = slurp(expPath);
    if (!src) { free(exp); return; }
    if (!exp) {
        report_missing_expected(subDir, name);
        free(src);
        return;
    }

    OwlFormatContext* ctx = build_subproject_context(subDir, name);
    char* out = owl_format_ctx(src, ctx);
    owl_format_context_free(ctx);

    if (!out) {
        g_fail++;
        fprintf(stderr, "  FAIL %s/%s: owl_format a renvoye NULL\n", subDir, name);
        free(src); free(exp);
        return;
    }

    strip_cr(out);
    strip_cr(exp);

    if (strcmp(out, exp) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL %s/%s (cross-fichier — casse non canonisee)\n",
                subDir, name);
    }

    free(out); free(src); free(exp);
}

/* Verifie si un chemin est un dossier. */
static bool is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

/* Verifie si `<dir>/projet.owl` existe (frontiere de projet, ). */
static bool dir_has_projet_marker(const char* dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s/projet.owl", dir);
    struct stat st;
    return stat(p, &st) == 0;
}

/* Scanne un sous-dossier projet : pour chaque `*.owl` qui n'est ni
 * `projet.owl` ni `*.expected.owl`, lance run_one_in_subproject. */
static void run_subproject(const char* subDir) {
    DIR* d = opendir(subDir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 4, ".owl") != 0) continue;
        if (has_suffix(n, EXPECTED_SUFFIX)) { check_orphan_expected(subDir, n); continue; }
        /* Compagnon declare : fournit des symboles au contexte cross-fichier,
         * n'est pas une fixture. Compte, pas silencieux. */
        if (has_suffix(n, COMPANION_SUFFIX)) { g_companions++; continue; }
        /* projet.owl : marqueur, non formate (sa DSL est un chantier futur). */
        if (strcmp(n, "projet.owl") == 0) continue;
        run_one_in_subproject(subDir, n);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ *
 * Parite EOL byte-level
 *
 * Les fixtures ci-dessus passent par strip_cr() des DEUX cotes : elles
 * comparent le contenu, jamais le line-ending. Ce harnais est donc
 * structurellement aveugle a deux regressions :
 *   1. une derive du formateur vers la normalisation d'EOL (CRLF -> LF),
 *      explicitement hors doctrine (, axe B abandonne) ;
 *   2. le critere central « `fmt --check` rend le meme verdict sur un
 *      contenu donne, quel que soit son EOL » — que strip_cr rend
 *      intestable puisqu'il efface justement la difference.
 *
 * Cette section travaille en memoire, en octets bruts, sans strip_cr :
 * chaque cas est un contenu en LF, duplique en CRLF, et les deux jumeaux
 * doivent rendre le meme verdict, le meme contenu modulo EOL, et
 * conserver chacun son propre EOL.
 * ------------------------------------------------------------------ */

/* Duplique `src` en remplacant chaque '\n' par "\r\n". Caller free(). */
static char* expand_to_crlf(const char* src) {
    size_t n = strlen(src), extra = 0;
    for (size_t i = 0; i < n; i++) if (src[i] == '\n') extra++;
    char* out = malloc(n + extra + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '\n') out[w++] = '\r';
        out[w++] = src[i];
    }
    out[w] = '\0';
    return out;
}

/* Duplique `src` en retirant les '\r'. Caller free(). */
static char* collapse_to_lf(const char* src) {
    char* out = malloc(strlen(src) + 1);
    if (!out) return NULL;
    strcpy(out, src);
    strip_cr(out);
    return out;
}

static size_t count_nl(const char* s) {
    size_t c = 0;
    for (const char* p = s; *p; p++) if (*p == '\n') c++;
    return c;
}

/* Tout '\n' est-il precede d'un '\r' ? (EOL CRLF integralement preserve) */
static bool all_nl_are_crlf(const char* s) {
    for (const char* p = s; *p; p++) {
        if (*p == '\n' && (p == s || *(p - 1) != '\r')) return false;
    }
    return true;
}

/* Compteurs dedies : la section parite EOL ne gonfle pas le decompte des
 * fixtures (qui reste lisible comme "N fixtures, M echecs"). */
static int g_eol_pass = 0;
static int g_eol_fail = 0;

static void eol_check(const char* caseName, bool cond, const char* what) {
    if (cond) {
        g_eol_pass++;
    } else {
        g_eol_fail++;
        fprintf(stderr, "  FAIL fmt-eol-parity/%s: %s\n", caseName, what);
    }
}

/* Un cas = un contenu canonique en LF. Le jumeau CRLF est derive. */
typedef struct {
    const char* name;
    const char* srcLF;
} EolCase;

static void run_eol_parity_case(const EolCase* c) {
    char* srcLF = collapse_to_lf(c->srcLF);   /* defensif : source garantie LF */
    char* srcCRLF = expand_to_crlf(srcLF);
    char* outLF = owl_format(srcLF);
    char* outCRLF = owl_format(srcCRLF);

    if (!outLF || !outCRLF) {
        g_eol_fail++;
        fprintf(stderr, "  FAIL fmt-eol-parity/%s: owl_format a renvoye NULL\n",
                c->name);
        free(srcLF); free(srcCRLF); free(outLF); free(outCRLF);
        return;
    }

    /* 1. Verdict `fmt --check` : "le formateur veut-il modifier ce fichier ?"
     *    Comparaison OCTET a OCTET avec la source, donc sensible a l'EOL.
     *    Le meme contenu doit rendre le meme verdict dans les deux EOL —
     *    c'est ce qui prouve que le XOR ligne vide LF/CRLF est mort. */
    bool dirtyLF = (strcmp(outLF, srcLF) != 0);
    bool dirtyCRLF = (strcmp(outCRLF, srcCRLF) != 0);
    eol_check(c->name, dirtyLF == dirtyCRLF,
              dirtyLF ? "verdict fmt --check divergent (LF veut modifier, CRLF non)"
                      : "verdict fmt --check divergent (CRLF veut modifier, LF non)");

    /* 2. EOL preserve, dans les deux sens. C'est precisement ce que strip_cr
     *    rend invisible : une normalisation CRLF -> LF passerait le test de
     *    contenu et rougirait ici. */
    eol_check(c->name, all_nl_are_crlf(outCRLF),
              "EOL non preserve : une sortie CRLF contient un '\\n' nu");
    eol_check(c->name, strchr(outLF, '\r') == NULL,
              "EOL non preserve : un '\\r' injecte dans une sortie LF");

    /* 3. Contenu identique modulo EOL : les deux jumeaux convergent vers le
     *    meme canon. */
    char* outCRLFasLF = collapse_to_lf(outCRLF);
    eol_check(c->name, strcmp(outCRLFasLF, outLF) == 0,
              "contenu divergent entre LF et CRLF (hors line-ending)");
    free(outCRLFasLF);

    /* 4. Idempotence byte-level, dans les deux EOL. */
    char* again = owl_format(outLF);
    eol_check(c->name, again && strcmp(again, outLF) == 0,
              "non idempotent en LF (2e passe modifie la sortie)");
    free(again);
    again = owl_format(outCRLF);
    eol_check(c->name, again && strcmp(again, outCRLF) == 0,
              "non idempotent en CRLF (2e passe modifie la sortie)");
    free(again);

    /* 5. Une ligne vide voulue reste une ligne : on retire l'indentation
     *    parasite, jamais la ligne. Le nombre de '\n' est conserve. */
    eol_check(c->name, count_nl(outLF) == count_nl(srcLF),
              "nombre de lignes modifie en LF (une ligne vide a ete avalee)");
    eol_check(c->name, count_nl(outCRLF) == count_nl(srcCRLF),
              "nombre de lignes modifie en CRLF (une ligne vide a ete avalee)");

    free(srcLF); free(srcCRLF); free(outLF); free(outCRLF);
}

static int run_eol_parity(void) {
    g_eol_pass = 0;
    g_eol_fail = 0;
    static const EolCase cases[] = {
        /* Ligne vide deja nue dans un bloc indente : le formateur ne doit rien
         * y toucher, ni en LF ni en CRLF. */
        { "ligne_vide_nue",
          "PROCEDURE P()\n\tTrace(\"a\")\n\n\tTrace(\"b\")\nFIN\n" },
        /* Ligne vide portant une indentation parasite : strippee dans les deux
         * EOL (le verdict "veut modifier" doit etre le meme des deux cotes). */
        { "ligne_vide_indentee",
          "PROCEDURE P()\n\tTrace(\"a\")\n\t\n\tTrace(\"b\")\nFIN\n" },
        /* Espaces au lieu de tabulations. */
        { "ligne_vide_espaces",
          "PROCEDURE P()\n\tTrace(\"a\")\n    \n\tTrace(\"b\")\nFIN\n" },
        /* Melange espaces + tabulations. */
        { "ligne_vide_mixte",
          "PROCEDURE P()\n\tTrace(\"a\")\n \t \n\tTrace(\"b\")\nFIN\n" },
        /* Plusieurs lignes vides consecutives, indentations differentes. */
        { "lignes_vides_consecutives",
          "PROCEDURE P()\n\tTrace(\"a\")\n\t\n  \n\t\t\n\tTrace(\"b\")\nFIN\n" },
        /* Ligne vide au niveau 0 (hors bloc). */
        { "ligne_vide_niveau_zero",
          "Trace(\"a\")\n\nTrace(\"b\")\n" },
        /* Imbrication profonde : l'indentation restauree est plus large. */
        { "ligne_vide_imbriquee",
          "PROCEDURE P()\n\tSI Vrai ALORS\n\t\tPOUR i = 1 A 3\n\t\t\t\n"
          "\t\t\tTrace(i)\n\t\tFIN\n\tFIN\nFIN\n" },
        /* Ligne vide juste avant la fermeture de bloc. */
        { "ligne_vide_avant_fin",
          "PROCEDURE P()\n\tTrace(\"a\")\n\t\nFIN\n" },
        /* Ligne vide en fin de fichier. */
        { "ligne_vide_fin_fichier",
          "PROCEDURE P()\n\tTrace(\"a\")\nFIN\n\t\n" },
        /* Ligne vide dans une chaine multi-ligne backtick : contenu OPAQUE,
         * emis verbatim. Garde-fou : le fix ne doit pas fuiter dans ce
         * chemin (alterer ces lignes changerait la valeur runtime). */
        { "ligne_vide_dans_backtick",
          "PROCEDURE P()\n\ts est une chaine = `ligne1\n\t\nligne2`\n"
          "\tTrace(s)\nFIN\n" },
        /* Ligne d'attribut serialise : acquis de , garde-fou de
         * non-regression sur l'isolation du CR en fin de ligne ordinaire. */
        { "attribut_serialise",
          "MaStruct est une Structure\n\tchamp est une chaine <Serialise = \"x\">\nFIN\n" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        run_eol_parity_case(&cases[i]);
    }

    fprintf(stderr, "fmt-eol-parity: %d pass, %d fail\n", g_eol_pass, g_eol_fail);
    /* Meme garde ensemble vide que les fixtures : un tableau de cas vide
     * rendrait "0 fail" sans avoir rien verifie. */
    if (g_eol_pass + g_eol_fail == 0) {
        g_eol_fail++;
        fprintf(stderr, "fmt-eol-parity: CORPUS VIDE — aucun cas execute.\n");
    }
    return g_eol_fail;
}

/* ------------------------------------------------------------------ *
 * Idempotence sur le corpus entier
 *
 * « Deux passes rendent le meme resultat qu'une. » L'invariant est verifiable
 * sur tout fichier `.owl` du depot, et son absence n'a rien de theorique :
 * elle a produit un faux depistage sur , ou un screening fonde sur UNE
 * passe a classe trois fichiers « blancs uniquement » alors que la reecriture
 * d'identifiants n'arrivait qu'a la seconde.
 *
 * La section `fmt-eol-parity` verifiait deja l'idempotence, mais sur onze cas
 * ecrits a la main : un corpus de laboratoire ne peut pas exhiber le fichier
 * reel ou la 2e passe decouvre une declaration que la 1re a canonisee. D'ou
 * cette passe, qui prend le corpus tel qu'il est.
 *
 * Portee : intra-fichier (`owl_format`, sans contexte cross-fichier). Le
 * contexte demanderait une decouverte de projet par fichier — cout sans
 * rapport avec ce qu'il ajoute, l'instabilite mesuree etant intra-fichier.
 * ------------------------------------------------------------------ */

#define IDEM_MAX_REPORTED 20

static int g_idem_pass = 0;
static int g_idem_fail = 0;
static int g_idem_reported = 0;

/* Formate deux fois et compare octet a octet. */
static void idem_check_file(const char* path) {
    char* src = slurp(path);
    if (!src) return;

    char* p1 = owl_format(src);
    if (!p1) {
        g_idem_fail++;
        if (g_idem_reported++ < IDEM_MAX_REPORTED)
            fprintf(stderr, "  FAIL fmt-idempotence/%s: 1re passe NULL\n", path);
        free(src);
        return;
    }
    char* p2 = owl_format(p1);
    if (!p2) {
        g_idem_fail++;
        if (g_idem_reported++ < IDEM_MAX_REPORTED)
            fprintf(stderr, "  FAIL fmt-idempotence/%s: 2e passe NULL\n", path);
        free(src); free(p1);
        return;
    }

    if (strcmp(p1, p2) == 0) {
        g_idem_pass++;
    } else {
        g_idem_fail++;
        if (g_idem_reported++ < IDEM_MAX_REPORTED) {
            /* Premier octet divergent : de quoi localiser sans dumper 300 lignes. */
            size_t i = 0;
            while (p1[i] && p2[i] && p1[i] == p2[i]) i++;
            fprintf(stderr, "  FAIL fmt-idempotence/%s: la 2e passe modifie la "
                            "sortie de la 1re (1er ecart a l'octet %zu)\n",
                    path, i);
        }
    }

    free(src); free(p1); free(p2);
}

/* Parcours recursif : tout `*.owl` sous `dir`. */
static void idem_walk(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;  /* . .. et dossiers caches */
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        if (is_dir(path)) { idem_walk(path); continue; }
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 4, ".owl") != 0) continue;
        idem_check_file(path);
    }
    closedir(d);
}

static int run_idempotence(void) {
    g_idem_pass = 0;
    g_idem_fail = 0;
    g_idem_reported = 0;

    /* Racines du corpus. Pas de walk depuis `.` : vendor/ et build/ pesent des
     * milliers de fichiers sans rapport. */
    static const char* roots[] = { "tests", "examples", "docs" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        idem_walk(roots[i]);
    }

    if (g_idem_reported > IDEM_MAX_REPORTED) {
        fprintf(stderr, "  ... %d autres fichiers non idempotents (non listes)\n",
                g_idem_reported - IDEM_MAX_REPORTED);
    }
    fprintf(stderr, "fmt-idempotence: %d fichiers, %d pass, %d fail\n",
            g_idem_pass + g_idem_fail, g_idem_pass, g_idem_fail);

    /* Meme garde ensemble vide que les autres sections : « 0 fail » sur zero
     * fichier examine ne prouve rien. */
    if (g_idem_pass + g_idem_fail == 0) {
        g_idem_fail++;
        fprintf(stderr, "fmt-idempotence: CORPUS VIDE — aucun .owl examine "
                        "(cwd = racine du depot attendue).\n");
    }
    return g_idem_fail;
}

/* ------------------------------------------------------------------ *
 * Accord entre `owl fmt --check` et `owl fmt`
 *
 * Le verificateur et l'ecriture partagent `owl_format_file_ctx` et un unique
 * `strcmp` ; `dryRun` ne choisit qu'entre imprimer et ecrire. Rien ne
 * l'imposait : un chemin qui normaliserait d'un cote et pas de l'autre
 * (fins de ligne, encodage, troncature) rendrait « non conforme » un fichier
 * que `owl fmt` laisse intact — et tout comptage de fichiers non conformes
 * deviendrait faux sans que rien ne rougisse. C'est ce qu'un releve de
 *  avait cru mesurer sur 15 fichiers.
 *
 * Le controle passe par le VRAI point d'entree fichier, sur un temporaire, et
 * confronte le verdict du dry-run a la modification effectivement ecrite.
 * ------------------------------------------------------------------ */

static int g_agree_pass = 0;
static int g_agree_fail = 0;

static void agree_check(const char* caseName, bool cond, const char* what) {
    if (cond) {
        g_agree_pass++;
    } else {
        g_agree_fail++;
        fprintf(stderr, "  FAIL fmt-check-accord/%s: %s\n", caseName, what);
    }
}

/* Ecrit `content` en octets bruts. */
static bool write_bytes(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(content);
    bool ok = (fwrite(content, 1, n, f) == n);
    fclose(f);
    return ok;
}

/* Un cas = un contenu depose dans un temporaire. `expectDirty` dit si le
 * formateur doit vouloir le modifier. Les deux directions comptent : un
 * fichier deja canon ne doit PAS etre declare non conforme (le sens qui
 * faussait la mesure), et un fichier non canon doit l'etre. */
static void run_agree_case(const char* caseName, const char* content,
                           bool expectDirty) {
    /* Point initial : ce temporaire nait dans le repertoire courant
     * — la racine du depot quand `make` lance ce self-test. Les walkers du
     * depot sautent les entrees pointees, donc une campagne `owl test`
     * concurrente ne le charge plus comme compagnon pour abandonner quand le
     * cas suivant l'efface. Le `remove` de fin nettoie, il ne ferme pas la
     * fenetre de course : elle est ouverte pendant l'execution. */
    char path[1024];
    snprintf(path, sizeof path, ".fmt_accord_%s.owl", caseName);
    if (!write_bytes(path, content)) {
        g_agree_fail++;
        fprintf(stderr, "  FAIL fmt-check-accord/%s: ecriture du temporaire "
                        "impossible\n", caseName);
        return;
    }

    /* Verdict du verificateur : 2 = « veut modifier » (ce que la CLI rend 1). */
    int rcDry = owl_format_file(path, /*dryRun*/ true);
    bool dryDirty = (rcDry == 2);

    /* Le fichier n'a pas bouge : le dry-run n'ecrit pas. */
    char* afterDry = slurp(path);
    agree_check(caseName, afterDry && strcmp(afterDry, content) == 0,
                "le dry-run a ecrit dans le fichier");
    free(afterDry);

    /* Ecriture reelle, puis mesure de la modification effective. */
    int rcWrite = owl_format_file(path, /*dryRun*/ false);
    char* afterWrite = slurp(path);
    bool wrote = (afterWrite && strcmp(afterWrite, content) != 0);
    free(afterWrite);

    agree_check(caseName, rcWrite == 0, "owl fmt a rendu un code d'erreur");
    agree_check(caseName, dryDirty == wrote,
                dryDirty ? "verdict divergent : --check dit non conforme, "
                           "l'ecriture est un no-op"
                         : "verdict divergent : --check dit conforme, "
                           "l'ecriture a modifie le fichier");
    agree_check(caseName, dryDirty == expectDirty,
                expectDirty ? "--check n'a pas vu un contenu non canon"
                            : "--check declare non conforme un contenu canon");

    remove(path);
}

static int run_check_accord(void) {
    g_agree_pass = 0;
    g_agree_fail = 0;

    /* Contenu deja canon, en LF puis en CRLF : le meme contenu doit rendre le
     * meme verdict quel que soit son EOL. C'est l'EOL mixte qui etait soupconne
     * de faire mentir le verificateur — il a donc son cas. */
    static const char* canonLF =
        "PROCEDURE FmtAccordCanon()\n\tTrace(\"a\")\nFIN\n";
    static const char* canonCRLF =
        "PROCEDURE FmtAccordCanon()\r\n\tTrace(\"a\")\r\nFIN\r\n";
    static const char* canonMixte =
        "PROCEDURE FmtAccordCanon()\r\n\tTrace(\"a\")\nFIN\r\n";
    /* Non canon : mot-cle en mauvaise casse + espace avant la virgule. */
    static const char* dirty =
        "procedure FmtAccordSale()\n\tTrace(\"a\" ,\"b\")\nfin\n";

    run_agree_case("canon_lf", canonLF, false);
    run_agree_case("canon_crlf", canonCRLF, false);
    run_agree_case("canon_mixte", canonMixte, false);
    run_agree_case("non_canon", dirty, true);

    fprintf(stderr, "fmt-check-accord: %d pass, %d fail\n",
            g_agree_pass, g_agree_fail);
    if (g_agree_pass + g_agree_fail == 0) {
        g_agree_fail++;
        fprintf(stderr, "fmt-check-accord: CORPUS VIDE — aucun cas execute.\n");
    }
    return g_agree_fail;
}

int fmt_stability_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    g_companions = 0;

    DIR* d = opendir(FMT_DIR);
    if (!d) {
        /* Dossier absent = ECHEC, plus un skip vert. Le harnais est branche sur
         * `make all` depuis la racine du depot : s'il ne trouve pas son corpus,
         * c'est qu'on l'execute sans corpus, pas qu'il n'y a rien a verifier.
         * La parite EOL travaille en memoire, elle tourne quand meme. */
        fprintf(stderr, "fmt-stability: CORPUS INTROUVABLE — %s absent "
                        "(cwd = racine du depot attendue). 0 cas examine.\n",
                FMT_DIR);
        return 1 + run_eol_parity();
    }

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        size_t len = strlen(n);

        /* Sous-dossier ? Si oui, check qu'il porte un projet.owl marqueur
         * (frontiere ) et traite ses fichiers cross-fichier. */
        if (n[0] != '.' && len > 0) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", FMT_DIR, n);
            if (is_dir(sub)) {
                if (dir_has_projet_marker(sub)) {
                    run_subproject(sub);
                }
                continue;
            }
        }

        if (len < 5 || strcmp(n + len - 4, ".owl") != 0) continue;
        /* Snapshots `*.expected.owl` : pas des entrees, mais on verifie que
         * chacun en a une. */
        if (has_suffix(n, EXPECTED_SUFFIX)) { check_orphan_expected(FMT_DIR, n); continue; }
        if (has_suffix(n, COMPANION_SUFFIX)) { g_companions++; continue; }
        run_one(n);
    }
    closedir(d);

    fprintf(stderr, "fmt-stability: %d cas executes (%d pass, %d fail), "
                    "%d compagnons declares\n",
            g_pass + g_fail, g_pass, g_fail, g_companions);

    /* Garde ensemble vide : sans ca, "0 fail" signifie soit "tout passe" soit
     * "rien n'a ete examine", et les deux se ressemblent trop. */
    if (g_pass + g_fail == 0) {
        g_fail++;
        fprintf(stderr, "fmt-stability: CORPUS VIDE — aucune fixture appariee "
                        "dans %s. Un controle qui n'examine rien ne prouve "
                        "rien.\n", FMT_DIR);
    }

    /* Parite EOL byte-level : hors du decompte fixtures, meme code de retour. */
    int eolFail = run_eol_parity();
    /* Idempotence corpus + accord --check/fmt : memes compteurs separes, meme
     * code de retour agrege. */
    int idemFail = run_idempotence();
    int accordFail = run_check_accord();
    return g_fail + eolFail + idemFail + accordFail;
}
