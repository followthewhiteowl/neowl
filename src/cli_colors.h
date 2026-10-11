// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_colors.h — palette ANSI canon OWL pour les commandes doc CLI.
 *
 * Reprend les couleurs exactes du thème VSCode officiel (cf.
 * owl-integration/vscode/package.json → editor.tokenColorCustomizations).
 *
 * Mode :
 *   - Par défaut, détection automatique tty + truecolor.
 *   - cli_colors_force(mode) pour forcer plain / colored explicitement
 *     (utilisé par --plain et --no-color).
 *
 * Usage :
 *   cli_colors_init();
 *   printf("%s%s%s\n", C_KEYWORD, "PROCEDURE", C_RESET);
 *
 * En mode plain, toutes les macros C_* valent "" (chaîne vide), donc
 * le même printf produit juste "PROCEDURE\n" sans modification.
 */

#ifndef OWL_CLI_COLORS_H
#define OWL_CLI_COLORS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLI_COLOR_AUTO    = 0,   /* défaut : tty + truecolor → colored */
    CLI_COLOR_FORCE   = 1,   /* force couleurs même hors tty */
    CLI_COLOR_PLAIN   = 2,   /* force aucune séquence ANSI */
} CliColorMode;

/* À appeler une fois en début de programme, avant tout printf coloré.
 * Inspecte isatty(stdout), $TERM, $COLORTERM, $NO_COLOR.
 * En mode FORCE, ignore l'environnement. */
void cli_colors_init(void);
void cli_colors_force(CliColorMode mode);

/* Accesseurs runtime — retournent "" en mode plain, séquence ANSI
 * 24-bit sinon. À utiliser dans printf via %s. */
const char* cli_c_reset(void);

/* Palette OWL — alignée sur le thème VSCode canon. */
const char* cli_c_keyword(void);     /* #FF8000 bold — SI/POUR/PROCEDURE/ET/OU/FIN */
const char* cli_c_type(void);        /* #9575CD — chaîne/entier/types user-defined */
const char* cli_c_constant(void);    /* #BAB819 italic — Vrai/Faux/Null/httpGet */
const char* cli_c_native(void);      /* #91B5FE — Trace/ChaîneDécoupe/Info/native funcs */
const char* cli_c_error_global(void);/* #5C6BC0 — ErreurDétectée */
const char* cli_c_string(void);      /* #BA68C8 — chaînes, nombres */
const char* cli_c_interp_var(void);  /* #4CAF50 — variables dans [%var%] */
const char* cli_c_interp_brk(void);  /* #CE93D8 bold — [% et %] */
const char* cli_c_comment(void);     /* #808080 — commentaires */
const char* cli_c_proc_name(void);   /* #008054 — noms de PROCEDURE déclarées + déclarations de champs */
const char* cli_c_ident(void);       /* #4CAF50 — variables locales + identifiants utilisateur par défaut */

/* Couleurs utilitaires pour l'UI CLI (entêtes, séparateurs). */
const char* cli_c_dim(void);         /* texte secondaire */
const char* cli_c_emphasis(void);    /* gras blanc */
const char* cli_c_section(void);     /* entête de section (chrome CLI, distinct du code OWL) */
const char* cli_c_cmd(void);         /* nom d'une sous-commande shell (chrome CLI) */
const char* cli_c_arg(void);         /* argument <placeholder> (chrome CLI) */
const char* cli_c_md_heading(void);  /* entête markdown # ## ### */

/* Macros courtes — équivalentes mais plus lisibles côté call site.
 * Elles retournent une const char* "" en mode plain. */
#define C_RESET         cli_c_reset()
#define C_KEYWORD       cli_c_keyword()
#define C_TYPE          cli_c_type()
#define C_CONST         cli_c_constant()
#define C_NATIVE        cli_c_native()
#define C_ERRGLOBAL     cli_c_error_global()
#define C_STRING        cli_c_string()
#define C_INTERP_VAR    cli_c_interp_var()
#define C_INTERP_BRK    cli_c_interp_brk()
#define C_COMMENT       cli_c_comment()
#define C_PROC          cli_c_proc_name()
#define C_IDENT         cli_c_ident()
#define C_DIM           cli_c_dim()
#define C_EMPH          cli_c_emphasis()
#define C_SECTION       cli_c_section()
#define C_CMD           cli_c_cmd()
#define C_ARG           cli_c_arg()
#define C_MD_HEADING    cli_c_md_heading()

#ifdef __cplusplus
}
#endif

#endif /* OWL_CLI_COLORS_H */
