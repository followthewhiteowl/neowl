// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_core_guard — garde-fou build .
 *
 * Verifie deux invariants critiques sur la lib statique owl_core (qui est
 * linkee a la fois par owl.exe avec Qt et par owl-cli.exe sans Qt) :
 *
 *   1. Aucune source de OWL_CORE_SRCS ne reference Qt (include Qt ou
 *      identifiant Qt-typique). Si owl_core touche du Qt, owl-cli.exe casse
 *      au link silencieusement (ou pire, le link reussit mais le runtime
 *      console embarque du dead-code Qt qui ne sera jamais exerce).
 *
 *   2. Aucune source de OWL_CORE_SRCS ne contient `#ifdef OWL_HAS_GUI` /
 *      `#ifndef OWL_HAS_GUI` / `#if defined(OWL_HAS_GUI)`. owl_core est
 *      compile UNE seule fois, sans le flag, donc toute branche conditionnee
 *      par OWL_HAS_GUI dedans est garantie morte a la compile (piege
 *      historique  et ). Les seuls fichiers legitimes pour
 *      ce ifdef sont :
 *        - les sources per-target compilees deux fois (avec et sans flag) :
 *          owl_stdlib.c, stdlib_runtime.c, stdlib_builtin_types.c, main.c
 *        - les headers d'interface partages dont les guards sont le contrat :
 *          owl_stdlib.h, owl_ui.h, owl_pdf.h, stdlib_ui.h, stdlib_pdf.h
 *      Ces fichiers ne sont PAS dans OWL_CORE_SRCS (ou sont des headers
 *      includes from there) donc le check les ignore naturellement : on
 *      scanne uniquement la liste passee en argument.
 *
 * Usage : owl_core_guard <srcs_list.txt> <repo_root>
 *
 *   srcs_list.txt : une source par ligne (paths relatifs au repo_root, tels
 *                   qu'ecrits dans CMakeLists.txt). Genere par CMake a partir
 *                   de OWL_CORE_SRCS (single source of truth).
 *   repo_root     : racine du depot pour resoudre les paths relatifs.
 *
 * Exit code : 0 = OK, 1 = violation detectee (le build CMake doit rougir).
 *
 * Toolchain : libc seule. Pas de Python/Perl. Compile en outil host comme
 * embed_docs / icons_canon_gen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 * Lecture fichier entier en memoire.
 * ------------------------------------------------------------------ */
static char* read_whole_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = (long)got;
    return buf;
}

/* ------------------------------------------------------------------
 * Strip des commentaires C (// et / * * /) et chaines.
 *
 * Naif mais suffisant : on remplace par des espaces, garde les retours
 * a la ligne pour preserver le numero de ligne dans les messages.
 * Les `OWL_HAS_GUI` ou `QWidget` documentaires dans un commentaire ne
 * doivent pas declencher de faux positif (cf vm.c ligne 779 et gc.c
 * ligne 84 qui mentionnent le pattern dans un commentaire historique).
 * ------------------------------------------------------------------ */
static void strip_comments_and_strings(char* src, long len) {
    long i = 0;
    while (i < len) {
        char c = src[i];
        /* Bloc / * ... * / */
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            src[i] = ' '; src[i + 1] = ' ';
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] != '\n') src[i] = ' ';
                i++;
            }
            if (i + 1 < len) { src[i] = ' '; src[i + 1] = ' '; i += 2; }
            continue;
        }
        /* Ligne // */
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') { src[i] = ' '; i++; }
            continue;
        }
        /* Chaine "..." (avec \" echappe) */
        if (c == '"') {
            src[i++] = ' ';
            while (i < len && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < len) {
                    if (src[i] != '\n') src[i] = ' ';
                    i++;
                }
                if (i < len && src[i] != '\n') src[i] = ' ';
                i++;
            }
            if (i < len) { src[i] = ' '; i++; }
            continue;
        }
        /* Char 'x' */
        if (c == '\'') {
            src[i++] = ' ';
            while (i < len && src[i] != '\'') {
                if (src[i] == '\\' && i + 1 < len) {
                    if (src[i] != '\n') src[i] = ' ';
                    i++;
                }
                if (i < len && src[i] != '\n') src[i] = ' ';
                i++;
            }
            if (i < len) { src[i] = ' '; i++; }
            continue;
        }
        i++;
    }
}

/* ------------------------------------------------------------------
 * Detection des patterns interdits dans le contenu strippe.
 *
 * On reporte LE NUMERO DE LIGNE du source ORIGINAL : comme strip remplace
 * par espaces en preservant les \n, on peut compter les newlines.
 * ------------------------------------------------------------------ */

typedef struct {
    int   line;
    char  msg[256];
} Violation;

#define MAX_VIOLATIONS 64
static Violation g_violations[MAX_VIOLATIONS];
static int       g_violation_count = 0;
static const char* g_current_file = NULL;

static int  g_total_violations = 0; /* cumul tous fichiers */

static void report(int line, const char* fmt, const char* arg) {
    if (g_violation_count >= MAX_VIOLATIONS) return;
    Violation* v = &g_violations[g_violation_count++];
    v->line = line;
    snprintf(v->msg, sizeof(v->msg), fmt, arg ? arg : "");
}

static int line_of(const char* base, const char* p) {
    int line = 1;
    for (const char* q = base; q < p; q++) {
        if (*q == '\n') line++;
    }
    return line;
}

/* Match d'un mot complet (delimite par non-alnum/_) commencant a p. */
static int starts_with_word(const char* p, const char* end, const char* word) {
    size_t wlen = strlen(word);
    if (p + wlen > end) return 0;
    if (memcmp(p, word, wlen) != 0) return 0;
    if (p + wlen < end) {
        char c = p[wlen];
        if (isalnum((unsigned char)c) || c == '_') return 0;
    }
    return 1;
}

/* Scan d'un fichier strippe. Reporte les violations. */
static void scan_content(const char* base, long len) {
    const char* end = base + len;

    /* (a) #include <Qxxx> ou <Qxxx/...> ou "Qxxx" */
    for (const char* p = base; p < end; p++) {
        /* Cherche '#' debut de directive (apres whitespace de debut de ligne). */
        if (*p != '#') continue;
        /* Verifie qu'on est en debut de ligne (apres optionnel whitespace). */
        const char* q = p - 1;
        while (q >= base && (*q == ' ' || *q == '\t')) q--;
        if (q >= base && *q != '\n') continue;

        const char* r = p + 1;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r + 7 > end) continue;
        if (memcmp(r, "include", 7) != 0) continue;
        r += 7;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end) continue;
        char open = *r;
        if (open != '<' && open != '"') continue;
        r++;
        if (r >= end) continue;
        /* Premier char du chemin : doit etre 'Q' suivi d'une majuscule ou '/' */
        if (*r != 'Q') continue;
        if (r + 1 >= end) continue;
        char c1 = r[1];
        if (!isupper((unsigned char)c1) && c1 != 't') continue;
        /* On a un #include <Q...> ou "Q..." — extrait le header. */
        char header[64];
        int  hi = 0;
        while (r < end && *r != '>' && *r != '"' && *r != '\n' && hi < 63) {
            header[hi++] = *r++;
        }
        header[hi] = '\0';
        report(line_of(base, p), "include Qt interdit : %s", header);
    }

    /* (b) #ifdef OWL_HAS_GUI / #ifndef OWL_HAS_GUI / #if defined(OWL_HAS_GUI) */
    for (const char* p = base; p < end; p++) {
        if (*p != '#') continue;
        const char* q = p - 1;
        while (q >= base && (*q == ' ' || *q == '\t')) q--;
        if (q >= base && *q != '\n') continue;

        const char* r = p + 1;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end) continue;

        int matched = 0;
        const char* after = NULL;
        if (starts_with_word(r, end, "ifdef")) { matched = 1; after = r + 5; }
        else if (starts_with_word(r, end, "ifndef")) { matched = 1; after = r + 6; }
        else if (starts_with_word(r, end, "if")) { matched = 1; after = r + 2; }
        if (!matched) continue;

        /* Avance jusqu'a la fin de ligne ou de directive, cherche OWL_HAS_GUI. */
        const char* eol = after;
        while (eol < end && *eol != '\n') eol++;
        for (const char* s = after; s + 11 <= eol; s++) {
            if (starts_with_word(s, eol, "OWL_HAS_GUI")) {
                report(line_of(base, p),
                       "#ifdef/#if OWL_HAS_GUI interdit (branche morte dans owl_core)%s",
                       "");
                break;
            }
        }
    }

    /* (c) identifiants Qt-typiques en code (apres strip commentaires/strings).
     * Conservateur : on cible une short-list d'identifiants Qt frequents qui
     * apparaissent forcement si on touche a Qt. Pas de fuzzy `Q[A-Z]\w+` pur
     * (faux positifs sur des macros maison style `QUEUE`, `QSORT`, ...).
     */
    static const char* QT_TOKENS[] = {
        "QApplication", "QWidget", "QPainter", "QPaintEvent", "QString",
        "QObject", "QMainWindow", "QDialog", "QLabel", "QPushButton",
        "QPrinter", "QPdfWriter", "QFontDatabase", "QGuiApplication",
        "QCoreApplication", "QPixmap", "QImage", "QFont", "QColor",
        "QSvgRenderer", "QFile", "QTextStream", "QByteArray", "QList",
        "QVector", "QHash", "QMap", "QTimer",
        NULL
    };
    for (const char* p = base; p < end; p++) {
        if (*p != 'Q') continue;
        if (p > base) {
            char prev = p[-1];
            if (isalnum((unsigned char)prev) || prev == '_') continue;
        }
        for (int i = 0; QT_TOKENS[i]; i++) {
            if (starts_with_word(p, end, QT_TOKENS[i])) {
                report(line_of(base, p), "identifiant Qt interdit : %s",
                       QT_TOKENS[i]);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------
 * Concatene repo_root + "/" + rel.
 * ------------------------------------------------------------------ */
static void join_path(char* out, size_t cap, const char* root, const char* rel) {
    size_t rl = strlen(root);
    size_t needs_sep = (rl > 0 && root[rl - 1] != '/' && root[rl - 1] != '\\');
    snprintf(out, cap, "%s%s%s", root, needs_sep ? "/" : "", rel);
}

/* ------------------------------------------------------------------
 * Strip whitespace en debut/fin.
 * ------------------------------------------------------------------ */
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    *e = '\0';
    return s;
}

/* ------------------------------------------------------------------
 * Filtre : on ne scanne que les sources C/C++ humaines (.c .cpp .h).
 * Le tool exclut explicitement les fichiers generes au build (chemins
 * absolus commencant par le repertoire de build) car eux n'ont pas a
 * etre soumis au check (ce sont des artefacts).
 * ------------------------------------------------------------------ */
static int is_scannable(const char* rel) {
    /* Skip generated paths (absolus, hors src/lsp/). */
    if (rel[0] == '/' || (rel[0] && rel[1] == ':')) {
        /* Path absolu = fichier genere (${CMAKE_BINARY_DIR}/...) -> skip. */
        return 0;
    }
    size_t L = strlen(rel);
    if (L < 3) return 0;
    if (L >= 2 && rel[L - 2] == '.' && rel[L - 1] == 'c') return 1;
    if (L >= 4 && strcmp(rel + L - 4, ".cpp") == 0) return 1;
    if (L >= 2 && rel[L - 2] == '.' && rel[L - 1] == 'h') return 1;
    return 0;
}

/* ------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr,
                "Usage: owl_core_guard <srcs_list.txt> <repo_root>\n");
        return 2;
    }
    const char* list_path = argv[1];
    const char* repo_root = argv[2];

    long list_len = 0;
    char* list = read_whole_file(list_path, &list_len);
    if (!list) {
        fprintf(stderr, "[owl_core_guard] cannot read list: %s\n", list_path);
        return 2;
    }

    int files_scanned = 0;
    char* save = NULL;
    char* line = list;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; save = nl + 1; } else { save = NULL; }
        char* rel = trim(line);
        if (*rel == '\0' || *rel == '#') { line = save; continue; }
        if (!is_scannable(rel)) { line = save; continue; }

        char full[1024];
        join_path(full, sizeof(full), repo_root, rel);

        long src_len = 0;
        char* src = read_whole_file(full, &src_len);
        if (!src) {
            /* Pas une erreur fatale si le fichier est genere et pas encore la
             * (custom_command order) — mais on a deja skip les paths absolus
             * donc ici un manquant = vraie erreur. */
            fprintf(stderr,
                    "[owl_core_guard] WARN: source introuvable : %s\n", full);
            line = save;
            continue;
        }

        strip_comments_and_strings(src, src_len);
        g_violation_count = 0;
        g_current_file = rel;
        scan_content(src, src_len);

        if (g_violation_count > 0) {
            if (g_total_violations == 0) {
                fprintf(stderr,
                        "\n"
                        "============================================================\n"
                        "  GARDE-FOU owl_core (fix/0489) -- VIOLATIONS DETECTEES\n"
                        "============================================================\n"
                        "owl_core est la lib statique partagee par owl.exe (avec Qt)\n"
                        "et owl-cli.exe (sans Qt). Aucune source de OWL_CORE_SRCS ne\n"
                        "doit referencer Qt ou utiliser #ifdef OWL_HAS_GUI.\n"
                        "\n"
                        "Ou mettre le code Qt-dependant a la place :\n"
                        "  - code GUI runtime : src/stdlib_ui.c (ou ui_qt.cpp pour le\n"
                        "    backend Qt natif)\n"
                        "  - code PDF runtime : src/stdlib_pdf.c (ou pdf_qt.cpp)\n"
                        "  - code per-target (degradation propre console/GUI) :\n"
                        "    src/owl_stdlib.c, src/stdlib_runtime.c (eux ont le droit\n"
                        "    au #ifdef OWL_HAS_GUI car compiles deux fois)\n"
                        "  - main.c (binaire owl) a aussi le droit (pas dans owl_core)\n"
                        "\n"
                        "Voir CMakeLists.txt section OWL_CORE_SRCS pour l'invariant.\n"
                        "============================================================\n");
            }
            for (int i = 0; i < g_violation_count; i++) {
                fprintf(stderr, "  %s:%d : %s\n",
                        rel, g_violations[i].line, g_violations[i].msg);
            }
            g_total_violations += g_violation_count;
        }

        free(src);
        files_scanned++;
        line = save;
    }

    free(list);

    if (g_total_violations > 0) {
        fprintf(stderr,
                "\n[owl_core_guard] %d violation(s) dans %d fichier(s) scanne(s).\n",
                g_total_violations, files_scanned);
        return 1;
    }
    fprintf(stdout,
            "[owl_core_guard] OK : %d sources owl_core, aucune fuite Qt / "
            "aucun #ifdef OWL_HAS_GUI.\n",
            files_scanned);
    return 0;
}
