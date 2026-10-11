// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_scanner_h
#define owl_scanner_h

#include "common.h"

/* ================================================================
 * TOKEN TYPES
 * ================================================================ */
typedef enum {
    /* --- Single-char tokens --- */
    TOKEN_LEFT_PAREN,        /* ( */
    TOKEN_RIGHT_PAREN,       /* ) */
    TOKEN_LEFT_BRACE,        /* { */
    TOKEN_RIGHT_BRACE,       /* } */
    TOKEN_LEFT_BRACKET,      /* [ */
    TOKEN_RIGHT_BRACKET,     /* ] */
    TOKEN_COMMA,             /* , */
    TOKEN_DOT,               /* . */
    TOKEN_SEMICOLON,         /* ; */
    TOKEN_COLON,             /* : */
    TOKEN_TILDE,             /* ~ */
    TOKEN_CARET,             /* ^ */
    TOKEN_PERCENT,           /* % */
    TOKEN_AMPERSAND,         /* & */
    TOKEN_PIPE,              /* | */
    TOKEN_QUESTION,          /* ? */
    TOKEN_QUESTION_QUESTION, /* ?? (null-coalesce) */
    TOKEN_QUESTION_EQUAL,    /* ??= (null-coalesce assign) */
    TOKEN_QUESTION_DOT,      /* ?. (safe navigation) */
    TOKEN_QUESTION_STAR,     /* ??* (coalesce to type default) */

    /* --- Multi-char punctuation --- */
    TOKEN_DOUBLE_BRACKET_OPEN,  /* [[ */
    TOKEN_DOUBLE_BRACKET_CLOSE, /* ]] */
    TOKEN_DOUBLE_COLON,      /* :: */
    TOKEN_DOUBLE_DOT,        /* .. */
    TOKEN_TRIPLE_DOT,        /* ... */
    TOKEN_DOUBLE_PIPE,       /* || (XOR) */

    /* --- Arithmetic operators --- */
    TOKEN_PLUS,              /* + */
    TOKEN_MINUS,             /* - */
    TOKEN_STAR,              /* * */
    TOKEN_SLASH,             /* / */
    TOKEN_PLUS_EQUAL,        /* += */
    TOKEN_MINUS_EQUAL,       /* -= */
    TOKEN_STAR_EQUAL,        /* *= */
    TOKEN_SLASH_EQUAL,       /* /= */
    TOKEN_PERCENT_EQUAL,     /* %= */
    TOKEN_PLUS_PLUS,         /* ++ */
    TOKEN_MINUS_MINUS,       /* -- */

    /* --- Lambda --- */
    TOKEN_FAT_ARROW,         /* => (lambda canon 1000024845) */

    /* --- Comparison operators --- */
    TOKEN_EQUAL,             /* = */
    TOKEN_NOT_EQUAL,         /* <> */
    TOKEN_LESS,              /* < */
    TOKEN_LESS_EQUAL,        /* <= */
    TOKEN_GREATER,           /* > */
    TOKEN_GREATER_EQUAL,     /* >= */

    /* --- Flexible string comparison --- */
    TOKEN_TILDE_EQUAL,       /* ~= */
    TOKEN_DOUBLE_TILDE,      /* ~~ */
    TOKEN_STARTS_WITH,       /* [= */
    TOKEN_ENDS_WITH,         /* =] */
    TOKEN_CONTAINS,          /* [=] */
    TOKEN_STARTS_WITH_FLEX,  /* [~ */
    TOKEN_ENDS_WITH_FLEX,    /* ~] */
    TOKEN_CONTAINS_FLEX,     /* [~] */
    TOKEN_STARTS_WITH_VFLEX, /* [~~ */
    TOKEN_ENDS_WITH_VFLEX,   /* ~~] */
    TOKEN_CONTAINS_VFLEX,    /* [~~] */

    /* --- Assignment operators --- */
    TOKEN_COPY,              /* <= (force copy - context-dependent with LESS_EQUAL) */
    TOKEN_REFERENCE,         /* <- */
    /* TOKEN_SWAP removed — use a temporary variable instead */

    /* --- Literals --- */
    TOKEN_INTEGER,           /* 42, 0xFF */
    TOKEN_REAL,              /* 3.14 */
    TOKEN_NUMERIC,           /* 0n12345.67 */
    TOKEN_MONETARY,          /* 0m12345.67 */
    TOKEN_STRING,            /* "hello" — raw, pas d'interpolation */
    TOKEN_STRING_INTERP,     /* %"hello [%var%]" — interpolation [%%] active */
    TOKEN_BACKTICK_STRING,   /* `hello [%var%]` — interpolation [%%] active (canon 2024) */
    TOKEN_MULTILINE_STRING,  /* [content] - multiline */

    /* --- Identifier --- */
    TOKEN_IDENTIFIER,

    /* --- Keywords: Control flow --- */
    TOKEN_SI,                /* SI / IF */
    TOKEN_ALORS,             /* ALORS / THEN */
    TOKEN_SINON,             /* SINON / ELSE */
    TOKEN_FIN,               /* FIN / END */
    TOKEN_POUR,              /* POUR / FOR */
    TOKEN_A,                 /* A / TO (full-eval, borne ré-évaluée à chaque tour) */
    TOKEN_A_SC,              /* _A_ / _TO_ (borne évaluée une seule fois) */
    TOKEN_PAS,               /* PAS / STEP (also NOT) — full-eval, step ré-évalué */
    TOKEN_PAS_SC,            /* _PAS_ / _STEP_ (step évalué une seule fois) */
    TOKEN_TOUT,              /* TOUT / TOUTE / ALL / EACH */
    TOKEN_DE,                /* DE / OF */
    TOKEN_DANS,              /* DANS / IN */
    TOKEN_TANTQUE,           /* TANTQUE / WHILE */
    TOKEN_BOUCLE,            /* BOUCLE / LOOP */
    TOKEN_FAIRE,             /* FAIRE / DO */
    TOKEN_SELON,             /* SELON / SWITCH */
    TOKEN_CAS,               /* CAS / CASE */
    TOKEN_AUTRE,             /* AUTRE / OTHER / DEFAULT */
    TOKEN_SORTIR,            /* SORTIR / BREAK */
    TOKEN_CONTINUER,         /* CONTINUER / CONTINUE */
    TOKEN_RENVOYER,          /* RENVOYER / RESULT */
    TOKEN_RETOUR,            /* RETOUR / RETURN */
    /* TOKEN_GOTO removed — anti-pattern, use structured control flow */
    TOKEN_AVEC,              /* AVEC / WITH */
    TOKEN_SUR,               /* SUR / ON (also for size specs) */

    /* --- Keywords: Declaration --- */
    TOKEN_EST,               /* EST / IS */
    TOKEN_SONT,              /* SONT / ARE */
    TOKEN_CONSTANTE,         /* CONSTANTE / CONSTANT */
    TOKEN_LOCAL,             /* LOCAL */
    TOKEN_GLOBAL,            /* GLOBAL */

    /* --- Keywords: Types --- */
    TOKEN_BOOLEEN,           /* BOOLEEN / BOOLEAN */
    TOKEN_ENTIER,            /* ENTIER / INTEGER */
    TOKEN_REEL,              /* REEL / REAL */
    TOKEN_MONETAIRE,         /* MONETAIRE / CURRENCY */
    TOKEN_NUMERIQUE,         /* NUMERIQUE / NUMERIC */
    TOKEN_CHAINE,            /* CHAINE / STRING */
    TOKEN_CARACTERE,         /* CARACTERE / CHARACTER */
    TOKEN_DATE,              /* DATE */
    TOKEN_HEURE,             /* HEURE / TIME */
    TOKEN_DATEHEURE,         /* DATEHEURE / DATETIME */
    TOKEN_DUREE,             /* DUREE / DURATION */
    TOKEN_BUFFER,            /* BUFFER */
    TOKEN_JSON_KW,           /* JSON */
    TOKEN_UUID,              /* UUID */
    TOKEN_SANS,              /* SANS / UNSIGNED (part of "sans signe") */
    TOKEN_SIGNE,             /* SIGNE (part of "sans signe") */

    /* --- Keywords: Composite types --- */
    TOKEN_TABLEAU,           /* TABLEAU / ARRAY */
    TOKEN_STRUCTURE,         /* STRUCTURE */
    TOKEN_ENUMERATION,       /* ENUMERATION */
    TOKEN_COMBINAISON,       /* COMBINAISON / COMBINATION */
    TOKEN_FILE,              /* FILE / QUEUE */
    TOKEN_PILE,              /* PILE / STACK */
    TOKEN_LISTE,             /* LISTE / LIST */
    TOKEN_ASSOCIATIF,        /* ASSOCIATIF / ASSOCIATIVE */
    TOKEN_FIXE,              /* FIXE / FIXED */
    TOKEN_DYNAMIQUE,         /* DYNAMIQUE / DYNAMIC */
    TOKEN_PAR,               /* PAR / BY (for array dimensions) */

    /* --- Keywords: Procedures --- */
    TOKEN_PROCEDURE,         /* PROCEDURE */
    TOKEN_FONCTION,          /* FONCTION / FUNCTION */

    /* --- Keywords: OOP --- */
    TOKEN_CLASSE,            /* CLASSE / CLASS */
    TOKEN_HERITE,            /* HERITE / INHERITS */
    TOKEN_INTERFACE,         /* INTERFACE */
    TOKEN_IMPLEMENTE,        /* IMPLEMENTE / IMPLEMENTS */
    TOKEN_PUBLIC,            /* PUBLIC */
    TOKEN_PRIVE,             /* PRIVE / PRIVATE */
    TOKEN_PROTEGE,           /* PROTEGE / PROTECTED */
    TOKEN_ABSTRAITE,         /* ABSTRAITE / ABSTRACT (canon : modificateur
                              * sur Classe et PROCEDURE/FONCTION) */
    TOKEN_GLOBALE,           /* GLOBALE / STATIC */
    TOKEN_LIBERER,           /* LIBERER / FREE */

    /* --- Keywords: Logical --- */
    TOKEN_ET,                /* ET / AND */
    TOKEN_OU,                /* OU / OR */
    TOKEN_ET_SC,             /* _ET_ / _AND_ (short-circuit) */
    TOKEN_OU_SC,             /* _OU_ / _OR_ (short-circuit) */
    TOKEN_MODULO,            /* MODULO */
    TOKEN_DANS_SC,           /* _DANS_ / _IN_ (short-circuit) */
    TOKEN_BIT_SHL,           /* bitDécaleGauche (canon : opérateur de décalage gauche) */
    TOKEN_BIT_SHR,           /* bitDécaleDroite (canon : opérateur de décalage droite) */

    /* --- Keywords: Literals --- */
    TOKEN_VRAI,              /* VRAI / TRUE */
    TOKEN_FAUX,              /* FAUX / FALSE */
    TOKEN_NUL,               /* NULL */

    /* --- Keywords: Exception --- */
    TOKEN_QUAND,             /* QUAND / WHEN */
    TOKEN_EXCEPTION,         /* EXCEPTION */
    TOKEN_ENFIN,             /* ENFIN / FINALLY (cleanup garanti) */

    /* --- Keywords: Special --- */
    TOKEN_OBJET,             /* OBJET / THIS / OBJECT */
    TOKEN_MOIMEME,           /* MOIMEME / MYSELF — widget courant emetteur d'un handler.
                              * Resout au runtime via la stack des contextes actifs :
                              * handler d'evenement -> widget emetteur ; sinon
                              * fenetre/etat top-level. Erreur compile en main.owl
                              * top-level, erreur runtime si stack vide ailleurs. */
    TOKEN_MONCHAMP,          /* MONCHAMP / MYCONTROL — le champ courant, sans repli.
                              * Variante STRICTE de MoiMême : resout uniquement au
                              * champ emetteur (event_sender). Une fenetre ou l'absence
                              * de champ -> erreur (pas de repli fenetre/etat). Erreur
                              * compile au top-level programme, runtime sinon. */
    TOKEN_MAFENETRE,         /* MAFENETRE / MYWINDOW — la fenetre courante.
                              * Resout au runtime via vm_window_stack_top(). Aucune
                              * fenetre ouverte -> erreur. Erreur compile au top-level
                              * programme, runtime sinon. */
    TOKEN_ANCETRE,           /* ANCETRE / ANCESTOR */
    TOKEN_OWL_RESERVED,      /* OWL (nom du langage, réservé scanner-only) */
    TOKEN_ELEMENT,           /* ELEMENT */
    TOKEN_SEPAREE,           /* SEPAREE / SEPARATED */
    TOKEN_CONSTRUCTEUR,      /* CONSTRUCTEUR / CONSTRUCTOR */
    TOKEN_DESTRUCTEUR,       /* DESTRUCTEUR / DESTRUCTOR */

    /* --- Filler words (consumed, produce no token) --- */
    /* UN, UNE, DES, D', DU, LE, LA, LES */
    /* These are handled internally by the scanner */

    /* --- Keywords: GUI declarations (DSL fenetre v1) --- */
    TOKEN_FENETRE,           /* FENETRE / WINDOW (top-level window decl) */
    TOKEN_LIBELLE,           /* LIBELLE / LABEL */
    TOKEN_BOUTON,            /* BOUTON / BUTTON */
    TOKEN_SAISIE,            /* SAISIE / INPUT */
    TOKEN_TABLE,             /* TABLE / TABLE (Champ Table V1) */
    TOKEN_COLONNE,           /* COLONNE / COLUMN (declaration d'une colonne) */
    TOKEN_CONTENEUR,         /* CONTENEUR / CONTAINER (Lot F — boite layout) */

    /* --- Keywords: Etats (DSL Etat V2) --- */
    TOKEN_ETAT,              /* ETAT / REPORT (top-level report decl) */

    /* --- DSL fenetre v2 --- */
    TOKEN_PROPRIETES,        /* PROPRIETES / PROPERTIES (bloc statique) */

    /* --- Special tokens --- */
    TOKEN_NEWLINE,           /* significant line break */
    TOKEN_EOF,
    TOKEN_ERROR,

    TOKEN_COUNT              /* total number of token types */
} TokenType;

/* ================================================================
 * TOKEN
 * ================================================================ */
typedef struct {
    TokenType type;
    const char* start;  /* pointer into source */
    int length;
    int line;
    int column;
} Token;

/* ================================================================
 * SCANNER
 * ================================================================ */
typedef struct {
    const char* source;
    const char* start;      /* start of current lexeme */
    const char* current;    /* current position */
    int line;
    int column;
    int startColumn;
    bool atLineStart;       /* for tracking newline significance */
    TokenType prevType;     /*  — type du dernier token significatif
                             * emis (hors newline) : sert a stripper l'article
                             * "un"/"une" UNIQUEMENT juste apres EST/SONT. */
    int bracketDepth;       /*  — profondeur de nesting `( [ {` courante
                             * (increm/decrem dans make_token). Porte par le
                             * Scanner pour survivre au save/restore du rollback
                             * speculatif du compilateur (Scanner peek = ...).
                             * Lu par synchronize() : le newline-stop anti-cascade
                             * ne s'applique qu'a profondeur 0 (statement plat).
                             * A profondeur >0 (dans un `({...})` multi-ligne) on
                             * garde le skip-to-keyword historique. */
} Scanner;

void scanner_init(Scanner* scanner, const char* source);
Token scanner_scan_token(Scanner* scanner);

/* Utility: get printable name for a token type */
const char* token_type_name(TokenType type);

/* Iterate keywords: returns name and token type for index i.
 * Returns NULL when exhausted. */
const char* scanner_keyword_at(int index, TokenType* outType);

/* Strip UTF-8 accents from a string, preserving case.
 * "ChaîneVersJSON" -> "ChaineVersJSON"
 * Returns length written. buf must be at least srcLen+1 bytes. */
int scanner_strip_accents(const char* src, int srcLen, char* buf, int bufSize);

/*  — forme canon d'un identifiant user (strip_accents + tolower
 * ASCII en une passe). "nClé" -> "ncle", "HTTPRequête" -> "httprequete".
 * Single source of truth pour le lookup case+accent insensible des globales
 * et types via les index secondaires (vm.globals_canon_idx / vm.types_canon_idx).
 * Retourne la longueur ecrite. buf doit faire au moins srcLen+1 octets. */
int scanner_canonicalize_identifier(const char* src, int srcLen, char* buf, int bufSize);

/*  — REGLE UNIQUE de fermeture d'une chaine-bloc `[ ... ]`.
 * `lineStart` pointe sur le premier caractere d'une ligne du contenu
 * (indentation comprise). Renvoie true si cette ligne ferme le bloc :
 * son premier caractere non-blanc est `]`, et le premier caractere
 * non-blanc qui suit ce `]` est une fin de ligne, la fin du fichier, ou
 * une ponctuation de structure de la forme englobante `,` `)` `}` `]`
 * (litteral multilingue `],`, appel `])`, dernier element `]}`,
 * imbrication `]]`). Ce qui vient APRES cette ponctuation est indifferent
 * — cas courant `ChaineConstruit([ ... ], "Alice", 3)`.
 *
 * Un `]` suivi de texte libre reste du CONTENU : la lettre de la doc
 * (« toute ligne commencant par ] ») fermerait par accident un modele
 * contenant du JSON.
 *
 * DEUX appelants, et la regle doit rester unique ici : le scanner
 * (scan_multiline_string, src/scanner.c) et le formateur
 * (src/formatter.c, boucle in_multiline). Quand les deux divergent, le
 * formateur reecrit au save le CONTENU d'une chaine — c'est le defaut
 * mesure qui a motive la mise en commun. */
bool owl_multiline_block_line_closes(const char* lineStart);

/* true si `s` est un identifiant OWL legal en entier : premier caractere
 * lettre / '_' / accent, suivants + chiffres. Chaine vide ou NULL -> false.
 * Memes regles que le scanner (is_ident_start / is_ident_char). */
bool owl_is_valid_identifier(const char* s);

#endif
