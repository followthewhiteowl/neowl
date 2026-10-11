// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * Console I/O (stdin / stdout / stderr) — Console* family.
 *
 * Semantics:
 *   ConsoleEcrit(texte)   writes to stdout WITHOUT an automatic newline.
 *   ConsoleErreur(texte)  writes to stderr without newline.
 *   ConsoleLit()          reads one line from stdin, returns chaine
 *                          (trailing \n and \r stripped).
 *
 * Use `Trace(x)` for the Trace-style auto-newline behaviour, or append
 * RC / "\n" explicitly.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Script-level command-line arguments captured by main(). argv[0..count-1]
 * are strings to be handed to the script (script name excluded). */
static int     g_script_argc = 0;
static char**  g_script_argv = NULL;

void stdlib_console_set_script_args(int argc, char** argv) {
    g_script_argc = argc;
    g_script_argv = argv;
}

/* Convert any Value to its printable string form (reuses Trace logic).
 * Keeping this small: we only format scalars and objects with chars. */
static void print_value_to(FILE* f, Value v) {
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        fwrite(s->chars, 1, (size_t)s->length, f);
        return;
    }
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        fwrite(b->bytes, 1, (size_t)b->length, f);
        return;
    }
    /* Fallback: use the VM's generic value_print which writes to stdout.
     * For stderr we build a string via fprintf on primitives. */
    if (IS_BOOL(v))     { fputs(AS_BOOL(v) ? "Vrai" : "Faux", f); return; }
    if (IS_NIL(v))      { fputs("Null", f); return; }
    if (IS_INTEGER(v))  { fprintf(f, "%lld", (long long)AS_INTEGER(v)); return; }
    if (IS_REAL(v))     {
        char buf[40]; owl_real_to_str(AS_REAL(v), buf, sizeof(buf));
        fputs(buf, f); return;
    }
    if (IS_CURRENCY(v)) { char b[48]; currency_to_str(AS_CURRENCY(v), b, sizeof(b)); fputs(b, f); return; }
    /* Complex types: serialize via value_print which targets stdout, so for
     * stderr we just skip (user can format upstream). */
    if (f == stdout) value_print(v);
}

static Value fn_console_ecrit(int argCount, Value* args) {
    for (int i = 0; i < argCount; i++) print_value_to(stdout, args[i]);
    fflush(stdout);
    return NIL_VAL;
}

static Value fn_console_erreur(int argCount, Value* args) {
    for (int i = 0; i < argCount; i++) print_value_to(stderr, args[i]);
    fflush(stderr);
    return NIL_VAL;
}

static Value fn_console_lit(int argCount, Value* args) {
    /* Optional first arg: prompt printed to stdout before reading. */
    if (argCount >= 1) {
        print_value_to(stdout, args[0]);
        fflush(stdout);
    }
    char* buf = NULL;
    size_t cap = 0, len = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap = cap < 64 ? 64 : cap * 2;
            buf = (char*)realloc(buf, cap);
        }
        buf[len++] = (char)c;
    }
    if (buf == NULL) return OBJ_VAL(obj_copy_string("", 0));
    /* Trim trailing \r for Windows line endings. */
    if (len > 0 && buf[len - 1] == '\r') len--;
    Value out = OBJ_VAL(obj_copy_string(buf, (int)len));
    free(buf);
    return out;
}

/* Cherche une option nommée /name=val ou -name=val ou /name ou -name dans
 * g_script_argv. Retourne :
 *   - la valeur après '=' si /name=val ou -name=val
 *   - chaine vide "" si /name ou -name (sans valeur)
 *   - NULL si pas trouvé. */
static const char* find_named_option(const char* name) {
    int nameLen = (int)strlen(name);
    for (int i = 0; i < g_script_argc; i++) {
        const char* a = g_script_argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        if (strncmp(a + 1, name, (size_t)nameLen) != 0) continue;
        char tail = a[1 + nameLen];
        if (tail == '=') return a + 2 + nameLen;     /* /name=value */
        if (tail == '\0') return "";                  /* /name sans valeur */
    }
    return NULL;
}

/* LigneCommande() / LigneCommande(N) / LigneCommande(nom [, defaut])
 *
 * Doc 3013051. Trois formes :
 *   - LigneCommande()           -> chaine totale (args joints par espace)
 *   - LigneCommande(N)          -> Nème argument (1-based), "" hors borne
 *   - LigneCommande("nom")      -> valeur de /nom=val ou -nom=val, "" si absent
 *   - LigneCommande("nom",def)  -> idem mais defaut si absent
 *
 * Note OWL : argv est tokenise par le CRT, on ne fait pas la reinjection
 * d'espaces autour du '=' que la doc montre ("/opt1 = param2"). On retourne
 * l'argv brut (ex. "/opt1=param2"). Divergence assumee. */
static Value fn_ligne_commande(int argCount, Value* args) {
    /* Forme 1 : chaine totale */
    if (argCount == 0) {
        int total = 0;
        for (int i = 0; i < g_script_argc; i++) total += (int)strlen(g_script_argv[i]) + 1;
        if (total <= 0) return OBJ_VAL(obj_copy_string("", 0));
        char* buf = (char*)malloc((size_t)total);
        int off = 0;
        for (int i = 0; i < g_script_argc; i++) {
            if (i > 0) buf[off++] = ' ';
            int len = (int)strlen(g_script_argv[i]);
            memcpy(buf + off, g_script_argv[i], (size_t)len);
            off += len;
        }
        Value out = OBJ_VAL(obj_copy_string(buf, off));
        free(buf);
        return out;
    }

    /* Forme 2 : Nème argument (1-based) */
    if (argCount >= 1 && IS_INTEGER(args[0])) {
        int64_t n = AS_INTEGER(args[0]);
        if (n < 1 || n > g_script_argc) return OBJ_VAL(obj_copy_string("", 0));
        const char* a = g_script_argv[n - 1];
        return OBJ_VAL(obj_copy_string(a, (int)strlen(a)));
    }

    /* Formes 3/4 : option nommée, avec ou sans valeur par défaut */
    if (argCount >= 1 && IS_STRING(args[0])) {
        ObjString* name = AS_STRING(args[0]);
        const char* val = find_named_option(name->chars);
        if (val != NULL) return OBJ_VAL(obj_copy_string(val, (int)strlen(val)));
        /* Pas trouvé : retour défaut si fourni, sinon "" */
        if (argCount >= 2 && IS_STRING(args[1])) {
            ObjString* def = AS_STRING(args[1]);
            return OBJ_VAL(obj_copy_string(def->chars, def->length));
        }
        return OBJ_VAL(obj_copy_string("", 0));
    }

    return OBJ_VAL(obj_copy_string("", 0));
}

/* ================================================================
 * PRESSE-PAPIER (Windows clipboard) — VersPressePapier / DuPressePapier /
 * EffacePressePapier. Doc 3024033 / 3024032 / 3024041.
 *
 * IMPORTANT pour les tests : VersPressePapier ECRASE le clipboard du user.
 * Toujours mocker via MockNative dans les tests automatises pour eviter
 * de polluer l'environnement de l'utilisateur.
 *
 * V1 : Windows uniquement (user32). Texte plat ANSI/UTF-16. Pas de RTF
 * ni d'images. Sur autres OS : retour silencieux (no-op + chaine vide).
 * ================================================================ */

#ifdef _WIN32
#  include <windows.h>
#endif

/* VersPressePapier(texte) — ecrit le texte dans le clipboard systeme.
 * Retourne booleen Vrai si succes, Faux sinon. */
static Value fn_vers_pressepapier(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return BOOL_VAL(false);
#ifdef _WIN32
    ObjString* s = AS_STRING(args[0]);
    /* UTF-8 source -> UTF-16 pour Windows clipboard CF_UNICODETEXT */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s->chars, s->length, NULL, 0);
    if (wlen <= 0) return BOOL_VAL(false);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (!hMem) return BOOL_VAL(false);
    wchar_t* wbuf = (wchar_t*)GlobalLock(hMem);
    if (!wbuf) { GlobalFree(hMem); return BOOL_VAL(false); }
    MultiByteToWideChar(CP_UTF8, 0, s->chars, s->length, wbuf, wlen);
    wbuf[wlen] = L'\0';
    GlobalUnlock(hMem);
    if (!OpenClipboard(NULL)) { GlobalFree(hMem); return BOOL_VAL(false); }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return BOOL_VAL(false);
    }
    /* Apres SetClipboardData succes, Windows possede hMem. Ne pas free. */
    CloseClipboard();
    return BOOL_VAL(true);
#else
    (void)args;
    return BOOL_VAL(false);
#endif
}

/* DuPressePapier() — lit le contenu texte du clipboard (UTF-8). Retourne ""
 * si vide, autre format que texte, ou erreur. */
static Value fn_du_pressepapier(int argCount, Value* args) {
    (void)argCount; (void)args;
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return OBJ_VAL(obj_copy_string("", 0));
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        CloseClipboard();
        return OBJ_VAL(obj_copy_string("", 0));
    }
    wchar_t* wbuf = (wchar_t*)GlobalLock(h);
    if (!wbuf) {
        CloseClipboard();
        return OBJ_VAL(obj_copy_string("", 0));
    }
    int wlen = (int)wcslen(wbuf);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    char* u8buf = (char*)malloc((size_t)u8len + 1);
    if (!u8buf) {
        GlobalUnlock(h);
        CloseClipboard();
        return OBJ_VAL(obj_copy_string("", 0));
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, u8buf, u8len, NULL, NULL);
    u8buf[u8len] = '\0';
    GlobalUnlock(h);
    CloseClipboard();
    Value result = OBJ_VAL(obj_copy_string(u8buf, u8len));
    free(u8buf);
    return result;
#else
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* EffacePressePapier() — vide le clipboard. Retourne booleen succes. */
static Value fn_efface_pressepapier(int argCount, Value* args) {
    (void)argCount; (void)args;
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return BOOL_VAL(false);
    BOOL ok = EmptyClipboard();
    CloseClipboard();
    return BOOL_VAL(ok ? true : false);
#else
    return BOOL_VAL(false);
#endif
}

/* ================================================================
 * Registry declarations — Console family (canonical names).
 * ================================================================ */

static const OwlParam p_msg_variadic[] = {
    {"msg", "tout", "Valeur(s) à écrire — concaténées sans séparateur"},
    {0}
};
static const OwlParam p_prompt[] = {
    {"prompt", "chaîne", "Texte affiché avant la lecture (optionnel)"},
    {0}
};
/* Ecrit/Erreur : vraies variadiques Ecrit(v1,...,vN) -> arité libre (-1). */
static const OwlSignature sig_ecrit[]   = { {-1, p_msg_variadic, "rien",    "sans retour à la ligne"}, {0} };
static const OwlSignature sig_erreur[]  = { {-1, p_msg_variadic, "rien",    "stderr, sans retour à la ligne"}, {0} };
/* Lit([prompt]) : prompt optionnel -> formes discrètes 0..1. */
static const OwlSignature sig_lit[]     = {
    {0, NULL,     "chaîne", "ligne lue (sans \\n final)"},
    {1, p_prompt, "chaîne", "ligne lue après affichage du prompt"},
    {0}
};
static const OwlParam p_ligne_cmd_n[]    = { {"indice", "entier", "Indice 1-based de l'argument"}, {0} };
static const OwlParam p_ligne_cmd_named[] = {
    {"nom",    "chaîne", "Nom de l'option (/nom=val ou -nom=val)"},
    {"défaut", "chaîne", "Valeur par défaut si absent (optionnel)"},
    {0}
};
/* LigneCommande : 3 formes, arité réelle 0..2.
 * Forme 1 arg = soit Nème argument (entier), soit option nommée (chaîne) :
 * la distinction se fait par typage à l'exécution. Forme 2 args = option
 * nommée + valeur par défaut. Plus de variadique masquée. */
static const OwlSignature sig_ligne_cmd[] = {
    { 0, NULL,              "chaîne", "Ligne de commande complète" },
    { 1, p_ligne_cmd_n,     "chaîne", "Nème argument (1-based), \"\" hors borne" },
    { 2, p_ligne_cmd_named, "chaîne", "Valeur de l'option nommée, ou défaut" },
    {0}
};

static const OwlParam p_pp_set[]  = { {"texte", "chaîne", "Texte à copier dans le presse-papier"}, {0} };
static const OwlSignature sig_pp_set[]  = { {1, p_pp_set, "booléen", "Vrai si succès"}, {0} };
static const OwlSignature sig_pp_get[]  = { {0, NULL,    "chaîne",  "Contenu texte du presse-papier (\"\" si vide ou autre format)"}, {0} };
static const OwlSignature sig_pp_del[]  = { {0, NULL,    "booléen", "Vrai si succès"}, {0} };

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "ConsoleEcrit",
        .nameEN     = "ConsoleWrite",
        .category   = "Console",
        .descFR     = "Écrit une chaîne dans stdout sans retour à la ligne.",
        .descEN     = "Write a string to stdout without newline.",
        .signatures = sig_ecrit,
        .impl       = fn_console_ecrit,
    },
    {
        .canonical  = "ConsoleErreur",
        .nameEN     = "ConsoleError",
        .category   = "Console",
        .descFR     = "Écrit une chaîne dans stderr.",
        .descEN     = "Write a string to stderr.",
        .signatures = sig_erreur,
        .impl       = fn_console_erreur,
    },
    {
        .canonical  = "ConsoleLit",
        .nameEN     = "ConsoleRead",
        .category   = "Console",
        .descFR     = "Lit une ligne depuis stdin. Retourne la chaîne sans \\n final.",
        .descEN     = "Read a line from stdin. Returns the string without trailing \\n.",
        .signatures = sig_lit,
        .impl       = fn_console_lit,
    },
    {
        .canonical  = "LigneCommande",
        .nameEN     = "CommandLine",
        .category   = "Console",
        .descFR     = "Récupère les arguments passés au script. 0 arg : ligne complète. 1 entier : Nème arg (1-based). 1 chaîne : valeur de l'option /nom=val ou -nom=val. 2 chaînes : idem + défaut.",
        .descEN     = "Get script arguments. 0 args: full line. 1 int: Nth arg (1-based). 1 string: value of /name=val or -name=val. 2 strings: same with default.",
        .signatures = sig_ligne_cmd,
        .impl       = fn_ligne_commande,
    },
    {
        .canonical  = "VersPressePapier",
        .nameEN     = "ToClipboard",
        .category   = "Système",
        .descFR     = "Écrit du texte dans le presse-papier système (Windows uniquement à ce jour). ATTENTION : écrase le clipboard utilisateur, mocker via MockNative dans les tests.",
        .descEN     = "Write text to the system clipboard (currently Windows only). WARNING: overwrites user clipboard, mock via MockNative in tests.",
        .signatures = sig_pp_set,
        .impl       = fn_vers_pressepapier,
    },
    {
        .canonical  = "DuPressePapier",
        .nameEN     = "FromClipboard",
        .category   = "Système",
        .descFR     = "Lit le contenu texte du presse-papier (UTF-8). \"\" si vide ou autre format.",
        .descEN     = "Read text content of the clipboard (UTF-8). \"\" if empty or other format.",
        .signatures = sig_pp_get,
        .impl       = fn_du_pressepapier,
    },
    {
        .canonical  = "EffacePressePapier",
        .nameEN     = "ClearClipboard",
        .category   = "Système",
        .descFR     = "Vide le presse-papier système.",
        .descEN     = "Clear the system clipboard.",
        .signatures = sig_pp_del,
        .impl       = fn_efface_pressepapier,
    },
};

void stdlib_console_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}
