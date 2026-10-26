// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "scanner.h"
#include "memory.h"
#include "owl_errors.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

/* ================================================================
 * UTF-8 AND ACCENT HANDLING
 * ================================================================ */

/* Normalize a single UTF-8 character to its ASCII base (lowercase, no accent).
 * Returns the number of bytes consumed from src.
 * Writes the normalized ASCII char to *out. */
static int normalize_utf8_char(const char* src, char* out) {
    unsigned char c = (unsigned char)src[0];

    /* ASCII range */
    if (c < 0x80) {
        *out = (char)tolower(c);
        return 1;
    }

    /* Two-byte UTF-8: 110xxxxx 10xxxxxx */
    if ((c & 0xE0) == 0xC0 && ((unsigned char)src[1] & 0xC0) == 0x80) {
        uint16_t cp = ((c & 0x1F) << 6) | ((unsigned char)src[1] & 0x3F);

        /* Map French accented characters to ASCII base */
        switch (cp) {
            /* A variants */
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
                *out = 'a'; return 2;
            /* C cedilla */
            case 0xC7: case 0xE7:
                *out = 'c'; return 2;
            /* E variants */
            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                *out = 'e'; return 2;
            /* I variants */
            case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            case 0xEC: case 0xED: case 0xEE: case 0xEF:
                *out = 'i'; return 2;
            /* N tilde */
            case 0xD1: case 0xF1:
                *out = 'n'; return 2;
            /* O variants */
            case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
            case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
                *out = 'o'; return 2;
            /* U variants */
            case 0xD9: case 0xDA: case 0xDB: case 0xDC:
            case 0xF9: case 0xFA: case 0xFB: case 0xFC:
                *out = 'u'; return 2;
            /* Y variants */
            case 0xDD: case 0xFD: case 0xFF:
                *out = 'y'; return 2;
            default:
                /* Unknown 2-byte char: keep as-is (won't match keywords) */
                *out = '?';
                return 2;
        }
    }

    /* Three-byte UTF-8: skip */
    if ((c & 0xF0) == 0xE0) {
        *out = '?';
        return 3;
    }

    /* Four-byte UTF-8: skip */
    if ((c & 0xF8) == 0xF0) {
        *out = '?';
        return 4;
    }

    /* Invalid UTF-8 */
    *out = '?';
    return 1;
}

/* Normalize a UTF-8 string into a lowercase, accent-stripped ASCII buffer.
 * Caller must provide buf of sufficient size. Returns length written. */
static int normalize_string(const char* src, int srcLen, char* buf, int bufSize) {
    int si = 0, di = 0;
    while (si < srcLen && di < bufSize - 1) {
        char normalized;
        int consumed = normalize_utf8_char(src + si, &normalized);
        buf[di++] = normalized;
        si += consumed;
    }
    buf[di] = '\0';
    return di;
}

/*  — forme canon d'un identifiant user (strip_accents + tolower).
 * Compose les deux operations en une passe : strip accent UTF-8, puis
 * tolower ASCII inconditionnel sur le resultat. Source de verite pour le
 * lookup case+accent insensible des globales/types (index secondaires
 * vm.globals_canon_idx et vm.types_canon_idx). */
int scanner_canonicalize_identifier(const char* src, int srcLen, char* buf, int bufSize) {
    int len = scanner_strip_accents(src, srcLen, buf, bufSize);
    for (int i = 0; i < len; i++) {
        buf[i] = (char)tolower((unsigned char)buf[i]);
    }
    return len;
}

/* Strip accents from UTF-8 string, preserving original case.
 * "ChaîneVersJSON" -> "ChaineVersJSON"
 * Uses the same accent mapping as normalize_utf8_char but keeps case for ASCII. */
int scanner_strip_accents(const char* src, int srcLen, char* buf, int bufSize) {
    int si = 0, di = 0;
    while (si < srcLen && di < bufSize - 1) {
        unsigned char c = (unsigned char)src[si];
        if (c < 0x80) {
            /* ASCII: keep original case */
            buf[di++] = src[si++];
        } else if ((c & 0xE0) == 0xC0 && si + 1 < srcLen) {
            /* 2-byte UTF-8: strip accent, infer case from uppercase codepoint */
            uint16_t cp = ((c & 0x1F) << 6) | ((unsigned char)src[si+1] & 0x3F);
            bool upper = (cp >= 0xC0 && cp <= 0xDE); /* uppercase range */
            char base;
            normalize_utf8_char(src + si, &base); /* gives lowercase */
            buf[di++] = upper ? (char)toupper((unsigned char)base) : base;
            si += 2;
        } else if ((c & 0xF0) == 0xE0) {
            buf[di++] = '?'; si += 3;
        } else if ((c & 0xF8) == 0xF0) {
            buf[di++] = '?'; si += 4;
        } else {
            buf[di++] = '?'; si++;
        }
    }
    buf[di] = '\0';
    return di;
}

/* ================================================================
 * KEYWORD TABLE
 * ================================================================
 * Each entry maps a normalized (lowercase, no accent) string to a token type.
 * Multiple entries can map to the same token (FR/EN aliases, plural forms, etc.)
 */

typedef struct {
    const char* name;    /* normalized form (lowercase ascii, no accents) */
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    /* --- Control flow --- */
    {"si",            TOKEN_SI},
    {"if",            TOKEN_SI},
    {"alors",         TOKEN_ALORS},
    {"then",          TOKEN_ALORS},
    {"sinon",         TOKEN_SINON},
    {"else",          TOKEN_SINON},
    {"fin",           TOKEN_FIN},
    {"end",           TOKEN_FIN},
    {"pour",          TOKEN_POUR},
    {"for",           TOKEN_POUR},
    {"a",             TOKEN_A},
    {"to",            TOKEN_A},
    {"_a_",           TOKEN_A_SC},       /* borne évaluée une seule fois (canon) */
    {"_to_",          TOKEN_A_SC},
    {"pas",           TOKEN_PAS},        /* STEP or NOT depending on context */
    {"_pas_",         TOKEN_PAS_SC},     /* step évalué une seule fois (canon) */
    {"step",          TOKEN_PAS},
    {"_step_",        TOKEN_PAS_SC},
    {"not",           TOKEN_PAS},
    /* `non` n'est PAS un alias de PAS : c'est une constante UI
     * de valeur 0 (cf. fonction OuiNon). L'unique operateur
     * de negation logique est `PAS` (cf. doc langage operateurs
     * logiques : ET, _ET_, OU, _OU_, PAS, DANS, _DANS_). Aliaser `non` ici
     * empechait le canon `SI OuiNon("...") = Non ALORS` de fonctionner. */
    {"tout",          TOKEN_TOUT},
    {"toute",         TOKEN_TOUT},
    {"tous",          TOKEN_TOUT},      /* synonyme canon : POUR TOUS x DE ... */
    {"chaque",        TOKEN_TOUT},      /* synonyme canon : POUR CHAQUE x DE ... */
    {"each",          TOKEN_TOUT},
    {"every",         TOKEN_TOUT},      /* EN synonyme : FOR EVERY x OF ... */
    {"all",           TOKEN_TOUT},
    {"de",            TOKEN_DE},
    {"of",            TOKEN_DE},
    {"dans",          TOKEN_DANS},
    {"in",            TOKEN_DANS},
    {"tantque",       TOKEN_TANTQUE},
    {"while",         TOKEN_TANTQUE},
    {"boucle",        TOKEN_BOUCLE},
    {"loop",          TOKEN_BOUCLE},
    {"faire",         TOKEN_FAIRE},
    {"do",            TOKEN_FAIRE},
    {"selon",         TOKEN_SELON},
    {"switch",        TOKEN_SELON},
    {"cas",           TOKEN_CAS},
    {"case",          TOKEN_CAS},
    {"autre",         TOKEN_AUTRE},
    {"other",         TOKEN_AUTRE},
    {"default",       TOKEN_AUTRE},
    {"sortir",        TOKEN_SORTIR},
    {"break",         TOKEN_SORTIR},
    /* `exit` n'est PAS un alias EN de SORTIR : collision avec des
     * identifiants utilisateur legitimes (PROCEDURE Exit, etc.). La
     * combinaison `EXIT SWITCH` est reconnue contextuellement par le
     * statement dispatch via un lookahead identifier_1char. */
    {"continuer",     TOKEN_CONTINUER},
    {"continue",      TOKEN_CONTINUER},
    {"renvoyer",      TOKEN_RENVOYER},
    {"result",        TOKEN_RENVOYER},
    {"retour",        TOKEN_RETOUR},
    {"return",        TOKEN_RETOUR},
    {"avec",          TOKEN_AVEC},
    {"with",          TOKEN_AVEC},
    {"sur",           TOKEN_SUR},
    {"on",            TOKEN_SUR},

    /* --- Declaration --- */
    {"est",           TOKEN_EST},
    {"is",            TOKEN_EST},
    {"sont",          TOKEN_SONT},
    {"are",           TOKEN_SONT},
    {"constante",     TOKEN_CONSTANTE},
    {"constant",      TOKEN_CONSTANTE},
    {"local",         TOKEN_LOCAL},
    {"locale",        TOKEN_LOCAL},
    {"locales",       TOKEN_LOCAL},
    {"global",        TOKEN_GLOBAL},

    /* --- DSL Fenetre v2 : bloc statique de proprietes --- */
    {"proprietes",    TOKEN_PROPRIETES},
    {"properties",    TOKEN_PROPRIETES},

    /* --- Primitive types --- */
    {"booleen",       TOKEN_BOOLEEN},
    {"booleens",      TOKEN_BOOLEEN},
    {"boolean",       TOKEN_BOOLEEN},
    {"booleans",      TOKEN_BOOLEEN},
    {"entier",        TOKEN_ENTIER},
    {"entiers",       TOKEN_ENTIER},
    {"integer",       TOKEN_ENTIER},
    {"integers",      TOKEN_ENTIER},
    {"reel",          TOKEN_REEL},
    {"reels",         TOKEN_REEL},
    {"real",          TOKEN_REEL},
    {"reals",         TOKEN_REEL},
    {"monetaire",     TOKEN_MONETAIRE},
    {"monetaires",    TOKEN_MONETAIRE},
    {"currency",      TOKEN_MONETAIRE},
    {"numerique",     TOKEN_NUMERIQUE},
    {"numeriques",    TOKEN_NUMERIQUE},
    {"numeric",       TOKEN_NUMERIQUE},
    {"chaine",        TOKEN_CHAINE},
    {"chaines",       TOKEN_CHAINE},
    {"string",        TOKEN_CHAINE},
    {"strings",       TOKEN_CHAINE},
    {"caractere",     TOKEN_CARACTERE},
    {"character",     TOKEN_CARACTERE},
    {"date",          TOKEN_DATE},
    {"heure",         TOKEN_HEURE},
    {"time",          TOKEN_HEURE},
    {"dateheure",     TOKEN_DATEHEURE},
    {"datetime",      TOKEN_DATEHEURE},
    {"duree",         TOKEN_DUREE},
    {"duration",      TOKEN_DUREE},
    {"buffer",        TOKEN_BUFFER},
    {"json",          TOKEN_JSON_KW},
    {"uuid",          TOKEN_UUID},
    {"sans",          TOKEN_SANS},
    {"unsigned",      TOKEN_SANS},
    {"signe",         TOKEN_SIGNE},

    /* --- Composite types --- */
    {"tableau",       TOKEN_TABLEAU},
    {"tableaux",      TOKEN_TABLEAU},
    {"array",         TOKEN_TABLEAU},
    {"structure",     TOKEN_STRUCTURE},
    {"enumeration",   TOKEN_ENUMERATION},
    {"combinaison",   TOKEN_COMBINAISON},
    {"combination",   TOKEN_COMBINAISON},
    {"file",          TOKEN_FILE},
    {"queue",         TOKEN_FILE},
    {"pile",          TOKEN_PILE},
    {"stack",         TOKEN_PILE},
    {"liste",         TOKEN_LISTE},
    {"list",          TOKEN_LISTE},
    {"associatif",    TOKEN_ASSOCIATIF},
    {"associative",   TOKEN_ASSOCIATIF},
    {"fixe",          TOKEN_FIXE},
    {"fixed",         TOKEN_FIXE},
    {"dynamique",     TOKEN_DYNAMIQUE},
    {"dynamic",       TOKEN_DYNAMIQUE},
    {"par",           TOKEN_PAR},
    {"by",            TOKEN_PAR},

    /* --- Procedures --- */
    {"procedure",     TOKEN_PROCEDURE},
    {"fonction",      TOKEN_FONCTION},
    {"function",      TOKEN_FONCTION},

    /* --- OOP --- */
    {"classe",        TOKEN_CLASSE},
    {"class",         TOKEN_CLASSE},
    {"herite",        TOKEN_HERITE},
    {"inherits",      TOKEN_HERITE},
    {"interface",     TOKEN_INTERFACE},
    {"implemente",    TOKEN_IMPLEMENTE},
    {"implements",    TOKEN_IMPLEMENTE},
    {"public",        TOKEN_PUBLIC},
    {"publique",      TOKEN_PUBLIC},
    {"prive",         TOKEN_PRIVE},
    {"privee",        TOKEN_PRIVE},
    {"private",       TOKEN_PRIVE},
    {"protege",       TOKEN_PROTEGE},
    {"protegee",      TOKEN_PROTEGE},
    {"protected",     TOKEN_PROTEGE},
    {"abstraite",     TOKEN_ABSTRAITE},
    {"abstract",      TOKEN_ABSTRAITE},
    {"globale",       TOKEN_GLOBALE},
    {"static",        TOKEN_GLOBALE},
    {"liberer",       TOKEN_LIBERER},
    {"free",          TOKEN_LIBERER},
    {"delete",        TOKEN_LIBERER},

    /* --- Logical --- */
    {"et",            TOKEN_ET},
    {"and",           TOKEN_ET},
    {"ou",            TOKEN_OU},
    {"or",            TOKEN_OU},
    {"_et_",          TOKEN_ET_SC},
    {"_and_",         TOKEN_ET_SC},
    {"_ou_",          TOKEN_OU_SC},
    {"_or_",          TOKEN_OU_SC},
    /* `modulo` is both an infix operator (`21 modulo 4` == `21%4`)
     * AND a native function (`Modulo(21, 4)`). The scanner emits the same
     * TOKEN_MODULO for the infix form; the native call is disambiguated
     * by a following `(`. */
    {"modulo",        TOKEN_MODULO},
    {"_dans_",        TOKEN_DANS_SC},
    {"_in_",          TOKEN_DANS_SC},

    /* --- Binary shift operators (canon §"Opérateurs de décalage").
     * Utilisables en infix (`a bitDécaleGauche b`) ET en forme fonction
     * (`bitDécaleGauche(a, b)`). Le compilateur gère les deux via ParseRule
     * prefix+infix sur le même token. */
    {"bitdecalegauche",  TOKEN_BIT_SHL},
    {"bitshiftleft",     TOKEN_BIT_SHL},
    {"bitdecaledroite",  TOKEN_BIT_SHR},
    {"bitshiftright",    TOKEN_BIT_SHR},

    /* --- Literals --- */
    {"vrai",          TOKEN_VRAI},
    {"true",          TOKEN_VRAI},
    {"faux",          TOKEN_FAUX},
    {"false",         TOKEN_FAUX},
    {"null",          TOKEN_NUL},

    /* --- Exception --- */
    {"quand",         TOKEN_QUAND},
    {"when",          TOKEN_QUAND},
    {"exception",     TOKEN_EXCEPTION},
    {"enfin",         TOKEN_ENFIN},
    {"finally",       TOKEN_ENFIN},

    /* --- Special keywords --- */
    {"objet",         TOKEN_OBJET},
    {"this",          TOKEN_OBJET},
    {"object",        TOKEN_OBJET},
    /* MoiMême / MySelf — widget emetteur d'un handler. Scanner stripe les
     * accents (invariant 2) et lowercase (invariant 1) avant lookup, donc
     * `MoiMême`/`MoiMeme`/`MOIMEME` arrivent tous a `moimeme`. */
    {"moimeme",       TOKEN_MOIMEME},
    {"myself",        TOKEN_MOIMEME},
    /* MonChamp / MyControl — variante stricte de MoiMême (champ courant, sans
     * repli). MaFenêtre / MyWindow — fenetre courante. Le scanner stripe accents
     * et lowercase avant lookup, donc `MaFenêtre`/`MaFenetre`/`MAFENETRE`
     * arrivent tous a `mafenetre`. */
    {"monchamp",      TOKEN_MONCHAMP},
    {"mycontrol",     TOKEN_MONCHAMP},
    {"mafenetre",     TOKEN_MAFENETRE},
    {"mywindow",      TOKEN_MAFENETRE},
    {"ancetre",       TOKEN_ANCETRE},
    {"ancestor",      TOKEN_ANCETRE},
    /* OWL : nom du langage, reserve scanner-only. Bloque l'utilisation
     * comme nom de variable/procedure mais aucune semantique attachee
     * — par principe de protection du namespace. */
    {"owl",           TOKEN_OWL_RESERVED},
    {"element",       TOKEN_ELEMENT},
    {"separee",       TOKEN_SEPAREE},
    {"separated",     TOKEN_SEPAREE},
    {"constructeur",  TOKEN_CONSTRUCTEUR},
    {"constructor",   TOKEN_CONSTRUCTEUR},
    {"destructeur",   TOKEN_DESTRUCTEUR},
    {"destructor",    TOKEN_DESTRUCTEUR},

    /* --- GUI declarations (DSL fenetre v1) ---
     * Bilingue FR + EN. Window/Label/Button/Input deviennent reserves :
     * pour des variables string utiliser sLabel/sTexte/sNom/etc.
     * (convention prefixe hongrois light deja en usage). */
    {"fenetre",       TOKEN_FENETRE},
    {"window",        TOKEN_FENETRE},
    {"libelle",       TOKEN_LIBELLE},
    {"label",         TOKEN_LIBELLE},
    {"bouton",        TOKEN_BOUTON},
    {"button",        TOKEN_BOUTON},
    {"saisie",        TOKEN_SAISIE},
    {"input",         TOKEN_SAISIE},
    {"table",         TOKEN_TABLE},      /* identique FR/EN */
    {"colonne",       TOKEN_COLONNE},
    {"column",        TOKEN_COLONNE},
    {"conteneur",     TOKEN_CONTENEUR},
    {"container",     TOKEN_CONTENEUR},
    /* "combo" / "champ" / "field" NE sont PAS des keywords scanner (conflit avec
     * variables courantes type "combo est un Champ Combo" ou "champ est une chaine").
     * La detection
     * du modifier "Champ X" se fait contextuellement dans le compiler. */

    /* --- DSL Etat V2 --- */
    {"etat",          TOKEN_ETAT},
    {"report",        TOKEN_ETAT},

    {NULL, TOKEN_EOF} /* sentinel */
};

const char* scanner_keyword_at(int index, TokenType* outType) {
    if (keywords[index].name == NULL) return NULL;
    if (outType) *outType = keywords[index].type;
    return keywords[index].name;
}

/*  — la table de fillers INCONDITIONNELS a disparu. Elle avalait
 * "uns unes des du le la les from" sans emettre de token, en toute position :
 * les cinq mots francais courants (le/la/les/des/du, et leurs formes accentuees
 * qui normalisent vers les memes) etaient donc inutilisables
 * comme identifiant, avec une erreur qui portait sur le mot SUIVANT
 * ("Expression attendue" sur `est`, pour `du est une duree = 90`).
 * Mesure sur tout le corpus compilable du depot (tests + examples + docs) :
 * seul "des" etait reellement consomme, exclusivement en position d'article
 * apres SONT (`x, y sont des entiers`) -> il rejoint "un"/"une" comme filler
 * CONTEXTUEL dans scan_identifier. Les sept autres n'etaient consommes nulle
 * part ("from" visait un `INHERITS FROM` qui n'existe pas : la syntaxe EN est
 * `Inherits <Base>`) -> purement et simplement liberes.
 * Le seul filler inconditionnel restant est l'elision "d'", traitee dans
 * scan_identifier ("d" seul n'en est pas un). */

/* ================================================================
 * SCANNER HELPERS
 * ================================================================ */

void scanner_init(Scanner* scanner, const char* source) {
    scanner->source = source;
    scanner->start = source;
    scanner->current = source;
    scanner->line = 1;
    scanner->column = 1;
    scanner->startColumn = 1;
    scanner->atLineStart = true;
    scanner->prevType = TOKEN_EOF;   /*  — neutre : != EST/SONT */
    scanner->bracketDepth = 0;       /*  */
}

static bool is_at_end(Scanner* s) {
    return *s->current == '\0';
}

static char advance(Scanner* s) {
    char c = *s->current++;
    s->column++;
    return c;
}

static char peek(Scanner* s) {
    return *s->current;
}

static char peek_next(Scanner* s) {
    if (is_at_end(s)) return '\0';
    return s->current[1];
}

/* Reserved for future use (compound keyword lookahead) */
/*
static char peek_at(Scanner* s, int offset) {
    for (int i = 0; i < offset; i++) {
        if (s->current[i] == '\0') return '\0';
    }
    return s->current[offset];
}
*/

static bool match(Scanner* s, char expected) {
    if (is_at_end(s)) return false;
    if (*s->current != expected) return false;
    s->current++;
    s->column++;
    return true;
}

static Token make_token(Scanner* s, TokenType type) {
    Token token;
    token.type = type;
    token.start = s->start;
    token.length = (int)(s->current - s->start);
    token.line = s->line;
    token.column = s->startColumn;

    /*  — suivi de la profondeur de nesting. Seuls les six brackets
     * simples comptent : `[[` / `]]` et les operateurs de comparaison souple
     * (`[=`, `[~`, `=]`, ...) sont des types de token distincts, jamais du
     * nesting. Decrement borne a 0 : un `)` orphelin (source malformee en
     * cours de recuperation d'erreur) ne fait pas passer la profondeur sous
     * zero, ce qui casserait le test `== 0` de synchronize(). */
    switch (type) {
        case TOKEN_LEFT_PAREN:
        case TOKEN_LEFT_BRACE:
        case TOKEN_LEFT_BRACKET:
            s->bracketDepth++;
            break;
        case TOKEN_RIGHT_PAREN:
        case TOKEN_RIGHT_BRACE:
        case TOKEN_RIGHT_BRACKET:
            if (s->bracketDepth > 0) s->bracketDepth--;
            break;
        default:;
    }
    return token;
}

/* Construit un Token TOKEN_ERROR a partir du registry owl_errors. Rend le
 * template FR via lookup + substitution {key} par paires va_args, et pose
 * la string rendue dans un buffer statique. Le compiler consomme le
 * message immediatement (error_at_current -> print), donc duree de vie OK
 * pour un scanner mono-erreur a la fois. */
static Token error_token_registry(Scanner* s, OwlErrorCode code, ...) {
    static char buf[1024];
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    const char* tpl = d ? d->templateFR : "";
    /* Rendu manuel ici pour eviter double API. On reutilise toutefois la
     * meme logique : copy + substitution {key} via va_args. */
    va_list args;
    va_start(args, code);
    int o = 0;
    const char* p = tpl;
    while (p && *p && o < (int)sizeof(buf) - 1) {
        if (*p == '{') {
            const char* end = strchr(p, '}');
            if (!end) { buf[o++] = *p++; continue; }
            int keyLen = (int)(end - p - 1);
            if (keyLen <= 0) { buf[o++] = *p++; continue; }
            /* Lookup dans va_args. */
            va_list copy;
            va_copy(copy, args);
            const char* val = NULL;
            while (1) {
                const char* name = va_arg(copy, const char*);
                if (name == NULL) break;
                const char* v = va_arg(copy, const char*);
                if ((int)strlen(name) == keyLen &&
                    memcmp(name, p + 1, keyLen) == 0) { val = v; break; }
            }
            va_end(copy);
            if (val) {
                int vlen = (int)strlen(val);
                int room = (int)sizeof(buf) - 1 - o;
                if (vlen > room) vlen = room;
                memcpy(buf + o, val, vlen);
                o += vlen;
            } else {
                int span = (int)(end - p + 1);
                int room = (int)sizeof(buf) - 1 - o;
                if (span > room) span = room;
                memcpy(buf + o, p, span);
                o += span;
            }
            p = end + 1;
        } else {
            buf[o++] = *p++;
        }
    }
    buf[o] = '\0';
    va_end(args);

    Token token;
    token.type = TOKEN_ERROR;
    token.start = buf;
    token.length = o;
    token.line = s->line;
    token.column = s->startColumn;
    return token;
}

/* Check if a character is valid in an identifier (ASCII + UTF-8 continuation) */
static bool is_ident_char(char c) {
    unsigned char uc = (unsigned char)c;
    return (uc >= 'a' && uc <= 'z') ||
           (uc >= 'A' && uc <= 'Z') ||
           (uc >= '0' && uc <= '9') ||
           uc == '_' ||
           uc >= 0x80;  /* UTF-8 continuation bytes and start bytes */
}

static bool is_ident_start(char c) {
    unsigned char uc = (unsigned char)c;
    return (uc >= 'a' && uc <= 'z') ||
           (uc >= 'A' && uc <= 'Z') ||
           uc == '_' ||
           uc >= 0x80;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Valide qu'une chaine entiere est un identifiant OWL legal : premier
 * caractere is_ident_start (lettre, '_', ou octet UTF-8 >= 0x80 pour les
 * accents), caracteres suivants is_ident_char (idem + chiffres). Vide ->
 * invalide. Source unique des regles d'identifiant, reutilisee hors scanner
 * (ex. validation du nom passe a `owl new`). */
bool owl_is_valid_identifier(const char* s) {
    if (s == NULL || *s == '\0') return false;
    if (!is_ident_start(s[0])) return false;
    for (const char* p = s + 1; *p; p++) {
        if (!is_ident_char(*p)) return false;
    }
    return true;
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* ================================================================
 * SKIP WHITESPACE AND COMMENTS
 * ================================================================ */

/* Line-continuation: a lone '_' at end of a line (optionally
 * followed by spaces/tabs before the newline) means "join this line with
 * the next one". Returns true if a continuation was consumed. */
static bool check_line_continuation(Scanner* s) {
    if (peek(s) != '_') return false;
    /* Look ahead past `_` and any horizontal whitespace to confirm we hit
     * a newline (and the `_` isn't the start of an identifier like `_A_`). */
    const char* p = s->current + 1;
    /* If the character right after `_` is an identifier char, it's NOT a
     * continuation — it's part of `_A_`, `_PAS_`, etc. */
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '_') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (*p != '\n') return false;
    /* Confirmed: consume `_`, trailing spaces, and the newline. */
    advance(s);                               /* skip `_` */
    while (peek(s) == ' ' || peek(s) == '\t' || peek(s) == '\r') advance(s);
    if (peek(s) == '\n') {
        s->line++;
        s->column = 0;
        advance(s);
    }
    return true;
}

/* Triple-dot continuation: `...` at end of line means "join this
 * line with the next one". Identical to `_` continuation but uses `...`.
 * Returns true if a continuation was consumed.
 *
 * Doc: "L'opérateur "..." (triple point) permet de continuer une ligne
 * logique sur la ligne physique suivante."  */
static bool check_triple_dot_continuation(Scanner* s) {
    if (peek(s) != '.') return false;
    if (s->current[1] != '.' || s->current[2] != '.') return false;
    /* Look ahead past `...` and any horizontal whitespace + comment to
     * confirm we hit a newline. */
    const char* p = s->current + 3;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    /* Optional trailing single-line comment after `...` (canon allows it) */
    if (*p == '/' && *(p+1) == '/') {
        while (*p != '\n' && *p != '\0') p++;
    }
    if (*p != '\n') return false;
    /* Confirmed: consume `...`, trailing spaces/comment, and the newline. */
    advance(s); advance(s); advance(s);  /* skip `...` */
    while (peek(s) == ' ' || peek(s) == '\t' || peek(s) == '\r') advance(s);
    if (peek(s) == '/' && peek_next(s) == '/') {
        while (peek(s) != '\n' && !is_at_end(s)) advance(s);
    }
    if (peek(s) == '\n') {
        s->line++;
        s->column = 0;
        advance(s);
    }
    return true;
}

static void skip_whitespace(Scanner* s) {
    for (;;) {
        char c = peek(s);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(s);
                break;
            case '_':
                if (check_line_continuation(s)) break;
                return;
            case '.':
                if (check_triple_dot_continuation(s)) break;
                return;
            case '/':
                if (peek_next(s) == '/') {
                    /* Single-line comment: skip to end of line */
                    while (peek(s) != '\n' && !is_at_end(s)) advance(s);
                    break;
                } else if (peek_next(s) == '*') {
                    /* Multi-line comment */
                    advance(s); /* skip / */
                    advance(s); /* skip * */
                    int depth = 1;
                    while (depth > 0 && !is_at_end(s)) {
                        if (peek(s) == '/' && peek_next(s) == '*') {
                            advance(s); advance(s);
                            depth++;
                        } else if (peek(s) == '*' && peek_next(s) == '/') {
                            advance(s); advance(s);
                            depth--;
                        } else {
                            if (peek(s) == '\n') {
                                s->line++;
                                s->column = 0;
                            }
                            advance(s);
                        }
                    }
                    break;
                }
                return;
            default:
                return;
        }
    }
}

/* ================================================================
 * NUMBER SCANNING
 * ================================================================ */

static Token scan_number(Scanner* s) {
    bool isNumeric = false;   /* 0n prefix */
    bool isMonetary = false;  /* 0m prefix */
    bool isReal = false;

    /* Check for 0x (hex), 0n (numeric), 0m (monetary) prefix */
    if (s->start[0] == '0' && (int)(s->current - s->start) == 1) {
        if (peek(s) == 'x' || peek(s) == 'X') {
            advance(s); /* skip x */
            while (is_hex_digit(peek(s)) || peek(s) == '_') advance(s);
            return make_token(s, TOKEN_INTEGER);
        }
        if (peek(s) == 'n' || peek(s) == 'N') {
            advance(s); /* skip n */
            isNumeric = true;
        }
        if (peek(s) == 'm' || peek(s) == 'M') {
            advance(s); /* skip m */
            isMonetary = true;
        }
        /* Signe `-` collé au préfixe 0n/0m (sans espace, suivi d'un chiffre) :
         * fait partie du littéral négatif exact (0n-888.5 = numérique -888.5),
         * pas une soustraction. `0n - 888.5` (espacé) reste une soustraction. */
        if ((isNumeric || isMonetary) && peek(s) == '-' && is_digit(peek_next(s))) {
            advance(s); /* skip - */
        }
    }

    /* Consume digits, underscores (thousands separator) */
    while (is_digit(peek(s)) || peek(s) == '_') advance(s);

    /* Space as thousands separator: only if followed by exactly 3 digits then non-digit */
    /* We skip this for now - too ambiguous with statement separation */

    /* Decimal point */
    if (peek(s) == '.' && is_digit(peek_next(s))) {
        isReal = true;
        advance(s); /* skip . */
        while (is_digit(peek(s)) || peek(s) == '_') advance(s);
    }

    /* Exponent: [eE][+-]?[0-9]+ — accepted on integers too (1e3 → 1000.0). */
    if ((peek(s) == 'e' || peek(s) == 'E') && !isNumeric && !isMonetary) {
        char sign = peek_next(s);
        int lookahead = (sign == '+' || sign == '-') ? 2 : 1;
        /* Only consume the exponent if at least one digit follows. */
        if (is_digit(sign) ||
            ((sign == '+' || sign == '-') && is_digit(s->current[lookahead]))) {
            isReal = true;
            advance(s); /* skip e/E */
            if (peek(s) == '+' || peek(s) == '-') advance(s);
            while (is_digit(peek(s))) advance(s);
        }
    }

    if (isMonetary) return make_token(s, TOKEN_MONETARY);
    if (isNumeric) return make_token(s, TOKEN_NUMERIC);
    if (isReal) return make_token(s, TOKEN_REAL);
    return make_token(s, TOKEN_INTEGER);
}

/* ================================================================
 * STRING SCANNING
 * ================================================================ */

static Token scan_string(Scanner* s) {
    /* RAW strings — no C-style escape sequences.
     * Only special: "" doubled → single literal " (handled by compiler).
     * Backslash is fully literal: "C:\Users\admin" is 13 chars.
     * For special chars (newline, tab) use RC / LF / CR / TAB / Caract(N). */
    /* Capture la position du delimiteur ouvrant : si on tombe en EOF
     * sans " fermant, on veut pointer le " ouvrant (actionnable pour
     * auto-correction) et pas l'EOF (inutilisable). */
    int openLine = s->line;
    int openColumn = s->startColumn;
    advance(s); /* skip opening " */

    while (!is_at_end(s)) {
        if (peek(s) == '"') {
            /* Doubling: "" → literal ". Consume both, continue. */
            if (peek_next(s) == '"') {
                advance(s); advance(s);
                continue;
            }
            break;  /* true closing quote */
        }
        if (peek(s) == '\n') {
            /* "..." est mono-ligne strict : un saut de ligne avant le "
             * fermant = chaine non terminee, pointee sur le " ouvrant (la
             * vraie cause). On NE consomme PAS le \n pour que le scanner
             * resynchronise sur le debut de la ligne suivante (pas de
             * cascade). Le multi-ligne passe par [ … ] / backticks. */
            Token tok = error_token_registry(s, E_LEX_UNTERMINATED_STRING, NULL);
            tok.line = openLine;
            tok.column = openColumn;
            return tok;
        }
        advance(s);
    }

    if (is_at_end(s)) {
        Token tok = error_token_registry(s, E_LEX_UNTERMINATED_STRING, NULL);
        tok.line = openLine;
        tok.column = openColumn;
        return tok;
    }

    advance(s); /* closing " */
    return make_token(s, TOKEN_STRING);
}

/* %"..." — string raw avec interpolation [%%] active (opt-in).
 * Le `%` a déjà été consommé par le caller. On consomme le `"` ouvrant et
 * on scanne comme une string normale. Le compiler vérifie le type de token
 * pour décider d'interpoler ou non. */
static Token scan_string_interp(Scanner* s) {
    /* Capture la position du % ouvrant pour reporter une erreur EOF
     * actionnable (cf. scan_string). */
    int openLine = s->line;
    int openColumn = s->startColumn;
    /* Consomme le " ouvrant */
    advance(s);
    while (!is_at_end(s)) {
        if (peek(s) == '"') {
            if (peek_next(s) == '"') { advance(s); advance(s); continue; }
            break;
        }
        if (peek(s) == '\n') { s->line++; s->column = 0; }
        advance(s);
    }
    if (is_at_end(s)) {
        Token tok = error_token_registry(s, E_LEX_UNTERMINATED_STRING_INTERP, NULL);
        tok.line = openLine;
        tok.column = openColumn;
        return tok;
    }
    advance(s); /* closing " */
    return make_token(s, TOKEN_STRING_INTERP);
}

/* `...` (backtick AltGr+7, canon 2024) — raw + interpolation [%%] auto.
 * Doublage `` pour ` interne. Multiligne supporté nativement. */
static Token scan_backtick_string(Scanner* s) {
    /* Capture la position du ` ouvrant pour reporter une erreur EOF
     * actionnable (cf. scan_string). */
    int openLine = s->line;
    int openColumn = s->startColumn;
    advance(s); /* opening ` */
    while (!is_at_end(s)) {
        if (peek(s) == '`') {
            /* Doublage `` → backtick littéral */
            if (peek_next(s) == '`') { advance(s); advance(s); continue; }
            break;
        }
        if (peek(s) == '\n') { s->line++; s->column = 0; }
        advance(s);
    }
    if (is_at_end(s)) {
        Token tok = error_token_registry(s, E_LEX_UNTERMINATED_BACKTICK, NULL);
        tok.line = openLine;
        tok.column = openColumn;
        return tok;
    }
    advance(s); /* closing ` */
    return make_token(s, TOKEN_BACKTICK_STRING);
}

/* Regle unique de fermeture de la chaine-bloc — contrat documente dans
 * scanner.h. Partagee avec le formateur (src/formatter.c). */
bool owl_multiline_block_line_closes(const char* lineStart) {
    const char* p = lineStart;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ']') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\n' || *p == '\r' || *p == '\0'
        || *p == ',' || *p == ')' || *p == '}' || *p == ']';
}

/* Multiline string with [ ... ] notation.
 * Called when we know this [ starts a multiline string (not an operator).
 * The heuristic: [ at start of expression followed by newline. */
static Token scan_multiline_string(Scanner* s) {
    /* Multi-line string: [ on its own line, content on next lines,
     * closing ] alone on its own line (leading whitespace OK).
     *
     * The token spans from the opening [ to the closing ] (inclusive).
     * The compiler will strip the delimiters and surrounding whitespace. */

    /* Capture la position du [ ouvrant pour reporter une erreur EOF
     * actionnable (cf. scan_string). */
    int openLine = s->line;
    int openColumn = s->startColumn;

    while (!is_at_end(s)) {
        char c = peek(s);
        if (c == '\n') {
            /* Newline: advance past it, then check if the next line is just `]` */
            s->line++;
            s->column = 0;
            advance(s);
            /* Skip leading whitespace of new line */
            const char* lineStart = s->current;
            while (peek(s) == ' ' || peek(s) == '\t') advance(s);
            /* Closing line ? Regle partagee avec le formateur — voir
             * owl_multiline_block_line_closes (scanner.h) pour le contrat. */
            if (owl_multiline_block_line_closes(s->current)) {
                /* Closing found. Consume the ]. */
                advance(s);
                return make_token(s, TOKEN_MULTILINE_STRING);
            }
            /* Not a closing line: reset position to lineStart and keep scanning */
            s->current = lineStart;
            continue;
        }
        advance(s);
    }

    {
        Token tok = error_token_registry(s, E_LEX_UNTERMINATED_MULTILINE, NULL);
        tok.line = openLine;
        tok.column = openColumn;
        return tok;
    }
}

/* ================================================================
 * IDENTIFIER AND KEYWORD SCANNING
 * ================================================================ */

/*  : detection caracteres invisibles (definitions plus bas). */
typedef struct { unsigned cp; int bytes; } InvisibleChar;
static bool scanner_peek_invisible(Scanner* s, InvisibleChar* out);
static void invisible_describe(unsigned cp, const char** hex, const char** name);

static Token scan_identifier(Scanner* s) {
    /* Consume full identifier including UTF-8 chars */
    while (is_ident_char(peek(s))) {
        /*  : un caractere invisible (NBSP, ZWSP...) colle au milieu
         * d'un identifiant est sinon avale en silence -> "variable inconnue"
         * trompeuse quand decl et usage divergent. On stoppe l'identifiant
         * ici ; le check debut-de-token (scan_token_impl) nomme le coupable
         * au prochain scan. (On evite de retourner une erreur d'ici pour ne
         * pas heurter la logique d'elision d' / filler du caller.) */
        InvisibleChar inv;
        if (scanner_peek_invisible(s, &inv)) break;
        advance(s);
    }

    /* Handle d' (French elision): if identifier is "d" followed by apostrophe */
    int rawLen = (int)(s->current - s->start);
    char normalized[256];
    int normLen = normalize_string(s->start, rawLen, normalized, sizeof(normalized));

    /* Check for d' elision */
    if (normLen == 1 && normalized[0] == 'd' && peek(s) == '\'') {
        advance(s); /* consume the apostrophe */
        /* "d'" is a filler word */
        return make_token(s, TOKEN_ERROR); /* special: will be skipped by caller */
    }

    /*  +  — les articles ne sont des fillers QU'EN position
     * d'article : juste apres la copule EST/SONT (`x est un entier`, `y est une
     * chaine`, `x, y sont des entiers`). Partout ailleurs (membre d'enum
     * `Un = 1`, variable, champ de structure/classe, parametre, apres `.`...)
     * ce sont des IDENTIFIANTS ordinaires. Sans ce gardage contextuel ils
     * etaient strippes partout et inutilisables comme nom. Miroir FR des EN
     * "a"/"an" (cote compilateur, consume_en_a_an_filler). `normalized` est
     * deja minuscule + accent-strippe. */
    if ((s->prevType == TOKEN_EST || s->prevType == TOKEN_SONT) &&
        ((normLen == 2 && memcmp(normalized, "un", 2) == 0) ||
         (normLen == 3 && memcmp(normalized, "une", 3) == 0) ||
         (normLen == 3 && memcmp(normalized, "des", 3) == 0))) {
        Token t = make_token(s, TOKEN_ERROR);
        t.type = TOKEN_ERROR; /* sentinel filler */
        t.start = NULL;
        return t;
    }

    /* Lookup in keyword table */
    for (int i = 0; keywords[i].name != NULL; i++) {
        if ((int)strlen(keywords[i].name) == normLen &&
            memcmp(keywords[i].name, normalized, normLen) == 0) {
            return make_token(s, keywords[i].type);
        }
    }

    /* Not a keyword: regular identifier */
    return make_token(s, TOKEN_IDENTIFIER);
}

/* ================================================================
 * MAIN SCAN FUNCTION
 * ================================================================ */

/*  : caracteres invisibles / largeur nulle qui se font passer pour
 * rien ou pour un espace normal, typiquement colles depuis un PDF/web. Hors
 * chaines et commentaires (consommes en amont par scan_string / skip_whitespace,
 * donc jamais vus ici), ils sont sinon avales dans un identifiant et produisent
 * une erreur trompeuse ("variable inconnue") qui ne nomme PAS le coupable.
 * On les detecte au debut de token et a l'interieur des identifiants, et on
 * emet une erreur nommee. */
static bool scanner_peek_invisible(Scanner* s, InvisibleChar* out) {
    const unsigned char* p = (const unsigned char*)s->current;
    /* Sequences ciblees. Le buffer est NUL-termine : une sequence tronquee en
     * fin de fichier ne matchera pas (octet de continuation = 0x00). */
    unsigned cp = 0; int n = 0;
    if (p[0] == 0xC2 && p[1] == 0xA0) { cp = 0x00A0; n = 2; }
    else if (p[0] == 0xE2 && p[1] == 0x80) {
        switch (p[2]) {
            case 0x87: cp = 0x2007; break;  case 0x89: cp = 0x2009; break;
            case 0x8A: cp = 0x200A; break;  case 0xAF: cp = 0x202F; break;
            case 0x8B: cp = 0x200B; break;  case 0x8C: cp = 0x200C; break;
            case 0x8D: cp = 0x200D; break;  default: break;
        }
        if (cp) n = 3;
    }
    else if (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0xA0) { cp = 0x2060; n = 3; }
    else if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) { cp = 0x3000; n = 3; }
    else if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { cp = 0xFEFF; n = 3; }
    if (!cp) return false;
    out->cp = cp; out->bytes = n;
    return true;
}

static void invisible_describe(unsigned cp, const char** hex, const char** name) {
    switch (cp) {
        case 0x00A0: *hex = "U+00A0"; *name = "espace insécable"; break;
        case 0x2007: *hex = "U+2007"; *name = "espace figure"; break;
        case 0x2009: *hex = "U+2009"; *name = "espace fine"; break;
        case 0x200A: *hex = "U+200A"; *name = "espace ultra-fine"; break;
        case 0x202F: *hex = "U+202F"; *name = "espace insécable étroit"; break;
        case 0x200B: *hex = "U+200B"; *name = "espace de largeur nulle"; break;
        case 0x200C: *hex = "U+200C"; *name = "antiliant de largeur nulle"; break;
        case 0x200D: *hex = "U+200D"; *name = "liant de largeur nulle"; break;
        case 0x2060: *hex = "U+2060"; *name = "liant de mots"; break;
        case 0x3000: *hex = "U+3000"; *name = "espace idéographique"; break;
        case 0xFEFF: *hex = "U+FEFF"; *name = "marque d'ordre des octets"; break;
        default:     *hex = "U+????"; *name = "invisible"; break;
    }
}

static Token scan_token_impl(Scanner* s) {
retry:
    skip_whitespace(s);

    s->start = s->current;
    s->startColumn = s->column;

    if (is_at_end(s)) return make_token(s, TOKEN_EOF);

    /*  : caractere invisible hors chaine/commentaire. */
    {
        InvisibleChar inv;
        if (scanner_peek_invisible(s, &inv)) {
            /* BOM en tout debut de fichier : convention editeur (UTF-8 BOM),
             * tolere et saute en silence. Le meme U+FEFF ailleurs = rejet. */
            if (inv.cp == 0xFEFF && s->current == s->source) {
                for (int i = 0; i < inv.bytes; i++) advance(s);
                goto retry;
            }
            const char* hex; const char* name;
            invisible_describe(inv.cp, &hex, &name);
            /* Avance au-dela des octets pour garantir la progression du scan. */
            for (int i = 0; i < inv.bytes; i++) advance(s);
            return error_token_registry(s, E_LEX_INVISIBLE_CHAR,
                                        "cp", hex, "name", name, NULL);
        }
    }

    char c = advance(s);

    /* --- Newlines --- */
    if (c == '\n') {
        s->line++;
        s->column = 1;
        s->atLineStart = true;
        return make_token(s, TOKEN_NEWLINE);
    }

    /* --- Numbers --- */
    if (is_digit(c)) {
        return scan_number(s);
    }

    /* --- Identifiers and keywords --- */
    if (is_ident_start(c)) {
        Token tok = scan_identifier(s);
        /* Handle filler words: skip and retry */
        if (tok.type == TOKEN_ERROR && tok.start == NULL) {
            goto retry;
        }
        /* Handle d' elision: skip and retry */
        if (tok.type == TOKEN_ERROR && tok.length > 0) {
            /* Check if this was the d' case by looking at what we consumed */
            char norm[4];
            normalize_string(s->start, (int)(s->current - s->start), norm, sizeof(norm));
            if (norm[0] == 'd') {
                goto retry;
            }
        }
        s->atLineStart = false;
        return tok;
    }

    s->atLineStart = false;

    /* --- Strings --- */
    if (c == '"') {
        /* back up ponter ET colonne : scan_string va re-advance() sur le
         * meme " ; sans le column-- l'octet ouvrant serait compte deux fois
         * et decalerait +1 tous les tokens suivants de la ligne. */
        s->current--; s->column--;
        return scan_string(s);
    }
    /* Backticks: `...` (canon 2024) — raw + interpolation [%%] auto. */
    if (c == '`') {
        s->current--; s->column--; /* idem : re-advance() dans scan_backtick_string */
        return scan_backtick_string(s);
    }

    /* --- Operators and punctuation --- */
    switch (c) {
        case '(': return make_token(s, TOKEN_LEFT_PAREN);
        case ')': return make_token(s, TOKEN_RIGHT_PAREN);
        case '{': return make_token(s, TOKEN_LEFT_BRACE);
        case '}': return make_token(s, TOKEN_RIGHT_BRACE);
        case ',': return make_token(s, TOKEN_COMMA);
        case '^': return make_token(s, TOKEN_CARET);
        case '%':
            if (match(s, '=')) return make_token(s, TOKEN_PERCENT_EQUAL);
            /* %"..." → string avec interpolation [%%] active (opt-in canon 2024) */
            if (peek(s) == '"') return scan_string_interp(s);
            return make_token(s, TOKEN_PERCENT);
        case '?':
            if (match(s, '?')) {
                if (match(s, '='))  return make_token(s, TOKEN_QUESTION_EQUAL);
                if (match(s, '*'))  return make_token(s, TOKEN_QUESTION_STAR);
                return make_token(s, TOKEN_QUESTION_QUESTION);
            }
            if (match(s, '.')) return make_token(s, TOKEN_QUESTION_DOT);
            return make_token(s, TOKEN_QUESTION);
        case '&': return make_token(s, TOKEN_AMPERSAND);

        case ';': return make_token(s, TOKEN_SEMICOLON);

        case ':':
            if (match(s, ':')) return make_token(s, TOKEN_DOUBLE_COLON);
            return make_token(s, TOKEN_COLON);

        case '.':
            if (match(s, '.')) {
                if (match(s, '.')) return make_token(s, TOKEN_TRIPLE_DOT);
                return make_token(s, TOKEN_DOUBLE_DOT);
            }
            /* Could be start of a real number like .5 */
            if (is_digit(peek(s))) return scan_number(s);
            return make_token(s, TOKEN_DOT);

        case '+':
            if (match(s, '+')) return make_token(s, TOKEN_PLUS_PLUS);
            if (match(s, '=')) return make_token(s, TOKEN_PLUS_EQUAL);
            return make_token(s, TOKEN_PLUS);

        case '-':
            if (match(s, '-')) return make_token(s, TOKEN_MINUS_MINUS);
            if (match(s, '=')) return make_token(s, TOKEN_MINUS_EQUAL);
            return make_token(s, TOKEN_MINUS);

        case '*':
            if (match(s, '=')) return make_token(s, TOKEN_STAR_EQUAL);
            return make_token(s, TOKEN_STAR);

        case '/':
            if (match(s, '=')) return make_token(s, TOKEN_SLASH_EQUAL);
            return make_token(s, TOKEN_SLASH);

        case '|':
            if (match(s, '|')) return make_token(s, TOKEN_DOUBLE_PIPE);
            return make_token(s, TOKEN_PIPE);

        case '~':
            if (match(s, '=')) return make_token(s, TOKEN_TILDE_EQUAL);
            if (match(s, '~')) {
                if (match(s, ']')) return make_token(s, TOKEN_ENDS_WITH_VFLEX);
                return make_token(s, TOKEN_DOUBLE_TILDE);
            }
            if (match(s, ']')) return make_token(s, TOKEN_ENDS_WITH_FLEX);
            return make_token(s, TOKEN_TILDE);

        case '=':
            if (match(s, ']')) return make_token(s, TOKEN_ENDS_WITH);
            if (match(s, '>')) return make_token(s, TOKEN_FAT_ARROW); /* lambda */
            return make_token(s, TOKEN_EQUAL);

        case '<':
            if (match(s, '=')) {
                /* <= could be LESS_EQUAL or COPY - parser decides.
                 * (Old SWAP operator <=> removed: use a temporary variable.) */
                return make_token(s, TOKEN_LESS_EQUAL);
            }
            if (match(s, '-')) return make_token(s, TOKEN_REFERENCE);
            if (match(s, '>')) return make_token(s, TOKEN_NOT_EQUAL);
            return make_token(s, TOKEN_LESS);

        case '>':
            if (match(s, '=')) return make_token(s, TOKEN_GREATER_EQUAL);
            return make_token(s, TOKEN_GREATER);

        case '[':
            /* Multi-line string: [ suivi de (espaces/tabs optionnels puis) un
             * retour-ligne.
             *   sTest = [
             *       content lines
             *       ]
             * Strict canon ( volet C) : `[` en fin de ligne est
             * TOUJOURS une chaine multi-ligne, quel que soit le contenu suivant
             * (pas de litteral tableau multi-ligne — inexistant en canon). Le
             * skip des espaces/tabs avant le test du retour-ligne garantit que
             * `[`<eol> et `[ `<eol> (espace) donnent le meme resultat. Sinon
             * `[` est un subscript / operateur. */
            {
                const char* p = s->current;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\n' || *p == '\r') {
                    return scan_multiline_string(s);
                }
            }
            /* [[ for buffer/range extraction */
            if (match(s, '[')) return make_token(s, TOKEN_DOUBLE_BRACKET_OPEN);
            /* Check for [= [=] */
            if (match(s, '=')) {
                if (match(s, ']')) return make_token(s, TOKEN_CONTAINS);
                return make_token(s, TOKEN_STARTS_WITH);
            }
            /* Check for [~ [~] [~~ [~~] */
            if (match(s, '~')) {
                if (match(s, '~')) {
                    if (match(s, ']')) return make_token(s, TOKEN_CONTAINS_VFLEX);
                    return make_token(s, TOKEN_STARTS_WITH_VFLEX);
                }
                if (match(s, ']')) return make_token(s, TOKEN_CONTAINS_FLEX);
                return make_token(s, TOKEN_STARTS_WITH_FLEX);
            }
            return make_token(s, TOKEN_LEFT_BRACKET);

        case ']':
            if (match(s, ']')) return make_token(s, TOKEN_DOUBLE_BRACKET_CLOSE);
            return make_token(s, TOKEN_RIGHT_BRACKET);

        case '\'':
            /* Apostrophe after a word like d' is handled in identifier scanning.
             * Standalone apostrophe is an error. */
            return error_token_registry(s, E_LEX_UNEXPECTED_CHAR_QUOTED, "char", "'", NULL);
    }

    return error_token_registry(s, E_LEX_UNEXPECTED_CHAR, NULL);
}

/* Public entry point. Wraps scan_token_impl with a progression invariant:
 * every call must either consume at least one byte or reach end-of-input.
 * A scanner that returns a token without advancing `current` (and not at EOF)
 * would loop forever in the tokenize loop — a hang regression. The assert is
 * compiled out in release (NDEBUG), zero runtime cost; it only arms debug/CI
 * builds to catch any future change that breaks the invariant. We do NOT force
 * an advance in release: the invariant already holds on every path today, this
 * is purely a guard against future regressions. */
Token scanner_scan_token(Scanner* s) {
    const char* loop_start = s->current;
    Token tok = scan_token_impl(s);
    assert((s->current > loop_start || is_at_end(s)) &&
           "scanner_scan_token made no progress: hang regression");
    (void)loop_start; /* unused when NDEBUG strips the assert */
    /*  — memorise le dernier token significatif (hors newline) pour que
     * scan_identifier ne strippe "un"/"une" qu'en position d'article (apres
     * EST/SONT). Un newline ne reinitialise pas : `x est\nun entier` garde le
     * comportement historique (article strippe a travers un saut de ligne). */
    if (tok.type != TOKEN_NEWLINE) {
        s->prevType = tok.type;
    }
    return tok;
}

/* ================================================================
 * TOKEN TYPE NAMES
 *
 * Pour les tokens techniques (ponctuation, litteraux, EOF) : nom de
 * debug. Pour les mots-cles user-facing : FORME CANON AFFICHEE — c'est
 * ce que `owl explain <mot>` titre, ce que le hover montre et ce que
 * --vscode-data injecte dans la grammar.
 *
 * Contrainte : sur ces mots-la, la chaine doit etre
 * BYTE-IDENTIQUE au canon emis par fmt_keywords[] (formatter.c). Une
 * fiche qui titre LIBÉRER pendant que le save ecrit Libérer fait lire
 * a l'utilisateur une forme qu'il n'obtiendra jamais. La casse n'est
 * donc pas libre ici : la famille objet est en casse mixte (Classe,
 * Hérite, Libérer, Constructeur), la copule et les prepositions en
 * minuscules (est, sont, de, par), les booleens en PascalCase (Vrai,
 * Faux, Null), le controle de flux en majuscules (SI, POUR, FIN).
 * Verrouille par `owl --self-test-fmt-canon` regle R5.
 * ================================================================ */

const char* token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_LEFT_PAREN:        return "LEFT_PAREN";
        case TOKEN_RIGHT_PAREN:       return "RIGHT_PAREN";
        case TOKEN_LEFT_BRACE:        return "LEFT_BRACE";
        case TOKEN_RIGHT_BRACE:       return "RIGHT_BRACE";
        case TOKEN_LEFT_BRACKET:      return "LEFT_BRACKET";
        case TOKEN_RIGHT_BRACKET:     return "RIGHT_BRACKET";
        case TOKEN_COMMA:             return "COMMA";
        case TOKEN_DOT:               return "DOT";
        case TOKEN_SEMICOLON:         return "SEMICOLON";
        case TOKEN_COLON:             return "COLON";
        case TOKEN_TILDE:             return "TILDE";
        case TOKEN_CARET:             return "CARET";
        case TOKEN_PERCENT:           return "PERCENT";
        case TOKEN_AMPERSAND:         return "AMPERSAND";
        case TOKEN_PIPE:              return "PIPE";
        case TOKEN_QUESTION:          return "QUESTION";
        case TOKEN_QUESTION_QUESTION: return "QUESTION_QUESTION";
        case TOKEN_QUESTION_EQUAL:    return "QUESTION_EQUAL";
        case TOKEN_QUESTION_DOT:      return "QUESTION_DOT";
        case TOKEN_QUESTION_STAR:     return "QUESTION_STAR";
        case TOKEN_DOUBLE_BRACKET_OPEN:  return "DOUBLE_BRACKET_OPEN";
        case TOKEN_DOUBLE_BRACKET_CLOSE: return "DOUBLE_BRACKET_CLOSE";
        case TOKEN_DOUBLE_COLON:      return "DOUBLE_COLON";
        case TOKEN_DOUBLE_DOT:        return "DOUBLE_DOT";
        case TOKEN_TRIPLE_DOT:        return "TRIPLE_DOT";
        case TOKEN_DOUBLE_PIPE:       return "DOUBLE_PIPE";
        case TOKEN_PLUS:              return "PLUS";
        case TOKEN_MINUS:             return "MINUS";
        case TOKEN_STAR:              return "STAR";
        case TOKEN_SLASH:             return "SLASH";
        case TOKEN_PLUS_EQUAL:        return "PLUS_EQUAL";
        case TOKEN_MINUS_EQUAL:       return "MINUS_EQUAL";
        case TOKEN_STAR_EQUAL:        return "STAR_EQUAL";
        case TOKEN_SLASH_EQUAL:       return "SLASH_EQUAL";
        case TOKEN_PERCENT_EQUAL:     return "PERCENT_EQUAL";
        case TOKEN_PLUS_PLUS:         return "PLUS_PLUS";
        case TOKEN_MINUS_MINUS:       return "MINUS_MINUS";
        case TOKEN_FAT_ARROW:         return "FAT_ARROW";
        case TOKEN_EQUAL:             return "EQUAL";
        case TOKEN_NOT_EQUAL:         return "NOT_EQUAL";
        case TOKEN_LESS:              return "LESS";
        case TOKEN_LESS_EQUAL:        return "LESS_EQUAL";
        case TOKEN_GREATER:           return "GREATER";
        case TOKEN_GREATER_EQUAL:     return "GREATER_EQUAL";
        case TOKEN_TILDE_EQUAL:       return "TILDE_EQUAL";
        case TOKEN_DOUBLE_TILDE:      return "DOUBLE_TILDE";
        case TOKEN_STARTS_WITH:       return "STARTS_WITH";
        case TOKEN_ENDS_WITH:         return "ENDS_WITH";
        case TOKEN_CONTAINS:          return "CONTAINS";
        case TOKEN_STARTS_WITH_FLEX:  return "STARTS_WITH_FLEX";
        case TOKEN_ENDS_WITH_FLEX:    return "ENDS_WITH_FLEX";
        case TOKEN_CONTAINS_FLEX:     return "CONTAINS_FLEX";
        case TOKEN_STARTS_WITH_VFLEX: return "STARTS_WITH_VFLEX";
        case TOKEN_ENDS_WITH_VFLEX:   return "ENDS_WITH_VFLEX";
        case TOKEN_CONTAINS_VFLEX:    return "CONTAINS_VFLEX";
        case TOKEN_COPY:              return "COPY";
        case TOKEN_REFERENCE:         return "REFERENCE";
        case TOKEN_INTEGER:           return "INTEGER";
        case TOKEN_REAL:              return "REAL";
        case TOKEN_NUMERIC:           return "NUMERIC";
        case TOKEN_MONETARY:          return "MONETARY";
        case TOKEN_STRING:            return "STRING";
        case TOKEN_STRING_INTERP:     return "STRING_INTERP";
        case TOKEN_BACKTICK_STRING:   return "BACKTICK_STRING";
        case TOKEN_MULTILINE_STRING:  return "MULTILINE_STRING";
        case TOKEN_IDENTIFIER:        return "IDENTIFIER";
        case TOKEN_SI:                return "SI";
        case TOKEN_ALORS:             return "ALORS";
        case TOKEN_SINON:             return "SINON";
        case TOKEN_FIN:               return "FIN";
        case TOKEN_POUR:              return "POUR";
        case TOKEN_A:                 return "A";
        case TOKEN_A_SC:              return "_A_";
        case TOKEN_PAS:               return "PAS";
        case TOKEN_PAS_SC:            return "_PAS_";
        case TOKEN_TOUT:              return "TOUT";
        case TOKEN_DE:                return "de";
        case TOKEN_DANS:              return "DANS";
        case TOKEN_TANTQUE:           return "TANTQUE";
        case TOKEN_BOUCLE:            return "BOUCLE";
        case TOKEN_FAIRE:             return "FAIRE";
        case TOKEN_SELON:             return "SELON";
        case TOKEN_CAS:               return "CAS";
        case TOKEN_AUTRE:             return "AUTRE";
        case TOKEN_SORTIR:            return "SORTIR";
        case TOKEN_CONTINUER:         return "CONTINUER";
        case TOKEN_RENVOYER:          return "RENVOYER";
        case TOKEN_RETOUR:            return "RETOUR";
        case TOKEN_AVEC:              return "AVEC";
        case TOKEN_SUR:               return "SUR";
        case TOKEN_EST:               return "est";
        case TOKEN_SONT:              return "sont";
        case TOKEN_CONSTANTE:         return "CONSTANTE";
        case TOKEN_LOCAL:             return "LOCAL";
        case TOKEN_GLOBAL:            return "GLOBAL";
        case TOKEN_BOOLEEN:           return "BOOLEEN";
        case TOKEN_ENTIER:            return "ENTIER";
        case TOKEN_REEL:              return "REEL";
        case TOKEN_MONETAIRE:         return "MONETAIRE";
        case TOKEN_NUMERIQUE:         return "NUMERIQUE";
        case TOKEN_CHAINE:            return "CHAINE";
        case TOKEN_CARACTERE:         return "CARACTERE";
        case TOKEN_DATE:              return "DATE";
        case TOKEN_HEURE:             return "HEURE";
        case TOKEN_DATEHEURE:         return "DATEHEURE";
        case TOKEN_DUREE:             return "DUREE";
        case TOKEN_BUFFER:            return "BUFFER";
        case TOKEN_JSON_KW:           return "JSON";
        case TOKEN_UUID:              return "UUID";
        case TOKEN_SANS:              return "SANS";
        case TOKEN_SIGNE:             return "SIGNE";
        case TOKEN_TABLEAU:           return "tableau";
        case TOKEN_STRUCTURE:         return "Structure";
        case TOKEN_ENUMERATION:       return "Énumération";
        case TOKEN_COMBINAISON:       return "Combinaison";
        case TOKEN_FILE:              return "file";
        case TOKEN_PILE:              return "pile";
        case TOKEN_LISTE:             return "liste";
        case TOKEN_ASSOCIATIF:        return "associatif";
        case TOKEN_FIXE:              return "FIXE";
        case TOKEN_DYNAMIQUE:         return "DYNAMIQUE";
        case TOKEN_PAR:               return "par";
        case TOKEN_PROCEDURE:         return "PROCEDURE";
        case TOKEN_FONCTION:          return "FONCTION";
        case TOKEN_CLASSE:            return "Classe";
        case TOKEN_HERITE:            return "Hérite";
        case TOKEN_INTERFACE:         return "Interface";
        case TOKEN_IMPLEMENTE:        return "implemente";
        case TOKEN_PUBLIC:            return "PUBLIC";
        case TOKEN_PRIVE:             return "PRIVÉ";
        case TOKEN_PROTEGE:           return "PROTÉGÉ";
        case TOKEN_ABSTRAITE:         return "ABSTRAITE";
        case TOKEN_GLOBALE:           return "GLOBALE";
        case TOKEN_LIBERER:           return "Libérer";
        case TOKEN_ET:                return "ET";
        case TOKEN_OU:                return "OU";
        case TOKEN_BIT_SHL:           return "bitDécaleGauche";
        case TOKEN_BIT_SHR:           return "bitDécaleDroite";
        case TOKEN_ET_SC:             return "_ET_";
        case TOKEN_OU_SC:             return "_OU_";
        case TOKEN_MODULO:            return "MODULO";
        case TOKEN_DANS_SC:           return "_DANS_";
        case TOKEN_VRAI:              return "Vrai";
        case TOKEN_FAUX:              return "Faux";
        case TOKEN_NUL:               return "Null";
        case TOKEN_QUAND:             return "QUAND";
        case TOKEN_EXCEPTION:         return "EXCEPTION";
        case TOKEN_ENFIN:             return "ENFIN";
        case TOKEN_OBJET:             return "objet";
        case TOKEN_MOIMEME:           return "MoiMême";
        case TOKEN_MONCHAMP:          return "MonChamp";
        case TOKEN_MAFENETRE:         return "MaFenêtre";
        case TOKEN_ANCETRE:           return "ANCETRE";
        case TOKEN_OWL_RESERVED:      return "OWL";
        case TOKEN_ELEMENT:           return "ÉLÉMENT";
        case TOKEN_SEPAREE:           return "SÉPARÉE";
        case TOKEN_CONSTRUCTEUR:      return "Constructeur";
        case TOKEN_DESTRUCTEUR:       return "Destructeur";
        case TOKEN_FENETRE:           return "FENETRE";
        case TOKEN_ETAT:              return "ETAT";
        case TOKEN_LIBELLE:           return "LIBELLE";
        case TOKEN_BOUTON:            return "BOUTON";
        case TOKEN_SAISIE:            return "SAISIE";
        case TOKEN_TABLE:             return "TABLE";
        case TOKEN_COLONNE:           return "COLONNE";
        case TOKEN_CONTENEUR:         return "CONTENEUR";
        case TOKEN_PROPRIETES:        return "PROPRIETES";
        case TOKEN_NEWLINE:           return "NEWLINE";
        case TOKEN_EOF:               return "EOF";
        case TOKEN_ERROR:             return "ERROR";
        case TOKEN_COUNT:             return "COUNT";
    }
    return "UNKNOWN";
}
