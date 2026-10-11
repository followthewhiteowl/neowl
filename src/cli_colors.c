// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_colors.c — implémentation palette ANSI canon OWL.
 *
 * Couleurs en ANSI 24-bit truecolor : ESC[38;2;R;G;Bm.
 * Reset commun  : ESC[0m.
 * Combinés avec style (bold ESC[1m, italic ESC[3m) selon le scope.
 *
 * Détection :
 *   - $NO_COLOR défini (peu importe la valeur) → plain (convention
 *     no-color.org).
 *   - $TERM=dumb ou non défini → plain.
 *   - !isatty(stdout) → plain (sauf cli_colors_force(FORCE)).
 *   - Sinon → colored (on assume truecolor sur les terminaux modernes ;
 *     fallback gracieux : un terminal qui ne comprend pas 24-bit
 *     affiche le texte sans couleur, jamais de garbage).
 */

#include "cli_colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #include <windows.h>
  #define isatty _isatty
  #define fileno _fileno
#else
  #include <unistd.h>
#endif

/* État global — sufficient pour un binaire CLI. */
static int g_initialized = 0;
static int g_enabled     = 0;

/* ESC[...] séquences pré-construites. Ordre = palette VSCode canon. */
#define ESC "\x1b["
#define RESET            ESC "0m"
#define BOLD             ESC "1m"
#define ITALIC           ESC "3m"
#define DIM              ESC "2m"

/* 24-bit foreground : ESC[38;2;R;G;Bm */
#define FG_FF8000        ESC "38;2;255;128;0m"   /* keyword orange */
#define FG_9575CD        ESC "38;2;149;117;205m" /* type violet */
#define FG_BAB819        ESC "38;2;186;184;25m"  /* constante jaune-vert */
#define FG_91B5FE        ESC "38;2;145;181;254m" /* native bleu clair */
#define FG_5C6BC0        ESC "38;2;92;107;192m"  /* error global indigo */
#define FG_BA68C8        ESC "38;2;186;104;200m" /* string rose-mauve */
#define FG_4CAF50        ESC "38;2;76;175;80m"   /* interp var vert */
#define FG_CE93D8        ESC "38;2;206;147;216m" /* interp brk rose pâle */
#define FG_808080        ESC "38;2;128;128;128m" /* commentaire gris */
#define FG_008054        ESC "38;2;0;128;84m"    /* nom procédure / champ vert foncé canon */
/* FG_4CAF50 défini plus haut (interp var) — réutilisé pour les
 * identifiants/variables, même couleur canon VSCode. */

/* Combinés style+couleur. */
static const char* C_RESET_STR       = RESET;

/* --- Palette OWL canon (alignée VSCode) --- */
static const char* C_KEYWORD_STR     = BOLD   FG_FF8000;
static const char* C_TYPE_STR        =        FG_9575CD;
static const char* C_CONST_STR       = ITALIC FG_BAB819;
static const char* C_NATIVE_STR      =        FG_91B5FE;
static const char* C_ERRGLOBAL_STR   =        FG_5C6BC0;
static const char* C_STRING_STR      =        FG_BA68C8;
static const char* C_INTERP_VAR_STR  =        FG_4CAF50;
static const char* C_INTERP_BRK_STR  = BOLD   FG_CE93D8;
static const char* C_COMMENT_STR     =        FG_808080;
static const char* C_PROC_STR        =        FG_008054;
static const char* C_IDENT_STR       =        FG_4CAF50;

/* --- Chrome CLI : distinct de la palette OWL pour ne pas embrouiller
 *     un utilisateur qui voit du « bleu » dans le menu et croit que
 *     c'est un appel à une fonction native. */
static const char* C_DIM_STR         = DIM;
static const char* C_EMPH_STR        = BOLD;
static const char* C_SECTION_STR     = BOLD   FG_008054;      /* entête section (vert foncé canon) */
static const char* C_CMD_STR         = BOLD;                  /* nom de commande shell */
static const char* C_ARG_STR         = DIM;                   /* <argument> */
static const char* C_MD_HEADING_STR  = BOLD   FG_008054;      /* # ## ### dans markdown (vert foncé canon) */

/* Vide en mode plain. */
static const char* EMPTY = "";

/* ------------------------------------------------------------------
 * Détection + init
 * ------------------------------------------------------------------ */

static int detect_color_capable(void) {
    /* NO_COLOR convention : présent (peu importe la valeur) → off. */
    if (getenv("NO_COLOR")) return 0;

    /* TERM=dumb ou absent → off. */
    const char* term = getenv("TERM");
    if (!term || strcmp(term, "dumb") == 0) {
#ifdef _WIN32
        /* Windows Terminal moderne ne fixe pas toujours $TERM ;
         * tester isatty + activation explicite VT100 plus bas. */
#else
        return 0;
#endif
    }

    /* isatty pour stdout. */
    if (!isatty(fileno(stdout))) return 0;

#ifdef _WIN32
    /* Activer le mode ANSI virtuel terminal sur Windows 10+. */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return 0;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) {
        /* Pas une vraie console — peut-être tty hérité (mingw bash).
         * On accepte si isatty disait oui. */
        return 1;
    }
    DWORD wanted = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (mode != wanted && !SetConsoleMode(hOut, wanted)) {
        /* Anciennes versions Windows : pas de VT. */
        return 0;
    }
#endif

    return 1;
}

void cli_colors_init(void) {
    if (g_initialized) return;
    g_enabled = detect_color_capable();
    g_initialized = 1;
}

void cli_colors_force(CliColorMode mode) {
    g_initialized = 1;
    switch (mode) {
        case CLI_COLOR_FORCE: g_enabled = 1; break;
        case CLI_COLOR_PLAIN: g_enabled = 0; break;
        case CLI_COLOR_AUTO:
        default:              g_enabled = detect_color_capable(); break;
    }
}

/* ------------------------------------------------------------------
 * Accesseurs
 * ------------------------------------------------------------------ */

#define GETTER(name, var)                                         \
    const char* cli_c_##name(void) {                              \
        if (!g_initialized) cli_colors_init();                    \
        return g_enabled ? var : EMPTY;                           \
    }

GETTER(reset,         C_RESET_STR)
GETTER(keyword,       C_KEYWORD_STR)
GETTER(type,          C_TYPE_STR)
GETTER(constant,      C_CONST_STR)
GETTER(native,        C_NATIVE_STR)
GETTER(error_global,  C_ERRGLOBAL_STR)
GETTER(string,        C_STRING_STR)
GETTER(interp_var,    C_INTERP_VAR_STR)
GETTER(interp_brk,    C_INTERP_BRK_STR)
GETTER(comment,       C_COMMENT_STR)
GETTER(proc_name,     C_PROC_STR)
GETTER(ident,         C_IDENT_STR)
GETTER(dim,           C_DIM_STR)
GETTER(emphasis,      C_EMPH_STR)
GETTER(section,       C_SECTION_STR)
GETTER(cmd,           C_CMD_STR)
GETTER(arg,           C_ARG_STR)
GETTER(md_heading,    C_MD_HEADING_STR)
