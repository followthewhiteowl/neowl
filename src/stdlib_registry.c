// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_registry.c — Famille d'accès à la base de registre Windows.
 *
 * 14 natives, groupées par usage :
 *  - CRUD valeur/clé : RegistreCréeClé, RegistreEcrit, RegistreLit,
 *    RegistreExiste, RegistreSupprimeValeur, RegistreSupprimeClé.
 *  - Manipulation de clés : RegistreCopieClé, RegistreRenomme,
 *    RegistreTypeValeur, RegistreCherche.
 *  - Énumération : RegistreListeClé, RegistreListeValeur,
 *    RegistrePremièreSousClé, RegistreCléSuivante.
 * Constantes : registreMode* (accès 32/64/auto), registreType* (type renvoyé
 * par RegistreTypeValeur), registreCherche* (options de recherche).
 *
 * ── Plateforme ──
 * La base de registre n'existe que sous Windows. Les symboles sont enregistrés
 * sur TOUTES les plateformes (surface langage / compile-check / LSP uniforme),
 * mais hors Windows chaque fonction renvoie Faux/vide + un soft-error
 * ErreurInfo explicite, sans jamais interrompre le programme. Pas d'émulation,
 * pas de registre fictif.
 *
 * ── Contrat d'échec doux ──
 * Toute opération impossible renvoie Faux (ou chaîne vide pour RegistreLit) et
 * pose un soft-error consultable via ErreurInfo(errMessage). Jamais de crash.
 *
 * ── Chemin de clé ──
 * Le chemin inclut la racine : "HKEY_CURRENT_USER\SOFTWARE\...". La racine est
 * le préfixe jusqu'au premier antislash (HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
 * HKEY_CLASSES_ROOT, HKEY_USERS, HKEY_CURRENT_CONFIG — abréviations acceptées).
 *
 * ── Mapping des types ──
 * Écriture : chaîne -> REG_SZ, entier -> REG_DWORD, réel -> binaire 8 octets.
 * Lecture : REG_SZ -> chaîne, REG_DWORD/REG_QWORD -> entier, binaire 8 o -> réel.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"
/*  : skip section compile-aware d'owl_errors.h pour éviter la collision
 * TokenType (scanner.h vs winnt.h tiré par windows.h). Même motif que
 * stdlib_runtime.c. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "object.h"
#include "value.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

/* Valeurs publiques des constantes de mode (exposées à l'utilisateur). */
#define OWL_REG_MODE_AUTO 0
#define OWL_REG_MODE_32   32
#define OWL_REG_MODE_64   64

/* Options de RegistreCherche : drapeaux combinables (entier). Le mode par
 * défaut (aucune option passée) explore clés + valeurs + données en récursif. */
#define OWL_REG_SEEK_KEY        1
#define OWL_REG_SEEK_VALUE      2
#define OWL_REG_SEEK_DATA       4
#define OWL_REG_SEEK_RECURSIVE  8
#define OWL_REG_SEEK_WHOLE      16
#define OWL_REG_SEEK_DEFAULT    (OWL_REG_SEEK_KEY | OWL_REG_SEEK_VALUE | \
                                 OWL_REG_SEEK_DATA | OWL_REG_SEEK_RECURSIVE)

/* Jetons renvoyés par RegistreTypeValeur et portés par les constantes
 * registreType* (valeurs chaîne opaques servant de discriminant). */
#define OWL_REGTYPE_STRING    "cha\xc3\xaene"            /* chaîne */
#define OWL_REGTYPE_STRINGENV "cha\xc3\xaeneEnv"         /* chaîneEnv */
#define OWL_REGTYPE_INT       "entier"
#define OWL_REGTYPE_INT8      "entier8"
#define OWL_REGTYPE_INTBE     "entierBigEndian"
#define OWL_REGTYPE_BINARY    "binaire"
#define OWL_REGTYPE_MULTI     "multicha\xc3\xaene"       /* multichaîne */

/* ================================================================
 * HELPERS COMMUNS (indépendants de la plateforme)
 * ================================================================ */

static const char* reg_arg_cstr(Value v) {
    return IS_STRING(v) ? AS_CSTRING(v) : "";
}

/* Détecte un éventuel paramètre de mode en tête. Le chemin de clé est toujours
 * une chaîne ; un premier argument entier est donc sans ambiguïté le mode
 * d'accès. Renvoie l'index du premier argument « métier » et pose *outMode. */
static int reg_consume_mode(int argCount, Value* args, long* outMode) {
    *outMode = OWL_REG_MODE_AUTO;
    if (argCount >= 1 && IS_INTEGER(args[0])) {
        *outMode = (long)AS_INTEGER(args[0]);
        return 1;
    }
    return 0;
}

#if !defined(_WIN32)
/* Soft-error « registre indisponible hors Windows ». Seulement nécessaire dans
 * la branche de repli non-Windows. Réutilise le code natif générique
 * « introuvable » (pas de code dédié au registre). */
static void reg_err_unavailable(const char* native) {
    owl_emit_native_error(E_NATIVE_NOT_FOUND, native,
        "name", "base de registre (disponible sous Windows uniquement)", NULL);
}
#endif

#if defined(_WIN32)

/* Soft-errors de la branche Windows, adossés aux codes natifs génériques. */
static void reg_err_bad_path(const char* native, const char* path) {
    owl_emit_native_error(E_NATIVE_PARSE_ERROR, native,
        "value", path,
        "expected", "chemin de cl\xc3\xa9 commen\xc3\xa7\x61nt par une racine connue "
                    "(HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_CLASSES_ROOT, "
                    "HKEY_USERS, HKEY_CURRENT_CONFIG)",
        NULL);
}
static void reg_err_not_found(const char* native, const char* what) {
    owl_emit_native_error(E_NATIVE_NOT_FOUND, native, "name", what, NULL);
}
static void reg_err_op(const char* native, const char* path, const char* detail) {
    owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, native,
        "path", path, "detail", detail, NULL);
}

/* Traduit le mode d'accès en drapeau de vue WOW64. */
static REGSAM reg_wow_flag(long mode) {
    if (mode == OWL_REG_MODE_32) return KEY_WOW64_32KEY;
    if (mode == OWL_REG_MODE_64) return KEY_WOW64_64KEY;
    return 0;  /* Auto : vue native du processus */
}

/* Scinde un chemin complet en racine HKEY + sous-clé. Renvoie false si la
 * racine n'est pas reconnue. *outSub pointe dans la chaîne d'origine (après le
 * premier antislash) ou vers "" si le chemin se limite à la racine. */
static bool reg_split_root(const char* path, HKEY* outRoot, const char** outSub) {
    const char* sep = strchr(path, '\\');
    size_t rootLen = sep ? (size_t)(sep - path) : strlen(path);

    static const struct { const char* name; HKEY hkey; } ROOTS[] = {
        { "HKEY_CURRENT_USER",   HKEY_CURRENT_USER   },
        { "HKEY_LOCAL_MACHINE",  HKEY_LOCAL_MACHINE  },
        { "HKEY_CLASSES_ROOT",   HKEY_CLASSES_ROOT   },
        { "HKEY_USERS",          HKEY_USERS          },
        { "HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
        { "HKCU",                HKEY_CURRENT_USER   },
        { "HKLM",                HKEY_LOCAL_MACHINE  },
        { "HKCR",                HKEY_CLASSES_ROOT   },
        { "HKU",                 HKEY_USERS          },
        { "HKCC",                HKEY_CURRENT_CONFIG },
    };

    for (size_t i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++) {
        if (strlen(ROOTS[i].name) == rootLen &&
            _strnicmp(path, ROOTS[i].name, rootLen) == 0) {
            *outRoot = ROOTS[i].hkey;
            *outSub = sep ? sep + 1 : "";
            return true;
        }
    }
    return false;
}

/* Correspondance insensible à la casse. Mot complet -> égalité stricte,
 * sinon sous-chaîne (comportement « contient »). */
static bool reg_str_match(const char* hay, const char* needle, bool wholeWord) {
    if (needle == NULL || *needle == '\0') return true;
    if (wholeWord) return _stricmp(hay, needle) == 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        if (_strnicmp(p, needle, nl) == 0) return true;
    }
    return false;
}

/* Parcours (optionnellement récursif) d'une clé ouverte à la recherche du
 * motif dans les noms de sous-clés, les noms de valeurs et/ou les données
 * chaîne, selon les drapeaux d'options. Au premier match, formate le résultat
 * « <chemin> TAB <élément> [TAB <donnée>] » dans out et renvoie true. */
static bool reg_seek_recursive(HKEY hKey, const char* keyPath, long options,
                               const char* needle, REGSAM wow,
                               char* out, size_t outCap) {
    bool wholeWord = (options & OWL_REG_SEEK_WHOLE) != 0;

    if (options & (OWL_REG_SEEK_VALUE | OWL_REG_SEEK_DATA)) {
        DWORD idx = 0;
        for (;;) {
            char  valName[16384];
            BYTE  data[8192];
            DWORD valLen = (DWORD)sizeof(valName);
            DWORD dataLen = (DWORD)sizeof(data);
            DWORD type = 0;
            LONG rc = RegEnumValueA(hKey, idx, valName, &valLen, NULL, &type,
                                    data, &dataLen);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            idx++;
            if (rc == ERROR_MORE_DATA) continue;  /* valeur trop volumineuse : ignorée */
            if (rc != ERROR_SUCCESS) break;

            if ((options & OWL_REG_SEEK_VALUE) &&
                reg_str_match(valName, needle, wholeWord)) {
                snprintf(out, outCap, "%s\t%s", keyPath, valName);
                return true;
            }
            if ((options & OWL_REG_SEEK_DATA) &&
                (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
                size_t dl = dataLen;
                if (dl >= sizeof(data)) dl = sizeof(data) - 1;
                data[dl] = '\0';
                if (reg_str_match((const char*)data, needle, wholeWord)) {
                    snprintf(out, outCap, "%s\t%s\t%s", keyPath, valName,
                             (const char*)data);
                    return true;
                }
            }
        }
    }

    {
        DWORD idx = 0;
        for (;;) {
            char  subName[512];
            DWORD subLen = (DWORD)sizeof(subName);
            LONG rc = RegEnumKeyExA(hKey, idx, subName, &subLen, NULL, NULL,
                                    NULL, NULL);
            if (rc != ERROR_SUCCESS) break;  /* y compris ERROR_NO_MORE_ITEMS */
            idx++;

            if ((options & OWL_REG_SEEK_KEY) &&
                reg_str_match(subName, needle, wholeWord)) {
                snprintf(out, outCap, "%s\t%s", keyPath, subName);
                return true;
            }
            if (options & OWL_REG_SEEK_RECURSIVE) {
                HKEY hSub;
                if (RegOpenKeyExA(hKey, subName, 0, KEY_READ | wow, &hSub)
                        == ERROR_SUCCESS) {
                    char childPath[2048];
                    snprintf(childPath, sizeof(childPath), "%s\\%s", keyPath, subName);
                    bool found = reg_seek_recursive(hSub, childPath, options,
                                                    needle, wow, out, outCap);
                    RegCloseKey(hSub);
                    if (found) return true;
                }
            }
        }
    }
    return false;
}

/* Petit tampon chaîne extensible (heap C, hors tas OWL — aucun déclenchement de
 * GC). Sert à accumuler les listes d'énumération dont la taille est inconnue
 * a priori. */
typedef struct { char* buf; size_t len; size_t cap; } RegStrBuf;

static void regsb_init(RegStrBuf* b) { b->buf = NULL; b->len = 0; b->cap = 0; }
static void regsb_free(RegStrBuf* b) { free(b->buf); b->buf = NULL; }

static void regsb_append(RegStrBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char* nb = (char*)realloc(b->buf, nc);
        if (nb == NULL) return;  /* OOM : on tronque silencieusement */
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* Lettre de type canonique renvoyée dans le résultat détaillé de
 * RegistreListeValeur : "T" chaîne, "N" numérique, "B" binaire. */
static const char* reg_type_letter(DWORD type) {
    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_MULTI_SZ:
            return "T";
        case REG_DWORD:
        case REG_DWORD_BIG_ENDIAN:
        case REG_QWORD:
            return "N";
        default:
            return "B";
    }
}

#endif /* _WIN32 */

/* ================================================================
 * NATIVES
 * ================================================================ */

/* RegistreCréeClé([<Mode>,] <Chemin>) -> booléen.
 * Crée la clé (et les sous-clés intermédiaires manquantes). Renvoie Vrai si la
 * clé a été créée, Faux si elle existait déjà (sans erreur) ou en cas d'échec
 * (avec erreur). */
static Value fn_registre_cree_cle(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreCr\xc3\xa9\x65\x43l\xc3\xa9", path);
        return BOOL_VAL(false);
    }

    HKEY hKey; DWORD disp;
    LONG rc = RegCreateKeyExA(root, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                              KEY_WRITE | reg_wow_flag(mode), NULL, &hKey, &disp);
    if (rc != ERROR_SUCCESS) {
        reg_err_op("RegistreCr\xc3\xa9\x65\x43l\xc3\xa9", path, "cr\xc3\xa9\x61tion impossible");
        return BOOL_VAL(false);
    }
    RegCloseKey(hKey);
    /* Clé déjà présente : succès système mais résultat Faux, SANS soft-error. */
    return BOOL_VAL(disp == REG_CREATED_NEW_KEY);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreCr\xc3\xa9\x65\x43l\xc3\xa9");
    return BOOL_VAL(false);
#endif
}

/* RegistreEcrit([<Mode>,] <Chemin>, <Nom>, <Valeur>) -> booléen.
 * La clé cible doit déjà exister. Type d'écriture selon le type de <Valeur>. */
static Value fn_registre_ecrit(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    const char* name = reg_arg_cstr(args[i + 1]);
    Value       val  = args[i + 2];

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreEcrit", path);
        return BOOL_VAL(false);
    }

    HKEY hKey;
    LONG rc = RegOpenKeyExA(root, sub, 0, KEY_SET_VALUE | reg_wow_flag(mode), &hKey);
    if (rc != ERROR_SUCCESS) {
        reg_err_not_found("RegistreEcrit", path);
        return BOOL_VAL(false);
    }

    LONG wr;
    if (IS_STRING(val)) {
        ObjString* s = AS_STRING(val);
        wr = RegSetValueExA(hKey, name, 0, REG_SZ,
                            (const BYTE*)s->chars, (DWORD)s->length + 1);
    } else if (IS_INTEGER(val)) {
        DWORD dw = (DWORD)AS_INTEGER(val);
        wr = RegSetValueExA(hKey, name, 0, REG_DWORD,
                            (const BYTE*)&dw, (DWORD)sizeof(dw));
    } else if (IS_REAL(val)) {
        double d = AS_REAL(val);
        wr = RegSetValueExA(hKey, name, 0, REG_BINARY,
                            (const BYTE*)&d, (DWORD)sizeof(d));
    } else {
        RegCloseKey(hKey);
        reg_err_op("RegistreEcrit", path, "type de valeur non pris en charge");
        return BOOL_VAL(false);
    }
    RegCloseKey(hKey);

    if (wr != ERROR_SUCCESS) {
        reg_err_op("RegistreEcrit", path, "\xc3\xa9\x63riture impossible");
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreEcrit");
    return BOOL_VAL(false);
#endif
}

/* RegistreLit([<Mode>,] <Chemin>, <Nom>) -> valeur (chaîne/entier/réel).
 * Valeur absente ou clé introuvable : chaîne vide + soft-error. */
static Value fn_registre_lit(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    const char* name = reg_arg_cstr(args[i + 1]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreLit", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    HKEY hKey;
    LONG rc = RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE | reg_wow_flag(mode), &hKey);
    if (rc != ERROR_SUCCESS) {
        reg_err_not_found("RegistreLit", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    DWORD type = 0, size = 0;
    rc = RegQueryValueExA(hKey, name, NULL, &type, NULL, &size);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        reg_err_not_found("RegistreLit", name);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    BYTE* buf = (BYTE*)malloc(size > 0 ? size : 1);
    if (buf == NULL) {
        RegCloseKey(hKey);
        reg_err_op("RegistreLit", path, "m\xc3\xa9moire insuffisante");
        return OBJ_VAL(obj_copy_string("", 0));
    }
    rc = RegQueryValueExA(hKey, name, NULL, &type, buf, &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        free(buf);
        reg_err_not_found("RegistreLit", name);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    Value out;
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        int len = (int)size;
        /* Retire le terminateur nul comptabilisé par le registre. */
        while (len > 0 && buf[len - 1] == '\0') len--;
        out = OBJ_VAL(obj_copy_string((const char*)buf, len));
    } else if (type == REG_DWORD) {
        DWORD dw = 0;
        if (size >= sizeof(dw)) memcpy(&dw, buf, sizeof(dw));
        out = INTEGER_VAL((int64_t)dw);
    } else if (type == REG_QWORD) {
        unsigned long long qw = 0;
        if (size >= sizeof(qw)) memcpy(&qw, buf, sizeof(qw));
        out = INTEGER_VAL((int64_t)qw);
    } else if (type == REG_BINARY && size == sizeof(double)) {
        double d = 0;
        memcpy(&d, buf, sizeof(d));
        out = REAL_VAL(d);
    } else {
        out = OBJ_VAL(obj_copy_string((const char*)buf, (int)size));
    }
    free(buf);
    return out;
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreLit");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistreExiste([<Mode>,] <Chemin> [, <Nom>]) -> booléen.
 * Sans <Nom> : teste l'existence de la CLÉ. Avec <Nom> : teste l'existence de
 * la VALEUR. Résultat booléen pur — ne pose pas de soft-error. */
static Value fn_registre_existe(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    bool hasName = (argCount - i) >= 2;
    const char* name = hasName ? reg_arg_cstr(args[i + 1]) : NULL;

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        return BOOL_VAL(false);
    }

    HKEY hKey;
    LONG rc = RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE | reg_wow_flag(mode), &hKey);
    if (rc != ERROR_SUCCESS) {
        return BOOL_VAL(false);
    }

    bool exists;
    if (hasName) {
        rc = RegQueryValueExA(hKey, name, NULL, NULL, NULL, NULL);
        exists = (rc == ERROR_SUCCESS);
    } else {
        exists = true;  /* l'ouverture a réussi -> la clé existe */
    }
    RegCloseKey(hKey);
    return BOOL_VAL(exists);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreExiste");
    return BOOL_VAL(false);
#endif
}

/* RegistreSupprimeValeur([<Mode>,] <Chemin>, <Nom>) -> booléen.
 * Valeur ou clé absente : Faux + soft-error. */
static Value fn_registre_supprime_valeur(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    const char* name = reg_arg_cstr(args[i + 1]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreSupprimeValeur", path);
        return BOOL_VAL(false);
    }

    HKEY hKey;
    LONG rc = RegOpenKeyExA(root, sub, 0, KEY_SET_VALUE | reg_wow_flag(mode), &hKey);
    if (rc != ERROR_SUCCESS) {
        reg_err_not_found("RegistreSupprimeValeur", path);
        return BOOL_VAL(false);
    }
    rc = RegDeleteValueA(hKey, name);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        reg_err_not_found("RegistreSupprimeValeur", name);
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreSupprimeValeur");
    return BOOL_VAL(false);
#endif
}

/* RegistreSupprimeClé([<Mode>,] <Chemin>) -> booléen.
 * Supprime la clé et toute sa descendance. Clé déjà absente : Vrai (idempotent). */
static Value fn_registre_supprime_cle(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    (void)mode;  /* RegDeleteTree ne prend pas de vue WOW64 explicite */
    const char* path = reg_arg_cstr(args[i]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreSupprimeCl\xc3\xa9", path);
        return BOOL_VAL(false);
    }

    LONG rc = RegDeleteTreeA(root, sub);
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) {
        return BOOL_VAL(true);  /* idempotent : absence = succès */
    }
    reg_err_op("RegistreSupprimeCl\xc3\xa9", path, "suppression impossible");
    return BOOL_VAL(false);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreSupprimeCl\xc3\xa9");
    return BOOL_VAL(false);
#endif
}

/* RegistreCopieClé([<Mode>,] <Source>, <Destination>) -> booléen.
 * Copie une clé entière (sous-clés et valeurs comprises) vers un autre
 * emplacement. Source absente : Faux + soft-error. */
static Value fn_registre_copie_cle(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* src = reg_arg_cstr(args[i]);
    const char* dst = reg_arg_cstr(args[i + 1]);
    REGSAM wow = reg_wow_flag(mode);

    HKEY sRoot; const char* sSub;
    HKEY dRoot; const char* dSub;
    if (!reg_split_root(src, &sRoot, &sSub)) {
        reg_err_bad_path("RegistreCopieCl\xc3\xa9", src);
        return BOOL_VAL(false);
    }
    if (!reg_split_root(dst, &dRoot, &dSub)) {
        reg_err_bad_path("RegistreCopieCl\xc3\xa9", dst);
        return BOOL_VAL(false);
    }

    HKEY hSrc;
    if (RegOpenKeyExA(sRoot, sSub, 0, KEY_READ | wow, &hSrc) != ERROR_SUCCESS) {
        reg_err_not_found("RegistreCopieCl\xc3\xa9", src);
        return BOOL_VAL(false);
    }
    HKEY hDst; DWORD disp;
    if (RegCreateKeyExA(dRoot, dSub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS | wow, NULL, &hDst, &disp)
            != ERROR_SUCCESS) {
        RegCloseKey(hSrc);
        reg_err_op("RegistreCopieCl\xc3\xa9", dst,
                   "cr\xc3\xa9\x61tion de la destination impossible");
        return BOOL_VAL(false);
    }
    LONG rc = RegCopyTreeA(hSrc, NULL, hDst);
    RegCloseKey(hSrc);
    RegCloseKey(hDst);
    if (rc != ERROR_SUCCESS) {
        reg_err_op("RegistreCopieCl\xc3\xa9", dst, "copie impossible");
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreCopieCl\xc3\xa9");
    return BOOL_VAL(false);
#endif
}

/* RegistreRenomme([<Mode>,] <AncienneClé>, <NouvelleClé>) -> booléen (clé)
 * RegistreRenomme([<Mode>,] <Chemin>, <AncienNom>, <NouveauNom>) -> booléen (valeur)
 * Renommage de clé = copie récursive vers le nouveau nom puis suppression de
 * l'ancienne ; destination déjà existante : Faux + soft-error. Renommage de
 * valeur = relecture/réécriture sous le nouveau nom puis suppression de
 * l'ancienne, en conservant le type. */
static Value fn_registre_renomme(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    int rem = argCount - i;
    REGSAM wow = reg_wow_flag(mode);

    if (rem >= 3) {
        /* Renommage d'une valeur dans une clé. */
        const char* path    = reg_arg_cstr(args[i]);
        const char* oldName = reg_arg_cstr(args[i + 1]);
        const char* newName = reg_arg_cstr(args[i + 2]);

        HKEY root; const char* sub;
        if (!reg_split_root(path, &root, &sub)) {
            reg_err_bad_path("RegistreRenomme", path);
            return BOOL_VAL(false);
        }
        HKEY hKey;
        if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | wow, &hKey)
                != ERROR_SUCCESS) {
            reg_err_not_found("RegistreRenomme", path);
            return BOOL_VAL(false);
        }
        if (RegQueryValueExA(hKey, newName, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            reg_err_op("RegistreRenomme", path,
                       "la valeur de destination existe d\xc3\xa9j\xc3\xa0");
            return BOOL_VAL(false);
        }
        DWORD type = 0, size = 0;
        if (RegQueryValueExA(hKey, oldName, NULL, &type, NULL, &size) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            reg_err_not_found("RegistreRenomme", oldName);
            return BOOL_VAL(false);
        }
        BYTE* buf = (BYTE*)malloc(size > 0 ? size : 1);
        if (buf == NULL) {
            RegCloseKey(hKey);
            reg_err_op("RegistreRenomme", path, "m\xc3\xa9moire insuffisante");
            return BOOL_VAL(false);
        }
        if (RegQueryValueExA(hKey, oldName, NULL, &type, buf, &size) != ERROR_SUCCESS) {
            free(buf);
            RegCloseKey(hKey);
            reg_err_not_found("RegistreRenomme", oldName);
            return BOOL_VAL(false);
        }
        LONG wr = RegSetValueExA(hKey, newName, 0, type, buf, size);
        free(buf);
        if (wr != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            reg_err_op("RegistreRenomme", path,
                       "\xc3\xa9\x63riture du nouveau nom impossible");
            return BOOL_VAL(false);
        }
        RegDeleteValueA(hKey, oldName);
        RegCloseKey(hKey);
        return BOOL_VAL(true);
    }

    /* Renommage d'une clé (copie récursive + suppression de l'ancienne). */
    const char* oldPath = reg_arg_cstr(args[i]);
    const char* newPath = reg_arg_cstr(args[i + 1]);
    HKEY oRoot; const char* oSub;
    HKEY nRoot; const char* nSub;
    if (!reg_split_root(oldPath, &oRoot, &oSub)) {
        reg_err_bad_path("RegistreRenomme", oldPath);
        return BOOL_VAL(false);
    }
    if (!reg_split_root(newPath, &nRoot, &nSub)) {
        reg_err_bad_path("RegistreRenomme", newPath);
        return BOOL_VAL(false);
    }

    HKEY hChk;
    if (RegOpenKeyExA(nRoot, nSub, 0, KEY_READ | wow, &hChk) == ERROR_SUCCESS) {
        RegCloseKey(hChk);
        reg_err_op("RegistreRenomme", newPath,
                   "la cl\xc3\xa9 de destination existe d\xc3\xa9j\xc3\xa0");
        return BOOL_VAL(false);
    }
    HKEY hSrc;
    if (RegOpenKeyExA(oRoot, oSub, 0, KEY_READ | wow, &hSrc) != ERROR_SUCCESS) {
        reg_err_not_found("RegistreRenomme", oldPath);
        return BOOL_VAL(false);
    }
    HKEY hDst; DWORD disp;
    if (RegCreateKeyExA(nRoot, nSub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS | wow, NULL, &hDst, &disp)
            != ERROR_SUCCESS) {
        RegCloseKey(hSrc);
        reg_err_op("RegistreRenomme", newPath,
                   "cr\xc3\xa9\x61tion de la destination impossible");
        return BOOL_VAL(false);
    }
    LONG rc = RegCopyTreeA(hSrc, NULL, hDst);
    RegCloseKey(hSrc);
    RegCloseKey(hDst);
    if (rc != ERROR_SUCCESS) {
        reg_err_op("RegistreRenomme", newPath, "copie impossible");
        return BOOL_VAL(false);
    }
    RegDeleteTreeA(oRoot, oSub);
    return BOOL_VAL(true);
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreRenomme");
    return BOOL_VAL(false);
#endif
}

/* RegistreTypeValeur([<Mode>,] <Chemin>, <Nom>) -> chaîne.
 * Renvoie une constante symbolique registreType* décrivant le type de la
 * valeur. Valeur ou clé absente : chaîne vide + soft-error. */
static Value fn_registre_type_valeur(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    const char* name = reg_arg_cstr(args[i + 1]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreTypeValeur", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE | reg_wow_flag(mode), &hKey)
            != ERROR_SUCCESS) {
        reg_err_not_found("RegistreTypeValeur", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    DWORD type = 0;
    LONG rc = RegQueryValueExA(hKey, name, NULL, &type, NULL, NULL);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        reg_err_not_found("RegistreTypeValeur", name);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    const char* t;
    switch (type) {
        case REG_SZ:               t = OWL_REGTYPE_STRING;    break;
        case REG_EXPAND_SZ:        t = OWL_REGTYPE_STRINGENV; break;
        case REG_DWORD:            t = OWL_REGTYPE_INT;       break;
        case REG_DWORD_BIG_ENDIAN: t = OWL_REGTYPE_INTBE;     break;
        case REG_QWORD:            t = OWL_REGTYPE_INT8;      break;
        case REG_MULTI_SZ:         t = OWL_REGTYPE_MULTI;     break;
        case REG_BINARY:           t = OWL_REGTYPE_BINARY;    break;
        default:                   t = OWL_REGTYPE_BINARY;    break;
    }
    return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreTypeValeur");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistreCherche([<Mode>,] <CheminDeBase>, <ChaîneRecherchée> [, <Options>]) -> chaîne.
 * Recherche le motif dans les noms de clés, de valeurs et/ou les données, en
 * récursif par défaut. Renvoie « <chemin> TAB <élément> [TAB <donnée>] » au
 * premier match, chaîne vide sinon. */
static Value fn_registre_cherche(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* basePath = reg_arg_cstr(args[i]);
    const char* needle   = reg_arg_cstr(args[i + 1]);
    long options = OWL_REG_SEEK_DEFAULT;
    if ((argCount - i) >= 3 && IS_INTEGER(args[i + 2])) {
        options = (long)AS_INTEGER(args[i + 2]);
    }

    /* Reprise de recherche non prise en charge : seul le chemin de clé (avant
     * la première tabulation) est conservé. */
    char baseBuf[2048];
    {
        const char* tab = strchr(basePath, '\t');
        size_t n = tab ? (size_t)(tab - basePath) : strlen(basePath);
        if (n >= sizeof(baseBuf)) n = sizeof(baseBuf) - 1;
        memcpy(baseBuf, basePath, n);
        baseBuf[n] = '\0';
    }

    HKEY root; const char* sub;
    if (!reg_split_root(baseBuf, &root, &sub)) {
        reg_err_bad_path("RegistreCherche", baseBuf);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    REGSAM wow = reg_wow_flag(mode);
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | wow, &hKey) != ERROR_SUCCESS) {
        reg_err_not_found("RegistreCherche", baseBuf);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char result[4096];
    result[0] = '\0';
    bool found = reg_seek_recursive(hKey, baseBuf, options, needle, wow,
                                    result, sizeof(result));
    RegCloseKey(hKey);
    /* Absence de correspondance = résultat normal (chaîne vide), pas une erreur. */
    if (!found) return OBJ_VAL(obj_copy_string("", 0));
    return OBJ_VAL(obj_copy_string(result, (int)strlen(result)));
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreCherche");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistreListeClé([<Mode>,] <Chemin>) -> chaîne.
 * Liste les noms des sous-clés directes, un par ligne (séparateur LF). Chaîne
 * vide si la clé ne contient aucune sous-clé ; chaîne vide + soft-error si la
 * clé est introuvable. */
static Value fn_registre_liste_cle(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreListeCl\xc3\xa9", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | reg_wow_flag(mode), &hKey)
            != ERROR_SUCCESS) {
        reg_err_not_found("RegistreListeCl\xc3\xa9", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    RegStrBuf b; regsb_init(&b);
    DWORD idx = 0; bool first = true;
    for (;;) {
        char  name[512];
        DWORD nameLen = (DWORD)sizeof(name);
        LONG rc = RegEnumKeyExA(hKey, idx, name, &nameLen, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;  /* y compris ERROR_NO_MORE_ITEMS */
        idx++;
        if (!first) regsb_append(&b, "\n", 1);
        regsb_append(&b, name, strlen(name));
        first = false;
    }
    RegCloseKey(hKey);

    Value out = OBJ_VAL(obj_copy_string(b.buf ? b.buf : "", (int)b.len));
    regsb_free(&b);
    return out;
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreListeCl\xc3\xa9");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistreListeValeur([<Mode>,] <Chemin> [, <Détaillé>]) -> chaîne.
 * Liste les noms des valeurs, un par ligne (séparateur LF). Avec <Détaillé> à
 * Vrai, ajoute après chaque nom une tabulation puis une lettre de type
 * (T chaîne, N numérique, B binaire). Chaîne vide si la clé ne contient aucune
 * valeur ; chaîne vide + soft-error si la clé est introuvable. */
static Value fn_registre_liste_valeur(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);
    bool detailed = (argCount - i) >= 2 && IS_BOOL(args[i + 1]) && AS_BOOL(args[i + 1]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistreListeValeur", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | reg_wow_flag(mode), &hKey)
            != ERROR_SUCCESS) {
        reg_err_not_found("RegistreListeValeur", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    RegStrBuf b; regsb_init(&b);
    DWORD idx = 0; bool first = true;
    for (;;) {
        char  name[16384];
        DWORD nameLen = (DWORD)sizeof(name);
        DWORD type = 0;
        LONG rc = RegEnumValueA(hKey, idx, name, &nameLen, NULL, &type, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        idx++;
        if (rc != ERROR_SUCCESS) continue;  /* valeur problématique : ignorée */
        if (!first) regsb_append(&b, "\n", 1);
        regsb_append(&b, name, strlen(name));
        if (detailed) {
            const char* t = reg_type_letter(type);
            regsb_append(&b, "\t", 1);
            regsb_append(&b, t, strlen(t));
        }
        first = false;
    }
    RegCloseKey(hKey);

    Value out = OBJ_VAL(obj_copy_string(b.buf ? b.buf : "", (int)b.len));
    regsb_free(&b);
    return out;
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreListeValeur");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistrePremièreSousClé([<Mode>,] <Chemin>) -> chaîne.
 * Renvoie le chemin complet de la première sous-clé. Amorce le parcours avec
 * RegistreCléSuivante. Chaîne vide (sans erreur) si la clé n'a aucune sous-clé ;
 * chaîne vide + soft-error si la clé est introuvable. */
static Value fn_registre_premiere_sous_cle(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* path = reg_arg_cstr(args[i]);

    HKEY root; const char* sub;
    if (!reg_split_root(path, &root, &sub)) {
        reg_err_bad_path("RegistrePremi\xc3\xa8reSousCl\xc3\xa9", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | reg_wow_flag(mode), &hKey)
            != ERROR_SUCCESS) {
        reg_err_not_found("RegistrePremi\xc3\xa8reSousCl\xc3\xa9", path);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char  name[512];
    DWORD nameLen = (DWORD)sizeof(name);
    LONG rc = RegEnumKeyExA(hKey, 0, name, &nameLen, NULL, NULL, NULL, NULL);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        /* Aucune sous-clé : résultat vide normal (pas une erreur). */
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char full[2600];
    snprintf(full, sizeof(full), "%s\\%s", path, name);
    return OBJ_VAL(obj_copy_string(full, (int)strlen(full)));
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistrePremi\xc3\xa8reSousCl\xc3\xa9");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* RegistreCléSuivante([<Mode>,] <Chemin>) -> chaîne.
 * <Chemin> est le chemin complet d'une sous-clé déjà obtenue. Renvoie le chemin
 * complet de la sous-clé frère suivante dans la même clé parente, ou une chaîne
 * vide en fin de parcours. L'ordre d'énumération Windows est stable tant que la
 * clé parente n'est pas modifiée, ce qui rend le couple Première/Suivante fiable
 * pour un balayage complet. Chaîne vide + soft-error si le chemin est invalide
 * ou introuvable. */
static Value fn_registre_cle_suivante(int argCount, Value* args) {
#if defined(_WIN32)
    long mode;
    int i = reg_consume_mode(argCount, args, &mode);
    const char* curPath = reg_arg_cstr(args[i]);

    const char* lastSep = strrchr(curPath, '\\');
    if (lastSep == NULL) {
        reg_err_bad_path("RegistreCl\xc3\xa9Suivante", curPath);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    size_t parentLen = (size_t)(lastSep - curPath);
    const char* leaf = lastSep + 1;
    char parent[2600];
    if (parentLen >= sizeof(parent)) parentLen = sizeof(parent) - 1;
    memcpy(parent, curPath, parentLen);
    parent[parentLen] = '\0';

    HKEY root; const char* sub;
    if (!reg_split_root(parent, &root, &sub)) {
        reg_err_bad_path("RegistreCl\xc3\xa9Suivante", curPath);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    HKEY hKey;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | reg_wow_flag(mode), &hKey)
            != ERROR_SUCCESS) {
        reg_err_not_found("RegistreCl\xc3\xa9Suivante", curPath);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    DWORD idx = 0; bool foundCur = false, haveNext = false;
    char  nextName[512];
    for (;;) {
        char  name[512];
        DWORD nameLen = (DWORD)sizeof(name);
        LONG rc = RegEnumKeyExA(hKey, idx, name, &nameLen, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;
        idx++;
        if (foundCur) {
            strncpy(nextName, name, sizeof(nextName) - 1);
            nextName[sizeof(nextName) - 1] = '\0';
            haveNext = true;
            break;
        }
        if (_stricmp(name, leaf) == 0) foundCur = true;
    }
    RegCloseKey(hKey);

    if (!foundCur) {
        /* La sous-clé courante n'existe pas parmi les frères : chemin invalide. */
        reg_err_not_found("RegistreCl\xc3\xa9Suivante", curPath);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (!haveNext) {
        /* Dernière sous-clé : fin de parcours normale (pas une erreur). */
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char full[2600];
    snprintf(full, sizeof(full), "%s\\%s", parent, nextName);
    return OBJ_VAL(obj_copy_string(full, (int)strlen(full)));
#else
    (void)argCount; (void)args;
    reg_err_unavailable("RegistreCl\xc3\xa9Suivante");
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* ================================================================
 * REGISTRY — signatures
 * ================================================================ */

static const OwlParam p_cle[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé, racine comprise "
     "(ex. HKEY_CURRENT_USER\\SOFTWARE\\MonApp).", NULL, NULL},
    {0}
};
static const OwlParam p_cle_sans_mode[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé, racine comprise.", NULL, NULL},
    {0}
};
static const OwlSignature sig_cree_cle[] = {
    {1, p_cle_sans_mode, "bool\xc3\xa9\x65n",
     "Vrai si la clé a été créée, Faux si elle existait déjà (sans erreur) ou "
     "en cas d'échec."},
    {2, p_cle, "bool\xc3\xa9\x65n",
     "Vrai si la clé a été créée, Faux si elle existait déjà ou en cas d'échec."},
    {0}
};
static const OwlSignature sig_supprime_cle[] = {
    {1, p_cle_sans_mode, "bool\xc3\xa9\x65n",
     "Vrai si la clé a été supprimée ou qu'elle était déjà absente (opération "
     "idempotente), Faux en cas d'échec."},
    {2, p_cle, "bool\xc3\xa9\x65n",
     "Vrai si la clé a été supprimée ou qu'elle était déjà absente, Faux en cas "
     "d'échec."},
    {0}
};

static const OwlParam p_ecrit[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé (doit déjà exister).", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à écrire.", NULL, NULL},
    {"valeur", "cha\xc3\xaene|entier|r\xc3\xa9\x65l", "Valeur à écrire.", NULL, NULL},
    {0}
};
static const OwlParam p_ecrit_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé (doit déjà exister).", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à écrire.", NULL, NULL},
    {"valeur", "cha\xc3\xaene|entier|r\xc3\xa9\x65l", "Valeur à écrire.", NULL, NULL},
    {0}
};
static const OwlSignature sig_ecrit[] = {
    {3, p_ecrit, "bool\xc3\xa9\x65n", "Vrai si l'écriture a réussi, Faux sinon."},
    {4, p_ecrit_mode, "bool\xc3\xa9\x65n", "Vrai si l'écriture a réussi, Faux sinon."},
    {0}
};

static const OwlParam p_lit[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à lire.", NULL, NULL},
    {0}
};
static const OwlParam p_lit_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à lire.", NULL, NULL},
    {0}
};
static const OwlSignature sig_lit[] = {
    {2, p_lit, "cha\xc3\xaene|entier|r\xc3\xa9\x65l",
     "Valeur lue (chaîne pour REG_SZ, entier pour une valeur numérique), ou "
     "chaîne vide si la lecture échoue."},
    {3, p_lit_mode, "cha\xc3\xaene|entier|r\xc3\xa9\x65l",
     "Valeur lue, ou chaîne vide si la lecture échoue."},
    {0}
};

static const OwlParam p_existe_cle[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {0}
};
static const OwlParam p_existe_val[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur dont on teste l'existence.", NULL, NULL},
    {0}
};
static const OwlParam p_existe_mode_val[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur dont on teste l'existence.", NULL, NULL},
    {0}
};
static const OwlSignature sig_existe[] = {
    {1, p_existe_cle, "bool\xc3\xa9\x65n", "Vrai si la clé existe."},
    {2, p_existe_val, "bool\xc3\xa9\x65n",
     "Vrai si la valeur existe dans la clé (ou, avec un mode en tête, si la clé "
     "existe)."},
    {3, p_existe_mode_val, "bool\xc3\xa9\x65n", "Vrai si la valeur existe dans la clé."},
    {0}
};

static const OwlParam p_supprime_val[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à supprimer.", NULL, NULL},
    {0}
};
static const OwlParam p_supprime_val_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur à supprimer.", NULL, NULL},
    {0}
};
static const OwlSignature sig_supprime_val[] = {
    {2, p_supprime_val, "bool\xc3\xa9\x65n", "Vrai si la valeur a été supprimée, Faux sinon."},
    {3, p_supprime_val_mode, "bool\xc3\xa9\x65n", "Vrai si la valeur a été supprimée, Faux sinon."},
    {0}
};

/* RegistreCopieClé */
static const OwlParam p_copie[] = {
    {"source", "cha\xc3\xaene", "Chemin complet de la clé source.", NULL, NULL},
    {"destination", "cha\xc3\xaene", "Chemin complet de la clé destination.", NULL, NULL},
    {0}
};
static const OwlParam p_copie_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"source", "cha\xc3\xaene", "Chemin complet de la clé source.", NULL, NULL},
    {"destination", "cha\xc3\xaene", "Chemin complet de la clé destination.", NULL, NULL},
    {0}
};
static const OwlSignature sig_copie[] = {
    {2, p_copie, "bool\xc3\xa9\x65n",
     "Vrai si la clé (et sa descendance) a été copiée, Faux sinon."},
    {3, p_copie_mode, "bool\xc3\xa9\x65n",
     "Vrai si la clé (et sa descendance) a été copiée, Faux sinon."},
    {0}
};

/* RegistreRenomme */
static const OwlParam p_renomme_cle[] = {
    {"ancienne", "cha\xc3\xaene", "Chemin complet de la clé à renommer.", NULL, NULL},
    {"nouvelle", "cha\xc3\xaene", "Chemin complet de la nouvelle clé.", NULL, NULL},
    {0}
};
static const OwlParam p_renomme_val[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé portant la valeur.", NULL, NULL},
    {"ancienNom", "cha\xc3\xaene", "Nom actuel de la valeur.", NULL, NULL},
    {"nouveauNom", "cha\xc3\xaene", "Nouveau nom de la valeur.", NULL, NULL},
    {0}
};
static const OwlParam p_renomme_val_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé portant la valeur.", NULL, NULL},
    {"ancienNom", "cha\xc3\xaene", "Nom actuel de la valeur.", NULL, NULL},
    {"nouveauNom", "cha\xc3\xaene", "Nouveau nom de la valeur.", NULL, NULL},
    {0}
};
static const OwlSignature sig_renomme[] = {
    {2, p_renomme_cle, "bool\xc3\xa9\x65n",
     "Vrai si la clé a été renommée, Faux si la destination existe déjà ou en "
     "cas d'échec."},
    {3, p_renomme_val, "bool\xc3\xa9\x65n",
     "Vrai si la valeur a été renommée, Faux sinon."},
    {4, p_renomme_val_mode, "bool\xc3\xa9\x65n",
     "Vrai si la valeur a été renommée, Faux sinon."},
    {0}
};

/* RegistreTypeValeur */
static const OwlParam p_typeval[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur dont on veut le type.", NULL, NULL},
    {0}
};
static const OwlParam p_typeval_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"nom", "cha\xc3\xaene", "Nom de la valeur dont on veut le type.", NULL, NULL},
    {0}
};
static const OwlSignature sig_typeval[] = {
    {2, p_typeval, "cha\xc3\xaene",
     "Constante registreType* décrivant le type de la valeur, ou chaîne vide si "
     "la valeur est absente."},
    {3, p_typeval_mode, "cha\xc3\xaene",
     "Constante registreType* décrivant le type de la valeur, ou chaîne vide si "
     "la valeur est absente."},
    {0}
};

/* RegistreCherche */
static const OwlParam p_cherche[] = {
    {"cheminDeBase", "cha\xc3\xaene", "Clé à partir de laquelle la recherche démarre.", NULL, NULL},
    {"chaineRecherchee", "cha\xc3\xaene", "Chaîne de caractères recherchée.", NULL, NULL},
    {0}
};
static const OwlParam p_cherche_opt[] = {
    {"cheminDeBase", "cha\xc3\xaene", "Clé à partir de laquelle la recherche démarre.", NULL, NULL},
    {"chaineRecherchee", "cha\xc3\xaene", "Chaîne de caractères recherchée.", NULL, NULL},
    {"options", "entier", "Combinaison de constantes registreCherche* restreignant la recherche.", NULL, NULL},
    {0}
};
static const OwlParam p_cherche_mode_opt[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"cheminDeBase", "cha\xc3\xaene", "Clé à partir de laquelle la recherche démarre.", NULL, NULL},
    {"chaineRecherchee", "cha\xc3\xaene", "Chaîne de caractères recherchée.", NULL, NULL},
    {"options", "entier", "Combinaison de constantes registreCherche* restreignant la recherche.", NULL, NULL},
    {0}
};
static const OwlSignature sig_cherche[] = {
    {2, p_cherche, "cha\xc3\xaene",
     "Résultat « <chemin> + TAB + <élément trouvé> » de la première "
     "correspondance, ou chaîne vide si rien n'est trouvé."},
    {3, p_cherche_opt, "cha\xc3\xaene",
     "Première correspondance trouvée, ou chaîne vide si rien n'est trouvé."},
    {4, p_cherche_mode_opt, "cha\xc3\xaene",
     "Première correspondance trouvée, ou chaîne vide si rien n'est trouvé."},
    {0}
};

/* RegistreListeClé */
static const OwlSignature sig_liste_cle[] = {
    {1, p_cle_sans_mode, "cha\xc3\xaene",
     "Noms des sous-clés directes, un par ligne (séparateur LF), ou chaîne vide "
     "si la clé n'a pas de sous-clé."},
    {2, p_cle, "cha\xc3\xaene",
     "Noms des sous-clés directes, un par ligne (séparateur LF), ou chaîne vide "
     "si la clé n'a pas de sous-clé."},
    {0}
};

/* RegistreListeValeur */
static const OwlParam p_liste_val[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"detaille", "bool\xc3\xa9\x65n", "Vrai pour ajouter le type de chaque valeur "
     "après une tabulation ; Faux (par défaut) pour ne lister que les noms.", NULL, NULL},
    {0}
};
static const OwlParam p_liste_val_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet de la clé.", NULL, NULL},
    {"detaille", "bool\xc3\xa9\x65n", "Vrai pour ajouter le type de chaque valeur "
     "après une tabulation ; Faux (par défaut) pour ne lister que les noms.", NULL, NULL},
    {0}
};
static const OwlSignature sig_liste_val[] = {
    {1, p_cle_sans_mode, "cha\xc3\xaene",
     "Noms des valeurs, un par ligne (séparateur LF), ou chaîne vide si la clé "
     "n'a pas de valeur."},
    {2, p_liste_val, "cha\xc3\xaene",
     "Noms des valeurs, un par ligne (séparateur LF). Avec le résultat détaillé, "
     "chaque nom est suivi d'une tabulation et d'une lettre de type (T chaîne, "
     "N numérique, B binaire). Avec un mode en tête, résultat simple."},
    {3, p_liste_val_mode, "cha\xc3\xaene",
     "Noms des valeurs, un par ligne (séparateur LF), avec le type après une "
     "tabulation si le résultat détaillé est demandé."},
    {0}
};

/* RegistrePremièreSousClé */
static const OwlSignature sig_premiere[] = {
    {1, p_cle_sans_mode, "cha\xc3\xaene",
     "Chemin complet de la première sous-clé, ou chaîne vide si la clé n'a pas "
     "de sous-clé."},
    {2, p_cle, "cha\xc3\xaene",
     "Chemin complet de la première sous-clé, ou chaîne vide si la clé n'a pas "
     "de sous-clé."},
    {0}
};

/* RegistreCléSuivante */
static const OwlParam p_suivante[] = {
    {"chemin", "cha\xc3\xaene", "Chemin complet d'une sous-clé déjà obtenue.", NULL, NULL},
    {0}
};
static const OwlParam p_suivante_mode[] = {
    {"mode", "entier", "Mode d'accès (registreModeAuto par défaut).", NULL, NULL},
    {"chemin", "cha\xc3\xaene", "Chemin complet d'une sous-clé déjà obtenue.", NULL, NULL},
    {0}
};
static const OwlSignature sig_suivante[] = {
    {1, p_suivante, "cha\xc3\xaene",
     "Chemin complet de la sous-clé frère suivante, ou chaîne vide en fin de "
     "parcours."},
    {2, p_suivante_mode, "cha\xc3\xaene",
     "Chemin complet de la sous-clé frère suivante, ou chaîne vide en fin de "
     "parcours."},
    {0}
};

static const OwlNativeDef DEFS_REGISTRY[] = {
    {
        .canonical  = "RegistreCr\xc3\xa9\x65\x43l\xc3\xa9",   /* RegistreCréeClé */
        .canonicalAscii = "RegistreCreeCle",
        .nameEN     = "RegistryCreateKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Crée une clé dans la base de registre, en créant au passage "
                      "les sous-clés intermédiaires manquantes. Renvoie Vrai si la "
                      "clé a été créée, Faux si elle existait déjà — recréer une clé "
                      "présente n'est pas une erreur. Disponible sous Windows "
                      "uniquement ; ailleurs, renvoie Faux avec une erreur "
                      "consultable via ErreurInfo.",
        .descEN     = "Creates a key in the registry, creating any missing "
                      "intermediate subkeys along the way. Returns True if the key "
                      "was created, False if it already existed — recreating an "
                      "existing key is not an error. Available on Windows only; "
                      "elsewhere it returns False with an error readable via "
                      "ErreurInfo.",
        .signatures = sig_cree_cle,
        .impl       = fn_registre_cree_cle,
    },
    {
        .canonical  = "RegistreEcrit",
        .nameEN     = "RegistrySetValue",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Écrit une valeur dans une clé de la base de registre. La clé "
                      "doit déjà exister. Le type d'écriture suit le type de la "
                      "donnée : une chaîne devient une valeur chaîne, un entier une "
                      "valeur numérique sur 4 octets. Renvoie Vrai en cas de succès, "
                      "Faux + erreur sinon. Disponible sous Windows uniquement.",
        .descEN     = "Writes a value into a registry key. The key must already "
                      "exist. The stored type follows the data type: a string "
                      "becomes a string value, an integer a 4-byte numeric value. "
                      "Returns True on success, False + error otherwise. Available "
                      "on Windows only.",
        .signatures = sig_ecrit,
        .impl       = fn_registre_ecrit,
    },
    {
        .canonical  = "RegistreLit",
        .nameEN     = "RegistryQueryValue",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Lit la valeur nommée d'une clé de la base de registre. "
                      "Renvoie une chaîne pour une valeur chaîne, un entier pour une "
                      "valeur numérique. Renvoie une chaîne vide, avec une erreur "
                      "consultable via ErreurInfo, si la valeur ou la clé est "
                      "absente — sans jamais interrompre le programme. Disponible "
                      "sous Windows uniquement.",
        .descEN     = "Reads the named value of a registry key. Returns a string "
                      "for a string value, an integer for a numeric value. Returns "
                      "an empty string, with an error readable via ErreurInfo, when "
                      "the value or key is missing — never interrupting the program. "
                      "Available on Windows only.",
        .signatures = sig_lit,
        .impl       = fn_registre_lit,
    },
    {
        .canonical  = "RegistreExiste",
        .nameEN     = "RegistryExist",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Teste l'existence d'une clé ou d'une valeur dans la base de "
                      "registre. Sans nom de valeur, teste l'existence de la clé ; "
                      "avec un nom, teste l'existence de la valeur dans la clé. "
                      "Renvoie un booléen, sans lever d'erreur bloquante. Disponible "
                      "sous Windows uniquement.",
        .descEN     = "Tests whether a key or a value exists in the registry. "
                      "Without a value name, tests the existence of the key; with a "
                      "name, tests the existence of the value in the key. Returns a "
                      "boolean, without raising a blocking error. Available on "
                      "Windows only.",
        .signatures = sig_existe,
        .impl       = fn_registre_existe,
    },
    {
        .canonical  = "RegistreSupprimeValeur",
        .nameEN     = "RegistryDeleteValue",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Supprime une valeur nommée dans une clé de la base de "
                      "registre. Renvoie Vrai en cas de succès ; une valeur ou une "
                      "clé absente renvoie Faux avec une erreur consultable via "
                      "ErreurInfo, sans crash. Disponible sous Windows uniquement.",
        .descEN     = "Deletes a named value from a registry key. Returns True on "
                      "success; a missing value or key returns False with an error "
                      "readable via ErreurInfo, without crashing. Available on "
                      "Windows only.",
        .signatures = sig_supprime_val,
        .impl       = fn_registre_supprime_valeur,
    },
    {
        .canonical  = "RegistreSupprimeCl\xc3\xa9",   /* RegistreSupprimeClé */
        .canonicalAscii = "RegistreSupprimeCle",
        .nameEN     = "RegistryDeleteKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Supprime une clé de la base de registre, avec toutes ses "
                      "valeurs et sous-clés. Supprimer une clé déjà absente est "
                      "considéré comme un succès (opération idempotente). Renvoie "
                      "Vrai en cas de succès, Faux + erreur en cas d'échec réel. "
                      "Disponible sous Windows uniquement.",
        .descEN     = "Deletes a registry key together with all its values and "
                      "subkeys. Deleting an already-missing key counts as success "
                      "(idempotent operation). Returns True on success, False + "
                      "error on a real failure. Available on Windows only.",
        .signatures = sig_supprime_cle,
        .impl       = fn_registre_supprime_cle,
    },
    {
        .canonical  = "RegistreCopieCl\xc3\xa9",   /* RegistreCopieClé */
        .canonicalAscii = "RegistreCopieCle",
        .nameEN     = "RegistryCopyKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Copie une clé de la base de registre, avec toutes ses "
                      "sous-clés et ses valeurs, vers un autre emplacement. La clé "
                      "destination est créée si besoin. Renvoie Vrai en cas de "
                      "succès, Faux + erreur consultable via ErreurInfo si la "
                      "source est absente ou en cas d'échec. Disponible sous "
                      "Windows uniquement.",
        .descEN     = "Copies a registry key, together with all its subkeys and "
                      "values, to another location. The destination key is created "
                      "if needed. Returns True on success, False + an error "
                      "readable via ErreurInfo when the source is missing or on "
                      "failure. Available on Windows only.",
        .signatures = sig_copie,
        .impl       = fn_registre_copie_cle,
    },
    {
        .canonical  = "RegistreRenomme",
        .nameEN     = "RegistryRename",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Renomme une clé ou une valeur de la base de registre. Avec "
                      "deux chemins de clé, renomme la clé (copie récursive puis "
                      "suppression de l'ancienne) ; échoue si la destination "
                      "existe déjà. Avec un chemin de clé et deux noms, renomme la "
                      "valeur en conservant son type. Renvoie Vrai en cas de "
                      "succès, Faux + erreur sinon. Disponible sous Windows "
                      "uniquement.",
        .descEN     = "Renames a registry key or value. With two key paths, renames "
                      "the key (recursive copy then deletion of the old one); fails "
                      "if the destination already exists. With a key path and two "
                      "names, renames the value while keeping its type. Returns True "
                      "on success, False + error otherwise. Available on Windows "
                      "only.",
        .signatures = sig_renomme,
        .impl       = fn_registre_renomme,
    },
    {
        .canonical  = "RegistreTypeValeur",
        .nameEN     = "RegistryValueType",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Renvoie le type d'une valeur de la base de registre sous la "
                      "forme d'une constante symbolique registreType* (chaîne, "
                      "entier, binaire…). Renvoie une chaîne vide, avec une erreur "
                      "consultable via ErreurInfo, si la valeur est absente. "
                      "Disponible sous Windows uniquement.",
        .descEN     = "Returns the type of a registry value as a symbolic "
                      "registryType* constant (string, integer, binary…). Returns "
                      "an empty string, with an error readable via ErreurInfo, when "
                      "the value is missing. Available on Windows only.",
        .signatures = sig_typeval,
        .impl       = fn_registre_type_valeur,
    },
    {
        .canonical  = "RegistreCherche",
        .nameEN     = "RegistrySeek",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Recherche une chaîne de caractères dans la base de registre, "
                      "dans les noms de clés, les noms de valeurs et/ou les "
                      "données, de façon récursive par défaut. Les constantes "
                      "registreCherche* permettent de restreindre la recherche. "
                      "Renvoie la première correspondance trouvée, ou une chaîne "
                      "vide si rien n'est trouvé. Disponible sous Windows "
                      "uniquement.",
        .descEN     = "Searches for a string in the registry, within key names, "
                      "value names and/or data, recursively by default. The "
                      "registrySeek* constants restrict the search. Returns the "
                      "first match found, or an empty string if nothing matches. "
                      "Available on Windows only.",
        .signatures = sig_cherche,
        .impl       = fn_registre_cherche,
    },
    {
        .canonical  = "RegistreListeCl\xc3\xa9",   /* RegistreListeClé */
        .canonicalAscii = "RegistreListeCle",
        .nameEN     = "RegistryListKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Liste les sous-clés directes d'une clé de la base de registre "
                      "sous la forme d'une chaîne, un nom de sous-clé par ligne "
                      "(séparateur saut de ligne). Renvoie une chaîne vide si la clé "
                      "n'a pas de sous-clé, ou une chaîne vide avec une erreur "
                      "consultable via ErreurInfo si la clé est introuvable. "
                      "Disponible sous Windows uniquement.",
        .descEN     = "Lists the direct subkeys of a registry key as a string, one "
                      "subkey name per line (line-feed separator). Returns an empty "
                      "string when the key has no subkey, or an empty string with an "
                      "error readable via ErreurInfo when the key is missing. "
                      "Available on Windows only.",
        .signatures = sig_liste_cle,
        .impl       = fn_registre_liste_cle,
    },
    {
        .canonical  = "RegistreListeValeur",
        .nameEN     = "RegistryListValue",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Liste les valeurs d'une clé de la base de registre sous la "
                      "forme d'une chaîne, un nom de valeur par ligne (séparateur "
                      "saut de ligne). Avec le résultat détaillé, chaque nom est "
                      "suivi d'une tabulation puis d'une lettre de type (T chaîne, "
                      "N numérique, B binaire). Renvoie une chaîne vide si la clé "
                      "n'a pas de valeur, ou une chaîne vide + erreur via ErreurInfo "
                      "si la clé est introuvable. Disponible sous Windows uniquement.",
        .descEN     = "Lists the values of a registry key as a string, one value "
                      "name per line (line-feed separator). With the detailed "
                      "result, each name is followed by a tab then a type letter "
                      "(T string, N numeric, B binary). Returns an empty string when "
                      "the key has no value, or an empty string + error via "
                      "ErreurInfo when the key is missing. Available on Windows only.",
        .signatures = sig_liste_val,
        .impl       = fn_registre_liste_valeur,
    },
    {
        .canonical  = "RegistrePremi\xc3\xa8reSousCl\xc3\xa9",   /* RegistrePremièreSousClé */
        .canonicalAscii = "RegistrePremiereSousCle",
        .nameEN     = "RegistryFirstSubKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Renvoie le chemin complet de la première sous-clé d'une clé "
                      "de la base de registre. Combinée à RegistreCléSuivante, elle "
                      "amorce le parcours de toutes les sous-clés. Renvoie une "
                      "chaîne vide si la clé n'a pas de sous-clé, ou une chaîne vide "
                      "+ erreur via ErreurInfo si la clé est introuvable. Disponible "
                      "sous Windows uniquement.",
        .descEN     = "Returns the full path of the first subkey of a registry key. "
                      "Combined with RegistryNextKey, it starts a walk over all "
                      "subkeys. Returns an empty string when the key has no subkey, "
                      "or an empty string + error via ErreurInfo when the key is "
                      "missing. Available on Windows only.",
        .signatures = sig_premiere,
        .impl       = fn_registre_premiere_sous_cle,
    },
    {
        .canonical  = "RegistreCl\xc3\xa9Suivante",   /* RegistreCléSuivante */
        .canonicalAscii = "RegistreCleSuivante",
        .nameEN     = "RegistryNextKey",
        .category   = "Syst\xc3\xa8me",
        .descFR     = "Renvoie le chemin complet de la sous-clé suivant une sous-clé "
                      "donnée, au sein de la même clé parente. Forme avec "
                      "RegistrePremièreSousClé le couple de parcours des sous-clés : "
                      "la chaîne vide signale la fin. Renvoie une chaîne vide + "
                      "erreur via ErreurInfo si le chemin est invalide ou "
                      "introuvable. Disponible sous Windows uniquement.",
        .descEN     = "Returns the full path of the subkey following a given subkey "
                      "within the same parent key. Together with "
                      "RegistryFirstSubKey, it forms the subkey-walking pair: an "
                      "empty string signals the end. Returns an empty string + error "
                      "via ErreurInfo when the path is invalid or missing. Available "
                      "on Windows only.",
        .signatures = sig_suivante,
        .impl       = fn_registre_cle_suivante,
    },
};

static const OwlConstDef CONSTS_REGISTRY[] = {
    {
        .name = "registreModeAuto", .category = "Syst\xc3\xa8me",
        .value = INTEGER_VAL(OWL_REG_MODE_AUTO),
        .descFR = "Mode d'accès au registre par défaut : vue native du processus.",
        .descEN = "Default registry access mode: the process's native view.",
        .nameEN = "registryModeAuto",
    },
    {
        .name = "registreMode32", .category = "Syst\xc3\xa8me",
        .value = INTEGER_VAL(OWL_REG_MODE_32),
        .descFR = "Force l'accès à la vue 32 bits du registre (WOW6432Node).",
        .descEN = "Forces access to the 32-bit view of the registry (WOW6432Node).",
        .nameEN = "registryMode32",
    },
    {
        .name = "registreMode64", .category = "Syst\xc3\xa8me",
        .value = INTEGER_VAL(OWL_REG_MODE_64),
        .descFR = "Force l'accès à la vue 64 bits du registre.",
        .descEN = "Forces access to the 64-bit view of the registry.",
        .nameEN = "registryMode64",
    },
    {
        .name = "registreCherche\x43l\xc3\xa9", .category = "Syst\xc3\xa8me",  /* registreChercheClé */
        .value = INTEGER_VAL(OWL_REG_SEEK_KEY),
        .descFR = "Option de RegistreCherche : recherche dans les noms de clés.",
        .descEN = "RegistrySeek option: search within key names.",
        .nameEN = "registrySeekKey",
    },
    {
        .name = "registreChercheValeur", .category = "Syst\xc3\xa8me",
        .value = INTEGER_VAL(OWL_REG_SEEK_VALUE),
        .descFR = "Option de RegistreCherche : recherche dans les noms de valeurs.",
        .descEN = "RegistrySeek option: search within value names.",
        .nameEN = "registrySeekValue",
    },
    {
        .name = "registreChercheDonn\xc3\xa9\x65", .category = "Syst\xc3\xa8me",  /* registreChercheDonnée */
        .value = INTEGER_VAL(OWL_REG_SEEK_DATA),
        .descFR = "Option de RegistreCherche : recherche dans les données de type chaîne.",
        .descEN = "RegistrySeek option: search within string-type data.",
        .nameEN = "registrySeekData",
    },
    {
        .name = "registreChercheR\xc3\xa9\x63ursif", .category = "Syst\xc3\xa8me",  /* registreChercheRécursif */
        .value = INTEGER_VAL(OWL_REG_SEEK_RECURSIVE),
        .descFR = "Option de RegistreCherche : recherche récursive depuis la clé de base.",
        .descEN = "RegistrySeek option: recursive search from the base key.",
        .nameEN = "registrySeekRecursive",
    },
    {
        .name = "registreChercheMotComplet", .category = "Syst\xc3\xa8me",
        .value = INTEGER_VAL(OWL_REG_SEEK_WHOLE),
        .descFR = "Option de RegistreCherche : correspondance sur le mot complet uniquement.",
        .descEN = "RegistrySeek option: match on the whole word only.",
        .nameEN = "registrySeekWholeWord",
    },
};

/* Constantes registreType* : valeurs chaîne construites au démarrage (jetons
 * renvoyés par RegistreTypeValeur). Table source adossée aux macros OWL_REGTYPE_*. */
static const struct {
    const char* nameFR; const char* nameEN; const char* value;
    const char* descFR; const char* descEN;
} REGTYPE_CONSTS[] = {
    {"registreTypeCha\xc3\xaene", "registryTypeString", OWL_REGTYPE_STRING,
     "Type de valeur de la base de registre : chaîne de caractères.",
     "Registry value type: character string."},
    {"registreTypeCha\xc3\xaeneEnv", "registryTypeStringEnv", OWL_REGTYPE_STRINGENV,
     "Type de valeur de la base de registre : chaîne avec variables d'environnement.",
     "Registry value type: string containing environment variables."},
    {"registreTypeEntier", "registryTypeInt", OWL_REGTYPE_INT,
     "Type de valeur de la base de registre : entier sur 4 octets.",
     "Registry value type: 4-byte integer."},
    {"registreTypeEntier_8", "registryTypeInt_8", OWL_REGTYPE_INT8,
     "Type de valeur de la base de registre : entier sur 8 octets.",
     "Registry value type: 8-byte integer."},
    {"registreTypeEntierBigEndian", "registryTypeIntegerBigEndian", OWL_REGTYPE_INTBE,
     "Type de valeur de la base de registre : entier au format gros-boutiste.",
     "Registry value type: big-endian integer."},
    {"registreTypeBinaire", "registryTypeBinary", OWL_REGTYPE_BINARY,
     "Type de valeur de la base de registre : données binaires.",
     "Registry value type: binary data."},
    {"registreTypeMultiCha\xc3\xaene", "registryTypeMultiString", OWL_REGTYPE_MULTI,
     "Type de valeur de la base de registre : liste de chaînes.",
     "Registry value type: list of strings."},
};

void stdlib_registry_init(void) {
    owl_registry_register_module(DEFS_REGISTRY,
        (int)(sizeof(DEFS_REGISTRY) / sizeof(DEFS_REGISTRY[0])));
    owl_registry_register_consts(CONSTS_REGISTRY,
        (int)(sizeof(CONSTS_REGISTRY) / sizeof(CONSTS_REGISTRY[0])));

    /* Constantes registreType* : valeur chaîne construite ici. On roote chaque
     * ObjString sur la pile VM le temps que register_consts la fixe dans
     * vm.globals (root permanent), puis on dépile. */
    {
        enum { NT = (int)(sizeof(REGTYPE_CONSTS) / sizeof(REGTYPE_CONSTS[0])) };
        static OwlConstDef typeConsts[NT];
        for (int i = 0; i < NT; i++) {
            ObjString* s = obj_copy_string(REGTYPE_CONSTS[i].value,
                                           (int)strlen(REGTYPE_CONSTS[i].value));
            vm_push(OBJ_VAL(s));
            typeConsts[i].name     = REGTYPE_CONSTS[i].nameFR;
            typeConsts[i].category = "Syst\xc3\xa8me";
            typeConsts[i].value    = OBJ_VAL(s);
            typeConsts[i].descFR   = REGTYPE_CONSTS[i].descFR;
            typeConsts[i].descEN   = REGTYPE_CONSTS[i].descEN;
            typeConsts[i].nameEN   = REGTYPE_CONSTS[i].nameEN;
        }
        owl_registry_register_consts(typeConsts, NT);
        for (int i = 0; i < NT; i++) vm_pop();
    }
}
